/* plot.c - function plots drawn with Unicode braille (2x4 dots per cell) */
#include "term.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char bits;
    signed char color;     /* -2 empty, -1 axis, 0.. function */
} Cell;

static int aborted;
static const unsigned char dotbit[2][4] = {{0x01, 0x02, 0x04, 0x40}, {0x08, 0x10, 0x20, 0x80}};

static num fval(Plot *pl, int i, num x, int *ok)
{
    char msg[64];
    num y;
    g_sp = pl->nslots[i];
    g_vstack[0] = x;
    *ok = eval_safe(pl->fn[i], g_vstack, &y, msg, sizeof msg);
    g_sp = 0;
    if (!*ok) {
        if (!strcmp(msg, "interrupted")) aborted = 1;
        return NAN;
    }
    return y;
}

static int uses_trig(const Node *n, int depth)
{
    if (!n || depth > 30) return 0;
    if ((n->kind == N_CALL1 || n->kind == N_CALL2 || n->kind == N_CALLN) && n->u.fn->cat == CAT_TRIG) return 1;
    if (n->kind == N_UCALL && uses_trig(n->u.uf->body, depth + 1)) return 1;
    if (uses_trig(n->a, depth + 1) || uses_trig(n->b, depth + 1)) return 1;
    if (n->kind == N_CALLN || n->kind == N_UCALL || n->kind >= N_IF) {
        if (n->kind == N_CONV) return 0;
        for (int i = 0; i < n->nargs; i++)
            if (uses_trig(n->args[i], depth + 1)) return 1;
    }
    return 0;
}

static int parse_range(const char *s, num *lo, num *hi, char *err, size_t errsz)
{
    char buf[512];
    str_copy(buf, sizeof buf, s);
    int depth = 0;
    char *comma = NULL;
    for (char *p = buf; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        else if (*p == ',' && depth == 0 && !comma) comma = p;
    }
    if (!comma) {
        snprintf(err, errsz, "plot: range must look like [min, max]");
        return -1;
    }
    *comma = 0;
    char *part[2] = {buf, comma + 1};
    num v[2];
    for (int i = 0; i < 2; i++) {
        CalcResult r;
        calc_eval(part[i], EV_PREVIEW, &r);
        if (r.kind != RES_VALUE || !isfinite(r.value)) {
            snprintf(err, errsz, "plot: bad range value '%s'%s%s", str_trim(part[i]),
                     r.kind == RES_ERROR ? ": " : "", r.kind == RES_ERROR ? r.msg : "");
            return -1;
        }
        v[i] = r.value;
    }
    if (!(v[0] < v[1])) {
        snprintf(err, errsz, "plot: range needs min < max");
        return -1;
    }
    *lo = v[0];
    *hi = v[1];
    return 0;
}

