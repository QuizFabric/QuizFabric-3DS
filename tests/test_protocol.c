// Host-side tests for the JSON and Socket.IO layers.
//
// json.c and sio.c deliberately avoid 3DS headers, so they build and run on a
// normal PC. Run with tests\run_tests.bat.

#include "../source/json.h"
#include "../source/sio.h"

#include <stdio.h>
#include <string.h>

// --- ws.c stub: sio.c only ever calls ws_send_text ---------------------------
static char g_sent[4096];

bool ws_send_text(WsConn* c, const char* data, int len)
{
	(void)c;
	int n = len < (int)sizeof(g_sent) - 1 ? len : (int)sizeof(g_sent) - 1;
	memcpy(g_sent, data, (size_t)n);
	g_sent[n] = '\0';
	return true;
}

// --- tiny harness ------------------------------------------------------------
static int g_pass = 0, g_fail = 0;

static void ok(int cond, const char* what)
{
	if (cond) {
		g_pass++;
	} else {
		g_fail++;
		printf("  FAIL: %s\n", what);
	}
}

static void eqs(const char* got, const char* want, const char* what)
{
	if (got && !strcmp(got, want)) {
		g_pass++;
	} else {
		g_fail++;
		printf("  FAIL: %s\n        got  '%s'\n        want '%s'\n",
		       what, got ? got : "(null)", want);
	}
}

static void eqi(long got, long want, const char* what)
{
	if (got == want) {
		g_pass++;
	} else {
		g_fail++;
		printf("  FAIL: %s (got %ld, want %ld)\n", what, got, want);
	}
}

static SioMsg M;

// sio_decode mutates its input, so every case works on a fresh copy.
static void decode(const char* literal)
{
	static char buf[8192];
	snprintf(buf, sizeof(buf), "%s", literal);
	sio_decode(buf, (int)strlen(buf), &M);
}

// --- tests -------------------------------------------------------------------

static void test_engineio_open(void)
{
	printf("engine.io open\n");
	decode("0{\"sid\":\"aBc123\",\"upgrades\":[\"websocket\"],"
	       "\"pingInterval\":25000,\"pingTimeout\":20000,\"maxPayload\":1000000}");
	eqi(M.kind, SIO_OPEN, "kind is SIO_OPEN");
	eqi(M.pingInterval, 25000, "pingInterval");
	eqi(M.pingTimeout, 20000, "pingTimeout");
}

static void test_control_packets(void)
{
	printf("control packets\n");
	decode("2");
	eqi(M.kind, SIO_PING, "'2' is a ping");

	decode("40{\"sid\":\"xyz789\"}");
	eqi(M.kind, SIO_CONNECTED, "'40' is namespace connect");

	decode("41");
	eqi(M.kind, SIO_DISCONNECTED, "'41' is disconnect");

	decode("44{\"message\":\"nope\"}");
	eqi(M.kind, SIO_CONNECT_ERROR, "'44' is connect error");
}

static void test_new_question_mc(void)
{
	printf("new_question (multiple choice)\n");
	decode("42[\"new_question\",{\"question\":\"Which block is craftable?\","
	       "\"answers\":[\"Bedrock\",\"Chest\",\"Barrier\",\"Command Block\"],"
	       "\"type\":\"mc\"}]");

	eqi(M.kind, SIO_EVENT, "kind is SIO_EVENT");
	eqs(M.event, "new_question", "event name");
	eqs(json_ostr(&M.doc, M.data, "question", NULL),
	    "Which block is craftable?", "question text");
	eqs(json_ostr(&M.doc, M.data, "type", NULL), "mc", "type");

	int arr = json_obj_get(&M.doc, M.data, "answers");
	eqi(json_arr_len(&M.doc, arr), 4, "answer count");
	eqs(json_str(&M.doc, json_arr_at(&M.doc, arr, 0), NULL), "Bedrock", "answer 0");
	eqs(json_str(&M.doc, json_arr_at(&M.doc, arr, 3), NULL), "Command Block", "answer 3");
	eqi(json_arr_at(&M.doc, arr, 4), -1, "out-of-range answer is -1");
}

