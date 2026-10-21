/* tui.c - interactive full-screen calculator */
#include "term.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INPUT_MAX 1024
#define HIST_MAX  1000
#define ENTRY_MAX 600
#define UNDO_MAX  64
#define VL_MAX    4096
#define PANEL_W   32

enum { E_VALUE, E_ERROR, E_INFO, E_FUNC };
enum { EK_NONE, EK_INSERT, EK_DELETE, EK_OTHER };
enum { VL_EXPR, VL_EXPR_RES, VL_RES, VL_TEXT };

typedef struct {
    char *in, *text;
    num val;
    int kind, err_pos;
} Entry;

typedef struct {
    char s[INPUT_MAX];
    int len, cur;
} Snap;

typedef struct {
    int ent, type, off, len;
} VLine;

/* character classes for syntax highlighting */
enum { C_DEF, C_NUM, C_FUNC, C_VAR, C_CONST, C_KW, C_OP, C_PAREN, C_PMATCH, C_ERR, C_DIM, C_CMD, C_ERRPOS };

static struct {
    char buf[INPUT_MAX];
    int len, cur, hscroll;
    Entry ent[ENTRY_MAX];
    int nent, scroll;
    char *hist[HIST_MAX];
    int nhist, hpos, hbrowse;
    char hsaved[INPUT_MAX], hprefix[INPUT_MAX];
    int comp_active, comp_idx, comp_n, comp_start, comp_len;
    char comp[64][NAME_LEN];
    char hint[512];
    int last_edit;
    CalcResult prev;
    int prev_valid, dirty;
    char msg[256];
    int msg_err;
    int w, h, mouse, paste, quit;
    int row_ent[512];
    int input_row, pane_top, pane_h;
    int hdr_x[3][2];
    long long busy_start;
    int busy_shown, previewing;
} S;

static Snap undo_st[UNDO_MAX], redo_st[UNDO_MAX];
static int nundo, nredo;

static void render(void);

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_msg(int err, const char *fmt, ...) PRINTF_LIKE(2, 3);

static void set_msg(int err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(S.msg, sizeof S.msg, fmt, ap);
    va_end(ap);
    S.msg_err = err;
}

/* ------------------------------------------------------------- entries */

static void add_entry(int kind, const char *in, const char *text, num val, int err_pos)
{
    if (S.nent == ENTRY_MAX) {
        free(S.ent[0].in);
        free(S.ent[0].text);
        memmove(S.ent, S.ent + 1, sizeof(Entry) * (ENTRY_MAX - 1));
        S.nent--;
    }
    Entry *e = &S.ent[S.nent++];
    e->kind = kind;
    e->in = xstrdup(in ? in : "");
    e->text = xstrdup(text ? text : "");
    e->val = val;
    e->err_pos = err_pos;
    S.scroll = 0;
}

static void clear_entries(void)
{
    for (int i = 0; i < S.nent; i++) {
        free(S.ent[i].in);
        free(S.ent[i].text);
    }
    S.nent = 0;
    S.scroll = 0;
}

static const Entry *last_value(void)
{
    for (int i = S.nent - 1; i >= 0; i--)
        if (S.ent[i].kind == E_VALUE) return &S.ent[i];
    return NULL;
}

/* ------------------------------------------------------- input history */

static void hist_add(const char *line)
{
    if (S.nhist && !strcmp(S.hist[S.nhist - 1], line)) return;
    if (S.nhist == HIST_MAX) {
        free(S.hist[0]);
        memmove(S.hist, S.hist + 1, sizeof(char *) * (HIST_MAX - 1));
        S.nhist--;
    }
    S.hist[S.nhist++] = xstrdup(line);
}

static void hist_load(void)
{
    char path[1024], line[INPUT_MAX];
    FILE *f = fopen(home_path(".tcalc_history", path, sizeof path), "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *s = str_trim(line);
        if (*s) hist_add(s);
    }
    fclose(f);
}

static void hist_save(void)
{
    char path[1024];
    FILE *f = fopen(home_path(".tcalc_history", path, sizeof path), "w");
    if (!f) return;
    for (int i = 0; i < S.nhist; i++) fprintf(f, "%s\n", S.hist[i]);
    fclose(f);
}

/* ------------------------------------------------------- line editing */

static void snap_push(Snap *st, int *n)
{
    if (*n == UNDO_MAX) {
        memmove(st, st + 1, sizeof(Snap) * (UNDO_MAX - 1));
        (*n)--;
    }
    Snap *p = &st[(*n)++];
    memcpy(p->s, S.buf, (size_t)S.len + 1);
    p->len = S.len;
    p->cur = S.cur;
}

static void snap_load(const Snap *p)
{
    memcpy(S.buf, p->s, (size_t)p->len + 1);
    S.len = p->len;
    S.cur = p->cur;
}

/* start a new undo group unless this edit continues the previous one */
static void edit_begin(int kind)
{
    if (kind != S.last_edit || kind == EK_OTHER) {
        snap_push(undo_st, &nundo);
        nredo = 0;
    }
    S.last_edit = kind;
    S.hbrowse = 0;
    S.dirty = 1;
}

static void undo(void)
{
    if (!nundo) {
        set_msg(0, "nothing to undo");
        return;
    }
    snap_push(redo_st, &nredo);
    snap_load(&undo_st[--nundo]);
    S.last_edit = EK_NONE;
    S.dirty = 1;
}

static void redo(void)
{
    if (!nredo) {
        set_msg(0, "nothing to redo");
        return;
    }
    snap_push(undo_st, &nundo);
    snap_load(&redo_st[--nredo]);
    S.last_edit = EK_NONE;
    S.dirty = 1;
}

static void insert_text(const char *t)
{
    int n = (int)strlen(t);
    if (S.len + n >= INPUT_MAX) n = INPUT_MAX - 1 - S.len;
    if (n <= 0) return;
    memmove(S.buf + S.cur + n, S.buf + S.cur, (size_t)(S.len - S.cur + 1));
    memcpy(S.buf + S.cur, t, (size_t)n);
    S.len += n;
    S.cur += n;
    S.dirty = 1;
}

