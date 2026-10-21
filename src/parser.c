/* parser.c - symbol tables, Pratt parser with constant folding and the
 * top-level statement evaluator */
#include "calc.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

Var g_vars[MAX_VARS];
int g_nvars;
UFunc g_funcs[MAX_FUNCS];
int g_nfuncs;

static Var *v_ans;
static Arena stmt_arena;     /* AST of the statement being evaluated */
static Arena def_arena;      /* body of the function being defined */
static UFunc *pending_func;  /* placeholder created by a failed definition */

/* ------------------------------------------------------------ symbols */

static int name_is(const char *stored, const char *name, int len)
{
    return !memcmp(stored, name, (size_t)len) && stored[len] == 0;
}

Var *var_find(const char *name, int len)
{
    if (len <= 0 || len >= NAME_LEN) return NULL;
    for (int i = 0; i < g_nvars; i++)
        if (g_vars[i].used && name_is(g_vars[i].name, name, len)) return &g_vars[i];
    return NULL;
}

Var *var_create(const char *name, int len)
{
    Var *v = var_find(name, len);
    if (v) return v;
    if (g_nvars >= MAX_VARS || len <= 0 || len >= NAME_LEN) return NULL;
    v = &g_vars[g_nvars++];
    memset(v, 0, sizeof *v);
    memcpy(v->name, name, (size_t)len);
    v->used = 1;
    return v;
}

UFunc *func_find(const char *name, int len)
{
    if (len <= 0 || len >= NAME_LEN) return NULL;
    for (int i = 0; i < g_nfuncs; i++)
        if (g_funcs[i].used && name_is(g_funcs[i].name, name, len)) return &g_funcs[i];
    return NULL;
}

UFunc *func_declare(const char *name, int len, int nparams)
{
    UFunc *f = func_find(name, len);
    if (f) return f;
    if (g_nfuncs >= MAX_FUNCS || len <= 0 || len >= NAME_LEN) return NULL;
    f = &g_funcs[g_nfuncs++];
    memset(f, 0, sizeof *f);
    memcpy(f->name, name, (size_t)len);
    f->nparams = nparams;
    f->used = 1;
    return f;
}

int is_reserved(const char *name, int len)
{
    return (len == 3 && (!memcmp(name, "mod", 3) || !memcmp(name, "xor", 3)));
}

int sym_delete(const char *name)
{
    int len = (int)strlen(name);
    Var *v = var_find(name, len);
    if (v && v->defined) {
        if (v->readonly) return -1;
        v->defined = 0;
        return 1;
    }
    UFunc *f = func_find(name, len);
    if (f && f->body) {
        arena_free(&f->arena);
        free(f->src);
        f->src = NULL;
        f->body = NULL;
        return 2;
    }
    return builtin_find(name, len) ? -1 : 0;
}

void sym_reset_user(void)
{
    for (int i = 0; i < g_nvars; i++)
        if (!g_vars[i].readonly) g_vars[i].defined = 0;
    for (int i = 0; i < g_nfuncs; i++) sym_delete(g_funcs[i].name);
}

void var_set_ans(num v)
{
    v_ans->val = v;
}

int ident_class(const char *name, int len)
{
    if (is_reserved(name, len)) return IC_KEYWORD;
    Var *v = var_find(name, len);
    if (v && v->readonly) return IC_CONST;
    if (v && v->defined) return IC_VAR;
    if (builtin_find(name, len) || func_find(name, len)) return IC_FUNC;
    return IC_UNKNOWN;
}

void calc_init(void)
{
    builtins_init();
    for (int i = 0; i < g_nconsts; i++) {
        Var *v = var_create(g_consts[i].name, (int)strlen(g_consts[i].name));
        v->val = g_consts[i].val;
        v->defined = v->readonly = 1;
    }
    v_ans = var_create("ans", 3);
    v_ans->defined = v_ans->readonly = 1;
}

/* ------------------------------------------------------------- parser */

enum {
    BP_NONE, BP_STMT, BP_OR, BP_AND, BP_EQ, BP_CMP, BP_BOR, BP_BXOR, BP_BAND,
    BP_SHIFT, BP_ADD, BP_MUL, BP_UNARY, BP_POW, BP_POST
};

