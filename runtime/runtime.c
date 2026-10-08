// co runtime: printing, strings, growable slices, maps, panics.
//
// This file is compiled to LLVM bitcode, embedded in the compiler and linked
// into every program before optimization, so these functions inline into
// generated code. Keep hot paths small and rare paths out of line.

// Fortified libc wrappers would be inlined into every program; some
// toolchains (nix's clang) force them on from the command line.
#undef _FORTIFY_SOURCE

#include "co_abi.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CO_COLD __attribute__((noinline, cold))
#define CO_LIKELY(x) __builtin_expect(!!(x), 1)
#define CO_UNLIKELY(x) __builtin_expect(!!(x), 0)

void co_main(void);

_Noreturn void co_panic(const char *msg) {
  fflush(stdout);
  fprintf(stderr, "panic: %s\n", msg);
  exit(101);
}

_Noreturn void co_panic_str(const CoVec *s) {
  fflush(stdout);
  fprintf(stderr, "panic: %.*s\n", (int)s->len, s->ptr ? s->ptr : "");
  exit(101);
}

// An error returned from `func main() !`.
_Noreturn void co_main_failed(const CoVec *err) {
  fflush(stdout);
  fprintf(stderr, "error: %.*s\n", (int)err->len, err->ptr ? err->ptr : "");
  exit(1);
}

_Noreturn void co_panic_bounds(int64_t idx, int64_t len) {
  fflush(stdout);
  fprintf(stderr, "panic: index out of range [%" PRId64 "] with length %" PRId64 "\n", idx, len);
  exit(101);
}

// Live heap allocations; reported at exit when CO_DEBUG_ALLOC is set, so
// tests can verify that drops free everything exactly once.
static int64_t live_allocs;

static void *xrealloc(void *p, size_t n) {
  void *r = realloc(p, n ? n : 1);
  if (!r)
    co_panic("out of memory");
  if (!p)
    live_allocs++;
  return r;
}

void co_free(void *p) {
  if (p)
    live_allocs--;
  free(p);
}

// ----- printing -----

void co_print_int(int64_t v) { printf("%" PRId64, v); }
void co_print_float(double v) { printf("%g", v); }
void co_print_bool(int32_t v) { fputs(v ? "true" : "false", stdout); }
void co_print_str(const CoVec *s) { fwrite(s->ptr, 1, (size_t)s->len, stdout); }
void co_print_cstr(const char *s) { fputs(s, stdout); }
void co_print_space(void) { putchar(' '); }
void co_print_newline(void) { putchar('\n'); }

// ----- strings -----

static void str_from(CoVec *out, const char *p, int64_t len) {
  out->ptr = len ? xrealloc(NULL, (size_t)len) : NULL;
  if (len)
    memcpy(out->ptr, p, (size_t)len);
  out->len = len;
  out->cap = len;
}

void co_str_lit(CoVec *out, const char *p, int64_t len) { str_from(out, p, len); }
void co_str_clone(CoVec *out, const CoVec *s) { str_from(out, s->ptr, s->len); }

void co_str_concat(CoVec *out, const CoVec *a, const CoVec *b) {
  int64_t len = a->len + b->len;
  out->ptr = len ? xrealloc(NULL, (size_t)len) : NULL;
  if (a->len)
    memcpy(out->ptr, a->ptr, (size_t)a->len);
  if (b->len)
    memcpy(out->ptr + a->len, b->ptr, (size_t)b->len);
  out->len = len;
  out->cap = len;
}

int64_t co_str_cmp(const CoVec *a, const CoVec *b) {
  int64_t n = a->len < b->len ? a->len : b->len;
  int c = n ? memcmp(a->ptr, b->ptr, (size_t)n) : 0;
  if (c)
    return c < 0 ? -1 : 1;
  return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}

void co_str_from_int(CoVec *out, int64_t v) {
  char buf[32];
  int n = snprintf(buf, sizeof buf, "%" PRId64, v);
  str_from(out, buf, n);
}

void co_str_from_float(CoVec *out, double v) {
  char buf[64];
  int n = snprintf(buf, sizeof buf, "%g", v);
  str_from(out, buf, n);
}

void co_str_from_bool(CoVec *out, int32_t v) { str_from(out, v ? "true" : "false", v ? 4 : 5); }

// ----- slices -----

void co_vec_new(CoVec *out, int64_t cap, int64_t elem_size) {
  out->ptr = cap ? xrealloc(NULL, (size_t)(cap * elem_size)) : NULL;
  out->len = 0;
  out->cap = cap;
}

static CO_COLD void vec_grow(CoVec *v, int64_t elem_size) {
  int64_t cap = v->cap ? v->cap * 2 : 4;
  v->ptr = xrealloc(v->ptr, (size_t)(cap * elem_size));
  v->cap = cap;
}

void co_vec_push(CoVec *v, const void *elem, int64_t elem_size) {
  if (CO_UNLIKELY(v->len == v->cap))
    vec_grow(v, elem_size);
  memcpy(v->ptr + v->len * elem_size, elem, (size_t)elem_size);
  v->len++;
}

