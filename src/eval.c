/* eval.c - AST evaluator, error trampoline and numerical algorithms
 * (series, adaptive Gauss-Kronrod integration, Ridders differentiation,
 * Newton/Brent root finding) */
#include "calc.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>

num g_vstack[VSTACK_SIZE];
int g_sp, g_depth;
long long g_budget = LLONG_MAX / 2;
unsigned g_tick;
int (*g_interrupt_cb)(void);

ErrCtx *g_errctx;
char g_err_msg[256];
int g_err_pos, g_err_eof;

void err_push(ErrCtx *c)
{
    c->prev = g_errctx;
    g_errctx = c;
    g_err_eof = 0;
    g_err_pos = -1;
}

void err_pop(ErrCtx *c) { g_errctx = c->prev; }

void calc_error(int pos, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err_msg, sizeof g_err_msg, fmt, ap);
    va_end(ap);
    g_err_pos = pos;
    if (!g_errctx) {
        fprintf(stderr, "tcalc: internal error: %s\n", g_err_msg);
        abort();
    }
    longjmp(g_errctx->jb, 1);
}

void calc_tick_slow(void)
{
    g_budget -= 1024;
    if (g_budget < 0) calc_error(-1, "computation too long");
    if (g_interrupt_cb && g_interrupt_cb()) calc_error(-1, "interrupted");
}

static int approx_eq(num a, num b)
{
    return a == b || fabs(a - b) <= 1e-12 * fmax(fabs(a), fabs(b));
}

/* ----------------------------------------------------------- series */

static num series(const Node *n, num *fr)
{
    const char *nm = n->kind == N_SUM ? "sigma" : "product";
    num a = eval(n->args[1], fr), b = eval(n->args[2], fr);
    if (!isfinite(a) || !isfinite(b)) calc_error(-1, "%s: bounds must be finite", nm);
    num ra = nearbyint(a), rb = nearbyint(b);
    if (fabs(a - ra) > 1e-9 || fabs(b - rb) > 1e-9) calc_error(-1, "%s: bounds must be integers", nm);
    if (rb - ra > 1e9) calc_error(-1, "%s: too many terms", nm);
    const Node *body = n->args[0];
    int slot = n->slot;
    if (n->kind == N_SUM) {                     /* Neumaier compensated sum */
        num s = 0, c = 0;
        for (num k = ra; k <= rb; k += 1) {
            fr[slot] = k;
            num t = eval(body, fr), u = s + t;
            c += fabs(s) >= fabs(t) ? (s - u) + t : (t - u) + s;
            s = u;
            TICK();
        }
        return s + c;
    }
    num p = 1;
    for (num k = ra; k <= rb; k += 1) {
        fr[slot] = k;
        p *= eval(body, fr);
        TICK();
    }
    return p;
}

/* ------------------------------------------------------ integration */

typedef struct {
    const Node *body;
    num *fr;
    int slot, mode;
    num a, b;
} FCtx;

static num feval(FCtx *c, num x)
{
    c->fr[c->slot] = x;
    TICK();
    return eval(c->body, c->fr);
}

/* integrand after the substitution that maps infinite ranges to finite */
static num int_f(FCtx *c, num t)
{
    num x, w;
    switch (c->mode) {
    case 1: { num u = 1 - t; x = c->a + t / u; w = 1 / (u * u); break; }   /* [a, inf)  */
    case 2: { num u = 1 - t; x = c->b - t / u; w = 1 / (u * u); break; }   /* (-inf, b] */
    case 3: { num u = 1 - t * t; x = t / u; w = (1 + t * t) / (u * u); break; } /* R */
    default: x = t; w = 1; break;
    }
    if (!isfinite(x) || !isfinite(w)) return 0;
    num y = feval(c, x);
    return y == 0 ? 0 : y * w;
}

static const double xgk[8] = {
    0.991455371120812639206854697526329, 0.949107912342758524526189684047851,
    0.864864423359769072789712788640926, 0.741531185599394439863864773280788,
    0.586087235467691130294144845693013, 0.405845151377397166906606412076961,
    0.207784955007898467600689403773245, 0.000000000000000000000000000000000};
static const double wgk[8] = {
    0.022935322010529224963732008058970, 0.063092092629978553290700663189204,
    0.104790010322250183839876322541518, 0.140653259715525918745189590510238,
    0.169004726639267902826583426598550, 0.190350578064785409913256402421014,
    0.204432940075298892414161999234649, 0.209482141084727828012999174891714};
