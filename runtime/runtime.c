// co runtime: printing, strings, growable slices, panics.
// Strings and slices share one layout: { ptr, len, cap }.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *ptr;
  int64_t len;
  int64_t cap;
} CoVec;

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

void co_vec_push(CoVec *v, const void *elem, int64_t elem_size) {
  if (v->len == v->cap) {
    int64_t cap = v->cap ? v->cap * 2 : 4;
    v->ptr = xrealloc(v->ptr, (size_t)(cap * elem_size));
    v->cap = cap;
  }
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

int main(void) {
  co_main();
  fflush(stdout);
  if (getenv("CO_DEBUG_ALLOC"))
    fprintf(stderr, "co: live allocations at exit: %" PRId64 "\n", live_allocs);
  return 0;
}