static void set_line(const char *t)
{
    str_copy(S.buf, sizeof S.buf, t);
    S.len = S.cur = (int)strlen(S.buf);
    S.dirty = 1;
}

static void delete_range(int a, int b)
{
    if (a < 0) a = 0;
    if (b > S.len) b = S.len;
    if (a >= b) return;
    memmove(S.buf + a, S.buf + b, (size_t)(S.len - b + 1));
    S.len -= b - a;
    S.cur = a;
    S.dirty = 1;
}

static int word_left(int p)
{
    while (p > 0 && !is_ident_char((unsigned char)S.buf[p - 1]) && S.buf[p - 1] != '.') p--;
    while (p > 0 && (is_ident_char((unsigned char)S.buf[p - 1]) || S.buf[p - 1] == '.')) p--;
    return p;
}

static int word_right(int p)
{
    while (p < S.len && !is_ident_char((unsigned char)S.buf[p]) && S.buf[p] != '.') p++;
    while (p < S.len && (is_ident_char((unsigned char)S.buf[p]) || S.buf[p] == '.')) p++;
    return p;
}

static void insert_codepoint(uint32_t cp)
{
    char one[2] = {(char)cp, 0};
    const char *t = one;
    if (cp >= 0x80) {
        t = unicode_alias(cp);
        if (!t) {
            set_msg(1, "unsupported character U+%04X", (unsigned)cp);
            return;
        }
        if (!strcmp(t, "deg") && S.cur > 0 &&
            (is_ident_char((unsigned char)S.buf[S.cur - 1]) || S.buf[S.cur - 1] == '.'))
            t = " deg";
    } else if (cp < 0x20) {
        return;
    }
    edit_begin(EK_INSERT);
    /* calculator style: an operator on an empty line continues from ans */
    if (S.len == 0 && t[1] == 0 && strchr("+*/^%", t[0]) && last_value()) insert_text("ans");
    insert_text(t);
}

/* ----------------------------------------------------- highlighting */

static const char *cls_sgr(int c)
{
    switch (c) {
    case C_NUM: return T.num;
    case C_FUNC: return T.func;
    case C_VAR: return T.var;
    case C_CONST: return T.cst;
    case C_KW: return T.kw;
    case C_OP: return T.op;
    case C_PAREN: return T.paren;
    case C_PMATCH: return T.paren_hl;
    case C_ERR: return T.err;
    case C_DIM: return T.dim;
    case C_CMD: return T.func;
    case C_ERRPOS: return T.err_hl;
    default: return T.reset;
    }
}

/* Classify every byte of s; returns the number of unclosed '('.
 * cursor >= 0 highlights the parenthesis pair next to the cursor. */
static int classify(const char *s, int n, unsigned char *cls, int cursor)
{
    static int stack[INPUT_MAX];
    static int match[INPUT_MAX];
    int sp = 0, start = 0;
    memset(cls, C_DEF, (size_t)n);
    for (int i = 0; i < n; i++) match[i] = -1;
    const char *hash = memchr(s, '#', (size_t)n);
    int end = hash ? (int)(hash - s) : n;
    for (int i = end; i < n; i++) cls[i] = C_DIM;

    const char *p = s;
    while (*p == ' ') p++;
    if (*p == ':') {                                   /* command word */
        int i = (int)(p - s);
        cls[i++] = C_CMD;
        while (i < end && s[i] != ' ') cls[i++] = C_CMD;
        start = i;
    }
    char tmp[INPUT_MAX];
    memcpy(tmp, s, (size_t)end);
    tmp[end] = 0;
    Lexer lx;
    lex_init(&lx, tmp, start);
    for (; lx.t.kind != T_EOF; lex_next(&lx)) {
        Token t = lx.t;
        int c = C_OP;
        switch (t.kind) {
        case T_NUM: c = C_NUM; break;
        case T_ERROR: c = C_ERR; break;
        case T_IDENT:
            switch (ident_class(tmp + t.pos, t.len)) {
            case IC_FUNC: c = C_FUNC; break;
            case IC_VAR: c = C_VAR; break;
            case IC_CONST: c = C_CONST; break;
            case IC_KEYWORD: c = C_KW; break;
            default: c = C_DEF;
            }
            break;
        case T_LPAREN:
            c = C_PAREN;
            if (sp < INPUT_MAX) stack[sp++] = t.pos;
            break;
        case T_RPAREN:
            if (sp > 0) {
                int o = stack[--sp];
                match[o] = t.pos;
                match[t.pos] = o;
                c = C_PAREN;
            } else {
                c = C_ERR;
            }
            break;
        default: break;
        }
        for (int i = t.pos; i < t.pos + t.len && i < n; i++) cls[i] = (unsigned char)c;
    }
    if (cursor >= 0) {
        int pos = -1;
        if (cursor > 0 && (s[cursor - 1] == '(' || s[cursor - 1] == ')') && match[cursor - 1] >= 0) pos = cursor - 1;
        else if (cursor < n && (s[cursor] == '(' || s[cursor] == ')') && match[cursor] >= 0) pos = cursor;
        if (pos >= 0) cls[pos] = cls[match[pos]] = C_PMATCH;
    }
    return sp;
}

/* draw s[off..off+len) with the classes in cls */
static void draw_hl(Line *l, const char *s, const unsigned char *cls, int off, int len)
{
    int cur = -1;
    for (int i = off; i < off + len && l->col < l->width; i++) {
        if (cls[i] != cur) {
            cur = cls[i];
            ln_sgr(l, cls_sgr(cur));
        }
        ln_text(l, s + i, 1);
    }
    ln_sgr(l, T.reset);
}

/* ---------------------------------------------------------- layout */

static void value_text(char *out, size_t n, num v)
{
    fmt_number(out, n, v);
}