typedef struct {
    Lexer lx;
    const char *src;
    Arena *arena;
    int depth, nslots, nscope, in_func, preview, autoclosed;
    char scope_name[MAX_LOCALS][NAME_LEN];
    int scope_slot[MAX_LOCALS];
    UFunc *defining;
    int defining_np;
} Parser;

static NORETURN PRINTF_LIKE(3, 4) void perr(Parser *p, int pos, const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    (void)p;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    calc_error(pos, "%s", msg);
}

/* error that may just mean "the user has not finished typing" */
static NORETURN PRINTF_LIKE(3, 4) void perr_inc(Parser *p, int pos, const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (p->lx.t.kind == T_EOF) g_err_eof = 1;
    calc_error(pos, "%s", msg);
}

static NORETURN void unexpected(Parser *p)
{
    Token t = p->lx.t;
    switch (t.kind) {
    case T_EOF: perr_inc(p, t.pos, "incomplete expression");
    case T_NUM: perr(p, t.pos, "missing operator before number");
    case T_RPAREN: perr(p, t.pos, "unmatched ')'");
    case T_ERROR:
        if (p->src[t.pos] >= '0' && p->src[t.pos] <= '9') perr(p, t.pos, "invalid number");
        perr(p, t.pos, "invalid character '%.*s'", t.len, p->src + t.pos);
    case T_ASSIGN: perr(p, t.pos, "cannot assign here (use == to compare)");
    default: perr(p, t.pos, "unexpected '%.*s'", t.len, p->src + t.pos);
    }
}

static Node *new_node(Parser *p, int kind)
{
    Node *n = arena_alloc(p->arena, sizeof *n);
    memset(n, 0, sizeof *n);
    n->kind = (unsigned char)kind;
    return n;
}

static Node *mk_num(Parser *p, num v)
{
    Node *n = new_node(p, N_NUM);
    n->u.val = v;
    return n;
}

static Node *fold(Node *n)
{
    num v = eval(n, NULL);
    n->kind = N_NUM;
    n->u.val = v;
    n->a = n->b = NULL;
    n->args = NULL;
    n->nargs = 0;
    return n;
}

static Node *mk_un(Parser *p, int kind, Node *a)
{
    Node *n = new_node(p, kind);
    n->a = a;
    return a->kind == N_NUM ? fold(n) : n;
}

static Node *mk_bin(Parser *p, int kind, Node *a, Node *b)
{
    Node *n = new_node(p, kind);
    n->a = a;
    n->b = b;
    return (a->kind == N_NUM && b->kind == N_NUM) ? fold(n) : n;
}

static Node **copy_args(Parser *p, Node **args, int n)
{
    Node **v = arena_alloc(p->arena, sizeof(Node *) * (size_t)(n ? n : 1));
    memcpy(v, args, sizeof(Node *) * (size_t)n);
    return v;
}

static int scope_find(Parser *p, const char *name, int len)
{
    for (int i = p->nscope - 1; i >= 0; i--)
        if (name_is(p->scope_name[i], name, len)) return p->scope_slot[i];
    return -1;
}

static void scope_push(Parser *p, const char *name, int len, int slot)
{
    if (p->nscope >= MAX_LOCALS) perr(p, p->lx.t.pos, "too many local variables");
    memcpy(p->scope_name[p->nscope], name, (size_t)len);
    p->scope_name[p->nscope][len] = 0;
    p->scope_slot[p->nscope++] = slot;
}

static int starts_operand(int k)
{
    return k == T_NUM || k == T_IDENT || k == T_MINUS || k == T_PLUS || k == T_BANG || k == T_TILDE;
}

static int is_kw(Parser *p, const Token *t, const char *kw)
{
    return t->kind == T_IDENT && t->len == (int)strlen(kw) && !memcmp(p->src + t->pos, kw, (size_t)t->len);
}

static Node *parse_expr(Parser *p, int minbp);