static const double wg[4] = {
    0.129484966168869693270611432679082, 0.279705391489276667901467771423780,
    0.381830050505118944950369775488975, 0.417959183673469387755102040816327};

static num qk15(FCtx *c, num a, num b, num *err)
{
    num ctr = 0.5 * (a + b), hl = 0.5 * (b - a);
    num fc = int_f(c, ctr), rg = fc * wg[3], rk = fc * wgk[7];
    for (int j = 0; j < 3; j++) {
        int k = 2 * j + 1;
        num d = hl * xgk[k], f1 = int_f(c, ctr - d), f2 = int_f(c, ctr + d);
        rg += wg[j] * (f1 + f2);
        rk += wgk[k] * (f1 + f2);
    }
    for (int j = 0; j < 4; j++) {
        int k = 2 * j;
        num d = hl * xgk[k];
        rk += wgk[k] * (int_f(c, ctr - d) + int_f(c, ctr + d));
    }
    *err = fabs((rk - rg) * hl);
    return rk * hl;
}

#define INT_MAXSEG 250

static num integrate(const Node *n, num *fr)
{
    num a = eval(n->args[1], fr), b = eval(n->args[2], fr);
    if (isnan(a) || isnan(b)) return NAN;
    if (a == b) return 0;
    num sign = 1;
    if (a > b) {
        num t = a;
        a = b;
        b = t;
        sign = -1;
    }
    FCtx c = {n->args[0], fr, n->slot, 0, a, b};
    num lo = a, hi = b;
    if (isinf(a) && isinf(b)) c.mode = 3, lo = -1, hi = 1;
    else if (isinf(b)) c.mode = 1, lo = 0, hi = 1;
    else if (isinf(a)) c.mode = 2, lo = 0, hi = 1;

    struct { num a, b, r, e; } seg[INT_MAXSEG];
    int ns = 1;
    seg[0].a = lo;
    seg[0].b = hi;
    seg[0].r = qk15(&c, lo, hi, &seg[0].e);
    for (;;) {
        num tot = 0, err = 0;
        int w = 0;
        for (int i = 0; i < ns; i++) {
            tot += seg[i].r;
            err += seg[i].e;
            if (seg[i].e > seg[w].e) w = i;
        }
        if (err <= fmax(1e-14, 1e-12 * fabs(tot)) || ns >= INT_MAXSEG || !isfinite(tot))
            return sign * tot;
        num m = 0.5 * (seg[w].a + seg[w].b);
        if (m <= seg[w].a || m >= seg[w].b) return sign * tot;
        seg[ns].a = m;
        seg[ns].b = seg[w].b;
        seg[w].b = m;
        seg[w].r = qk15(&c, seg[w].a, m, &seg[w].e);
        seg[ns].r = qk15(&c, m, seg[ns].b, &seg[ns].e);
        ns++;
    }
}

/* ---------------------------------------------------- differentiation */

/* round v to the digits its error estimate supports (hides 6.00000000000001) */
static num round_to_err(num v, num err)
{
    if (!isfinite(v) || !isfinite(err) || err <= 0 || v == 0 || err > 1e-6 * fabs(v)) return v;
    int e = (int)floor(log10(err)) + 1;
    if (e < -300) return v;
    if (e < 0) {
        if (e < -22) return v;
        num inv = pow(10, -e), s = v * inv;
        return fabs(s) < 9e15 ? nearbyint(s) / inv : v;
    }
    num q = pow(10, e);
    return nearbyint(v / q) * q;
}

static num stencil(FCtx *c, num x, num h, int order)
{
    h = (x + h) - x;                 /* make h exactly representable around x */
    switch (order) {
    case 1: return (feval(c, x + h) - feval(c, x - h)) / (2 * h);
    case 2: return (feval(c, x + h) - 2 * feval(c, x) + feval(c, x - h)) / (h * h);
    case 3:
        return (feval(c, x + 2 * h) - 2 * feval(c, x + h) + 2 * feval(c, x - h) - feval(c, x - 2 * h)) /
               (2 * h * h * h);
    default:
        return (feval(c, x + 2 * h) - 4 * feval(c, x + h) + 6 * feval(c, x) - 4 * feval(c, x - h) +
                feval(c, x - 2 * h)) / (h * h * h * h);
    }
}

#define NTAB 10