/* alternative forms of v: 3/4, pi/2, sqrt2/2, 0xFF ... */
static void value_extras(char *out, size_t n, num v)
{
    char a[128];
    out[0] = 0;
    if (fmt_fraction(a, sizeof a, v) || fmt_symbolic(a, sizeof a, v)) {
        strncat(out, a, n - strlen(out) - 1);
    }
    if (v == floor(v) && fabs(v) >= 2 && fabs(v) < 9.2e18) {
        int b1 = g_set.base == 10 ? 16 : 10, b2 = g_set.base == 2 ? 16 : 2;
        if (b1 == 10) {
            int saved = g_set.base;
            g_set.base = 10;
            fmt_number(a, sizeof a, v);
            g_set.base = saved;
        } else {
            fmt_in_base(a, sizeof a, v, b1, 0);
        }
        if (*out) strncat(out, "  ", n - strlen(out) - 1);
        strncat(out, a, n - strlen(out) - 1);
        if (fabs(v) < 65536 && fmt_in_base(a, sizeof a, v, b2, 1)) {
            strncat(out, "  ", n - strlen(out) - 1);
            strncat(out, a, n - strlen(out) - 1);
        }
    }
}

static void entry_result(const Entry *e, char *out, size_t n)
{
    if (e->kind == E_VALUE) {
        char v[256];
        value_text(v, sizeof v, e->val);
        snprintf(out, n, "= %s", v);
    } else if (e->kind == E_FUNC) {
        str_copy(out, n, "defined");
    } else {
        out[0] = 0;
    }
}

/* bytes of s[0..len) that fit in w columns, breaking after a space if possible */
static int wrap_len(const char *s, int len, int w)
{
    int cols = 0, b = 0, space = -1;
    while (b < len && cols < w) {
        if (s[b] == ' ') space = b;
        b += utf8_len((unsigned char)s[b]);
        cols++;
    }
    if (b >= len) return len;
    if (s[b] == ' ') return b + 1;
    return space > 0 ? space + 1 : b;
}

static int unclosed_parens(const char *s)
{
    Lexer lx;
    int depth = 0;
    for (lex_init(&lx, s, 0); lx.t.kind != T_EOF; lex_next(&lx)) {
        if (lx.t.kind == T_LPAREN) depth++;
        else if (lx.t.kind == T_RPAREN && depth > 0) depth--;
    }
    return depth;
}

/* split one entry into visual lines of width w */
static int entry_lines(int idx, int w, VLine *out, int max)
{
    const Entry *e = &S.ent[idx];
    int n = 0, cw = w - 2 > 4 ? w - 2 : 4, il = (int)strlen(e->in);
    char res[300];
    entry_result(e, res, sizeof res);
    int rl = (int)strlen(res);
    int text_inline = (e->kind == E_INFO || e->kind == E_ERROR) && !strchr(e->text, '\n');
    if (text_inline) rl = (int)strlen(e->text) + (e->kind == E_ERROR ? 2 : 0);
    if (il && rl && 1 + il + 2 + rl + 1 <= w && n < max) {
        out[n++] = (VLine){idx, VL_EXPR_RES, 0, il};
        return n;
    }
    for (int off = 0; off < il && n < max; off += cw) out[n++] = (VLine){idx, VL_EXPR, off, il - off < cw ? il - off : cw};
    if (e->kind == E_VALUE || e->kind == E_FUNC) {
        for (int off = 0; off < rl && n < max; off += cw) out[n++] = (VLine){idx, VL_RES, off, rl - off < cw ? rl - off : cw};
    } else {
        const char *t = e->text;
        int tl = (int)strlen(t), tw = e->kind == E_ERROR ? cw - 2 : cw;
        for (int ls = 0; ls < tl && n < max;) {
            int le = ls;
            while (le < tl && t[le] != '\n') le++;
            if (le == ls) out[n++] = (VLine){idx, VL_TEXT, ls, 0};
            for (int off = ls, k; off < le && n < max; off += k) {
                k = wrap_len(t + off, le - off, tw);
                out[n++] = (VLine){idx, VL_TEXT, off, k};
            }
            ls = le + 1;
        }
    }
    return n;
}

static void draw_vline(Line *l, const VLine *v, int w)
{
    const Entry *e = &S.ent[v->ent];
    static unsigned char cls[INPUT_MAX];
    char res[300];
    int il = (int)strlen(e->in);
    if (v->type == VL_EXPR || v->type == VL_EXPR_RES) {
        classify(e->in, il, cls, -1);
        if (e->kind == E_ERROR && e->err_pos >= 0 && e->err_pos < il) cls[e->err_pos] = C_ERRPOS;
        if (e->kind == E_INFO) memset(cls, C_DIM, (size_t)il);
        ln_spaces(l, 1);
        draw_hl(l, e->in, cls, v->off, v->len);
    }
    if (v->type == VL_EXPR_RES) {
        const char *t = res, *sgr = T.result;
        if (e->kind == E_VALUE || e->kind == E_FUNC) {
            entry_result(e, res, sizeof res);
            if (e->kind == E_FUNC) sgr = T.ok;
            else if (isnan(e->val) || isinf(e->val)) sgr = T.err;
        } else {
            snprintf(res, sizeof res, "%s%s", e->kind == E_ERROR ? "! " : "", e->text);
            sgr = e->kind == E_ERROR ? T.err : T.info;
        }
        int tl = (int)strlen(t);
        ln_spaces(l, w - 1 - l->col - tl);
        ln_sgr(l, sgr);
        ln_text(l, t, -1);
        ln_sgr(l, T.reset);
    } else if (v->type == VL_RES) {
        entry_result(e, res, sizeof res);
        int tl = v->len;
        ln_spaces(l, w - 1 - tl);
        ln_sgr(l, e->kind == E_FUNC ? T.ok : (isnan(e->val) || isinf(e->val)) ? T.err : T.result);
        ln_text(l, res + v->off, v->len);
        ln_sgr(l, T.reset);
    } else if (v->type == VL_TEXT) {
        ln_spaces(l, 1);
        if (e->kind == E_ERROR) {
            ln_sgr(l, T.err);
            ln_text(l, v->off == 0 ? "! " : "  ", 2);
        } else {
            ln_sgr(l, T.info);
        }
        ln_text(l, e->text + v->off, v->len);
        ln_sgr(l, T.reset);
    }
}