int plot_parse(Plot *pl, const char *spec, char *err, size_t errsz)
{
    char buf[1024], *grp[2];
    int ng = 0;
    memset(pl, 0, sizeof *pl);
    str_copy(buf, sizeof buf, spec);
    char *s = str_trim(buf);
    for (;;) {                                    /* trailing [a, b] groups */
        size_t L = strlen(s);
        while (L && s[L - 1] == ' ') s[--L] = 0;
        if (!L || s[L - 1] != ']' || ng == 2) break;
        char *open = strrchr(s, '[');
        if (!open) {
            snprintf(err, errsz, "plot: unmatched ']'");
            return -1;
        }
        s[L - 1] = 0;
        *open = 0;
        grp[ng++] = open + 1;
    }
    if (ng == 2) {
        if (parse_range(grp[1], &pl->xmin, &pl->xmax, err, errsz) ||
            parse_range(grp[0], &pl->ymin, &pl->ymax, err, errsz))
            return -1;
        pl->has_xrange = pl->has_yrange = 1;
    } else if (ng == 1) {
        if (parse_range(grp[0], &pl->xmin, &pl->xmax, err, errsz)) return -1;
        pl->has_xrange = 1;
    }

    static const char *const locals[] = {"x"};
    char *save = NULL;
    for (char *piece = strtok_r(s, ";", &save); piece; piece = strtok_r(NULL, ";", &save)) {
        char expr[512];
        piece = str_trim(piece);
        if (!*piece) continue;
        if (pl->nfn == PLOT_MAX_FN) {
            snprintf(err, errsz, "plot: at most %d functions", PLOT_MAX_FN);
            plot_free(pl);
            return -1;
        }
        int len = (int)strlen(piece), simple = len < NAME_LEN;
        for (int i = 0; i < len && simple; i++) simple = is_ident_char((unsigned char)piece[i]);
        const Builtin *b = simple ? builtin_find(piece, len) : NULL;
        UFunc *uf = simple ? func_find(piece, len) : NULL;
        if ((b && b->type == FT_1) || (uf && uf->nparams == 1)) snprintf(expr, sizeof expr, "%s(x)", piece);
        else str_copy(expr, sizeof expr, piece);
        CalcResult r;
        int ns;
        Node *n = calc_parse_expr(expr, &pl->arena, locals, 1, &ns, &r);
        if (!n) {
            snprintf(err, errsz, "plot: %s  (in: %s)", r.msg, expr);
            plot_free(pl);
            return -1;
        }
        pl->fn[pl->nfn] = n;
        pl->nslots[pl->nfn] = ns;
        str_copy(pl->label[pl->nfn++], sizeof pl->label[0], expr);
    }
    if (!pl->nfn) {
        snprintf(err, errsz, "plot: nothing to plot (example: :plot sin(x); x^2/10)");
        plot_free(pl);
        return -1;
    }
    if (!pl->has_xrange) {
        int trig = 0;
        for (int i = 0; i < pl->nfn; i++) trig |= uses_trig(pl->fn[i], 0);
        num r = !trig ? 10 : g_set.angle == ANG_DEG ? 360 : g_set.angle == ANG_GRAD ? 400 : 2 * C_PI;
        pl->xmin = -r;
        pl->xmax = r;
    }
    aborted = 0;
    if (!pl->has_yrange) plot_autoscale_y(pl, 400);
    return 0;
}

void plot_free(Plot *pl) { arena_free(&pl->arena); }

static int cmp_num(const void *a, const void *b)
{
    num x = *(const num *)a, y = *(const num *)b;
    return x < y ? -1 : x > y;
}

void plot_autoscale_y(Plot *pl, int samples)
{
    num *v = xmalloc(sizeof(num) * (size_t)(samples * pl->nfn));
    int n = 0, ok;
    for (int i = 0; i < pl->nfn && !aborted; i++)
        for (int k = 0; k < samples && !aborted; k++) {
            num y = fval(pl, i, pl->xmin + (k + 0.5) * (pl->xmax - pl->xmin) / samples, &ok);
            if (ok && isfinite(y)) v[n++] = y;
        }
    num lo = -1, hi = 1;
    if (n) {
        qsort(v, (size_t)n, sizeof *v, cmp_num);
        num p5 = v[(int)(0.05 * (n - 1))], p95 = v[(int)(0.95 * (n - 1))];
        lo = v[0];
        hi = v[n - 1];
        if (p95 > p5 && hi - lo > 4 * (p95 - p5)) {     /* poles: ignore the spikes */
            lo = p5 - 0.1 * (p95 - p5);
            hi = p95 + 0.1 * (p95 - p5);
        }
        if (hi - lo < 1e-12 * fmax(1.0, fabs(hi))) {
            lo -= 1;
            hi += 1;
        }
        num pad = (hi - lo) * 0.08;
        lo -= pad;
        hi += pad;
    }
    free(v);
    pl->ymin = lo;
    pl->ymax = hi;
}

static void set_dot(Cell *c, int cw, int px, int py, int color)
{
    Cell *cell = &c[(py / 4) * cw + px / 2];
    cell->bits |= dotbit[px & 1][py & 3];
    if (color >= 0 || cell->color == -2) cell->color = (signed char)color;
}