static void expect_close(Parser *p)
{
    if (p->lx.t.kind == T_RPAREN) {
        lex_next(&p->lx);
        return;
    }
    if (p->lx.t.kind == T_EOF) {                  /* missing ')' are auto-closed */
        p->autoclosed = 1;
        return;
    }
    if (p->lx.t.kind == T_NUM || p->lx.t.kind == T_ERROR || p->lx.t.kind == T_ASSIGN) unexpected(p);
    perr(p, p->lx.t.pos, "expected ')' or ','");
}

static void expect_comma(Parser *p, const Builtin *b)
{
    if (p->lx.t.kind == T_COMMA) {
        lex_next(&p->lx);
        return;
    }
    if (p->lx.t.kind == T_NUM || p->lx.t.kind == T_ERROR) unexpected(p);
    perr_inc(p, p->lx.t.pos, "missing argument: %s", b->sig);
}

/* Parse "(a, b, ...)" after a function name; returns the argument count. */
static int parse_arglist(Parser *p, Node **args)
{
    int n = 0;
    lex_next(&p->lx);                        /* '(' */
    if (p->lx.t.kind == T_RPAREN) {
        lex_next(&p->lx);
        return 0;
    }
    if (p->lx.t.kind == T_EOF) {
        p->autoclosed = 1;
        return 0;
    }
    for (;;) {
        if (n >= MAX_ARGS) perr(p, p->lx.t.pos, "too many arguments");
        args[n++] = parse_expr(p, BP_NONE);
        if (p->lx.t.kind == T_COMMA) {
            lex_next(&p->lx);
            continue;
        }
        expect_close(p);
        return n;
    }
}

static Node *parse_call(Parser *p, const Builtin *b, const Token *t)
{
    Node *args[MAX_ARGS];
    int n = 0;
    if (p->lx.t.kind == T_LPAREN) {
        n = parse_arglist(p, args);
    } else if (b->minargs == 0 && (b->flags & BF_NOPAREN)) {
        n = 0;
    } else if (b->type == FT_1 && starts_operand(p->lx.t.kind)) {
        args[n++] = parse_expr(p, BP_UNARY);           /* sqrt 2, sin x */
    } else {
        perr_inc(p, t->pos, "%s needs arguments: %s", b->name, b->sig);
    }
    if (n < b->minargs || (b->maxargs >= 0 && n > b->maxargs)) {
        if (p->autoclosed && p->lx.t.kind == T_EOF) g_err_eof = 1;
        calc_error(t->pos, "wrong number of arguments for %s", b->sig);
    }

    Node *nd = new_node(p, b->type == FT_1 ? N_CALL1 : b->type == FT_2 ? N_CALL2 : N_CALLN);
    nd->u.fn = b;
    nd->nargs = (unsigned char)n;
    int allconst = !(b->flags & BF_NOFOLD);
    for (int i = 0; i < n; i++)
        if (args[i]->kind != N_NUM) allconst = 0;
    if (b->type == FT_1) nd->a = args[0];
    else if (b->type == FT_2) nd->a = args[0], nd->b = args[1];
    else nd->args = copy_args(p, args, n);
    return allconst ? fold(nd) : nd;
}

static Node *parse_ucall(Parser *p, UFunc *f, const Token *t)
{
    Node *args[MAX_ARGS];
    if (p->lx.t.kind != T_LPAREN) perr(p, t->pos, "%s is a function: use %s(...)", f->name, f->name);
    int n = parse_arglist(p, args);
    int want = (f == p->defining) ? p->defining_np : f->nparams;
    if (n != want) {
        if (p->autoclosed && p->lx.t.kind == T_EOF) g_err_eof = 1;
        calc_error(t->pos, "%s expects %d argument%s, got %d", f->name, want, want == 1 ? "" : "s", n);
    }
    Node *nd = new_node(p, N_UCALL);
    nd->u.uf = f;
    nd->nargs = (unsigned char)n;
    nd->args = copy_args(p, args, n);
    return nd;
}

/* For forms like sigma(expr, k, a, b) the variable name follows the
 * expression; peek ahead to find it before parsing the expression. */