static void draw_header(Line *l, int w)
{
    char fm[32], right[96];
    const char *ang = angle_name(g_set.angle), *base = base_name(g_set.base);
    fmt_mode_name(fm, sizeof fm);
    snprintf(right, sizeof right, " %s  %s  %s ", ang, base, fm);
    ln_sgr(l, T.bar_hl);
    int title = w >= 34 ? ln_text(l, " tcalc ", -1) : 0;
    ln_sgr(l, T.bar_dim);
    if (S.scroll && w >= 40) {
        char sc[32];
        snprintf(sc, sizeof sc, " %s%d", G.up, S.scroll);
        ln_text(l, sc, -1);
    }
    (void)title;
    int rw = (int)strlen(right);
    ln_spaces(l, w - l->col - rw);
    /* remember where each mode is, so a tap can cycle it */
    int x = l->col + 1;
    S.hdr_x[0][0] = x, S.hdr_x[0][1] = x + (int)strlen(ang);
    x = S.hdr_x[0][1] + 2;
    S.hdr_x[1][0] = x, S.hdr_x[1][1] = x + (int)strlen(base);
    x = S.hdr_x[1][1] + 2;
    S.hdr_x[2][0] = x, S.hdr_x[2][1] = x + (int)strlen(fm);
    ln_sgr(l, T.bar);
    ln_text(l, right, -1);
    ln_pad(l);
    ln_sgr(l, T.reset);
}

static void draw_separator(Line *l, int w)
{
    const char *hints = w >= 64 ? " Tab complete  \xe2\x86\x91\xe2\x86\x93 history  F1 :help  ^D quit "
                      : w >= 44 ? " Tab \xe2\x86\x91\xe2\x86\x93 :help "
                                : "";
    if (g_opt_ascii) hints = w >= 64 ? " Tab complete  Up/Down history  F1 :help  ^D quit " : w >= 44 ? " Tab Up/Dn :help " : "";
    ln_sgr(l, T.border);
    int hl = 0;
    for (const char *p = hints; *p; p += utf8_len((unsigned char)*p)) hl++;
    ln_repeat(l, G.h, w - hl - 2 > 0 ? w - hl - 2 : w);
    ln_sgr(l, T.dim);
    ln_text(l, hints, -1);
    ln_sgr(l, T.border);
    ln_repeat(l, G.h, w - l->col);
    ln_sgr(l, T.reset);
}

static int draw_input(Line *l, int w)
{
    static unsigned char cls[INPUT_MAX];
    int ghost = classify(S.buf, S.len, cls, S.cur);
    if (S.buf[0] == ':' || S.paste) ghost = 0;
    int avail = w - 2;
    if (S.cur - S.hscroll >= avail) S.hscroll = S.cur - avail + 1;
    if (S.cur < S.hscroll) S.hscroll = S.cur;
    if (S.hscroll < 0) S.hscroll = 0;
    ln_sgr(l, T.prompt);
    ln_text(l, S.hscroll ? G.lell : G.prompt, -1);
    ln_text(l, " ", 1);
    draw_hl(l, S.buf, cls, S.hscroll, S.len - S.hscroll);
    if (ghost > 0 && l->col < w) {
        ln_sgr(l, T.ghost);
        for (int i = 0; i < ghost && l->col < w; i++) ln_text(l, ")", 1);
        ln_sgr(l, T.reset);
    }
    ln_pad(l);
    return 2 + S.cur - S.hscroll;
}

static void draw_status(Line *l, int w)
{
    char v[256], ex[256], line[600];
    if (S.msg[0]) {
        ln_sgr(l, S.msg_err ? T.err : T.info);
        ln_text(l, " ", 1);
        ln_text(l, S.msg, -1);
    } else if (S.hint[0]) {
        ln_sgr(l, T.dim);
        ln_text(l, " ", 1);
        ln_text(l, S.hint, -1);
    } else if (S.prev_valid && S.prev.kind == RES_VALUE) {
        value_text(v, sizeof v, S.prev.value);
        value_extras(ex, sizeof ex, S.prev.value);
        ln_sgr(l, T.dim);
        ln_text(l, " = ", -1);
        ln_sgr(l, isnan(S.prev.value) ? T.err : T.result);
        ln_text(l, v, -1);
        if (ex[0] && l->col + 3 + (int)strlen(ex) <= w) {
            ln_sgr(l, T.dim);
            ln_text(l, "   ", 3);
            ln_text(l, ex, -1);
        }
    } else if (S.prev_valid && S.prev.kind == RES_FUNC) {
        ln_sgr(l, T.dim);
        snprintf(line, sizeof line, " define %s", S.prev.msg);
        ln_text(l, line, -1);
    } else if (S.prev_valid && S.prev.kind == RES_ERROR && !S.prev.err_eof) {
        ln_sgr(l, T.err);
        ln_text(l, " ! ", -1);
        ln_text(l, S.prev.msg, -1);
    } else if (S.len == 0) {
        const Entry *e = last_value();
        ln_sgr(l, T.dim);
        if (e && (value_extras(ex, sizeof ex, e->val), ex[0])) {
            snprintf(line, sizeof line, " ans %s %s", G.approx, ex);
            ln_text(l, line, -1);
        } else {
            ln_text(l, w >= 66 ? " type an expression, e.g. 2sin(30)+sqrt(16) ... :help for more"
                               : " e.g. 2sin(30)+sqrt(16)   :help", -1);
        }
    }
    ln_sgr(l, T.reset);
    ln_pad(l);
}

/* right-hand panel on wide screens: variables and functions */
static Buf panel;
static int pl_off[512], pl_len[512], pl_kind[512], npl;

static void panel_add(const char *s, int len, int kind)
{
    if (npl >= 512) return;
    pl_off[npl] = (int)panel.len;
    pl_len[npl] = len;
    pl_kind[npl++] = kind;
    buf_putn(&panel, s, (size_t)len);
}

static void panel_lines(const char *text)
{
    for (const char *p = text; *p;) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        panel_add(p, len, 0);
        p += len + (e ? 1 : 0);
    }
}

static void build_panel(void)
{
    Buf b = {0};
    panel.len = 0;
    npl = 0;
    panel_add(" variables", 10, 1);
    list_vars(&b);
    if (b.len) panel_lines(b.s);
    else panel_add("(none)", 6, 2);
    b.len = 0;
    list_funcs(&b);
    if (b.len) {
        panel_add("", 0, 0);
        panel_add(" functions", 10, 1);
        panel_lines(b.s);
    }
    buf_free(&b);
}