static void draw_canvas(Plot *pl, Cell *c, int cw, int ch)
{
    int W = cw * 2, H = ch * 4;
    num xr = pl->xmax - pl->xmin, yr = pl->ymax - pl->ymin;
    for (int i = 0; i < cw * ch; i++) c[i].bits = 0, c[i].color = -2;
    if (pl->ymin < 0 && pl->ymax > 0) {
        int py = (int)floor(pl->ymax / yr * H);
        if (py >= 0 && py < H)
            for (int px = 0; px < W; px++) set_dot(c, cw, px, py, -1);
    }
    if (pl->xmin < 0 && pl->xmax > 0) {
        int px = (int)floor(-pl->xmin / xr * W);
        if (px >= 0 && px < W)
            for (int py = 0; py < H; py++) set_dot(c, cw, px, py, -1);
    }
    for (int i = 0; i < pl->nfn && !aborted; i++) {
        num prev = NAN;
        for (int px = 0; px < W && !aborted; px++) {
            int ok;
            num y = fval(pl, i, pl->xmin + (px + 0.5) * xr / W, &ok);
            num py = (pl->ymax - y) / yr * H;
            if (!ok || !isfinite(py) || fabs(py) > 1e9) {
                prev = NAN;
                continue;
            }
            int a = (int)floor(py), b = a;
            if (!isnan(prev) && fabs(py - prev) < H) {
                int join = 1;
                if (fabs(py - prev) > H / 4.0) {      /* steep: a pole or a real slope? */
                    int ok2;
                    num ym = fval(pl, i, pl->xmin + px * xr / W, &ok2);
                    num pm = (pl->ymax - ym) / yr * H;
                    join = ok2 && pm >= fmin(prev, py) - 1 && pm <= fmax(prev, py) + 1;
                }
                if (join) {
                    int p0 = (int)floor(prev);
                    if (p0 < a) a = p0;
                    if (p0 > b) b = p0;
                }
            }
            prev = py;
            if (b < 0 || a >= H) continue;
            if (a < 0) a = 0;
            if (b >= H) b = H - 1;
            for (int yy = a; yy <= b; yy++) set_dot(c, cw, px, yy, i);
        }
    }
}

static const char *cell_glyph(Cell c, int ascii, char *buf)
{
    if (!c.bits) return " ";
    if (ascii) return c.color == -1 ? "." : "*";
    uint32_t cp = 0x2800u + c.bits;
    buf[0] = (char)(0xE0 | (cp >> 12));
    buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = (char)(0x80 | (cp & 0x3F));
    buf[3] = 0;
    return buf;
}

static const char *color_of(int c) { return c >= 0 ? T.plot[c % PLOT_MAX_FN] : T.axis; }

static void short_num(char *b, size_t n, num v)
{
    snprintf(b, n, "%.5g", fabs(v) < 1e-12 ? 0.0 : v);
}

/* ---------------------------------------------------------- interactive */

