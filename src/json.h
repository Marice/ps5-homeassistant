/* A small JSON reader for the request bodies: objects, arrays, integers, booleans, strings and null, into a
 * fixed pool of nodes. Fractions and exponents are refused (the API takes whole numbers only). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } json_type;

typedef struct {
    json_type type;
    int64_t number;      /* JSON_NUMBER; JSON_BOOL as 0 or 1 */
    const char *text;    /* JSON_STRING, and an object member's key: not terminated, see length/key_length */
    size_t length;
    const char *key;
    size_t key_length;
    int first_child;     /* JSON_ARRAY and JSON_OBJECT: index of the first element, or -1 */
    int next;            /* the next element of the parent, or -1 */
} json_node;

#define JSON_MAX_NODES 256

typedef struct {
    json_node nodes[JSON_MAX_NODES];
    int count;
} json_document;

/* Parses text into doc; the root is doc->nodes[0]. False when the text isn't valid or too large. */
bool json_parse(json_document *doc, const char *text, size_t length);
/* An object's member, or NULL. */
const json_node *json_member(const json_document *doc, const json_node *object, const char *key);
/* The first element of an array, and the one after an element, or NULL. */
const json_node *json_first(const json_document *doc, const json_node *array);
const json_node *json_next(const json_document *doc, const json_node *element);