static void draw_panel_row(Line *l, int row, int pw)
{
    ln_sgr(l, T.border);
    ln_text(l, G.v, -1);
    int c0 = l->col, kind = row < npl ? pl_kind[row] : -1;
    if (kind >= 0) {
        ln_sgr(l, kind == 1 ? T.bar_dim : kind == 2 ? T.dim : T.var);
        if (kind != 1) ln_text(l, " ", 1);
        ln_text(l, panel.s + pl_off[row], pl_len[row] < pw - 2 ? pl_len[row] : pw - 2);
        if (kind == 1) ln_spaces(l, pw - 1 - (l->col - c0));
    }
    ln_sgr(l, T.reset);
}

static void render(void)
{
    int w, h;
    term_size(&w, &h);
    if (w > 500) w = 500;
    if (h > 500) h = 500;
    S.w = w;
    S.h = h;
    scr_resize(w, h);
    if (S.dirty && !S.busy_start) {
        S.dirty = 0;
        S.prev_valid = 0;
        const char *s = S.buf;
        while (*s == ' ') s++;
        if (*s && !is_command(s)) {
            S.previewing = 1;                  /* bounded by its budget: no Ctrl+C check */
            calc_eval(S.buf, EV_PREVIEW, &S.prev);
            S.previewing = 0;
            S.prev_valid = 1;
        }
    }
    for (int i = 0; i < h && i < 512; i++) S.row_ent[i] = -1;

    int show_hdr = h >= 6, show_sep = h >= 5, show_status = h >= 3;
    int y = 0;
    if (show_hdr) draw_header(scr_line(y++), w);
    S.pane_top = y;
    S.input_row = h - 1 - show_status;
    S.pane_h = S.input_row - show_sep - y;
    int pw = w >= 100 ? PANEL_W : 0, hw = w - pw;
    if (pw) build_panel();

    /* history pane, bottom aligned */
    static VLine vl[VL_MAX];
    static int bstart[ENTRY_MAX], bcount[ENTRY_MAX];
    int nvl = 0, nb = 0, need = S.pane_h + S.scroll;
    for (int i = S.nent - 1; i >= 0 && nvl < need && nvl < VL_MAX - 64; i--) {
        bstart[nb] = nvl;
        bcount[nb] = entry_lines(i, hw, vl + nvl, VL_MAX - nvl);
        nvl += bcount[nb++];
    }
    if (S.scroll > nvl - S.pane_h) S.scroll = nvl - S.pane_h > 0 ? nvl - S.pane_h : 0;
    /* chronological order: oldest block last in vl */
    static VLine ord[VL_MAX];
    int no = 0;
    for (int b = nb - 1; b >= 0; b--)
        for (int j = 0; j < bcount[b]; j++) ord[no++] = vl[bstart[b] + j];
    int first = no - S.scroll - S.pane_h;
    for (int r = 0; r < S.pane_h; r++) {
        Line *l = scr_line(y + r);
        int k = first + r;
        l->width = hw;
        if (k >= 0 && k < no) {
            draw_vline(l, &ord[k], hw);
            if (y + r < 512) S.row_ent[y + r] = ord[k].ent;
        }
        ln_pad(l);
        l->width = w;
        if (pw) draw_panel_row(l, r, pw);
        ln_pad(l);
    }
    y += S.pane_h;
    if (show_sep) draw_separator(scr_line(y++), w);
    int cx = draw_input(scr_line(S.input_row), w);
    if (show_status) draw_status(scr_line(S.input_row + 1), w);
    scr_present(S.input_row, cx < w ? cx : w - 1, 1);
}

/* ---------------------------------------------------------- completion */

static int cmp_name(const void *a, const void *b) { return strcmp(a, b); }

static void comp_add(const char *name, const char *prefix, int plen)
{
    if (S.comp_n >= 64 || strncmp(name, prefix, (size_t)plen)) return;
    for (int i = 0; i < S.comp_n; i++)
        if (!strcmp(S.comp[i], name)) return;
    str_copy(S.comp[S.comp_n++], NAME_LEN, name);
}

static int is_callable(const char *name)
{
    int len = (int)strlen(name);
    const Builtin *b = builtin_find(name, len);
    if (b) return b->maxargs != 0 && !(b->flags & BF_NOPAREN);
    return func_find(name, len) != NULL;
}

static void build_hint(void)
{
    S.hint[0] = 0;
    for (int i = 0; i < S.comp_n; i++) {
        char item[NAME_LEN + 4];
        if (S.comp_active && i == S.comp_idx) snprintf(item, sizeof item, "[%s] ", S.comp[i]);
        else snprintf(item, sizeof item, "%s ", S.comp[i]);
        if (strlen(S.hint) + strlen(item) + 1 >= sizeof S.hint) break;
        strcat(S.hint, item);
    }
}

static void replace_word(int start, int oldlen, const char *word)
{
    delete_range(start, start + oldlen);
    S.cur = start;
    insert_text(word);
}

