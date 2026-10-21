/*
 * tcalc - advanced terminal calculator for Termux (and any POSIX terminal)
 * calc.h - shared declarations
 */
#ifndef TCALC_CALC_H
#define TCALC_CALC_H

/* POSIX + XSI APIs (sigaction flags, strtok_r, clock_gettime...) so that a
 * plain compiler call without -D flags works on glibc, musl, bionic (Termux)
 * and macOS */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define TCALC_VERSION "1.0.0"

typedef double num;

#if defined(__GNUC__) || defined(__clang__)
#define NORETURN __attribute__((noreturn))
#define PRINTF_LIKE(a, b) __attribute__((format(printf, a, b)))
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define NORETURN
#define PRINTF_LIKE(a, b)
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

#define C_PI    3.14159265358979323846
#define C_E     2.71828182845904523536
#define C_PHI   1.61803398874989484820
#define C_SQRT2 1.41421356237309504880

#define NAME_LEN    32      /* max identifier length incl. NUL */
#define MAX_VARS    1024
#define MAX_FUNCS   256
#define MAX_PARAMS  8
#define MAX_LOCALS  32      /* params + bound variables per frame */
#define MAX_ARGS    128     /* arguments in one call */
#define MAX_DEPTH   1000    /* user function recursion depth */
#define VSTACK_SIZE 65536   /* value stack (frames + call arguments) */

/* ------------------------------------------------------------ util.c */
typedef struct ArenaBlock ArenaBlock;
typedef struct { ArenaBlock *head; } Arena;
void *arena_alloc(Arena *a, size_t n);
void arena_reset(Arena *a);
void arena_free(Arena *a);