void plot_interactive(Plot *pl)
{
    Cell *cells = NULL;
    size_t cap = 0;
    int trace = 0, tpx = 0;
    num ix0 = pl->xmin, ix1 = pl->xmax, iy0 = pl->ymin, iy1 = pl->ymax;
    const char *mark = g_opt_ascii ? "*" : "\xe2\x96\xa0";
    aborted = 0;
    for (;;) {
        int w, h;
        term_size(&w, &h);
        if (w > 500) w = 500;
        if (h > 500) h = 500;
        scr_resize(w, h);
        int ch = h - 2 > 1 ? h - 2 : 1, cw = w, W = cw * 2;
        if ((size_t)(cw * ch) > cap) {
            cap = (size_t)(cw * ch);
            cells = xrealloc(cells, sizeof *cells * cap);
        }
        if (tpx >= W) tpx = W - 1;
        aborted = 0;
        draw_canvas(pl, cells, cw, ch);

        Line *l = scr_line(0);
        ln_sgr(l, T.bar_hl);
        ln_text(l, " plot ", -1);
        ln_sgr(l, T.bar);
        for (int i = 0; i < pl->nfn; i++) {
            ln_text(l, " ", 1);
            ln_sgr(l, T.reset);
            ln_sgr(l, color_of(i));
            ln_text(l, mark, -1);
            ln_sgr(l, T.bar);
            ln_text(l, " ", 1);
            ln_text(l, pl->label[i], -1);
        }
        ln_pad(l);
        num xr = pl->xmax - pl->xmin;
        for (int r = 0; r < ch; r++) {
            l = scr_line(r + 1);
            int cur = -9;
            for (int x = 0; x < cw; x++) {
                Cell c = cells[r * cw + x];
                int col = c.bits ? c.color : -3;
                int is_trace = trace && x == tpx / 2;
                if (is_trace) col = 100 + col;
                if (col != cur) {
                    ln_sgr(l, T.reset);
                    if (is_trace) ln_sgr(l, T.sel);
                    if (c.bits) ln_sgr(l, color_of(c.color));
                    cur = col;
                }
                char gb[8];
                ln_text(l, cell_glyph(c, g_opt_ascii, gb), -1);
            }
            ln_sgr(l, T.reset);
        }
        char st[512], a[32], b[32], cc[32], d[32];
        l = scr_line(h - 1);
        ln_sgr(l, T.bar_dim);
        if (trace) {
            num x = pl->xmin + (tpx + 0.5) * xr / W;
            short_num(a, sizeof a, x);
            snprintf(st, sizeof st, " x=%s", a);
            ln_text(l, st, -1);
            for (int i = 0; i < pl->nfn; i++) {
                int ok;
                num y = fval(pl, i, x, &ok);
                if (ok && !isnan(y)) short_num(a, sizeof a, y);
                else str_copy(a, sizeof a, "-");
                ln_text(l, "  ", 2);
                ln_sgr(l, T.reset);
                ln_sgr(l, color_of(i));
                ln_text(l, mark, -1);
                ln_sgr(l, T.bar_dim);
                ln_text(l, a, -1);
            }
            ln_text(l, "   t: exit trace", -1);
        } else {
            static const char *const hints[] = {"  q back", "  t trace", "  +/- zoom", "  arrows pan",
                                                "  a auto-y", "  r reset"};
            short_num(a, sizeof a, pl->xmin);
            short_num(b, sizeof b, pl->xmax);
            short_num(cc, sizeof cc, pl->ymin);
            short_num(d, sizeof d, pl->ymax);
            snprintf(st, sizeof st, " x[%s, %s] y[%s, %s]", a, b, cc, d);
            for (int i = 0; i < 6 && strlen(st) + strlen(hints[i]) < (size_t)w; i++) strcat(st, hints[i]);
            ln_text(l, st, -1);
        }
        ln_pad(l);
        scr_present(h - 1, w - 1, 0);

        Key k;
        if (!term_read_key(&k, -1)) continue;
        num yr = pl->ymax - pl->ymin, zx = 1, zy = 1, cx = (pl->xmin + pl->xmax) / 2,
            cy = (pl->ymin + pl->ymax) / 2;
        int fast = (k.mods & (M_SHIFT | M_CTRL | M_ALT)) != 0;
        switch (k.code) {
        case K_RESIZE: scr_invalidate(); break;
        case K_QUIT: case K_ESC: case K_ENTER: case K_F1 + 4: case K_F1 + 9:
            free(cells);
            return;
        case K_LEFT:
        case K_RIGHT: {
            int dir = k.code == K_LEFT ? -1 : 1;
            if (trace) {
                tpx += dir * (fast ? 10 : 1);
                if (tpx < 0 || tpx >= W) {
                    num sh = dir * xr * 0.25;
                    pl->xmin += sh;
                    pl->xmax += sh;
                    tpx = tpx < 0 ? tpx + W / 4 : tpx - W / 4;
                }
            } else {
                num sh = dir * xr * (fast ? 0.5 : 0.1);
                pl->xmin += sh;
                pl->xmax += sh;
            }
            break;
        }
        case K_UP:
        case K_DOWN: {
            num sh = (k.code == K_UP ? 1 : -1) * yr * (fast ? 0.5 : 0.1);
            pl->ymin += sh;
            pl->ymax += sh;
            break;
        }
        case K_HOME: tpx = 0; break;
        case K_END: tpx = W - 1; break;
        case K_MOUSE:
            if (k.mbtn == 64 || k.mbtn == 65) {
                num f = k.mbtn == 64 ? 0.8 : 1.25;
                cx = pl->xmin + (k.mx + 0.5) / cw * xr;
                pl->xmin = cx + (pl->xmin - cx) * f;
                pl->xmax = cx + (pl->xmax - cx) * f;
                pl->ymin = cy + (pl->ymin - cy) * f;
                pl->ymax = cy + (pl->ymax - cy) * f;
            } else if (k.mbtn == 0 && k.mpress && k.my >= 1 && k.my <= ch) {
                trace = 1;
                tpx = k.mx * 2 + 1;
            }
            break;
        case K_CHAR:
            if (k.mods & M_CTRL) {
                if (k.ch == 'c' || k.ch == 'd' || k.ch == 'q') {
                    free(cells);
                    return;
                }
                break;
            }
            switch (k.ch) {
            case 'q': case 'Q':
                free(cells);
                return;
            case '+': case '=': zx = zy = 0.8; break;
            case '-': case '_': zx = zy = 1.25; break;
            case 'x': zx = 0.8; break;
            case 'X': zx = 1.25; break;
            case 'y': zy = 0.8; break;
            case 'Y': zy = 1.25; break;
            case 'a': aborted = 0; plot_autoscale_y(pl, 400); break;
            case 't': trace = !trace; if (trace) tpx = W / 2; break;
            case 'r': case '0':
                pl->xmin = ix0, pl->xmax = ix1, pl->ymin = iy0, pl->ymax = iy1;
                break;
            case 'h': case 'l': {
                num sh = (k.ch == 'h' ? -1 : 1) * xr * 0.1;
                pl->xmin += sh;
                pl->xmax += sh;
                break;
            }
            case 'k': case 'j': {
                num sh = (k.ch == 'k' ? 1 : -1) * yr * 0.1;
                pl->ymin += sh;
                pl->ymax += sh;
                break;
            }
            default: break;
            }
            break;
        default: break;
        }
        if (zx != 1) {
            pl->xmin = cx + (pl->xmin - cx) * zx;
            pl->xmax = cx + (pl->xmax - cx) * zx;
        }
        if (zy != 1) {
            pl->ymin = cy + (pl->ymin - cy) * zy;
            pl->ymax = cy + (pl->ymax - cy) * zy;
        }
        if (!(pl->xmax - pl->xmin > 1e-300) || !(pl->ymax - pl->ymin > 1e-300)) {
            pl->xmin = ix0, pl->xmax = ix1, pl->ymin = iy0, pl->ymax = iy1;
        }
    }
}