// Bitwise copy; the compiler deep-clones elements that own memory afterwards.
void co_vec_clone_bits(CoVec *out, const CoVec *src, int64_t elem_size) {
  co_vec_new(out, src->len, elem_size);
  if (src->len)
    memcpy(out->ptr, src->ptr, (size_t)(src->len * elem_size));
  out->len = src->len;
}

// ----- maps -----
//
// Insertion-ordered hash maps. Entries live in one array in insertion order
// (so iteration is predictable); `index` is an open-addressing table of
// entry positions (+1; 0 = empty, -1 = deleted). An entry is:
//   int64 alive | key (rounded up to 8 bytes) | value (rounded up to 8 bytes)
// Keys are copied in (strings are cloned), so callers only lend keys.
// The compiler passes `kind`, `ks` (key size) and `vs` (value size) as
// constants; once inlined, each call site is specialized to its types.

static int64_t round8(int64_t n) { return (n + 7) & ~(int64_t)7; }
static int64_t entry_size(int64_t ks, int64_t vs) { return 8 + round8(ks) + round8(vs); }

static uint64_t load64(const char *p) {
  uint64_t v;
  memcpy(&v, p, 8);
  return v;
}

static uint64_t load32(const char *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

// 64x64 -> 128-bit multiply, folded to 64 bits (wyhash's mixing step).
static uint64_t mix(uint64_t a, uint64_t b) {
  __uint128_t r = (__uint128_t)a * b;
  return (uint64_t)r ^ (uint64_t)(r >> 64);
}

// Hashes 8 bytes at a time, reading the tail with overlapping loads so it
// never reads past the end or loops over single bytes.
static uint64_t hash_bytes(const char *p, int64_t len) {
  uint64_t n = (uint64_t)len, t;
  uint64_t h = 0x9e3779b97f4a7c15ull ^ n;
  if (n > 8) {
    for (; n > 8; p += 8, n -= 8)
      h = mix(h ^ load64(p), 0xa0761d6478bd642full);
    t = load64(p + n - 8);
  } else if (n >= 4) {
    t = load32(p) << 32 | load32(p + n - 4);
  } else if (n) {
    t = (uint64_t)(unsigned char)p[0] << 16 | (uint64_t)(unsigned char)p[n >> 1] << 8 | (unsigned char)p[n - 1];
  } else {
    t = 0;
  }
  return mix(h ^ t, 0xe7037ed1a0b428dbull);
}

static uint64_t hash_key(const void *k, int kind) {
  uint64_t h;
  if (kind == CO_KEY_STR) {
    const CoVec *s = k;
    return hash_bytes(s->ptr, s->len);
  } else if (kind == CO_KEY_BOOL) {
    h = *(const unsigned char *)k ? 0x9e3779b97f4a7c15ull : 0x7f4a7c159e3779b9ull;
  } else {
    h = (uint64_t)*(const int64_t *)k;
  }
  h ^= h >> 33; // finalizer (from splitmix64/murmur3)
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  return h;
}

// Compares n bytes like hash_bytes reads them, without calling memcmp.
static int bytes_eq(const char *a, const char *b, int64_t n) {
  if (n > 8) {
    for (; n > 8; a += 8, b += 8, n -= 8)
      if (load64(a) != load64(b))
        return 0;
    return load64(a + n - 8) == load64(b + n - 8);
  }
  if (n >= 4)
    return load32(a) == load32(b) && load32(a + n - 4) == load32(b + n - 4);
  for (int64_t i = 0; i < n; i++)
    if (a[i] != b[i])
      return 0;
  return 1;
}

static int key_eq(const void *a, const void *b, int kind) {
  if (kind == CO_KEY_STR) {
    const CoVec *x = a, *y = b;
    return x->len == y->len && bytes_eq(x->ptr, y->ptr, x->len);
  }
  if (kind == CO_KEY_BOOL)
    return (*(const unsigned char *)a != 0) == (*(const unsigned char *)b != 0);
  return *(const int64_t *)a == *(const int64_t *)b;
}

// Returns the index-table slot holding `key`, or -1.
static int64_t find_slot(const CoMap *m, const void *key, int kind, int64_t es) {
  if (!m->icap)
    return -1;
  uint64_t mask = (uint64_t)m->icap - 1;
  for (uint64_t i = hash_key(key, kind) & mask;; i = (i + 1) & mask) {
    int64_t e = m->index[i];
    if (e == 0)
      return -1;
    if (e > 0 && key_eq(m->entries + (e - 1) * es + 8, key, kind))
      return (int64_t)i;
  }
}

static void index_insert(CoMap *m, int64_t entry, int kind, int64_t es) {
  uint64_t mask = (uint64_t)m->icap - 1;
  uint64_t i = hash_key(m->entries + entry * es + 8, kind) & mask;
  while (m->index[i] > 0)
    i = (i + 1) & mask;
  m->index[i] = entry + 1;
}

// Drops deleted entries and resizes to hold `cap` entries.
static CO_COLD void map_rebuild(CoMap *m, int kind, int64_t es, int64_t cap) {
  char *entries = xrealloc(NULL, (size_t)(cap * es));
  int64_t n = 0;
  for (int64_t i = 0; i < m->used; i++) {
    char *e = m->entries + i * es;
    if (*(int64_t *)e)
      memcpy(entries + n++ * es, e, (size_t)es);
  }
  co_free(m->entries);
  co_free(m->index);
  m->entries = entries;
  m->used = n;
  m->cap = cap;
  m->icap = 8;
  while (m->icap < cap * 2)
    m->icap *= 2;
  m->index = xrealloc(NULL, (size_t)m->icap * sizeof(int64_t));
  memset(m->index, 0, (size_t)m->icap * sizeof(int64_t));
  for (int64_t i = 0; i < n; i++)
    index_insert(m, i, kind, es);
}

void *co_map_get(const CoMap *m, const void *key, int32_t kind, int64_t ks, int64_t vs) {
  int64_t es = entry_size(ks, vs);
  int64_t slot = find_slot(m, key, kind, es);
  if (slot < 0)
    return NULL;
  return m->entries + (m->index[slot] - 1) * es + 8 + round8(ks);
}

// Returns the value for `key`, inserting a zeroed value first if missing.
void *co_map_slot(CoMap *m, const void *key, int32_t kind, int64_t ks, int64_t vs) {
  int64_t es = entry_size(ks, vs);
  int64_t slot = find_slot(m, key, kind, es);
  if (slot >= 0)
    return m->entries + (m->index[slot] - 1) * es + 8 + round8(ks);
  if (CO_UNLIKELY(m->used == m->cap))
    map_rebuild(m, kind, es, m->len * 2 < 8 ? 8 : m->len * 2);
  char *e = m->entries + m->used * es;
  memset(e, 0, (size_t)es);
  *(int64_t *)e = 1;
  if (kind == CO_KEY_STR)
    co_str_clone((CoVec *)(e + 8), key);
  else
    memcpy(e + 8, key, (size_t)ks);
  index_insert(m, m->used, kind, es);
  m->used++;
  m->len++;
  return e + 8 + round8(ks);
}

// Removes `key`; its value is moved to `out` (for the caller to drop).
int32_t co_map_delete(CoMap *m, const void *key, int32_t kind, int64_t ks, int64_t vs, void *out) {
  int64_t es = entry_size(ks, vs);
  int64_t slot = find_slot(m, key, kind, es);
  if (slot < 0)
    return 0;
  char *e = m->entries + (m->index[slot] - 1) * es;
  *(int64_t *)e = 0;
  if (kind == CO_KEY_STR)
    co_free(((CoVec *)(e + 8))->ptr);
  memcpy(out, e + 8 + round8(ks), (size_t)vs);
  m->index[slot] = -1;
  m->len--;
  return 1;
}

// Frees the map's storage (the compiler drops keys and values first).
void co_map_free(CoMap *m) {
  co_free(m->entries);
  co_free(m->index);
}

// Bitwise copy; the compiler deep-clones keys and values afterwards.
void co_map_clone_bits(CoMap *dst, const CoMap *src, int64_t ks, int64_t vs) {
  int64_t es = entry_size(ks, vs);
  *dst = *src;
  dst->entries = NULL;
  dst->index = NULL;
  if (src->cap) {
    dst->entries = xrealloc(NULL, (size_t)(src->cap * es));
    memcpy(dst->entries, src->entries, (size_t)(src->used * es));
  }
  if (src->icap) {
    dst->index = xrealloc(NULL, (size_t)src->icap * sizeof(int64_t));
    memcpy(dst->index, src->index, (size_t)src->icap * sizeof(int64_t));
  }
}

// Iteration: generated code walks entry slots 0..co_map_used(m), skipping
// deleted ones, so the entry layout stays private to this file. A clone made
// by co_map_clone_bits has its entries at the same slots as the original.

int64_t co_map_used(const CoMap *m) { return m->used; }

int32_t co_map_alive(const CoMap *m, int64_t i, int64_t ks, int64_t vs) {
  return *(const int64_t *)(m->entries + i * entry_size(ks, vs)) != 0;
}

void *co_map_key_at(const CoMap *m, int64_t i, int64_t ks, int64_t vs) {
  return m->entries + i * entry_size(ks, vs) + 8;
}

void *co_map_val_at(const CoMap *m, int64_t i, int64_t ks, int64_t vs) {
  return m->entries + i * entry_size(ks, vs) + 8 + round8(ks);
}

int main(void) {
  co_main();
  fflush(stdout);
  if (getenv("CO_DEBUG_ALLOC"))
    fprintf(stderr, "co: live allocations at exit: %" PRId64 "\n", live_allocs);
  return 0;
}