static void find_bound_var(Parser *p, const Builtin *b, const Token *t, char *name)
{
    Lexer lx = p->lx;
    int depth = 0;
    for (;;) {
        int k = lx.t.kind;
        if (k == T_EOF) {
            g_err_eof = 1;
            calc_error(t->pos, "%s: missing variable: %s", b->name, b->sig);
        }
        if (k == T_LPAREN) depth++;
        else if (k == T_RPAREN) {
            if (depth-- == 0) perr(p, t->pos, "%s needs a variable: %s", b->name, b->sig);
        } else if (k == T_COMMA && depth == 0) break;
        lex_next(&lx);
    }
    lex_next(&lx);
    if (lx.t.kind == T_EOF) {
        g_err_eof = 1;
        calc_error(lx.t.pos, "%s: missing variable: %s", b->name, b->sig);
    }
    if (lx.t.kind != T_IDENT || lx.t.len >= NAME_LEN)
        perr(p, lx.t.pos, "%s: argument 2 must be a variable name: %s", b->name, b->sig);
    const char *s = p->src + lx.t.pos;
    if (builtin_find(s, lx.t.len) || func_find(s, lx.t.len) || is_reserved(s, lx.t.len))
        perr(p, lx.t.pos, "'%.*s' cannot be used as a variable", lx.t.len, s);
    memcpy(name, s, (size_t)lx.t.len);
    name[lx.t.len] = 0;
}

static int expect_unit(Parser *p)
{
    Token t = p->lx.t;
    if (t.kind != T_IDENT) {
        if (t.kind == T_EOF) unexpected(p);
        perr(p, t.pos, "expected a unit name (see :units)");
    }
    int u = unit_lookup(p->src + t.pos, t.len);
    lex_next(&p->lx);
    if (u < 0) perr_inc(p, t.pos, "unknown unit '%.*s' (see :units)", t.len, p->src + t.pos);
    return u;
}

static Node *parse_special(Parser *p, const Builtin *b, const Token *t)
{
    if (p->lx.t.kind != T_LPAREN) perr_inc(p, t->pos, "%s needs arguments: %s", b->name, b->sig);
    if (b->special == SF_IF) {
        lex_next(&p->lx);
        Node *c = parse_expr(p, BP_NONE);
        expect_comma(p, b);
        Node *x = parse_expr(p, BP_NONE), *y = NULL;
        if (p->lx.t.kind == T_COMMA) {
            lex_next(&p->lx);
            y = parse_expr(p, BP_NONE);
        }
        expect_close(p);
        if (c->kind == N_NUM) return c->u.val != 0 ? x : (y ? y : mk_num(p, NAN));
        Node *n = new_node(p, N_IF);
        n->args = arena_alloc(p->arena, sizeof(Node *) * 3);
        n->args[0] = c;
        n->args[1] = x;
        n->args[2] = y;
        n->nargs = (unsigned char)(y ? 3 : 2);
        return n;
    }
    if (b->special == SF_CONV) {
        lex_next(&p->lx);
        Node *v = parse_expr(p, BP_NONE);
        expect_comma(p, b);
        int u1 = expect_unit(p);
        expect_comma(p, b);
        int u2 = expect_unit(p);
        expect_close(p);
        const char *e = unit_check(u1, u2);
        if (e) calc_error(t->pos, "%s", e);
        Node *n = new_node(p, N_CONV);
        n->a = v;
        n->u.unit[0] = u1;
        n->u.unit[1] = u2;
        return v->kind == N_NUM ? fold(n) : n;
    }

    /* bound-variable forms */
    char vname[NAME_LEN];
    lex_next(&p->lx);
    find_bound_var(p, b, t, vname);
    if (p->nslots >= MAX_LOCALS) perr(p, t->pos, "too many nested variables");
    int slot = p->nslots++, saved_scope = p->nscope;
    scope_push(p, vname, (int)strlen(vname), slot);
    int solve = b->special == SF_SOLVE;
    Node *body = parse_expr(p, solve ? BP_STMT : BP_NONE);
    if (solve && p->lx.t.kind == T_ASSIGN) {           /* solve(lhs = rhs, x) */
        lex_next(&p->lx);
        body = mk_bin(p, N_SUB, body, parse_expr(p, BP_STMT));
    } else if (solve && body->kind == N_EQ) {
        body->kind = N_SUB;
    }
    p->nscope = saved_scope;
    expect_comma(p, b);
    lex_next(&p->lx);                                  /* variable name */
    Node *rest[3];
    int nr = 0;
    while (p->lx.t.kind == T_COMMA) {
        lex_next(&p->lx);
        if (nr >= 3) perr(p, p->lx.t.pos, "too many arguments: %s", b->sig);
        rest[nr++] = parse_expr(p, BP_NONE);
    }
    if (p->lx.t.kind != T_RPAREN && p->lx.t.kind != T_EOF) unexpected(p);
    expect_close(p);

    int kind = 0, ok = 0;
    switch (b->special) {
    case SF_SUM: kind = N_SUM; ok = nr == 2; break;
    case SF_PROD: kind = N_PROD; ok = nr == 2; break;
    case SF_INTEG: kind = N_INTEG; ok = nr == 2; break;
    case SF_DERIV: kind = N_DERIV; ok = nr == 1 || nr == 2; break;
    case SF_SOLVE: kind = N_SOLVE; ok = nr <= 2; break;
    }
    if (!ok) {
        if (p->lx.t.kind == T_EOF && nr < 2) g_err_eof = 1;
        calc_error(t->pos, "wrong number of arguments for %s", b->sig);
    }
    Node *n = new_node(p, kind);
    n->slot = (unsigned short)slot;
    n->nargs = (unsigned char)(1 + nr);
    n->args = arena_alloc(p->arena, sizeof(Node *) * 4);
    n->args[0] = body;
    for (int i = 0; i < nr; i++) n->args[1 + i] = rest[i];
    return n;
}