static void test_new_question_type_in(void)
{
	printf("new_question (type-in, with images)\n");
	decode("42[\"new_question\",{\"question\":\"Name the mob\",\"type\":\"type\","
	       "\"images\":[\"/UploadedImages/Quiz/a.png\"]}]");

	eqs(M.event, "new_question", "event name");
	eqs(json_ostr(&M.doc, M.data, "type", NULL), "type", "type is type-in");
	eqi(json_arr_len(&M.doc, json_obj_get(&M.doc, M.data, "images")), 1, "one image");
	// A type-in question carries no answers array at all.
	eqi(json_obj_get(&M.doc, M.data, "answers"), -1, "no answers key");
}

static void test_leaderboard(void)
{
	printf("leaderboard\n");
	decode("42[\"leaderboard\",{\"leaderboard\":["
	       "{\"name\":\"Adam\",\"score\":950,\"last_correct\":true,\"avatar\":\"Pusheen\",\"left\":false},"
	       "{\"name\":\"Lucas\",\"score\":400,\"last_correct\":false,\"avatar\":\"Steve\",\"left\":false},"
	       "{\"name\":\"Ghost\",\"score\":0,\"last_correct\":null,\"avatar\":\"\",\"left\":true}],"
	       "\"correct_answer\":\"Chest\",\"submitted_answers\":{\"Adam\":\"Chest\"}}]");

	eqs(M.event, "leaderboard", "event name");
	eqs(json_ostr(&M.doc, M.data, "correct_answer", NULL), "Chest", "correct_answer");

	int arr = json_obj_get(&M.doc, M.data, "leaderboard");
	eqi(json_arr_len(&M.doc, arr), 3, "three rows");

	int r0 = json_arr_at(&M.doc, arr, 0);
	eqs(json_ostr(&M.doc, r0, "name", NULL), "Adam", "row0 name");
	eqi((long)json_onum(&M.doc, r0, "score", -1), 950, "row0 score");
	ok(json_obool(&M.doc, r0, "last_correct", false) == true, "row0 last_correct true");
	ok(json_obool(&M.doc, r0, "left", true) == false, "row0 not left");

	int r2 = json_arr_at(&M.doc, arr, 2);
	ok(json_obool(&M.doc, r2, "left", false) == true, "row2 left");
	// null last_correct must stay distinguishable from false.
	int lc = json_obj_get(&M.doc, r2, "last_correct");
	ok(lc >= 0 && M.doc.nodes[lc].type == JS_NULL, "row2 last_correct is null");
}

static void test_answer_revealed(void)
{
	printf("answer_revealed / error\n");
	decode("42[\"answer_revealed\",{\"correct\":true,\"correct_answer\":\"42\"}]");
	ok(json_obool(&M.doc, M.data, "correct", false) == true, "correct flag");
	eqs(json_ostr(&M.doc, M.data, "correct_answer", NULL), "42", "correct answer");

	decode("42[\"error\",{\"message\":\"Username unavailable. Please choose a different one.\"}]");
	eqs(M.event, "error", "error event name");
	eqs(json_ostr(&M.doc, M.data, "message", NULL),
	    "Username unavailable. Please choose a different one.", "error message");
}

static void test_escapes(void)
{
	printf("string escapes\n");
	decode("42[\"new_question\",{\"question\":\"He said \\\"go\\\" \\u00e9\\u00e8 "
	       "and\\nthen left\\\\\"}]");
	// é -> C3 A9, è -> C3 A8 in UTF-8.
	eqs(json_ostr(&M.doc, M.data, "question", NULL),
	    "He said \"go\" \xc3\xa9\xc3\xa8 and\nthen left\\", "unescaped question");
}

static void test_namespace_and_ack(void)
{
	printf("namespace / ack-id skipping\n");
	decode("42/admin,[\"ping_event\",{\"n\":1}]");
	eqi(M.kind, SIO_EVENT, "namespaced event decodes");
	eqs(M.event, "ping_event", "namespaced event name");

	decode("4217[\"acked\",{\"n\":2}]");
	eqi(M.kind, SIO_EVENT, "ack-id event decodes");
	eqs(M.event, "acked", "ack-id event name");
	eqi((long)json_onum(&M.doc, M.data, "n", -1), 2, "ack-id payload");
}

