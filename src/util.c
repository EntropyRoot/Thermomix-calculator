/* util.c - arena allocator, growable buffers, strings, PRNG */
#include "calc.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ memory */

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("tcalc: out of memory\n", stderr);
        abort();
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) {
        fputs("tcalc: out of memory\n", stderr);
        abort();
    }
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    size_t l = strlen(s);
    if (l > n) l = n;
    char *d = xmalloc(l + 1);
    memcpy(d, s, l);
    d[l] = 0;
    return d;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

void str_copy(char *dst, size_t n, const char *src)
{
    if (!n) return;
    size_t l = strlen(src);
    if (l >= n) l = n - 1;
    memcpy(dst, src, l);
    dst[l] = 0;
}

int is_ident_start(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

int is_ident_char(int c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

char *str_trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r' || s[l - 1] == '\n'))
        s[--l] = 0;
    return s;
}

/* ------------------------------------------------------------- arena */

struct ArenaBlock {
    ArenaBlock *next;
    size_t used, cap;
};

#define ARENA_HDR   ((sizeof(ArenaBlock) + 15) & ~(size_t)15)
#define ARENA_CHUNK 16384

void *arena_alloc(Arena *a, size_t n)
{
    n = (n + 15) & ~(size_t)15;
    ArenaBlock *b = a->head;
    if (!b || b->used + n > b->cap) {
        size_t cap = n > ARENA_CHUNK ? n : ARENA_CHUNK;
        ArenaBlock *nb = xmalloc(ARENA_HDR + cap);
        nb->next = b;
        nb->used = 0;
        nb->cap = cap;
        a->head = b = nb;
    }
    void *p = (char *)b + ARENA_HDR + b->used;
    b->used += n;
    return p;
}

void arena_reset(Arena *a)
{
    ArenaBlock *b = a->head;
    if (!b) return;
    for (ArenaBlock *n = b->next; n;) {
        ArenaBlock *t = n->next;
        free(n);
        n = t;
    }
    b->next = NULL;
    b->used = 0;
}

void arena_free(Arena *a)
{
    for (ArenaBlock *n = a->head; n;) {
        ArenaBlock *t = n->next;
        free(n);
        n = t;
    }
    a->head = NULL;
}

/* --------------------------------------------------------------- Buf */

void buf_reserve(Buf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra + 1) cap *= 2;
    b->s = xrealloc(b->s, cap);
    b->cap = cap;
}

void buf_putn(Buf *b, const char *s, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

void buf_puts(Buf *b, const char *s) { buf_putn(b, s, strlen(s)); }

void buf_putc(Buf *b, char c)
{
    buf_reserve(b, 1);
    b->s[b->len++] = c;
    b->s[b->len] = 0;
}

void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[512];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) {
        buf_putn(b, tmp, (size_t)n);
        return;
    }
    buf_reserve(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

void buf_free(Buf *b)
{
    free(b->s);
    b->s = NULL;
    b->len = b->cap = 0;
}

/* ------------------------------------------------- xoshiro256** PRNG */

static RngState rng = {{0x9E3779B97F4A7C15ull, 0xBF58476D1CE4E5B9ull,
                        0x94D049BB133111EBull, 0x2545F4914F6CDD1Dull}};

static uint64_t splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void rng_seed(uint64_t seed)
{
    for (int i = 0; i < 4; i++) rng.s[i] = splitmix64(&seed);
}

static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static uint64_t rng_next(void)
{
    uint64_t *s = rng.s;
    uint64_t r = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return r;
}

double rng_uniform(void) { return (double)(rng_next() >> 11) * 0x1.0p-53; }

void rng_save(RngState *st) { *st = rng; }
void rng_load(const RngState *st) { rng = *st; }