static num derivative(const Node *n, num *fr)
{
    num x = eval(n->args[1], fr);
    int order = 1;
    if (n->nargs > 2) {
        num o = eval(n->args[2], fr);
        if (o != 1 && o != 2 && o != 3 && o != 4) calc_error(-1, "deriv: order must be 1, 2, 3 or 4");
        order = (int)o;
    }
    if (!isfinite(x)) return NAN;
    FCtx c = {n->args[0], fr, n->slot, 0, 0, 0};
    const num con = 1.4, con2 = con * con;
    num h = (order == 1 ? 0.1 : order == 2 ? 0.4 : 0.8) * fmax(1.0, fabs(x) * 1e-2);
    num a[NTAB][NTAB], err = HUGE_VAL, ans = NAN;
    a[0][0] = stencil(&c, x, h, order);
    for (int i = 1; i < NTAB; i++) {
        h /= con;
        a[0][i] = stencil(&c, x, h, order);
        num fac = con2;
        for (int j = 1; j <= i; j++) {
            a[j][i] = (a[j - 1][i] * fac - a[j - 1][i - 1]) / (fac - 1);
            fac *= con2;
            num errt = fmax(fabs(a[j][i] - a[j - 1][i]), fabs(a[j][i] - a[j - 1][i - 1]));
            if (errt <= err) {
                err = errt;
                ans = a[j][i];
            }
        }
        if (fabs(a[i][i] - a[i - 1][i - 1]) >= 2 * err) break;
    }
    return round_to_err(ans, err);
}

/* ------------------------------------------------------- root finding */

static int brent(FCtx *c, num a, num b, num fa, num fb, num *out)
{
    num cc = b, fc = fb, d = b - a, e = d;
    for (int it = 0; it < 300; it++) {
        if ((fb > 0 && fc > 0) || (fb < 0 && fc < 0)) {
            cc = a;
            fc = fa;
            e = d = b - a;
        }
        if (fabs(fc) < fabs(fb)) {
            a = b; b = cc; cc = a;
            fa = fb; fb = fc; fc = fa;
        }
        num tol = 2 * DBL_EPSILON * fabs(b) + 1e-300, xm = 0.5 * (cc - b);
        if (fabs(xm) <= tol || fb == 0) {
            *out = b;
            return 1;
        }
        if (fabs(e) >= tol && fabs(fa) > fabs(fb)) {
            num s = fb / fa, p, q;
            if (a == cc) {
                p = 2 * xm * s;
                q = 1 - s;
            } else {
                num r;
                q = fa / fc;
                r = fb / fc;
                p = s * (2 * xm * q * (q - r) - (b - a) * (r - 1));
                q = (q - 1) * (r - 1) * (s - 1);
            }
            if (p > 0) q = -q;
            p = fabs(p);
            num m1 = 3 * xm * q - fabs(tol * q), m2 = fabs(e * q);
            if (2 * p < (m1 < m2 ? m1 : m2)) {
                e = d;
                d = p / q;
            } else {
                d = xm;
                e = d;
            }
        } else {
            d = xm;
            e = d;
        }
        a = b;
        fa = fb;
        b += fabs(d) > tol ? d : (xm >= 0 ? tol : -tol);
        fb = feval(c, b);
        if (isnan(fb)) return 0;
    }
    *out = b;
    return 1;
}

/* prefer a "round" neighbour of x when it is at least as good a root:
 * 2.0000000000000004 -> 2, 0.9999999987 (double root) -> 1 */
static num polish(FCtx *c, num x)
{
    num best = x, fbest = fabs(feval(c, x));
    if (!isfinite(x)) return x;
    if (fabs(x) < 1e-9 && fabs(feval(c, 0)) <= fbest) return 0;
    num mag = x == 0 ? 0 : floor(log10(fabs(x)));
    for (int d = 12; d >= 6; d--) {           /* fine -> coarse: ties go to coarser */
        num m = pow(10, d - 1 - mag);
        if (!isfinite(m) || fabs(x * m) > 9e15) continue;
        num r = nearbyint(x * m) / m, fr = fabs(feval(c, r));
        if (fr <= fbest) best = r, fbest = fr;
    }
    return best;
}

static int good_root(FCtx *c, num a, num b, num fa, num fb, num *out)
{
    num r;
    if (!brent(c, a, b, fa, fb, &r)) return 0;
    num fr = fabs(feval(c, r));
    if (!isfinite(fr) || fr > fmin(fabs(fa), fabs(fb))) return 0;   /* pole, not a root */
    *out = polish(c, r);
    return 1;
}