typedef struct { char *s; size_t len, cap; } Buf;
void buf_reserve(Buf *b, size_t extra);
void buf_putn(Buf *b, const char *s, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_putc(Buf *b, char c);
void buf_printf(Buf *b, const char *fmt, ...) PRINTF_LIKE(2, 3);
void buf_free(Buf *b);

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void str_copy(char *dst, size_t n, const char *src);
int is_ident_start(int c);
int is_ident_char(int c);
char *str_trim(char *s);

typedef struct { uint64_t s[4]; } RngState;
void rng_seed(uint64_t seed);
double rng_uniform(void);              /* [0, 1) */
void rng_save(RngState *st);
void rng_load(const RngState *st);

/* ----------------------------------------------------------- lexer.c */
typedef enum {
    T_EOF, T_NUM, T_IDENT, T_ERROR,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_CARET,
    T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK, T_COMMA, T_SEMI,
    T_ASSIGN, T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE, T_ANDAND, T_OROR,
    T_BANG, T_AMP, T_PIPE, T_TILDE, T_SHL, T_SHR
} TokKind;

typedef struct { int kind, pos, len; num val; } Token;
typedef struct { const char *src; int pos; Token t; } Lexer;

void lex_init(Lexer *lx, const char *src, int start);
void lex_next(Lexer *lx);
int utf8_decode(const char *s, uint32_t *cp);           /* bytes used */
const char *unicode_alias(uint32_t cp);                 /* ASCII alias or NULL */
size_t normalize_input(const char *in, char *out, size_t outsz);

/* ------------------------------------------------------------ AST */
typedef struct Node Node;
typedef struct Builtin Builtin;

typedef struct Var {
    char name[NAME_LEN];
    num val;
    unsigned char used, defined, readonly;
} Var;

typedef struct UFunc {
    char name[NAME_LEN];
    char params[MAX_PARAMS][NAME_LEN];
    int nparams, nslots;
    Node *body;          /* NULL = declared but not defined yet */
    char *src;           /* body source text */
    Arena arena;         /* owns body */
    unsigned char used;
} UFunc;

typedef enum {
    N_NUM, N_VAR, N_LOCAL, N_ASSIGN,
    N_NEG, N_NOT, N_BNOT, N_FACT,
    N_ADD, N_SUB, N_MUL, N_DIV, N_MOD, N_POW,
    N_EQ, N_NE, N_LT, N_LE, N_GT, N_GE, N_AND, N_OR,
    N_BAND, N_BOR, N_BXOR, N_SHL, N_SHR,
    N_CALL1, N_CALL2, N_CALLN, N_UCALL,
    N_IF, N_SUM, N_PROD, N_INTEG, N_DERIV, N_SOLVE, N_CONV
} NodeKind;

struct Node {
    unsigned char kind, nargs;
    unsigned short slot;          /* local slot / bound variable */
    union {
        num val;
        Var *var;
        const Builtin *fn;
        UFunc *uf;
        int unit[2];
    } u;
    Node *a, *b;
    Node **args;
};

/* --------------------------------------------------------- builtins.c */
typedef num (*Fn1)(num);
typedef num (*Fn2)(num, num);
typedef num (*FnN)(int, const num *);

enum { FT_1 = 1, FT_2, FT_N, FT_SPECIAL };
enum { BF_NOFOLD = 1, BF_NOPAREN = 2 };
enum { SF_IF = 1, SF_SUM, SF_PROD, SF_INTEG, SF_DERIV, SF_SOLVE, SF_CONV };
enum {
    CAT_TRIG, CAT_HYP, CAT_EXP, CAT_ROUND, CAT_NUMT, CAT_STAT, CAT_PROB,
    CAT_CALC, CAT_PROG, CAT_MISC, CAT_COUNT
};

struct Builtin {
    const char *name;
    signed char minargs, maxargs;     /* maxargs < 0: variadic */
    unsigned char type, flags, cat, special;
    Fn1 f1; Fn2 f2; FnN fn;
    const char *sig, *help;
};

extern const Builtin g_builtins[];
extern const int g_nbuiltins;
extern const char *const g_cat_names[CAT_COUNT];
void builtins_init(void);
const Builtin *builtin_find(const char *name, int len);

typedef struct { const char *name; num val; const char *help; } ConstDef;
extern const ConstDef g_consts[];
extern const int g_nconsts;

num fn_fact(num n);
num fn_pow(num a, num b);
num fmod_floor(num a, num b);
int is_prime_u64(uint64_t n);
int factorize(uint64_t n, uint64_t *f, int *e, int max);   /* returns count */
int64_t to_int64(num x, const char *what);

/* ------------------------------------------------------- parser.c */
extern Var g_vars[MAX_VARS];
extern int g_nvars;
extern UFunc g_funcs[MAX_FUNCS];
extern int g_nfuncs;

Var *var_find(const char *name, int len);
Var *var_create(const char *name, int len);
UFunc *func_find(const char *name, int len);
UFunc *func_declare(const char *name, int len, int nparams);
int is_reserved(const char *name, int len);
int sym_delete(const char *name);
void sym_reset_user(void);
void var_set_ans(num v);

enum { EV_PREVIEW = 1 };
enum { RES_EMPTY, RES_VALUE, RES_FUNC, RES_ERROR };

typedef struct {
    int kind;
    num value;
    int assigned;        /* last statement was an assignment */
    int err_pos;         /* byte offset of error or -1 */
    int err_eof;         /* error caused by incomplete input */
    char msg[256];       /* error text / definition text */
} CalcResult;

void calc_init(void);
int calc_eval(const char *src, int flags, CalcResult *res);
/* Parse an expression in which `locals` are bound to slots 0..n-1. */
Node *calc_parse_expr(const char *src, Arena *arena, const char *const *locals,
                      int nlocals, int *nslots, CalcResult *res);
int ident_class(const char *name, int len);   /* for syntax highlighting */
enum { IC_UNKNOWN, IC_FUNC, IC_VAR, IC_CONST, IC_KEYWORD };

/* --------------------------------------------------------- eval.c */
extern num g_vstack[VSTACK_SIZE];
extern int g_sp, g_depth;
extern long long g_budget;
extern unsigned g_tick;
extern int (*g_interrupt_cb)(void);

/* Error trampoline shared by parser and evaluator:
 *   ErrCtx ec; err_push(&ec);
 *   if (setjmp(ec.jb)) { err_pop(&ec); ...g_err_msg... }
 *   ...; err_pop(&ec);
 * Error details live in globals so they stay valid after longjmp. */
typedef struct ErrCtx {
    jmp_buf jb;
    struct ErrCtx *prev;
} ErrCtx;
extern ErrCtx *g_errctx;
extern char g_err_msg[256];
extern int g_err_pos, g_err_eof;
void err_push(ErrCtx *ctx);
void err_pop(ErrCtx *ctx);

NORETURN void calc_error(int pos, const char *fmt, ...) PRINTF_LIKE(2, 3);
void calc_tick_slow(void);
#define TICK() do { if (UNLIKELY((++g_tick & 0x3FFu) == 0)) calc_tick_slow(); } while (0)

num eval(const Node *n, num *fr);
/* Evaluate with error trapping; returns 0 on error (msg filled if non-NULL). */
int eval_safe(const Node *n, num *fr, num *out, char *msg, size_t msgsz);

/* -------------------------------------------------------- format.c */
enum { ANG_RAD, ANG_DEG, ANG_GRAD };
enum { FMT_AUTO, FMT_FIX, FMT_SCI, FMT_ENG };

typedef struct {
    int angle, base, fmt, digits, prec, group;
} Settings;
extern Settings g_set;

num ang_to_rad(num x);
num rad_to_ang(num x);
void fmt_number(char *buf, size_t n, num x);
void fmt_exact(char *buf, size_t n, num x);
int fmt_in_base(char *buf, size_t n, num x, int base, int group);
int fmt_fraction(char *buf, size_t n, num x);
int fmt_symbolic(char *buf, size_t n, num x);
const char *angle_name(int a);
const char *base_name(int b);
const char *fmt_mode_name(char *buf, size_t n);

/* --------------------------------------------------------- units.c */
int unit_lookup(const char *name, int len);
const char *unit_check(int from, int to);     /* NULL if compatible */
num unit_convert(num v, int from, int to);
void units_help(Buf *out);

/* ------------------------------------------------------ commands.c */
enum {
    CMD_NONE, CMD_OK, CMD_QUIT, CMD_HELP, CMD_PLOT, CMD_CLEAR,
    CMD_COPY, CMD_MOUSE, CMD_ERROR, CMD_CLEARHIST
};
enum { MSG_INFO, MSG_ERROR };

typedef struct {
    void (*print)(void *ctx, int kind, const char *text);
    void *ctx;
    int verbose;   /* report setting changes (interactive use) */
    int mouse;     /* current mouse state (TUI) */
} CmdIO;

int is_command(const char *line);
int cmd_execute(const char *line, CmdIO *io, const char **arg);
void help_text(Buf *out);
void list_vars(Buf *out);
void list_funcs(Buf *out);
void list_consts(Buf *out);
void factor_text(Buf *out, num x);
void bases_text(Buf *out, num x);
const char *const *command_names(int *count);

const char *home_path(const char *file, char *buf, size_t n);
int state_load(const char *path);
int state_save(const char *path);
int script_run(const char *path, CmdIO *io);

/* ------------------------------------------------------------ plot.c */
#define PLOT_MAX_FN 6
typedef struct {
    int nfn;
    Node *fn[PLOT_MAX_FN];
    int nslots[PLOT_MAX_FN];
    char label[PLOT_MAX_FN][64];
    double xmin, xmax, ymin, ymax;
    int has_xrange, has_yrange;
    Arena arena;
} Plot;

int plot_parse(Plot *pl, const char *spec, char *err, size_t errsz);
void plot_free(Plot *pl);
void plot_autoscale_y(Plot *pl, int samples);
void plot_print(Plot *pl, FILE *f, int width, int height, int color, int ascii);
void plot_interactive(Plot *pl);

/* ------------------------------------------------------------ tui.c */
int tui_run(void);
extern int g_opt_ascii, g_opt_color, g_opt_mouse;

#endif