static void complete(int dir)
{
    if (S.comp_active && S.comp_n > 0) {
        S.comp_idx = (S.comp_idx + dir + S.comp_n) % S.comp_n;
        replace_word(S.comp_start, S.comp_len, S.comp[S.comp_idx]);
        S.comp_len = (int)strlen(S.comp[S.comp_idx]);
        build_hint();
        return;
    }
    int st = S.cur;
    while (st > 0 && is_ident_char((unsigned char)S.buf[st - 1])) st--;
    int plen = S.cur - st;
    if (plen >= NAME_LEN) return;
    char prefix[NAME_LEN];
    memcpy(prefix, S.buf + st, (size_t)plen);
    prefix[plen] = 0;
    int cmd = st >= 1 && S.buf[st - 1] == ':' && strspn(S.buf, " ") == (size_t)(st - 1);
    S.comp_n = 0;
    if (cmd) {
        int n;
        const char *const *names = command_names(&n);
        for (int i = 0; i < n; i++) comp_add(names[i], prefix, plen);
    } else {
        if (plen == 0) {
            set_msg(0, "type the start of a name, then press Tab");
            return;
        }
        for (int i = 0; i < g_nbuiltins; i++) comp_add(g_builtins[i].name, prefix, plen);
        for (int i = 0; i < g_nvars; i++)
            if (g_vars[i].defined) comp_add(g_vars[i].name, prefix, plen);
        for (int i = 0; i < g_nfuncs; i++)
            if (g_funcs[i].used && g_funcs[i].body) comp_add(g_funcs[i].name, prefix, plen);
    }
    if (S.comp_n == 0) {
        set_msg(1, "no name starts with '%s'", prefix);
        return;
    }
    qsort(S.comp, (size_t)S.comp_n, NAME_LEN, cmp_name);
    edit_begin(EK_OTHER);
    if (S.comp_n == 1) {
        replace_word(st, plen, S.comp[0]);
        if (cmd) insert_text(" ");
        else if (is_callable(S.comp[0]) && S.buf[S.cur] != '(') insert_text("(");
        return;
    }
    int l = (int)strlen(S.comp[0]);
    for (int i = 1; i < S.comp_n; i++) {
        int k = 0;
        while (k < l && S.comp[i][k] == S.comp[0][k]) k++;
        l = k;
    }
    if (l > plen) {
        char lcp[NAME_LEN];
        memcpy(lcp, S.comp[0], (size_t)l);
        lcp[l] = 0;
        replace_word(st, plen, lcp);
        build_hint();
        return;
    }
    S.comp_active = 1;
    S.comp_idx = dir > 0 ? 0 : S.comp_n - 1;
    S.comp_start = st;
    replace_word(st, plen, S.comp[S.comp_idx]);
    S.comp_len = (int)strlen(S.comp[S.comp_idx]);
    build_hint();
}

/* ------------------------------------------------------------ pager */

static void pager(const char *title, const char *text)
{
    static int off[8192], len[8192];
    int top = 0;
    for (;;) {
        int w, h, n = 0;
        term_size(&w, &h);
        scr_resize(w, h);
        int tw = w - 1 > 10 ? w - 1 : 10, body = h - 2 > 1 ? h - 2 : 1;
        for (const char *p = text; *p && n < 8192;) {
            const char *e = strchr(p, '\n');
            int ll = e ? (int)(e - p) : (int)strlen(p);
            int done = 0;
            do {
                int k = wrap_len(p + done, ll - done, tw);
                off[n] = (int)(p - text) + done;
                len[n++] = k;
                done += k;
            } while (done < ll && n < 8192);
            p += ll + (e ? 1 : 0);
            if (!e) break;
        }
        if (top > n - body) top = n - body;
        if (top < 0) top = 0;

        Line *l = scr_line(0);
        ln_sgr(l, T.bar_hl);
        ln_text(l, " ", 1);
        ln_text(l, title, -1);
        ln_sgr(l, T.bar);
        ln_pad(l);
        for (int r = 0; r < body && r + 1 < h; r++) {
            l = scr_line(r + 1);
            int k = top + r;
            if (k < n) {
                const char *s = text + off[k];
                int upper = s[0] >= 'A' && s[0] <= 'Z' && (len[k] < 2 || (s[1] >= 'A' && s[1] <= 'Z'));
                ln_sgr(l, upper ? T.func : T.reset);
                ln_text(l, " ", 1);
                ln_text(l, s, len[k]);
            }
            ln_sgr(l, T.reset);
            ln_pad(l);
        }
        if (h >= 2) {
            char foot[128];
            snprintf(foot, sizeof foot, " %d-%d of %d   %s%s PgUp PgDn scroll   q close",
                     n ? top + 1 : 0, top + body < n ? top + body : n, n, G.up, G.down);
            l = scr_line(h - 1);
            ln_sgr(l, T.bar_dim);
            ln_text(l, foot, -1);
            ln_pad(l);
        }
        scr_present(h - 1, 0, 0);

        Key k;
        if (!term_read_key(&k, -1)) continue;
        switch (k.code) {
        case K_UP: top--; break;
        case K_DOWN: case K_ENTER: top++; break;
        case K_PGUP: top -= body; break;
        case K_PGDN: top += body; break;
        case K_HOME: top = 0; break;
        case K_END: top = n; break;
        case K_RESIZE: scr_invalidate(); break;
        case K_QUIT: S.quit = 1; return;
        case K_ESC: case K_F1: case K_F1 + 9: return;
        case K_MOUSE:
            if (k.mbtn == 64) top -= 3;
            else if (k.mbtn == 65) top += 3;
            break;
        case K_CHAR:
            if (k.ch == 'q' || k.ch == 'Q' || (k.mods & M_CTRL && (k.ch == 'c' || k.ch == 'd'))) return;
            if (k.ch == ' ' || k.ch == 'f') top += body;
            else if (k.ch == 'b') top -= body;
            else if (k.ch == 'j') top++;
            else if (k.ch == 'k') top--;
            else if (k.ch == 'g') top = 0;
            else if (k.ch == 'G') top = n;
            break;
        default: break;
        }
    }
}

static void show_help(void)
{
    Buf b = {0};
    help_text(&b);
    pager("tcalc help", b.s);
    buf_free(&b);
    scr_invalidate();
}

/* ------------------------------------------------------------ actions */

typedef struct {
    const char *cmd;
    int printed;
} PrintCtx;

static void tui_print(void *ctx, int kind, const char *text)
{
    PrintCtx *pc = ctx;
    add_entry(kind == MSG_ERROR ? E_ERROR : E_INFO, pc->printed ? "" : pc->cmd, text, 0, -1);
    pc->printed = 1;
}

static void open_plot(const char *spec)
{
    Plot pl;
    char err[256];
    if (plot_parse(&pl, spec, err, sizeof err) < 0) {
        add_entry(E_ERROR, "", err, 0, -1);
        return;
    }
    plot_interactive(&pl);
    plot_free(&pl);
    scr_invalidate();
}

static void copy_last(void)
{
    const Entry *e = last_value();
    if (!e) {
        set_msg(1, "nothing to copy yet");
        return;
    }
    char v[256];
    int g = g_set.group;
    g_set.group = 0;
    fmt_number(v, sizeof v, e->val);
    g_set.group = g;
    term_copy(v);
    set_msg(0, "copied %s to the clipboard", v);
}