static Node *parse_ident(Parser *p, int minbp)
{
    Token t = p->lx.t;
    const char *name = p->src + t.pos;
    int len = t.len;
    if (len >= NAME_LEN) perr(p, t.pos, "name too long");

    if (minbp == BP_NONE) {                          /* assignment? */
        Lexer save = p->lx;
        lex_next(&p->lx);
        int k = p->lx.t.kind;
        if (k == T_ASSIGN || k == T_PLUSEQ || k == T_MINUSEQ || k == T_STAREQ || k == T_SLASHEQ) {
            if (scope_find(p, name, len) >= 0)
                perr(p, t.pos, "cannot assign to parameter '%.*s'", len, name);
            if (func_find(name, len) || is_reserved(name, len))
                perr(p, t.pos, "'%.*s' is a function name", len, name);
            Var *v = var_create(name, len);
            if (!v) perr(p, t.pos, "too many variables");
            if (v->readonly) perr(p, t.pos, "'%.*s' is a constant", len, name);
            lex_next(&p->lx);
            Node *rhs = parse_expr(p, BP_NONE);
            if (k != T_ASSIGN) {
                if (!v->defined && !p->in_func) perr(p, t.pos, "undefined variable '%.*s'", len, name);
                Node *cur = new_node(p, N_VAR);
                cur->u.var = v;
                int op = k == T_PLUSEQ ? N_ADD : k == T_MINUSEQ ? N_SUB : k == T_STAREQ ? N_MUL : N_DIV;
                rhs = mk_bin(p, op, cur, rhs);
            }
            Node *n = new_node(p, N_ASSIGN);
            n->u.var = v;
            n->a = rhs;
            return n;
        }
        p->lx = save;
    }
    lex_next(&p->lx);

    int slot = scope_find(p, name, len);
    if (slot >= 0) {
        Node *n = new_node(p, N_LOCAL);
        n->slot = (unsigned short)slot;
        return n;
    }
    /* name( is a call; a bare name prefers a user variable, so "beta = 2"
     * or "gamma = 0.5" can coexist with the beta() and gamma() functions */
    const Builtin *b = builtin_find(name, len);
    UFunc *f = func_find(name, len);
    Var *v = var_find(name, len);
    int call = p->lx.t.kind == T_LPAREN;
    if (v && v->readonly && v != v_ans) return mk_num(p, v->val);   /* constant */
    if (!call && v && v->defined) {
        Node *n = new_node(p, N_VAR);
        n->u.var = v;
        return n;
    }
    if (b) return b->type == FT_SPECIAL ? parse_special(p, b, &t) : parse_call(p, b, &t);
    if (f) return parse_ucall(p, f, &t);
    if (v && (v->defined || p->in_func)) {
        Node *n = new_node(p, N_VAR);
        n->u.var = v;
        return n;
    }
    if (call) perr(p, t.pos, "unknown function '%.*s'", len, name);
    if (p->in_func && !is_reserved(name, len)) {     /* global defined later */
        v = var_create(name, len);
        if (!v) perr(p, t.pos, "too many variables");
        Node *n = new_node(p, N_VAR);
        n->u.var = v;
        return n;
    }
    if (p->lx.t.kind == T_EOF) g_err_eof = 1;
    calc_error(t.pos, "unknown variable '%.*s'", len, name);
}