static void test_malformed(void)
{
	printf("malformed input\n");
	decode("42[");
	ok(M.kind == SIO_UNKNOWN, "truncated array does not crash");

	decode("42[\"unterminated");
	ok(M.kind == SIO_UNKNOWN, "unterminated string does not crash");

	decode("");
	ok(M.kind == SIO_NONE, "empty message is SIO_NONE");

	decode("9zzz");
	ok(M.kind == SIO_UNKNOWN, "unknown engine.io type");

	decode("42[123,{}]");
	ok(M.kind == SIO_UNKNOWN, "non-string event name rejected");

	decode("42[\"noargs\"]");
	ok(M.kind == SIO_EVENT && M.data == -1, "event with no args has data == -1");
	// Reading fields off a missing payload must be safe.
	eqs(json_ostr(&M.doc, M.data, "whatever", "fallback"), "fallback",
	    "missing payload falls back");
}

static void test_emit(void)
{
	printf("outgoing packets\n");
	sio_connect_ns(NULL);
	eqs(g_sent, "40", "namespace connect packet");

	sio_pong(NULL);
	eqs(g_sent, "3", "pong packet");

	sio_emit(NULL, "join_game", "{\"code\":\"1234\",\"name\":\"Adam\",\"avatar\":\"Pusheen\"}");
	eqs(g_sent, "42[\"join_game\",{\"code\":\"1234\",\"name\":\"Adam\",\"avatar\":\"Pusheen\"}]",
	    "join_game emit");

	sio_emit(NULL, "submit_answer", "{\"code\":\"1234\",\"answer\":2,\"time_taken\":3.40}");
	eqs(g_sent, "42[\"submit_answer\",{\"code\":\"1234\",\"answer\":2,\"time_taken\":3.40}]",
	    "submit_answer emit");
}

static void test_escape_helper(void)
{
	printf("json_escape\n");
	char out[256];

	json_escape("plain", out, sizeof(out));
	eqs(out, "plain", "plain text unchanged");

	json_escape("say \"hi\"", out, sizeof(out));
	eqs(out, "say \\\"hi\\\"", "quotes escaped");

	json_escape("back\\slash", out, sizeof(out));
	eqs(out, "back\\\\slash", "backslash escaped");

	json_escape("line\nbreak\ttab", out, sizeof(out));
	eqs(out, "line\\nbreak\\ttab", "control chars escaped");

	// A name full of quotes must not overrun a small buffer.
	char small[12];
	json_escape("\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"", small, sizeof(small));
	ok(strlen(small) < sizeof(small), "escape respects buffer size");

	// Round-trip: escaped output must re-parse to the original.
	char doc[256];
	json_escape("tricky \"name\" \\ here", out, sizeof(out));
	snprintf(doc, sizeof(doc), "{\"name\":\"%s\"}", out);
	static JsDoc d;
	ok(json_parse(&d, doc), "escaped payload re-parses");
	eqs(json_ostr(&d, 0, "name", NULL), "tricky \"name\" \\ here", "round-trip");
}

static void test_json_edges(void)
{
	printf("json edge cases\n");
	static JsDoc d;
	char buf[256];

	snprintf(buf, sizeof(buf), "%s", "{\"a\":[],\"b\":{},\"c\":-12.5,\"d\":null}");
	ok(json_parse(&d, buf), "empty containers parse");
	eqi(json_arr_len(&d, json_obj_get(&d, 0, "a")), 0, "empty array length 0");
	ok(json_onum(&d, 0, "c", 0) == -12.5, "negative float");
	eqi(json_obj_get(&d, 0, "missing"), -1, "missing key is -1");

	snprintf(buf, sizeof(buf), "%s", "{\"a\":1,}");
	ok(!json_parse(&d, buf), "trailing comma rejected");

	snprintf(buf, sizeof(buf), "%s", "[1,2,3]");
	ok(json_parse(&d, buf), "top-level array parses");
	eqi(json_arr_len(&d, 0), 3, "top-level array length");

	// Deeply nested input must fail cleanly rather than smash the node pool.
	char deep[4096];
	int n = 0;
	for (int i = 0; i < 700; i++) deep[n++] = '[';
	for (int i = 0; i < 700; i++) deep[n++] = ']';
	deep[n] = '\0';
	json_parse(&d, deep); // must not crash either way
	ok(1, "deep nesting handled without crashing");
}

int main(void)
{
	printf("QuizFabric 3DS protocol tests\n");
	printf("=============================\n");
	test_engineio_open();
	test_control_packets();
	test_new_question_mc();
	test_new_question_type_in();
	test_leaderboard();
	test_answer_revealed();
	test_escapes();
	test_namespace_and_ack();
	test_malformed();
	test_emit();
	test_escape_helper();
	test_json_edges();

	printf("=============================\n");
	printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
