/* format.c - number formatting: modes, bases, fractions, symbolic forms */
#include "calc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

Settings g_set = {ANG_DEG, 10, FMT_AUTO, 6, 15, 0};

num ang_to_rad(num x)
{
    return g_set.angle == ANG_DEG ? x * (C_PI / 180) : g_set.angle == ANG_GRAD ? x * (C_PI / 200) : x;
}

num rad_to_ang(num x)
{
    return g_set.angle == ANG_DEG ? x * (180 / C_PI) : g_set.angle == ANG_GRAD ? x * (200 / C_PI) : x;
}

const char *angle_name(int a) { return a == ANG_DEG ? "DEG" : a == ANG_GRAD ? "GRAD" : "RAD"; }

const char *base_name(int b) { return b == 16 ? "HEX" : b == 2 ? "BIN" : b == 8 ? "OCT" : "DEC"; }

const char *fmt_mode_name(char *buf, size_t n)
{
    switch (g_set.fmt) {
    case FMT_FIX: snprintf(buf, n, "FIX %d", g_set.digits); break;
    case FMT_SCI: snprintf(buf, n, "SCI %d", g_set.digits); break;
    case FMT_ENG: snprintf(buf, n, "ENG %d", g_set.digits); break;
    default:
        if (g_set.prec == 15) str_copy(buf, n, "AUTO");
        else snprintf(buf, n, "AUTO %d", g_set.prec);
    }
    return buf;
}

/* "1.5e+07" -> "1.5e7", "2e-05" -> "2e-5" */
static void tidy_exp(char *s)
{
    char *e = strchr(s, 'e');
    if (!e) return;
    char *p = e + 1, *w = e + 1;
    if (*p == '+') p++;
    else if (*p == '-') *w++ = *p++;
    while (*p == '0' && p[1]) p++;
    memmove(w, p, strlen(p) + 1);
}

static void kill_neg_zero(char *s)
{
    if (s[0] != '-') return;
    for (const char *p = s + 1; *p && *p != 'e'; p++)
        if (*p >= '1' && *p <= '9') return;
    memmove(s, s + 1, strlen(s));
}

static void group_digits(char *s, size_t cap)
{
    char *p = s + (*s == '-');
    size_t nd = strspn(p, "0123456789");
    if (nd <= 3) return;
    size_t ncomma = (nd - 1) / 3, len = strlen(s);
    if (len + ncomma + 1 > cap) return;
    char *src_end = p + nd;
    memmove(src_end + ncomma, src_end, strlen(src_end) + 1);
    char *w = src_end + ncomma - 1;
    for (size_t i = 0; i < nd; i++) {
        *w-- = p[nd - 1 - i];
        if (i % 3 == 2 && i + 1 < nd) *w-- = ',';
    }
}

static void fmt_eng(char *out, size_t n, num x, int digits)
{
    if (x == 0) {
        snprintf(out, n, "%.*f", digits, 0.0);
        return;
    }
    int e3 = (int)floor(floor(log10(fabs(x))) / 3) * 3;
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%.*f", digits, x / pow(10, e3));
    if (fabs(atof(tmp)) >= 1000) {
        e3 += 3;
        snprintf(tmp, sizeof tmp, "%.*f", digits, x / pow(10, e3));
    }
    if (e3) snprintf(out, n, "%se%d", tmp, e3);
    else str_copy(out, n, tmp);
}

void fmt_number(char *buf, size_t n, num x)
{
    char tmp[512];
    if (isnan(x)) {
        str_copy(buf, n, "nan");
        return;
    }
    if (isinf(x)) {
        str_copy(buf, n, x > 0 ? "inf" : "-inf");
        return;
    }
    if (g_set.base != 10 && fmt_in_base(buf, n, x, g_set.base, g_set.group)) return;
    if (x == 0) x = 0;                                  /* no "-0" */
    if (g_set.fmt == FMT_FIX && fabs(x) < 1e16) {
        snprintf(tmp, sizeof tmp, "%.*f", g_set.digits, x);
    } else if (g_set.fmt == FMT_FIX || g_set.fmt == FMT_SCI) {
        snprintf(tmp, sizeof tmp, "%.*e", g_set.digits, x);
        tidy_exp(tmp);
    } else if (g_set.fmt == FMT_ENG) {
        fmt_eng(tmp, sizeof tmp, x, g_set.digits);
    } else if (x == floor(x) && fabs(x) <= 9007199254740992.0) {
        snprintf(tmp, sizeof tmp, "%.0f", x);           /* exact integers */
    } else {
        snprintf(tmp, sizeof tmp, "%.*g", g_set.prec, x);
        tidy_exp(tmp);
    }
    kill_neg_zero(tmp);
    if (g_set.group) group_digits(tmp, sizeof tmp);
    str_copy(buf, n, tmp);
}