static void run_command(const char *line)
{
    PrintCtx pc = {line, 0};
    CmdIO io = {.print = tui_print, .ctx = &pc, .verbose = 1, .mouse = S.mouse};
    const char *arg = NULL;
    switch (cmd_execute(line, &io, &arg)) {
    case CMD_QUIT: S.quit = 1; break;
    case CMD_HELP: show_help(); break;
    case CMD_PLOT: open_plot(arg); break;
    case CMD_CLEAR: clear_entries(); break;
    case CMD_COPY: copy_last(); break;
    case CMD_CLEARHIST:
        for (int i = 0; i < S.nhist; i++) free(S.hist[i]);
        S.nhist = 0;
        set_msg(0, "input history cleared");
        break;
    case CMD_MOUSE:
        S.mouse = arg && !strcmp(arg, "on");
        term_mouse(S.mouse);
        set_msg(0, "mouse/touch support %s", S.mouse ? "on" : "off");
        break;
    default: break;
    }
    S.dirty = 1;
}

static void submit(void)
{
    char line[INPUT_MAX];
    str_copy(line, sizeof line, S.buf);
    char *s = str_trim(line);
    S.hbrowse = 0;
    if (!*s) return;
    if (!is_command(s) && !strchr(s, '#')) {            /* store what was evaluated */
        size_t l = strlen(s);
        for (int n = unclosed_parens(s); n > 0 && l + 1 < sizeof line - (size_t)(s - line); n--) s[l++] = ')';
        s[l] = 0;
    }
    hist_add(s);
    if (is_command(s)) {
        edit_begin(EK_OTHER);
        set_line("");
        S.hscroll = 0;
        run_command(s);
        return;
    }
    CalcResult r;
    S.busy_start = now_ms();
    S.busy_shown = 0;
    calc_eval(s, 0, &r);
    S.busy_start = 0;
    if (S.busy_shown) S.msg[0] = 0;
    switch (r.kind) {
    case RES_VALUE: add_entry(E_VALUE, s, NULL, r.value, -1); break;
    case RES_FUNC: add_entry(E_FUNC, s, r.msg, 0, -1); break;
    case RES_ERROR: add_entry(E_ERROR, s, r.msg, 0, r.err_pos); break;
    default: break;
    }
    edit_begin(EK_OTHER);
    set_line("");
    S.hscroll = 0;
}

static void cycle_angle(void)
{
    g_set.angle = g_set.angle == ANG_DEG ? ANG_RAD : g_set.angle == ANG_RAD ? ANG_GRAD : ANG_DEG;
    set_msg(0, "angle mode: %s", angle_name(g_set.angle));
    S.dirty = 1;
}

static void cycle_base(void)
{
    g_set.base = g_set.base == 10 ? 16 : g_set.base == 16 ? 2 : g_set.base == 2 ? 8 : 10;
    set_msg(0, "output base: %s", base_name(g_set.base));
    S.dirty = 1;
}

static void cycle_fmt(void)
{
    char fm[32];
    g_set.fmt = (g_set.fmt + 1) % 4;
    set_msg(0, "number format: %s", fmt_mode_name(fm, sizeof fm));
    S.dirty = 1;
}

static void hist_move(int dir)
{
    if (!S.hbrowse) {
        S.hbrowse = 1;
        S.hpos = S.nhist;
        str_copy(S.hsaved, sizeof S.hsaved, S.buf);
        str_copy(S.hprefix, sizeof S.hprefix, S.buf);
    }
    size_t pl = strlen(S.hprefix);
    for (int i = S.hpos + dir; i >= 0 && i < S.nhist; i += dir) {
        if (!strncmp(S.hist[i], S.hprefix, pl) && strcmp(S.hist[i], S.buf)) {
            S.hpos = i;
            set_line(S.hist[i]);
            S.last_edit = EK_NONE;
            return;
        }
    }
    if (dir > 0) {
        S.hpos = S.nhist;
        set_line(S.hsaved);
        S.hbrowse = 0;
    } else if (S.nhist && pl) {
        set_msg(0, "no older entry starting with '%s'", S.hprefix);
    } else if (S.nhist) {
        set_msg(0, "start of history");
    }
}

static void on_mouse(const Key *k)
{
    if (k->mbtn == 64) {
        S.scroll += 3;
        return;
    }
    if (k->mbtn == 65) {
        S.scroll = S.scroll > 3 ? S.scroll - 3 : 0;
        return;
    }
    if (k->mbtn != 0 || !k->mpress) return;
    int x = k->mx, y = k->my;
    if (y == 0 && S.h >= 6) {
        if (x >= S.hdr_x[0][0] - 1 && x <= S.hdr_x[0][1]) cycle_angle();
        else if (x >= S.hdr_x[1][0] - 1 && x <= S.hdr_x[1][1]) cycle_base();
        else if (x >= S.hdr_x[2][0] - 1 && x <= S.hdr_x[2][1]) cycle_fmt();
        return;
    }
    if (y == S.input_row) {
        int c = S.hscroll + x - 2;
        S.cur = c < 0 ? 0 : c > S.len ? S.len : c;
        S.last_edit = EK_NONE;
        return;
    }
    if (y >= 0 && y < 512 && S.row_ent[y] >= 0) {
        const Entry *e = &S.ent[S.row_ent[y]];
        char t[INPUT_MAX];
        edit_begin(EK_OTHER);
        if (e->kind == E_VALUE) {
            fmt_exact(t, sizeof t, e->val);
            insert_text(t);
        } else if (e->in[0]) {
            str_copy(t, sizeof t, e->in);
            set_line(t);
        } else {
            return;
        }
        set_msg(0, "inserted %s", t);
    }
}