static num solve(const Node *n, num *fr)
{
    FCtx c = {n->args[0], fr, n->slot, 0, 0, 0};
    num r;
    if (n->nargs == 3) {                      /* solve(expr, x, a, b) */
        num a = eval(n->args[1], fr), b = eval(n->args[2], fr);
        num fa = feval(&c, a), fb = feval(&c, b);
        if (fa == 0) return a;
        if (fb == 0) return b;
        if (isnan(fa) || isnan(fb) || (fa > 0) == (fb > 0))
            calc_error(-1, "solve: f(a) and f(b) must have opposite signs");
        if (good_root(&c, a, b, fa, fb, &r)) return r;
        calc_error(-1, "solve: no root in interval (discontinuity?)");
    }
    num g = n->nargs == 2 ? eval(n->args[1], fr) : 0;
    if (!isfinite(g)) calc_error(-1, "solve: invalid starting point");

    /* 1. damped Newton */
    num x = g, fx = feval(&c, x);
    if (fx == 0) return x;
    for (int it = 0; it < 100 && isfinite(fx); it++) {
        num h = 1e-7 * fmax(1.0, fabs(x));
        num d = (feval(&c, x + h) - feval(&c, x - h)) / (2 * h);
        if (d == 0 || !isfinite(d)) break;
        num dx = fx / d, xn = x - dx, fn = feval(&c, xn);
        for (int k = 0; k < 40 && !(fabs(fn) < fabs(fx)) && fn != 0; k++) {
            dx *= 0.5;
            xn = x - dx;
            fn = feval(&c, xn);
        }
        if (!(fabs(fn) <= fabs(fx))) {
            if (fabs(fx) < 1e-10) return polish(&c, x);    /* stagnated at a root */
            break;
        }
        x = xn;
        fx = fn;
        if (fx == 0 || fabs(dx) <= 4 * DBL_EPSILON * fmax(1.0, fabs(x))) {
            num s = 1e-6 * fmax(1.0, fabs(x));
            num f1 = feval(&c, x - s), f2 = feval(&c, x + s);
            if (fx == 0 || (f1 > 0) != (f2 > 0) || fabs(fx) < 1e-12) return polish(&c, x);
            break;
        }
    }

    /* 2. expand outward from the guess looking for a sign change */
    num f0 = feval(&c, g), xp = g, fp = f0, xm = g, fm = f0;
    num d = 0.05 * fmax(1.0, fabs(g));
    for (int k = 0; k < 120 && d < 1e300; k++, d *= 1.5) {
        num x1 = g + d, f1 = feval(&c, x1);
        if (isfinite(fp) && isfinite(f1) && (fp > 0) != (f1 > 0)) {
            if (f1 == 0) return x1;
            if (good_root(&c, xp, x1, fp, f1, &r)) return r;
        }
        xp = x1;
        fp = f1;
        num x2 = g - d, f2 = feval(&c, x2);
        if (isfinite(fm) && isfinite(f2) && (fm > 0) != (f2 > 0)) {
            if (f2 == 0) return x2;
            if (good_root(&c, x2, xm, f2, fm, &r)) return r;
        }
        xm = x2;
        fm = f2;
    }
    calc_error(-1, "solve: no root found (try another guess or an interval)");
}

/* ---------------------------------------------------------- evaluator */

