// Minimal JSON parser for the QuizFabric 3DS client.
// Parses in place: the input buffer is mutated (strings are unescaped and
// NUL-terminated where they sit), so it must be writable and outlive the JsDoc.
#pragma once

#include <stdbool.h>

#define JSON_MAX_NODES 4096

typedef enum {
	JS_NULL = 0,
	JS_BOOL,
	JS_NUM,
	JS_STR,
	JS_ARR,
	JS_OBJ
} JsType;

typedef struct {
	JsType type;
	char*  key;   // member name when the parent is an object, else NULL
	char*  str;   // JS_STR payload
	double num;   // JS_NUM payload
	bool   bval;  // JS_BOOL payload
	int    first; // first child, or -1
	int    next;  // next sibling, or -1
} JsNode;

typedef struct {
	JsNode nodes[JSON_MAX_NODES];
	int    count;
	bool   ok;
} JsDoc;

// Parses `text` in place. Returns false on malformed input or node overflow.
// The root is node 0 when this returns true.
bool json_parse(JsDoc* d, char* text);

// Object/array navigation. Every index argument tolerates -1 (returns -1/def).
int    json_obj_get(JsDoc* d, int obj, const char* key);
int    json_arr_len(JsDoc* d, int arr);
int    json_arr_at(JsDoc* d, int arr, int i);

const char* json_str(JsDoc* d, int idx, const char* def);
double      json_num(JsDoc* d, int idx, double def);
bool        json_bool(JsDoc* d, int idx, bool def);

// Object-member shorthands.
const char* json_ostr(JsDoc* d, int obj, const char* key, const char* def);
double      json_onum(JsDoc* d, int obj, const char* key, double def);
bool        json_obool(JsDoc* d, int obj, const char* key, bool def);

// Writes a JSON string *body* (no surrounding quotes) into out.
// Returns the number of bytes written, always NUL-terminated.
int json_escape(const char* in, char* out, int outsz);
