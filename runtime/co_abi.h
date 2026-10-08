// The data layout shared by the co runtime (runtime.c) and the compiler
// (codegen.cpp). Both include this header, so the two can't drift apart.
// Valid C and C++.
#pragma once
#include <stdint.h>

// Strings, errors and slices: { ptr, len, cap }. Generated code indexes
// `ptr` and reads `len` directly.
typedef struct {
  char *ptr;
  int64_t len;
  int64_t cap;
} CoVec;

// Maps. Generated code only reads `len` (the same field position as in
// CoVec, so `len(x)` works on both); everything else is private to the
// runtime and reached through the co_map_* functions.
typedef struct {
  char *entries;
  int64_t len, used, cap;
  int64_t *index;
  int64_t icap;
} CoMap;

// How the runtime hashes and compares a map's keys.
enum { CO_KEY_INT = 0, CO_KEY_BOOL = 1, CO_KEY_STR = 2 };
