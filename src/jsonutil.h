/*
 * jsonutil.h - Tiny read-only JSON helpers for package metadata.
 *
 * Scans for a quoted key and returns the following scalar as text. This is
 * deliberately not a full parser: package metadata such as param.json is a
 * well-formed, flat document and only a handful of fields are ever needed.
 */
#ifndef FPKG_JSONUTIL_H
#define FPKG_JSONUTIL_H

#include <stddef.h>

/* Copies the string value that follows "key" into out. Returns 1 on success. */
int ju_get_string(const char *json, size_t len, const char *key,
                  char *out, size_t out_size);

/* Copies the numeric token that follows "key" into out. Returns 1 on success. */
int ju_get_number(const char *json, size_t len, const char *key,
                  char *out, size_t out_size);

/* Returns 1 when the key is present anywhere in the document. */
int ju_has_key(const char *json, size_t len, const char *key);

#endif /* FPKG_JSONUTIL_H */