static void ctrl_key(uint32_t c)
{
    switch (c) {
    case 'a': S.cur = 0; break;
    case 'e': S.cur = S.len; break;
    case 'b': if (S.cur > 0) S.cur--; break;
    case 'f': if (S.cur < S.len) S.cur++; break;
    case 'k': edit_begin(EK_OTHER); delete_range(S.cur, S.len); break;
    case 'u': edit_begin(EK_OTHER); delete_range(0, S.cur); break;
    case 'w': edit_begin(EK_OTHER); delete_range(word_left(S.cur), S.cur); break;
    case 'd':
        if (S.len == 0) S.quit = 1;
        else if (S.cur < S.len) { edit_begin(EK_DELETE); delete_range(S.cur, S.cur + 1); }
        break;
    case 'c':
        if (S.len) {
            edit_begin(EK_OTHER);
            set_line("");
            S.hscroll = 0;
        } else {
            set_msg(0, "press Ctrl+D (or type :q) to quit");
        }
        break;
    case 'l': clear_entries(); scr_invalidate(); break;
    case 'z': undo(); break;
    case 'y': redo(); break;
    case 'q': S.quit = 1; break;
    case 'p': hist_move(-1); break;
    case 'n': hist_move(1); break;
    case 'o': copy_last(); break;
    case 'g': show_help(); break;
    default: break;
    }
}

static void handle_key(const Key *k)
{
    if (k->code == K_RESIZE) {
        scr_invalidate();
        return;
    }
    if (k->code == K_QUIT) {
        S.quit = 1;
        return;
    }
    if (S.paste) {                                 /* bracketed paste */
        if (k->code == K_PASTE_END) S.paste = 0;
        else if (k->code == K_ENTER) {
            if (S.cur > 0 && S.buf[S.cur - 1] != ';') insert_text("; ");     /* lines -> statements */
        }
        else if (k->code == K_TAB) insert_text(" ");
        else if (k->code == K_CHAR && !k->mods) insert_codepoint(k->ch);
        S.dirty = 1;
        return;
    }
    if (k->code != K_TAB && k->code != K_BTAB) {
        S.comp_active = 0;
        S.hint[0] = 0;
    }
    if (k->code != K_MOUSE) S.msg[0] = 0;
    int moved = 1;
    switch (k->code) {
    case K_PASTE_BEGIN:
        edit_begin(EK_OTHER);
        S.paste = 1;
        break;
    case K_CHAR:
        moved = 0;
        if (k->mods & M_CTRL) ctrl_key(k->ch);
        else if (k->mods & M_ALT) {
            if (k->ch == 'b') S.cur = word_left(S.cur);
            else if (k->ch == 'f') S.cur = word_right(S.cur);
            else if (k->ch == 'd') { edit_begin(EK_OTHER); delete_range(S.cur, word_right(S.cur)); }
        } else if (k->ch == '?' && S.len == 0) {
            show_help();
        } else {
            insert_codepoint(k->ch);
        }
        break;
    case K_ENTER: submit(); break;
    case K_TAB: complete(1); break;
    case K_BTAB: complete(-1); break;
    case K_BACKSPACE:
        if (k->mods & (M_ALT | M_CTRL)) { edit_begin(EK_OTHER); delete_range(word_left(S.cur), S.cur); }
        else if (S.cur > 0) { edit_begin(EK_DELETE); delete_range(S.cur - 1, S.cur); }
        moved = 0;
        break;
    case K_DELETE:
        if (S.cur < S.len) { edit_begin(EK_DELETE); delete_range(S.cur, S.cur + 1); }
        moved = 0;
        break;
    case K_LEFT:
        if (k->mods & (M_CTRL | M_ALT)) S.cur = word_left(S.cur);
        else if (S.cur > 0) S.cur--;
        break;
    case K_RIGHT:
        if (k->mods & (M_CTRL | M_ALT)) S.cur = word_right(S.cur);
        else if (S.cur < S.len) S.cur++;
        break;
    case K_HOME: S.cur = 0; break;
    case K_END: S.cur = S.len; break;
    case K_UP: hist_move(-1); break;
    case K_DOWN: hist_move(1); break;
    case K_PGUP: S.scroll += S.pane_h > 2 ? S.pane_h - 1 : 1; break;
    case K_PGDN: S.scroll -= S.pane_h > 2 ? S.pane_h - 1 : 1; if (S.scroll < 0) S.scroll = 0; break;
    case K_ESC:
        if (S.scroll) S.scroll = 0;
        else if (S.len) { edit_begin(EK_OTHER); set_line(""); S.hscroll = 0; }
        break;
    case K_MOUSE: on_mouse(k); break;
    case K_F1: show_help(); break;
    case K_F1 + 1: cycle_angle(); break;
    case K_F1 + 2: cycle_base(); break;
    case K_F1 + 3: cycle_fmt(); break;
    case K_F1 + 4:
        if (S.len && !is_command(S.buf)) open_plot(S.buf);
        else set_msg(1, "type an expression in x first, e.g. sin(x)");
        break;
    case K_F1 + 9: S.quit = 1; break;
    default: break;
    }
    if (moved) S.last_edit = EK_NONE;
}

/* during long computations: show a notice and watch for Ctrl+C */
static int tui_interrupt(void)
{
    if (S.previewing) return 0;
    if (S.busy_start && !S.busy_shown && now_ms() - S.busy_start > 150) {
        S.busy_shown = 1;
        set_msg(0, "computing...  Ctrl+C cancels");
        render();
    }
    return term_check_interrupt();
}

int tui_run(void)
{
    if (term_open(g_opt_mouse) < 0) return -1;
    theme_init(g_opt_color);
    glyphs_init(g_opt_ascii);
    S.mouse = g_opt_mouse;
    g_interrupt_cb = tui_interrupt;
    hist_load();
    add_entry(E_INFO, "",
              "tcalc " TCALC_VERSION " - advanced calculator\n"
              "try:  2sin(30)+sqrt(16)   f(x)=x^2+1   :plot sin(x)\n"
              "F1 or :help for help   Ctrl+D to quit", 0, -1);
    S.dirty = 1;
    while (!S.quit) {
        if (!term_pending()) render();
        Key k;
        if (term_read_key(&k, -1)) handle_key(&k);
    }
    hist_save();
    g_interrupt_cb = NULL;
    term_close();
    clear_entries();
    for (int i = 0; i < S.nhist; i++) free(S.hist[i]);
    return 0;
}