num eval(const Node *n, num *fr)
{
    switch ((NodeKind)n->kind) {
    case N_NUM: return n->u.val;
    case N_VAR:
        if (UNLIKELY(!n->u.var->defined)) calc_error(-1, "undefined variable '%s'", n->u.var->name);
        return n->u.var->val;
    case N_LOCAL: return fr[n->slot];
    case N_ASSIGN: {
        num x = eval(n->a, fr);
        n->u.var->val = x;
        n->u.var->defined = 1;
        return x;
    }
    case N_NEG: return -eval(n->a, fr);
    case N_NOT: return eval(n->a, fr) == 0 ? 1 : 0;
    case N_BNOT: return (num)~to_int64(eval(n->a, fr), "~");
    case N_FACT: return fn_fact(eval(n->a, fr));
    case N_ADD: return eval(n->a, fr) + eval(n->b, fr);
    case N_SUB: return eval(n->a, fr) - eval(n->b, fr);
    case N_MUL: return eval(n->a, fr) * eval(n->b, fr);
    case N_DIV: return eval(n->a, fr) / eval(n->b, fr);
    case N_MOD: { num a = eval(n->a, fr); return fmod_floor(a, eval(n->b, fr)); }
    case N_POW: { num a = eval(n->a, fr); return fn_pow(a, eval(n->b, fr)); }
    case N_EQ: { num a = eval(n->a, fr); return approx_eq(a, eval(n->b, fr)); }
    case N_NE: { num a = eval(n->a, fr); return !approx_eq(a, eval(n->b, fr)); }
    case N_LT: { num a = eval(n->a, fr); return a < eval(n->b, fr); }
    case N_GT: { num a = eval(n->a, fr); return a > eval(n->b, fr); }
    case N_LE: { num a = eval(n->a, fr), b = eval(n->b, fr); return a < b || approx_eq(a, b); }
    case N_GE: { num a = eval(n->a, fr), b = eval(n->b, fr); return a > b || approx_eq(a, b); }
    case N_AND: return eval(n->a, fr) != 0 && eval(n->b, fr) != 0;
    case N_OR: return eval(n->a, fr) != 0 || eval(n->b, fr) != 0;
    case N_BAND: { int64_t a = to_int64(eval(n->a, fr), "&"); return (num)(a & to_int64(eval(n->b, fr), "&")); }
    case N_BOR: { int64_t a = to_int64(eval(n->a, fr), "|"); return (num)(a | to_int64(eval(n->b, fr), "|")); }
    case N_BXOR: { int64_t a = to_int64(eval(n->a, fr), "xor"); return (num)(a ^ to_int64(eval(n->b, fr), "xor")); }
    case N_SHL:
    case N_SHR: {
        int64_t a = to_int64(eval(n->a, fr), "shift"), s = to_int64(eval(n->b, fr), "shift");
        if (s < 0 || s > 63) calc_error(-1, "shift count must be 0..63");
        if (n->kind == N_SHL) return (num)(int64_t)((uint64_t)a << s);
        return (num)(a < 0 ? ~(~a >> s) : a >> s);
    }
    case N_CALL1: return n->u.fn->f1(eval(n->a, fr));
    case N_CALL2: { num a = eval(n->a, fr); return n->u.fn->f2(a, eval(n->b, fr)); }
    case N_CALLN: {
        int k = n->nargs;
        if (UNLIKELY(g_sp + k > VSTACK_SIZE)) calc_error(-1, "stack overflow");
        num *av = g_vstack + g_sp;
        g_sp += k;
        for (int i = 0; i < k; i++) av[i] = eval(n->args[i], fr);
        num r = n->u.fn->fn(k, av);
        g_sp -= k;
        return r;
    }
    case N_UCALL: {
        UFunc *f = n->u.uf;
        if (UNLIKELY(!f->body)) calc_error(-1, "function '%s' is not defined", f->name);
        if (UNLIKELY(n->nargs != f->nparams))
            calc_error(-1, "%s expects %d argument%s", f->name, f->nparams, f->nparams == 1 ? "" : "s");
        if (UNLIKELY(g_depth >= MAX_DEPTH)) calc_error(-1, "recursion too deep (limit %d)", MAX_DEPTH);
        int base = g_sp;
        if (UNLIKELY(base + f->nslots > VSTACK_SIZE)) calc_error(-1, "stack overflow");
        g_sp += f->nslots;
        num *nf = g_vstack + base;
        for (int i = 0; i < n->nargs; i++) nf[i] = eval(n->args[i], fr);
        TICK();
        g_depth++;
        num r = eval(f->body, nf);
        g_depth--;
        g_sp = base;
        return r;
    }
    case N_IF: {
        num c = eval(n->args[0], fr);
        if (isnan(c)) return NAN;
        if (c != 0) return eval(n->args[1], fr);
        return n->nargs > 2 ? eval(n->args[2], fr) : NAN;
    }
    case N_SUM:
    case N_PROD: return series(n, fr);
    case N_INTEG: return integrate(n, fr);
    case N_DERIV: return derivative(n, fr);
    case N_SOLVE: return solve(n, fr);
    case N_CONV: return unit_convert(eval(n->a, fr), n->u.unit[0], n->u.unit[1]);
    }
    return NAN;
}

int eval_safe(const Node *n, num *fr, num *out, char *msg, size_t msgsz)
{
    int sp = g_sp, depth = g_depth;
    ErrCtx ec;
    err_push(&ec);
    if (setjmp(ec.jb)) {
        err_pop(&ec);
        g_sp = sp;
        g_depth = depth;
        if (msg) str_copy(msg, msgsz, g_err_msg);
        return 0;
    }
    *out = eval(n, fr);
    err_pop(&ec);
    return 1;
}