/* ------------------------------------------------------------- stdout */

void plot_print(Plot *pl, FILE *f, int width, int height, int color, int ascii)
{
    char a[32], b[32], c[32], d[32], gb[8];
    if (width < 10) width = 10;
    if (height < 3) height = 3;
    Cell *cells = xmalloc(sizeof *cells * (size_t)(width * height));
    theme_init(color);
    aborted = 0;
    draw_canvas(pl, cells, width, height);
    for (int i = 0; i < pl->nfn; i++)
        fprintf(f, "%s%s%s %s  ", color ? color_of(i) : "", ascii ? "*" : "\xe2\x96\xa0", color ? T.reset : "",
                pl->label[i]);
    fputc('\n', f);
    for (int r = 0; r < height; r++) {
        int cur = -9;
        for (int x = 0; x < width; x++) {
            Cell cl = cells[r * width + x];
            int col = cl.bits ? cl.color : -3;
            if (col != cur && color) {
                fputs(cl.bits ? color_of(cl.color) : T.reset, f);
                cur = col;
            }
            fputs(cell_glyph(cl, ascii, gb), f);
        }
        if (color) fputs(T.reset, f);
        fputc('\n', f);
    }
    short_num(a, sizeof a, pl->xmin);
    short_num(b, sizeof b, pl->xmax);
    short_num(c, sizeof c, pl->ymin);
    short_num(d, sizeof d, pl->ymax);
    fprintf(f, "x: [%s, %s]   y: [%s, %s]\n", a, b, c, d);
    free(cells);
}