static Node *parse_prefix(Parser *p, int minbp)
{
    Token t = p->lx.t;
    switch (t.kind) {
    case T_NUM:
        lex_next(&p->lx);
        return mk_num(p, t.val);
    case T_LPAREN: {
        lex_next(&p->lx);
        if (p->lx.t.kind == T_RPAREN) perr(p, t.pos, "empty parentheses");
        Node *e = parse_expr(p, BP_NONE);
        expect_close(p);
        return e;
    }
    case T_MINUS:
        lex_next(&p->lx);
        return mk_un(p, N_NEG, parse_expr(p, BP_UNARY));
    case T_PLUS:
        lex_next(&p->lx);
        return parse_expr(p, BP_UNARY);
    case T_BANG:
        lex_next(&p->lx);
        return mk_un(p, N_NOT, parse_expr(p, BP_UNARY));
    case T_TILDE:
        lex_next(&p->lx);
        return mk_un(p, N_BNOT, parse_expr(p, BP_UNARY));
    case T_IDENT:
        return parse_ident(p, minbp);
    default:
        unexpected(p);
    }
}

static Node *parse_expr(Parser *p, int minbp)
{
    if (++p->depth > 250) perr(p, p->lx.t.pos, "expression too deeply nested");
    Node *lhs = parse_prefix(p, minbp);
    for (;;) {
        Token t = p->lx.t;
        int kind, lbp, implicit = 0, right = 0;
        switch (t.kind) {
        case T_OROR: kind = N_OR; lbp = BP_OR; break;
        case T_ANDAND: kind = N_AND; lbp = BP_AND; break;
        case T_EQ: kind = N_EQ; lbp = BP_EQ; break;
        case T_NE: kind = N_NE; lbp = BP_EQ; break;
        case T_LT: kind = N_LT; lbp = BP_CMP; break;
        case T_LE: kind = N_LE; lbp = BP_CMP; break;
        case T_GT: kind = N_GT; lbp = BP_CMP; break;
        case T_GE: kind = N_GE; lbp = BP_CMP; break;
        case T_PIPE: kind = N_BOR; lbp = BP_BOR; break;
        case T_AMP: kind = N_BAND; lbp = BP_BAND; break;
        case T_SHL: kind = N_SHL; lbp = BP_SHIFT; break;
        case T_SHR: kind = N_SHR; lbp = BP_SHIFT; break;
        case T_PLUS: kind = N_ADD; lbp = BP_ADD; break;
        case T_MINUS: kind = N_SUB; lbp = BP_ADD; break;
        case T_STAR: kind = N_MUL; lbp = BP_MUL; break;
        case T_SLASH: kind = N_DIV; lbp = BP_MUL; break;
        case T_PERCENT: kind = N_MOD; lbp = BP_MUL; break;
        case T_CARET: kind = N_POW; lbp = BP_POW; right = 1; break;
        case T_BANG:
            if (BP_POST <= minbp) goto done;
            lex_next(&p->lx);
            lhs = mk_un(p, N_FACT, lhs);
            continue;
        case T_IDENT:
            if (is_kw(p, &t, "mod")) kind = N_MOD, lbp = BP_MUL;
            else if (is_kw(p, &t, "xor")) kind = N_BXOR, lbp = BP_BXOR;
            else kind = N_MUL, lbp = BP_MUL, implicit = 1;     /* 2x, 3 pi */
            break;
        case T_LPAREN:
            kind = N_MUL; lbp = BP_MUL; implicit = 1;          /* 2(3+4) */
            break;
        default:
            goto done;
        }
        if (lbp <= minbp) break;
        if (!implicit) lex_next(&p->lx);
        Node *rhs = parse_expr(p, right ? lbp - 1 : lbp);
        lhs = mk_bin(p, kind, lhs, rhs);
    }
done:
    p->depth--;
    return lhs;
}

