/* builtins.c - built-in functions, constants and number theory helpers */
#include "calc.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BF_ANG BF_NOFOLD          /* depends on the angle mode */

static num fact_tab[171];

/* ------------------------------------------------------- core helpers */

num fn_fact(num n)
{
    if (isnan(n)) return NAN;
    if (n == floor(n)) {
        if (n < 0) return NAN;
        if (n > 170) return INFINITY;
        return fact_tab[(int)n];
    }
    return tgamma(n + 1);
}

num fn_pow(num a, num b)
{
    if (a < 0 && isfinite(b) && b != floor(b)) {
        /* real odd roots: (-8)^(1/3) = -2, (-8)^(2/3) = 4 */
        for (int q = 3; q < 100; q += 2) {
            num p = b * q, pr = nearbyint(p);
            if (fabs(p - pr) < 1e-9) {
                num r = pow(-a, b);
                return fmod(fabs(pr), 2) == 1 ? -r : r;
            }
        }
        return NAN;
    }
    if (b == 2) return a * a;              /* fast path, same correctly rounded result */
    return pow(a, b);
}

num fmod_floor(num a, num b)
{
    if (b == 0) return NAN;
    num r = fmod(a, b);
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

int64_t to_int64(num x, const char *what)
{
    if (!(x == floor(x)) || fabs(x) >= 9.2233720368547758e18)
        calc_error(-1, "%s: operands must be integers", what);
    return (int64_t)x;
}

static num need_int(num x, const char *fn)
{
    if (!(x == floor(x)) || fabs(x) > 9007199254740992.0)
        calc_error(-1, "%s: argument must be an integer", fn);
    return x;
}

/* --------------------------------------------------------- trigonometry */

static num quadrant(int q, int fn)
{
    static const num s[4] = {0, 1, 0, -1}, c[4] = {1, 0, -1, 0};
    if (fn == 0) return s[q & 3];
    if (fn == 1) return c[q & 3];
    return (q & 1) ? NAN : 0;
}

/* fn: 0 = sin, 1 = cos, 2 = tan; exact at multiples of 90 degrees (and 30/45
 * in degree mode), so sin(180) is 0 rather than 1.2e-16 */
static num trig(num x, int fn)
{
    if (!isfinite(x)) return NAN;
    if (g_set.angle == ANG_RAD) {
        num k = x / (C_PI / 2);
        if (fabs(k) < 1e15) {
            num kr = nearbyint(k);
            if (kr != 0 && fabs(x - kr * (C_PI / 2)) <= 4 * DBL_EPSILON * fabs(x))
                return quadrant((int)fmod(fmod(kr, 4) + 4, 4), fn);
        }
        return fn == 0 ? sin(x) : fn == 1 ? cos(x) : tan(x);
    }
    num full = g_set.angle == ANG_DEG ? 360 : 400;
    num r = fmod(x, full);
    if (r < 0) r += full;
    num q = r / (full / 4);
    if (q == floor(q)) return quadrant((int)q, fn);
    num o = r / (full / 8);
    if (fn == 2 && o == floor(o)) return ((int)o % 4 == 1) ? 1 : -1;        /* 45, 135, ... */
    if (full == 360 && fmod(r, 30) == 0 && fn != 2) {
        int m = (int)(r / 30);                                            /* 30, 60, ... */
        if (fn == 0 && (m == 1 || m == 5)) return 0.5;
        if (fn == 0 && (m == 7 || m == 11)) return -0.5;
        if (fn == 1 && (m == 2 || m == 10)) return 0.5;
        if (fn == 1 && (m == 4 || m == 8)) return -0.5;
    }
    num rad = r * (2 * C_PI / full);
    return fn == 0 ? sin(rad) : fn == 1 ? cos(rad) : tan(rad);
}

static num b_sin(num x) { return trig(x, 0); }
static num b_cos(num x) { return trig(x, 1); }
static num b_tan(num x) { return trig(x, 2); }
static num b_cot(num x) { return trig(x, 1) / trig(x, 0); }
static num b_sec(num x) { return 1 / trig(x, 1); }
static num b_csc(num x) { return 1 / trig(x, 0); }
static num b_asin(num x) { return rad_to_ang(asin(x)); }
static num b_acos(num x) { return rad_to_ang(acos(x)); }
static num b_atan(num x) { return rad_to_ang(atan(x)); }
static num b_acot(num x) { return rad_to_ang(x == 0 ? C_PI / 2 : atan(1 / x)); }
static num b_asec(num x) { return rad_to_ang(acos(1 / x)); }
static num b_acsc(num x) { return rad_to_ang(asin(1 / x)); }
static num b_atan2(num y, num x) { return rad_to_ang(atan2(y, x)); }

static num b_sinh(num x) { return sinh(x); }
static num b_cosh(num x) { return cosh(x); }
static num b_tanh(num x) { return tanh(x); }
static num b_asinh(num x) { return asinh(x); }
static num b_acosh(num x) { return acosh(x); }
static num b_atanh(num x) { return atanh(x); }
static num b_coth(num x) { return 1 / tanh(x); }
static num b_sech(num x) { return 1 / cosh(x); }
static num b_csch(num x) { return 1 / sinh(x); }

/* ------------------------------------------------ powers & logarithms */

static num b_exp(num x) { return exp(x); }
static num b_ln(num x) { return log(x); }
static num b_log10(num x) { return log10(x); }
static num b_log2(num x) { return log2(x); }
static num b_log1p(num x) { return log1p(x); }
static num b_expm1(num x) { return expm1(x); }
static num b_sqrt(num x) { return sqrt(x); }
static num b_cbrt(num x) { return cbrt(x); }
static num b_pow(num a, num b) { return fn_pow(a, b); }

static num b_log(int n, const num *a)
{
    if (n == 1) return log10(a[0]);
    if (a[1] == 2) return log2(a[0]);
    if (a[1] == 10) return log10(a[0]);
    num r = log(a[0]) / log(a[1]), k = nearbyint(r);
    if (fabs(r - k) < 1e-12 && k != 0 && pow(a[1], k) == a[0]) return k;     /* log(27, 3) = 3 */
    return r;
}

static num b_root(num x, num n)
{
    if (n == 2) return sqrt(x);
    if (n == 3) return cbrt(x);
    if (n == 0) return NAN;
    num r = (x < 0 && n == floor(n) && fmod(fabs(n), 2) == 1) ? -pow(-x, 1 / n) : pow(x, 1 / n);
    num k = nearbyint(r);
    if (k != r && fabs(r - k) < 1e-9 * fmax(1, fabs(r)) && pow(k, n) == x) return k;
    return r;
}

static num b_hypot(int n, const num *a)
{
    num r = 0;
    for (int i = 0; i < n; i++) r = hypot(r, a[i]);
    return r;
}

/* ------------------------------------------------------------ rounding */

static num b_abs(num x) { return fabs(x); }
static num b_floor(num x) { return floor(x); }
static num b_ceil(num x) { return ceil(x); }
static num b_trunc(num x) { return trunc(x); }
static num b_frac(num x) { return x - trunc(x); }
static num b_sign(num x) { return x > 0 ? 1 : x < 0 ? -1 : x; }

static num round_half_away(num y)
{
    /* a half that only misses because of binary representation (2.675*100)
     * still rounds away from zero, like a decimal calculator */
    num fl = floor(y), fr = y - fl;
    if (fabs(fr - 0.5) <= 8 * DBL_EPSILON * fmax(1.0, fabs(y))) return y >= 0 ? fl + 1 : fl;
    return round(y);
}

static num b_round(int n, const num *a)
{
    num x = a[0];
    if (n == 1) return round(x);
    num d = need_int(a[1], "round");
    if (!isfinite(x) || fabs(d) > 300) return x;
    num s = pow(10, fabs(d));
    if (d >= 0) {
        num y = x * s;
        if (!isfinite(y) || fabs(y) > 9e15) return x;
        return round_half_away(y) / s;
    }
    return round_half_away(x / s) * s;
}

static num b_clamp(int n, const num *a)
{
    (void)n;
    return a[0] < a[1] ? a[1] : a[0] > a[2] ? a[2] : a[0];
}

/* ------------------------------------------- number theory & combinatorics */

static num gcd2(num a, num b)
{
    a = fabs(a);
    b = fabs(b);
    while (b > 0) {
        num t = fmod(a, b);
        a = b;
        b = t;
    }
    return a;
}

static num b_gcd(int n, const num *a)
{
    num g = 0;
    for (int i = 0; i < n; i++) g = gcd2(g, need_int(a[i], "gcd"));
    return g;
}

static num b_lcm(int n, const num *a)
{
    num l = 1;
    for (int i = 0; i < n; i++) {
        num x = fabs(need_int(a[i], "lcm"));
        if (x == 0) return 0;
        l = l / gcd2(l, x) * x;
    }
    return l;
}

static num b_mod(num a, num b) { return fmod_floor(a, b); }
static num b_rem(num a, num b) { return fmod(a, b); }

static num b_ncr(num n, num k)
{
    if (n == floor(n) && k == floor(k) && n >= 0) {
        if (k < 0 || k > n) return 0;
        if (k > n - k) k = n - k;
        num r = 1;
        for (num i = 1; i <= k && isfinite(r); i++) r = r * (n - k + i) / i;
        return r < 9007199254740992.0 ? nearbyint(r) : r;
    }
    return fn_fact(n) / (fn_fact(k) * fn_fact(n - k));
}

static num b_npr(num n, num k)
{
    if (n == floor(n) && k == floor(k) && n >= 0) {
        if (k < 0 || k > n) return 0;
        num r = 1;
        for (num i = 0; i < k && isfinite(r); i++) r *= n - i;
        return r;
    }
    return fn_fact(n) / fn_fact(n - k);
}

static num b_fib(num n)
{
    if (isnan(n)) return NAN;
    if (n != floor(n))
        return (pow(C_PHI, n) - cos(C_PI * n) * pow(C_PHI, -n)) / sqrt(5.0);
    num m = fabs(n);
    if (m > 1476) return n < 0 && fmod(m, 2) == 0 ? -INFINITY : INFINITY;
    num a = 0, b = 1;
    for (num i = 0; i < m; i++) {
        num t = a + b;
        a = b;
        b = t;
    }
    return (n < 0 && fmod(m, 2) == 0) ? -a : a;
}

#if defined(__SIZEOF_INT128__)
__extension__ typedef unsigned __int128 u128;
static uint64_t mulmod(uint64_t a, uint64_t b, uint64_t m) { return (uint64_t)((u128)a * b % m); }
#else
static uint64_t mulmod(uint64_t a, uint64_t b, uint64_t m)
{
    uint64_t r = 0;
    a %= m;
    while (b) {
        if (b & 1) r = (r >= m - a) ? r - (m - a) : r + a;
        a = (a >= m - a) ? a - (m - a) : a + a;
        b >>= 1;
    }
    return r;
}
#endif

static uint64_t powmod(uint64_t b, uint64_t e, uint64_t m)
{
    uint64_t r = 1;
    b %= m;
    while (e) {
        if (e & 1) r = mulmod(r, b, m);
        b = mulmod(b, b, m);
        e >>= 1;
    }
    return r;
}

int is_prime_u64(uint64_t n)
{
    static const uint64_t bases[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    if (n < 2) return 0;
    for (int i = 0; i < 12; i++) {
        if (n == bases[i]) return 1;
        if (n % bases[i] == 0) return 0;
    }
    uint64_t d = n - 1;
    int s = 0;
    while (!(d & 1)) d >>= 1, s++;
    for (int i = 0; i < 12; i++) {
        uint64_t x = powmod(bases[i], d, n);
        if (x == 1 || x == n - 1) continue;
        int comp = 1;
        for (int r = 1; r < s && comp; r++) {
            x = mulmod(x, x, n);
            if (x == n - 1) comp = 0;
        }
        if (comp) return 0;
    }
    return 1;
}

static uint64_t ugcd(uint64_t a, uint64_t b)
{
    while (b) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static uint64_t pollard(uint64_t n)
{
    if (!(n & 1)) return 2;
    for (uint64_t c = 1;; c++) {
        uint64_t x = 2, y = 2, d = 1;
        while (d == 1) {
            x = (mulmod(x, x, n) + c) % n;
            y = (mulmod(y, y, n) + c) % n;
            y = (mulmod(y, y, n) + c) % n;
            d = ugcd(x > y ? x - y : y - x, n);
        }
        if (d != n) return d;
    }
}

static void factor_rec(uint64_t n, uint64_t *out, int *cnt)
{
    if (n == 1 || *cnt >= 64) return;
    if (is_prime_u64(n)) {
        out[(*cnt)++] = n;
        return;
    }
    uint64_t d = pollard(n);
    factor_rec(d, out, cnt);
    factor_rec(n / d, out, cnt);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int factorize(uint64_t n, uint64_t *f, int *e, int max)
{
    uint64_t pf[64];
    int cnt = 0, k = 0;
    for (uint64_t p = 2; p < 1000 && p * p <= n; p++)
        while (n % p == 0 && cnt < 64) pf[cnt++] = p, n /= p;
    if (n > 1) factor_rec(n, pf, &cnt);
    qsort(pf, (size_t)cnt, sizeof *pf, cmp_u64);
    for (int i = 0; i < cnt; i++) {
        if (k && f[k - 1] == pf[i]) e[k - 1]++;
        else if (k < max) f[k] = pf[i], e[k++] = 1;
    }
    return k;
}

static uint64_t need_uint(num x, const char *fn)
{
    need_int(x, fn);
    if (x < 0) calc_error(-1, "%s: argument must be non-negative", fn);
    return (uint64_t)x;
}

static num b_isprime(num x) { return is_prime_u64(need_uint(x, "isprime")); }

static num b_nextprime(num x)
{
    uint64_t n = need_uint(floor(x), "nextprime") + 1;
    while (!is_prime_u64(n)) n++;
    return (num)n;
}

static num b_prevprime(num x)
{
    uint64_t n = need_uint(ceil(x), "prevprime");
    if (n <= 2) return NAN;
    do n--; while (!is_prime_u64(n));
    return (num)n;
}

static num b_totient(num x)
{
    uint64_t n = need_uint(x, "totient"), f[64];
    int e[64];
    if (n == 0) return 0;
    int k = factorize(n, f, e, 64);
    uint64_t r = n;
    for (int i = 0; i < k; i++) r = r / f[i] * (f[i] - 1);
    return (num)r;
}

/* ---------------------------------------------------------- statistics */

static num b_sum(int n, const num *a)
{
    num s = 0, c = 0;
    for (int i = 0; i < n; i++) {
        num u = s + a[i];
        c += fabs(s) >= fabs(a[i]) ? (s - u) + a[i] : (a[i] - u) + s;
        s = u;
    }
    return s + c;
}

static num b_prod(int n, const num *a)
{
    num p = 1;
    for (int i = 0; i < n; i++) p *= a[i];
    return p;
}

static num b_mean(int n, const num *a) { return b_sum(n, a) / n; }

static num b_median(int n, const num *a)
{
    num t[MAX_ARGS];
    memcpy(t, a, sizeof(num) * (size_t)n);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && t[j - 1] > t[j]; j--) {
            num x = t[j];
            t[j] = t[j - 1];
            t[j - 1] = x;
        }
    return n % 2 ? t[n / 2] : 0.5 * (t[n / 2 - 1] + t[n / 2]);
}

static num variance(int n, const num *a, int sample)
{
    num m = 0, s = 0;
    for (int i = 0; i < n; i++) {        /* Welford */
        num d = a[i] - m;
        m += d / (i + 1);
        s += d * (a[i] - m);
    }
    return sample ? (n > 1 ? s / (n - 1) : NAN) : s / n;
}

static num b_var(int n, const num *a) { return variance(n, a, 1); }
static num b_varp(int n, const num *a) { return variance(n, a, 0); }
static num b_std(int n, const num *a) { return sqrt(variance(n, a, 1)); }
static num b_stdp(int n, const num *a) { return sqrt(variance(n, a, 0)); }

static num b_min(int n, const num *a)
{
    num m = a[0];
    for (int i = 1; i < n; i++) m = a[i] < m ? a[i] : m;
    return m;
}

static num b_max(int n, const num *a)
{
    num m = a[0];
    for (int i = 1; i < n; i++) m = a[i] > m ? a[i] : m;
    return m;
}

static num b_gmean(int n, const num *a)
{
    num s = 0;
    for (int i = 0; i < n; i++) s += log(a[i]);
    return exp(s / n);
}

static num b_hmean(int n, const num *a)
{
    num s = 0;
    for (int i = 0; i < n; i++) s += 1 / a[i];
    return n / s;
}

static num b_rms(int n, const num *a)
{
    num s = 0;
    for (int i = 0; i < n; i++) s += a[i] * a[i];
    return sqrt(s / n);
}

/* ------------------------------------------- special functions & probability */

static num b_gamma(num x) { return tgamma(x); }
static num b_lgamma(num x) { return lgamma(x); }
static num b_erf(num x) { return erf(x); }
static num b_erfc(num x) { return erfc(x); }
static num b_beta(num a, num b) { return exp(lgamma(a) + lgamma(b) - lgamma(a + b)); }

static void dist_args(int n, const num *a, num *mu, num *sd)
{
    *mu = n > 1 ? a[1] : 0;
    *sd = n > 2 ? a[2] : 1;
}

static num b_normpdf(int n, const num *a)
{
    num mu, sd;
    dist_args(n, a, &mu, &sd);
    num z = (a[0] - mu) / sd;
    return exp(-0.5 * z * z) / (sd * sqrt(2 * C_PI));
}

static num b_normcdf(int n, const num *a)
{
    num mu, sd;
    dist_args(n, a, &mu, &sd);
    return 0.5 * erfc(-(a[0] - mu) / (sd * C_SQRT2));
}

static num b_norminv(int n, const num *a)
{
    static const num A[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                            1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
    static const num B[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                            6.680131188771972e+01, -1.328068155288572e+01};
    static const num C[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                            -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
    static const num D[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                            3.754408661907416e+00};
    num p = a[0], mu, sd, x, q, r;
    dist_args(n, a, &mu, &sd);
    if (!(p > 0 && p < 1)) return p == 0 ? -INFINITY : p == 1 ? INFINITY : NAN;
    if (p < 0.02425) {
        q = sqrt(-2 * log(p));
        x = (((((C[0] * q + C[1]) * q + C[2]) * q + C[3]) * q + C[4]) * q + C[5]) /
            ((((D[0] * q + D[1]) * q + D[2]) * q + D[3]) * q + 1);
    } else if (p <= 1 - 0.02425) {
        q = p - 0.5;
        r = q * q;
        x = (((((A[0] * r + A[1]) * r + A[2]) * r + A[3]) * r + A[4]) * r + A[5]) * q /
            (((((B[0] * r + B[1]) * r + B[2]) * r + B[3]) * r + B[4]) * r + 1);
    } else {
        q = sqrt(-2 * log(1 - p));
        x = -(((((C[0] * q + C[1]) * q + C[2]) * q + C[3]) * q + C[4]) * q + C[5]) /
            ((((D[0] * q + D[1]) * q + D[2]) * q + D[3]) * q + 1);
    }
    num e = 0.5 * erfc(-x / C_SQRT2) - p, u = e * sqrt(2 * C_PI) * exp(x * x / 2);
    x -= u / (1 + x * u / 2);                       /* Halley refinement */
    return mu + sd * x;
}

static num b_rand(int n, const num *a)
{
    num u = rng_uniform();
    return n == 2 ? a[0] + (a[1] - a[0]) * u : u;
}

static num b_randint(num a, num b)
{
    a = ceil(a);
    b = floor(b);
    if (b < a) return NAN;
    return a + floor(rng_uniform() * (b - a + 1));
}

static num b_randn(int n, const num *a)
{
    num mu = n > 0 ? a[0] : 0, sd = n > 1 ? a[1] : 1, u1;
    do u1 = rng_uniform(); while (u1 <= 0);
    return mu + sd * sqrt(-2 * log(u1)) * cos(2 * C_PI * rng_uniform());
}

/* ---------------------------------------------------- programmer & misc */

static num b_xor(num a, num b) { return (num)(to_int64(a, "xor") ^ to_int64(b, "xor")); }

static num b_popcount(num x)
{
    uint64_t v = (uint64_t)to_int64(x, "popcount");
    int c = 0;
    while (v) v &= v - 1, c++;
    return c;
}

static num b_torad(num x) { return x * (C_PI / 180); }
static num b_todeg(num x) { return x * (180 / C_PI); }
/* one degree / radian / gradian expressed in the current angle unit */
static num unit_angle(num in_deg, num in_rad, num in_grad)
{
    return g_set.angle == ANG_DEG ? in_deg : g_set.angle == ANG_GRAD ? in_grad : in_rad;
}
static num b_udeg(int n, const num *a) { (void)n; (void)a; return unit_angle(1, C_PI / 180, 10.0 / 9); }
static num b_urad(int n, const num *a) { (void)n; (void)a; return unit_angle(180 / C_PI, 1, 200 / C_PI); }
static num b_ugrad(int n, const num *a) { (void)n; (void)a; return unit_angle(0.9, C_PI / 200, 1); }

/* ----------------------------------------------------------------- table */

#define B1(n, f, fl, c, s, h) {n, 1, 1, FT_1, fl, c, 0, f, NULL, NULL, s, h}
#define B2(n, f, fl, c, s, h) {n, 2, 2, FT_2, fl, c, 0, NULL, f, NULL, s, h}
#define BN(n, f, lo, hi, fl, c, s, h) {n, lo, hi, FT_N, fl, c, 0, NULL, NULL, f, s, h}
#define BS(n, sp, lo, hi, c, s, h) {n, lo, hi, FT_SPECIAL, BF_NOFOLD, c, sp, NULL, NULL, NULL, s, h}

const Builtin g_builtins[] = {
    B1("sin", b_sin, BF_ANG, CAT_TRIG, "sin(x)", "sine"),
    B1("cos", b_cos, BF_ANG, CAT_TRIG, "cos(x)", "cosine"),
    B1("tan", b_tan, BF_ANG, CAT_TRIG, "tan(x)", "tangent"),
    B1("cot", b_cot, BF_ANG, CAT_TRIG, "cot(x)", "cotangent"),
    B1("sec", b_sec, BF_ANG, CAT_TRIG, "sec(x)", "secant"),
    B1("csc", b_csc, BF_ANG, CAT_TRIG, "csc(x)", "cosecant"),
    B1("asin", b_asin, BF_ANG, CAT_TRIG, "asin(x)", "inverse sine"),
    B1("acos", b_acos, BF_ANG, CAT_TRIG, "acos(x)", "inverse cosine"),
    B1("atan", b_atan, BF_ANG, CAT_TRIG, "atan(x)", "inverse tangent"),
    B1("acot", b_acot, BF_ANG, CAT_TRIG, "acot(x)", "inverse cotangent"),
    B1("asec", b_asec, BF_ANG, CAT_TRIG, "asec(x)", "inverse secant"),
    B1("acsc", b_acsc, BF_ANG, CAT_TRIG, "acsc(x)", "inverse cosecant"),
    B2("atan2", b_atan2, BF_ANG, CAT_TRIG, "atan2(y, x)", "angle of the point (x, y)"),

    B1("sinh", b_sinh, 0, CAT_HYP, "sinh(x)", "hyperbolic sine"),
    B1("cosh", b_cosh, 0, CAT_HYP, "cosh(x)", "hyperbolic cosine"),
    B1("tanh", b_tanh, 0, CAT_HYP, "tanh(x)", "hyperbolic tangent"),
    B1("coth", b_coth, 0, CAT_HYP, "coth(x)", "hyperbolic cotangent"),
    B1("sech", b_sech, 0, CAT_HYP, "sech(x)", "hyperbolic secant"),
    B1("csch", b_csch, 0, CAT_HYP, "csch(x)", "hyperbolic cosecant"),
    B1("asinh", b_asinh, 0, CAT_HYP, "asinh(x)", "inverse hyperbolic sine"),
    B1("acosh", b_acosh, 0, CAT_HYP, "acosh(x)", "inverse hyperbolic cosine"),
    B1("atanh", b_atanh, 0, CAT_HYP, "atanh(x)", "inverse hyperbolic tangent"),

    B1("sqrt", b_sqrt, 0, CAT_EXP, "sqrt(x)", "square root (also: sqrt x, \xe2\x88\x9ax)"),
    B1("cbrt", b_cbrt, 0, CAT_EXP, "cbrt(x)", "cube root"),
    B2("root", b_root, 0, CAT_EXP, "root(x, n)", "n-th root (odd roots of negatives ok)"),
    B2("pow", b_pow, 0, CAT_EXP, "pow(x, y)", "x to the power y (same as x^y)"),
    B1("exp", b_exp, 0, CAT_EXP, "exp(x)", "e^x"),
    B1("ln", b_ln, 0, CAT_EXP, "ln(x)", "natural logarithm"),
    BN("log", b_log, 1, 2, 0, CAT_EXP, "log(x [, b])", "log base 10, or base b"),
    B1("log10", b_log10, 0, CAT_EXP, "log10(x)", "logarithm base 10"),
    B1("log2", b_log2, 0, CAT_EXP, "log2(x)", "logarithm base 2"),
    B1("log1p", b_log1p, 0, CAT_EXP, "log1p(x)", "ln(1+x), accurate for small x"),
    B1("expm1", b_expm1, 0, CAT_EXP, "expm1(x)", "e^x - 1, accurate for small x"),
    BN("hypot", b_hypot, 1, -1, 0, CAT_EXP, "hypot(a, b, ...)", "sqrt(a^2 + b^2 + ...)"),

    B1("abs", b_abs, 0, CAT_ROUND, "abs(x)", "absolute value"),
    B1("sign", b_sign, 0, CAT_ROUND, "sign(x)", "-1, 0 or 1"),
    B1("sgn", b_sign, 0, CAT_ROUND, "sgn(x)", "same as sign"),
    B1("floor", b_floor, 0, CAT_ROUND, "floor(x)", "round down"),
    B1("ceil", b_ceil, 0, CAT_ROUND, "ceil(x)", "round up"),
    BN("round", b_round, 1, 2, 0, CAT_ROUND, "round(x [, n])", "round (to n decimals)"),
    B1("trunc", b_trunc, 0, CAT_ROUND, "trunc(x)", "integer part"),
    B1("frac", b_frac, 0, CAT_ROUND, "frac(x)", "fractional part"),
    BN("clamp", b_clamp, 3, 3, 0, CAT_ROUND, "clamp(x, lo, hi)", "limit x to [lo, hi]"),

    BN("gcd", b_gcd, 1, -1, 0, CAT_NUMT, "gcd(a, b, ...)", "greatest common divisor"),
    BN("lcm", b_lcm, 1, -1, 0, CAT_NUMT, "lcm(a, b, ...)", "least common multiple"),
    B2("mod", b_mod, 0, CAT_NUMT, "mod(a, b)", "modulo, sign of b (also: a mod b, a % b)"),
    B2("rem", b_rem, 0, CAT_NUMT, "rem(a, b)", "remainder, sign of a"),
    B1("fact", fn_fact, 0, CAT_NUMT, "fact(n)", "factorial (also: n!)"),
    B2("ncr", b_ncr, 0, CAT_NUMT, "ncr(n, k)", "combinations"),
    B2("nCr", b_ncr, 0, CAT_NUMT, "nCr(n, k)", "combinations"),
    B2("comb", b_ncr, 0, CAT_NUMT, "comb(n, k)", "combinations"),
    B2("npr", b_npr, 0, CAT_NUMT, "npr(n, k)", "permutations"),
    B2("nPr", b_npr, 0, CAT_NUMT, "nPr(n, k)", "permutations"),
    B2("perm", b_npr, 0, CAT_NUMT, "perm(n, k)", "permutations"),
    B1("fib", b_fib, 0, CAT_NUMT, "fib(n)", "Fibonacci number"),
    B1("isprime", b_isprime, 0, CAT_NUMT, "isprime(n)", "1 if n is prime (exact up to 2^53)"),
    B1("nextprime", b_nextprime, 0, CAT_NUMT, "nextprime(n)", "smallest prime > n"),
    B1("prevprime", b_prevprime, 0, CAT_NUMT, "prevprime(n)", "largest prime < n"),
    B1("totient", b_totient, 0, CAT_NUMT, "totient(n)", "Euler's phi function"),

    BN("sum", b_sum, 1, -1, 0, CAT_STAT, "sum(a, b, ...)", "sum (compensated)"),
    BN("prod", b_prod, 1, -1, 0, CAT_STAT, "prod(a, b, ...)", "product"),
    BN("mean", b_mean, 1, -1, 0, CAT_STAT, "mean(a, b, ...)", "arithmetic mean"),
    BN("avg", b_mean, 1, -1, 0, CAT_STAT, "avg(a, b, ...)", "same as mean"),
    BN("median", b_median, 1, -1, 0, CAT_STAT, "median(a, b, ...)", "median"),
    BN("var", b_var, 1, -1, 0, CAT_STAT, "var(a, b, ...)", "sample variance"),
    BN("varp", b_varp, 1, -1, 0, CAT_STAT, "varp(a, b, ...)", "population variance"),
    BN("std", b_std, 1, -1, 0, CAT_STAT, "std(a, b, ...)", "sample standard deviation"),
    BN("stdev", b_std, 1, -1, 0, CAT_STAT, "stdev(a, b, ...)", "same as std"),
    BN("stdp", b_stdp, 1, -1, 0, CAT_STAT, "stdp(a, b, ...)", "population standard deviation"),
    BN("min", b_min, 1, -1, 0, CAT_STAT, "min(a, b, ...)", "minimum"),
    BN("max", b_max, 1, -1, 0, CAT_STAT, "max(a, b, ...)", "maximum"),
    BN("gmean", b_gmean, 1, -1, 0, CAT_STAT, "gmean(a, b, ...)", "geometric mean"),
    BN("hmean", b_hmean, 1, -1, 0, CAT_STAT, "hmean(a, b, ...)", "harmonic mean"),
    BN("rms", b_rms, 1, -1, 0, CAT_STAT, "rms(a, b, ...)", "root mean square"),

    B1("gamma", b_gamma, 0, CAT_PROB, "gamma(x)", "gamma function"),
    B1("lgamma", b_lgamma, 0, CAT_PROB, "lgamma(x)", "ln |gamma(x)|"),
    B2("beta", b_beta, 0, CAT_PROB, "beta(a, b)", "beta function"),
    B1("erf", b_erf, 0, CAT_PROB, "erf(x)", "error function"),
    B1("erfc", b_erfc, 0, CAT_PROB, "erfc(x)", "complementary error function"),
    BN("normpdf", b_normpdf, 1, 3, 0, CAT_PROB, "normpdf(x [, mu, sd])", "normal density"),
    BN("normcdf", b_normcdf, 1, 3, 0, CAT_PROB, "normcdf(x [, mu, sd])", "normal distribution P(X<=x)"),
    BN("norminv", b_norminv, 1, 3, 0, CAT_PROB, "norminv(p [, mu, sd])", "inverse normal distribution"),
    BN("rand", b_rand, 0, 2, BF_NOFOLD | BF_NOPAREN, CAT_PROB, "rand([a, b])", "random number in [0,1) or [a,b)"),
    B2("randint", b_randint, BF_NOFOLD, CAT_PROB, "randint(a, b)", "random integer in [a, b]"),
    BN("randn", b_randn, 0, 2, BF_NOFOLD | BF_NOPAREN, CAT_PROB, "randn([mu, sd])", "normally distributed random"),

    BS("if", SF_IF, 2, 3, CAT_CALC, "if(cond, a [, b])", "a if cond is non-zero, else b"),
    BS("sigma", SF_SUM, 4, 4, CAT_CALC, "sigma(expr, k, from, to)", "summation over integer k"),
    BS("product", SF_PROD, 4, 4, CAT_CALC, "product(expr, k, from, to)", "product over integer k"),
    BS("integ", SF_INTEG, 4, 4, CAT_CALC, "integ(expr, x, a, b)", "definite integral (a, b may be inf)"),
    BS("integral", SF_INTEG, 4, 4, CAT_CALC, "integral(expr, x, a, b)", "same as integ"),
    BS("deriv", SF_DERIV, 3, 4, CAT_CALC, "deriv(expr, x, x0 [, n])", "n-th derivative at x0 (n<=4)"),
    BS("diff", SF_DERIV, 3, 4, CAT_CALC, "diff(expr, x, x0 [, n])", "same as deriv"),
    BS("solve", SF_SOLVE, 2, 4, CAT_CALC, "solve(expr, x [, guess | a, b])", "root of expr (or lhs = rhs)"),

    B2("xor", b_xor, 0, CAT_PROG, "xor(a, b)", "bitwise xor (also: a xor b)"),
    B1("popcount", b_popcount, 0, CAT_PROG, "popcount(n)", "number of 1 bits"),

    B1("torad", b_torad, 0, CAT_MISC, "torad(x)", "degrees -> radians"),
    B1("todeg", b_todeg, 0, CAT_MISC, "todeg(x)", "radians -> degrees"),
    BN("deg", b_udeg, 0, 0, BF_NOFOLD | BF_NOPAREN, CAT_MISC, "deg", "one degree in the current mode: 30deg, 30\xc2\xb0"),
    BN("rad", b_urad, 0, 0, BF_NOFOLD | BF_NOPAREN, CAT_MISC, "rad", "one radian in the current mode: sin(2rad)"),
    BN("grad", b_ugrad, 0, 0, BF_NOFOLD | BF_NOPAREN, CAT_MISC, "grad", "one gradian in the current mode"),
    BS("conv", SF_CONV, 3, 3, CAT_MISC, "conv(x, from, to)", "unit conversion: conv(10, km, mi)"),
};
const int g_nbuiltins = (int)(sizeof g_builtins / sizeof g_builtins[0]);

const char *const g_cat_names[CAT_COUNT] = {
    "Trigonometry (uses the angle mode)", "Hyperbolic", "Powers, roots & logarithms",
    "Rounding & parts", "Number theory & combinatorics", "Statistics (any number of arguments)",
    "Special functions, probability & random", "Calculus & logic", "Programmer", "Angles & units",
};

const ConstDef g_consts[] = {
    {"pi", C_PI, "3.14159... (also \xcf\x80)"},
    {"e", C_E, "Euler's number 2.71828..."},
    {"tau", 2 * C_PI, "2*pi"},
    {"phi", C_PHI, "golden ratio 1.61803..."},
    {"inf", INFINITY, "infinity (for integ bounds)"},
};
const int g_nconsts = (int)(sizeof g_consts / sizeof g_consts[0]);

static const Builtin *sorted[sizeof g_builtins / sizeof g_builtins[0]];

static int cmp_builtin(const void *a, const void *b)
{
    return strcmp((*(const Builtin *const *)a)->name, (*(const Builtin *const *)b)->name);
}

void builtins_init(void)
{
    fact_tab[0] = 1;
    for (int i = 1; i <= 170; i++) fact_tab[i] = fact_tab[i - 1] * i;
    for (int i = 0; i < g_nbuiltins; i++) sorted[i] = &g_builtins[i];
    qsort(sorted, (size_t)g_nbuiltins, sizeof sorted[0], cmp_builtin);
}

const Builtin *builtin_find(const char *name, int len)
{
    int lo = 0, hi = g_nbuiltins - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *s = sorted[mid]->name;
        int c = strncmp(name, s, (size_t)len);
        if (c == 0 && s[len] != 0) c = -1;       /* name is a proper prefix of s */
        if (c == 0) return sorted[mid];
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}
