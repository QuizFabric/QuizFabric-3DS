#include "json.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
	JsDoc* d;
	char*  p;
	bool   fail;
} Ctx;

static int parse_value(Ctx* c);

static int node_new(Ctx* c, JsType t)
{
	if (c->d->count >= JSON_MAX_NODES) {
		c->fail = true;
		return -1;
	}
	int i = c->d->count++;
	JsNode* n = &c->d->nodes[i];
	n->type  = t;
	n->key   = NULL;
	n->str   = NULL;
	n->num   = 0.0;
	n->bval  = false;
	n->first = -1;
	n->next  = -1;
	return i;
}

static void skip_ws(Ctx* c)
{
	while (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r') c->p++;
}

// Encodes a code point as UTF-8 into dst. Returns bytes written.
static int utf8_put(char* dst, unsigned cp)
{
	if (cp < 0x80) {
		dst[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		dst[0] = (char)(0xC0 | (cp >> 6));
		dst[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	dst[0] = (char)(0xE0 | (cp >> 12));
	dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
	dst[2] = (char)(0x80 | (cp & 0x3F));
	return 3;
}

static int hex4(const char* s)
{
	int v = 0;
	for (int i = 0; i < 4; i++) {
		char ch = s[i];
		int  dv;
		if (ch >= '0' && ch <= '9')      dv = ch - '0';
		else if (ch >= 'a' && ch <= 'f') dv = ch - 'a' + 10;
		else if (ch >= 'A' && ch <= 'F') dv = ch - 'A' + 10;
		else return -1;
		v = (v << 4) | dv;
	}
	return v;
}

// Unescapes the string starting at c->p (which points at the opening quote).
// Writes the result over the source bytes; escapes never expand, so this is safe.
static char* parse_string_raw(Ctx* c)
{
	if (*c->p != '"') {
		c->fail = true;
		return NULL;
	}
	c->p++;
	char* out   = c->p; // write head
	char* start = out;
	while (*c->p && *c->p != '"') {
		if (*c->p == '\\') {
			c->p++;
			char e = *c->p;
			switch (e) {
			case '"':  *out++ = '"';  c->p++; break;
			case '\\': *out++ = '\\'; c->p++; break;
			case '/':  *out++ = '/';  c->p++; break;
			case 'b':  *out++ = '\b'; c->p++; break;
			case 'f':  *out++ = '\f'; c->p++; break;
			case 'n':  *out++ = '\n'; c->p++; break;
			case 'r':  *out++ = '\r'; c->p++; break;
			case 't':  *out++ = '\t'; c->p++; break;
			case 'u': {
				int cp = hex4(c->p + 1);
				if (cp < 0) {
					c->fail = true;
					return NULL;
				}
				c->p += 5;
				// Surrogate pair: consume the low half if present.
				if (cp >= 0xD800 && cp <= 0xDBFF && c->p[0] == '\\' && c->p[1] == 'u') {
					int lo = hex4(c->p + 2);
					if (lo >= 0xDC00 && lo <= 0xDFFF) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						c->p += 6;
						// Astral planes need 4 bytes; the 6 source bytes cover it.
						out[0] = (char)(0xF0 | (cp >> 18));
						out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
						out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
						out[3] = (char)(0x80 | (cp & 0x3F));
						out += 4;
						break;
					}
				}
				out += utf8_put(out, (unsigned)cp);
				break;
			}
			default:
				c->fail = true;
				return NULL;
			}
		} else {
			*out++ = *c->p++;
		}
	}
	if (*c->p != '"') {
		c->fail = true;
		return NULL;
	}
	c->p++;      // step past the closing quote
	*out = '\0'; // safe: out never passes the closing quote position
	return start;
}

static int parse_object(Ctx* c)
{
	int self = node_new(c, JS_OBJ);
	if (self < 0) return -1;
	c->p++; // '{'
	skip_ws(c);
	if (*c->p == '}') {
		c->p++;
		return self;
	}
	int prev = -1;
	for (;;) {
		skip_ws(c);
		char* key = parse_string_raw(c);
		if (c->fail) return -1;
		skip_ws(c);
		if (*c->p != ':') {
			c->fail = true;
			return -1;
		}
		c->p++;
		skip_ws(c);
		int child = parse_value(c);
		if (child < 0) return -1;
		c->d->nodes[child].key = key;
		if (prev < 0) c->d->nodes[self].first = child;
		else          c->d->nodes[prev].next  = child;
		prev = child;
		skip_ws(c);
		if (*c->p == ',') {
			c->p++;
			continue;
		}
		if (*c->p == '}') {
			c->p++;
			return self;
		}
		c->fail = true;
		return -1;
	}
}

static int parse_array(Ctx* c)
{
	int self = node_new(c, JS_ARR);
	if (self < 0) return -1;
	c->p++; // '['
	skip_ws(c);
	if (*c->p == ']') {
		c->p++;
		return self;
	}
	int prev = -1;
	for (;;) {
		skip_ws(c);
		int child = parse_value(c);
		if (child < 0) return -1;
		if (prev < 0) c->d->nodes[self].first = child;
		else          c->d->nodes[prev].next  = child;
		prev = child;
		skip_ws(c);
		if (*c->p == ',') {
			c->p++;
			continue;
		}
		if (*c->p == ']') {
			c->p++;
			return self;
		}
		c->fail = true;
		return -1;
	}
}

static int parse_value(Ctx* c)
{
	skip_ws(c);
	char ch = *c->p;
	if (ch == '{') return parse_object(c);
	if (ch == '[') return parse_array(c);
	if (ch == '"') {
		char* s = parse_string_raw(c);
		if (c->fail) return -1;
		int i = node_new(c, JS_STR);
		if (i < 0) return -1;
		c->d->nodes[i].str = s;
		return i;
	}
	if (!strncmp(c->p, "true", 4)) {
		c->p += 4;
		int i = node_new(c, JS_BOOL);
		if (i >= 0) c->d->nodes[i].bval = true;
		return i;
	}
	if (!strncmp(c->p, "false", 5)) {
		c->p += 5;
		int i = node_new(c, JS_BOOL);
		if (i >= 0) c->d->nodes[i].bval = false;
		return i;
	}
	if (!strncmp(c->p, "null", 4)) {
		c->p += 4;
		return node_new(c, JS_NULL);
	}
	if (ch == '-' || (ch >= '0' && ch <= '9')) {
		char* end = NULL;
		double v = strtod(c->p, &end);
		if (end == c->p) {
			c->fail = true;
			return -1;
		}
		c->p = end;
		int i = node_new(c, JS_NUM);
		if (i >= 0) c->d->nodes[i].num = v;
		return i;
	}
	c->fail = true;
	return -1;
}

bool json_parse(JsDoc* d, char* text)
{
	d->count = 0;
	d->ok    = false;
	if (!text) return false;

	Ctx c = { d, text, false };
	int root = parse_value(&c);
	if (c.fail || root != 0) return false;
	d->ok = true;
	return true;
}

static bool valid(JsDoc* d, int idx)
{
	return d && d->ok && idx >= 0 && idx < d->count;
}

int json_obj_get(JsDoc* d, int obj, const char* key)
{
	if (!valid(d, obj) || d->nodes[obj].type != JS_OBJ) return -1;
	for (int i = d->nodes[obj].first; i >= 0; i = d->nodes[i].next) {
		if (d->nodes[i].key && !strcmp(d->nodes[i].key, key)) return i;
	}
	return -1;
}

int json_arr_len(JsDoc* d, int arr)
{
	if (!valid(d, arr) || d->nodes[arr].type != JS_ARR) return 0;
	int n = 0;
	for (int i = d->nodes[arr].first; i >= 0; i = d->nodes[i].next) n++;
	return n;
}

int json_arr_at(JsDoc* d, int arr, int idx)
{
	if (!valid(d, arr) || d->nodes[arr].type != JS_ARR || idx < 0) return -1;
	int n = 0;
	for (int i = d->nodes[arr].first; i >= 0; i = d->nodes[i].next, n++) {
		if (n == idx) return i;
	}
	return -1;
}

const char* json_str(JsDoc* d, int idx, const char* def)
{
	if (!valid(d, idx) || d->nodes[idx].type != JS_STR) return def;
	return d->nodes[idx].str;
}

double json_num(JsDoc* d, int idx, double def)
{
	if (!valid(d, idx) || d->nodes[idx].type != JS_NUM) return def;
	return d->nodes[idx].num;
}

bool json_bool(JsDoc* d, int idx, bool def)
{
	if (!valid(d, idx)) return def;
	if (d->nodes[idx].type == JS_BOOL) return d->nodes[idx].bval;
	if (d->nodes[idx].type == JS_NUM)  return d->nodes[idx].num != 0.0;
	return def;
}

const char* json_ostr(JsDoc* d, int obj, const char* key, const char* def)
{
	return json_str(d, json_obj_get(d, obj, key), def);
}

double json_onum(JsDoc* d, int obj, const char* key, double def)
{
	return json_num(d, json_obj_get(d, obj, key), def);
}

bool json_obool(JsDoc* d, int obj, const char* key, bool def)
{
	return json_bool(d, json_obj_get(d, obj, key), def);
}

int json_escape(const char* in, char* out, int outsz)
{
	int o = 0;
	if (outsz <= 0) return 0;
	for (const unsigned char* s = (const unsigned char*)in; *s; s++) {
		// Worst case below is 6 bytes plus the terminator.
		if (o + 7 >= outsz) break;
		switch (*s) {
		case '"':  out[o++] = '\\'; out[o++] = '"';  break;
		case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
		case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
		case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
		case '\t': out[o++] = '\\'; out[o++] = 't';  break;
		default:
			if (*s < 0x20) {
				static const char hex[] = "0123456789abcdef";
				out[o++] = '\\';
				out[o++] = 'u';
				out[o++] = '0';
				out[o++] = '0';
				out[o++] = hex[(*s >> 4) & 0xF];
				out[o++] = hex[*s & 0xF];
			} else {
				out[o++] = (char)*s;
			}
			break;
		}
	}
	out[o] = '\0';
	return o;
}