/* name(p1, p2, ...) = body */
static int try_funcdef(Parser *p, CalcResult *res)
{
    Lexer save = p->lx;
    Token nt = p->lx.t;
    char params[MAX_PARAMS][NAME_LEN];
    int np = 0;
    lex_next(&p->lx);
    if (p->lx.t.kind != T_LPAREN) goto no;
    lex_next(&p->lx);
    if (p->lx.t.kind != T_RPAREN) {
        for (;;) {
            if (p->lx.t.kind != T_IDENT || np >= MAX_PARAMS || p->lx.t.len >= NAME_LEN) goto no;
            memcpy(params[np], p->src + p->lx.t.pos, (size_t)p->lx.t.len);
            params[np++][p->lx.t.len] = 0;
            lex_next(&p->lx);
            if (p->lx.t.kind == T_COMMA) {
                lex_next(&p->lx);
                continue;
            }
            if (p->lx.t.kind == T_RPAREN) break;
            goto no;
        }
    }
    lex_next(&p->lx);
    if (p->lx.t.kind != T_ASSIGN) goto no;
    lex_next(&p->lx);

    const char *name = p->src + nt.pos;
    int len = nt.len;
    if (len >= NAME_LEN) perr(p, nt.pos, "name too long");
    if (builtin_find(name, len) || is_reserved(name, len))
        perr(p, nt.pos, "'%.*s' is a built-in function", len, name);
    Var *v = var_find(name, len);
    if (v && (v->defined || v->readonly)) perr(p, nt.pos, "'%.*s' is a variable", len, name);
    for (int i = 0; i < np; i++) {
        int pl = (int)strlen(params[i]);
        if (builtin_find(params[i], pl) || is_reserved(params[i], pl) || func_find(params[i], pl))
            perr(p, nt.pos, "'%s' cannot be a parameter name", params[i]);
        for (int j = 0; j < i; j++)
            if (!strcmp(params[i], params[j])) perr(p, nt.pos, "duplicate parameter '%s'", params[i]);
    }

    UFunc *f = func_find(name, len);
    if (!f) {
        f = func_declare(name, len, np);
        if (!f) perr(p, nt.pos, "too many functions");
        if (!p->preview) pending_func = f;
    }
    if (!p->preview) {
        arena_reset(&def_arena);
        p->arena = &def_arena;
    }
    p->nscope = 0;
    for (int i = 0; i < np; i++) scope_push(p, params[i], (int)strlen(params[i]), i);
    p->nslots = np;
    p->in_func = 1;
    p->defining = f;
    p->defining_np = np;
    int body_pos = p->lx.t.pos;
    Node *body = parse_expr(p, BP_NONE);
    if (p->lx.t.kind != T_SEMI && p->lx.t.kind != T_EOF) unexpected(p);
    char *src = xstrndup(p->src + body_pos, (size_t)(p->lx.t.pos - body_pos));
    str_trim(src);

    char plist[MAX_PARAMS * NAME_LEN + 16] = "";
    for (int i = 0; i < np; i++) {
        if (i) strcat(plist, ", ");
        strcat(plist, params[i]);
    }
    snprintf(res->msg, sizeof res->msg, "%s(%.96s) = %.110s", f->name, plist, src);
    res->kind = RES_FUNC;
    if (p->preview) {
        free(src);
    } else {
        arena_free(&f->arena);
        f->arena = def_arena;
        def_arena.head = NULL;
        p->arena = &stmt_arena;
        free(f->src);
        f->src = src;
        f->body = body;
        f->nparams = np;
        f->nslots = p->nslots;
        memcpy(f->params, params, sizeof params);
        pending_func = NULL;
    }
    p->defining = NULL;
    p->in_func = 0;
    return 1;
no:
    p->lx = save;
    return 0;
}