/* shortest text that reads back as exactly x */
void fmt_exact(char *buf, size_t n, num x)
{
    if (isnan(x)) {
        str_copy(buf, n, "nan");
        return;
    }
    if (isinf(x)) {
        str_copy(buf, n, x > 0 ? "inf" : "-inf");
        return;
    }
    for (int p = 15; p <= 17; p++) {
        snprintf(buf, n, "%.*g", p, x);
        if (strtod(buf, NULL) == x) break;
    }
    tidy_exp(buf);
}

int fmt_in_base(char *buf, size_t n, num x, int base, int group)
{
    if (!(x == floor(x)) || fabs(x) >= 9.2233720368547758e18) return 0;
    int64_t v = (int64_t)x;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    char dig[72], out[128];
    int k = 0, o = 0;
    do {
        dig[k++] = "0123456789ABCDEF"[u % (uint64_t)base];
        u /= (uint64_t)base;
    } while (u);
    if (v < 0) out[o++] = '-';
    out[o++] = '0';
    out[o++] = base == 16 ? 'x' : base == 2 ? 'b' : base == 8 ? 'o' : 'd';
    int g = base == 8 ? 3 : 4;
    for (int i = k - 1; i >= 0; i--) {
        out[o++] = dig[i];
        if (group && i > 0 && i % g == 0) out[o++] = '_';
    }
    out[o] = 0;
    str_copy(buf, n, out);
    return 1;
}

/* best rational approximation p/q (q <= maxden) within relative tol */
static int rational(num v, long long maxden, num tol, long long *pp, long long *qq)
{
    num a0 = fabs(v), r = a0;
    long long h0 = 0, h1 = 1, k0 = 1, k1 = 0;
    for (int i = 0; i < 64; i++) {
        num a = floor(r);
        if (a > 1e12) break;
        long long ai = (long long)a, h2 = ai * h1 + h0, k2 = ai * k1 + k0;
        if (k2 > maxden || h2 > 1000000000000LL) break;
        h0 = h1, h1 = h2, k0 = k1, k1 = k2;
        if (fabs(a0 - (num)h1 / (num)k1) <= tol * a0) {
            *pp = v < 0 ? -h1 : h1;
            *qq = k1;
            return 1;
        }
        num f = r - a;
        if (f < 1e-15) break;
        r = 1 / f;
    }
    return 0;
}

int fmt_fraction(char *buf, size_t n, num x)
{
    long long p, q;
    if (!isfinite(x) || x == floor(x) || fabs(x) > 1e9) return 0;
    if (!rational(x, 1000, 5e-15, &p, &q) || q == 1) return 0;
    long long r = q;                 /* 5.772 = 1443/250 adds nothing; 1/3, 3/4 do */
    while (r % 2 == 0) r /= 2;
    while (r % 5 == 0) r /= 5;
    if (r == 1 && q > 16) return 0;
    snprintf(buf, n, "%lld/%lld", p, q);
    return 1;
}

static long long llgcd(long long a, long long b)
{
    while (b) {
        long long t = a % b;
        a = b;
        b = t;
    }
    return a < 0 ? -a : a;
}

int fmt_symbolic(char *buf, size_t n, num x)
{
    long long p, q;
    const char *pi = g_opt_ascii ? "pi" : "\xcf\x80";
    if (!isfinite(x) || x == 0 || fabs(x) > 1e6 || fabs(x) < 1e-6) return 0;
    if (rational(x, 100000, 5e-15, &p, &q)) return 0;          /* plain fraction */

    if (rational(x / C_PI, 1000, 4e-15, &p, &q)) {             /* k*pi/q */
        const char *sg = p < 0 ? "-" : "";
        long long a = p < 0 ? -p : p;
        if (a == 1 && q == 1) snprintf(buf, n, "%s%s", sg, pi);
        else if (q == 1) snprintf(buf, n, "%s%lld%s", sg, a, pi);
        else if (a == 1) snprintf(buf, n, "%s%s/%lld", sg, pi, q);
        else snprintf(buf, n, "%s%lld%s/%lld", sg, a, pi, q);
        return 1;
    }
    if (rational(x * x, 1000, 4e-15, &p, &q) && p > 0 && p < 1000000000LL) {   /* sqrt(p/q) */
        long long m = p * q, a = 1, b = m;
        for (long long f = 2; f * f <= b; f++)
            while (b % (f * f) == 0) b /= f * f, a *= f;
        if (b == 1) return 0;
        long long g = llgcd(a, q);
        a /= g;
        q /= g;
        char num_part[64];
        if (a == 1) snprintf(num_part, sizeof num_part, g_opt_ascii ? "sqrt(%lld)" : "\xe2\x88\x9a%lld", b);
        else snprintf(num_part, sizeof num_part, g_opt_ascii ? "%lld*sqrt(%lld)" : "%lld\xe2\x88\x9a%lld", a, b);
        if (q == 1) snprintf(buf, n, "%s%s", x < 0 ? "-" : "", num_part);
        else snprintf(buf, n, "%s%s/%lld", x < 0 ? "-" : "", num_part, q);
        return 1;
    }
    return 0;
}