static void parse_statement(Parser *p, CalcResult *res)
{
    p->nslots = 0;
    p->nscope = 0;
    p->in_func = 0;
    p->depth = 0;
    if (p->lx.t.kind == T_IDENT && try_funcdef(p, res)) return;
    Node *e = parse_expr(p, BP_NONE);
    if (p->lx.t.kind != T_SEMI && p->lx.t.kind != T_EOF) unexpected(p);
    g_sp = p->nslots;
    g_depth = 0;
    num v = eval(e, g_vstack);
    g_sp = 0;
    res->kind = RES_VALUE;
    res->value = v;
    res->assigned = e->kind == N_ASSIGN;
    var_set_ans(v);
}

int calc_eval(const char *src, int flags, CalcResult *res)
{
    static Var snap[MAX_VARS];
    int preview = flags & EV_PREVIEW;
    int snap_vars = g_nvars, snap_funcs = g_nfuncs;
    long long saved_budget = g_budget;
    RngState rs;

    memset(res, 0, sizeof *res);
    res->err_pos = -1;
    arena_reset(&stmt_arena);
    if (preview) {
        memcpy(snap, g_vars, sizeof(Var) * (size_t)snap_vars);
        rng_save(&rs);
        g_budget = 400000;
    }
    pending_func = NULL;

    Parser p;
    memset(&p, 0, sizeof p);
    p.src = src;
    p.arena = &stmt_arena;
    p.preview = preview;

    ErrCtx ec;
    err_push(&ec);
    if (setjmp(ec.jb)) {
        err_pop(&ec);
        res->kind = RES_ERROR;
        str_copy(res->msg, sizeof res->msg, g_err_msg);
        res->err_pos = g_err_pos;
        res->err_eof = g_err_eof;
        g_sp = 0;
        g_depth = 0;
        if (pending_func && !pending_func->body && pending_func == &g_funcs[g_nfuncs - 1]) {
            pending_func->used = 0;
            g_nfuncs--;
        }
        pending_func = NULL;
        goto out;
    }
    lex_init(&p.lx, src, 0);
    while (p.lx.t.kind != T_EOF) {
        if (p.lx.t.kind == T_SEMI) {
            lex_next(&p.lx);
            continue;
        }
        parse_statement(&p, res);
    }
    err_pop(&ec);
out:
    if (preview) {
        g_nvars = snap_vars;
        memcpy(g_vars, snap, sizeof(Var) * (size_t)snap_vars);
        g_nfuncs = snap_funcs;
        rng_load(&rs);
    }
    g_budget = saved_budget;
    return res->kind;
}

Node *calc_parse_expr(const char *src, Arena *arena, const char *const *locals, int nlocals,
                      int *nslots, CalcResult *res)
{
    static Parser p;            /* static: stays valid across longjmp */
    memset(&p, 0, sizeof p);
    memset(res, 0, sizeof *res);
    res->err_pos = -1;
    p.src = src;
    p.arena = arena;
    for (int i = 0; i < nlocals; i++) {
        memcpy(p.scope_name[i], locals[i], strlen(locals[i]) + 1);
        p.scope_slot[i] = i;
    }
    p.nscope = p.nslots = nlocals;

    ErrCtx ec;
    err_push(&ec);
    if (setjmp(ec.jb)) {
        err_pop(&ec);
        res->kind = RES_ERROR;
        str_copy(res->msg, sizeof res->msg, g_err_msg);
        res->err_pos = g_err_pos;
        return NULL;
    }
    lex_init(&p.lx, src, 0);
    Node *n = parse_expr(&p, BP_STMT);
    if (p.lx.t.kind != T_EOF) unexpected(&p);
    err_pop(&ec);
    *nslots = p.nslots;
    res->kind = RES_VALUE;
    return n;
}
