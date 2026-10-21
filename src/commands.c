/* commands.c - ':' commands, help text, listings and state persistence */
#include "calc.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *const cmd_names[] = {
    "help", "quit", "exit", "deg", "rad", "grad", "dec", "hex", "bin", "oct", "auto",
    "fix", "sci", "eng", "prec", "group", "vars", "funcs", "consts", "units", "del",
    "reset", "clear", "clearhist", "plot", "factor", "base", "copy", "mouse", "save",
    "version",
};

const char *const *command_names(int *count)
{
    *count = (int)(sizeof cmd_names / sizeof cmd_names[0]);
    return cmd_names;
}

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

int is_command(const char *line)
{
    line = skip_ws(line);
    if (*line == ':') return 1;
    char w[16];
    size_t n = strcspn(line, " \t");
    if (n == 0 || n >= sizeof w || *skip_ws(line + n)) return 0;
    memcpy(w, line, n);
    w[n] = 0;
    return !strcmp(w, "help") || !strcmp(w, "quit") || !strcmp(w, "exit") || !strcmp(w, "?");
}

static void say(CmdIO *io, int kind, const char *fmt, ...) PRINTF_LIKE(3, 4);

static void say(CmdIO *io, int kind, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (io && io->print) io->print(io->ctx, kind, msg);
}

/* confirmation of a setting change: only shown interactively */
static void note(CmdIO *io, const char *fmt, ...) PRINTF_LIKE(2, 3);

static void note(CmdIO *io, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    if (!io || !io->verbose || !io->print) return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    io->print(io->ctx, MSG_INFO, msg);
}

static void say_buf(CmdIO *io, Buf *b)
{
    while (b->len && b->s[b->len - 1] == '\n') b->s[--b->len] = 0;
    if (io && io->print) io->print(io->ctx, MSG_INFO, b->len ? b->s : "(none)");
    buf_free(b);
}

/* evaluate the argument of :factor / :base */
static int arg_value(const char *arg, CmdIO *io, num *out)
{
    CalcResult r;
    if (!*arg) {
        say(io, MSG_ERROR, "missing number");
        return 0;
    }
    calc_eval(arg, 0, &r);
    if (r.kind != RES_VALUE) {
        say(io, MSG_ERROR, "%s", r.kind == RES_ERROR ? r.msg : "not a value");
        return 0;
    }
    *out = r.value;
    return 1;
}

static int parse_onoff(const char *arg, int cur)
{
    if (!strcasecmp(arg, "on") || !strcmp(arg, "1")) return 1;
    if (!strcasecmp(arg, "off") || !strcmp(arg, "0")) return 0;
    return !cur;
}

void list_vars(Buf *out)
{
    char v[128];
    for (int i = 0; i < g_nvars; i++) {
        Var *x = &g_vars[i];
        if (!x->defined || x->readonly) continue;
        fmt_number(v, sizeof v, x->val);
        buf_printf(out, "%s = %s\n", x->name, v);
    }
}

void list_funcs(Buf *out)
{
    for (int i = 0; i < g_nfuncs; i++) {
        UFunc *f = &g_funcs[i];
        if (!f->used || !f->body) continue;
        buf_printf(out, "%s(", f->name);
        for (int j = 0; j < f->nparams; j++) buf_printf(out, "%s%s", j ? ", " : "", f->params[j]);
        buf_printf(out, ") = %s\n", f->src);
    }
}

void list_consts(Buf *out)
{
    char v[64];
    for (int i = 0; i < g_nconsts; i++) {
        fmt_exact(v, sizeof v, g_consts[i].val);
        buf_printf(out, "%-4s = %-18s %s\n", g_consts[i].name, v, g_consts[i].help);
    }
}

void factor_text(Buf *out, num x)
{
    if (x != floor(x) || fabs(x) < 2 || fabs(x) > 9007199254740992.0) {
        buf_puts(out, "factor: need an integer with 2 <= |n| <= 2^53");
        return;
    }
    uint64_t n = (uint64_t)fabs(x), f[64];
    int e[64], k = factorize(n, f, e, 64);
    const char *times = g_opt_ascii ? " * " : " \xc3\x97 ";
    buf_printf(out, "%s%llu = ", x < 0 ? "-" : "", (unsigned long long)n);
    if (x < 0) buf_puts(out, "-1");
    for (int i = 0; i < k; i++) {
        if (i || x < 0) buf_puts(out, times);
        buf_printf(out, "%llu", (unsigned long long)f[i]);
        if (e[i] > 1) buf_printf(out, "^%d", e[i]);
    }
    if (k == 1 && e[0] == 1 && x > 0) buf_puts(out, "  (prime)");
}

void bases_text(Buf *out, num x)
{
    char b[128];
    if (!fmt_in_base(b, sizeof b, x, 10, 0)) {
        buf_puts(out, "base: need an integer (|n| < 2^63)");
        return;
    }
    int saved = g_set.base;
    g_set.base = 10;
    fmt_number(b, sizeof b, x);
    g_set.base = saved;
    buf_printf(out, "DEC %s\n", b);
    fmt_in_base(b, sizeof b, x, 16, 1);
    buf_printf(out, "HEX %s\n", b);
    fmt_in_base(b, sizeof b, x, 8, 1);
    buf_printf(out, "OCT %s\n", b);
    fmt_in_base(b, sizeof b, x, 2, 1);
    buf_printf(out, "BIN %s", b);
}

void help_text(Buf *out)
{
    buf_puts(out,
        "tcalc " TCALC_VERSION " - advanced terminal calculator\n"
        "\n"
        "BASICS\n"
        "  2+3*4    (1+2)(3+4)    2pi    3!    2^10    7 mod 3    -7 % 3\n"
        "  Implicit multiplication: 2x  3(4+5)  2 sin(30)  pi r^2\n"
        "  Numbers: 1.5e3  .5  0xFF  0b1010  0o17  1_000_000  (Persian digits ok)\n"
        "  Missing ')' at the end are closed for you: sqrt(2  ->  sqrt(2)\n"
        "  ans = last result.  Several statements: a = 2; b = 3; a*b\n"
        "  Phone-keyboard symbols work too: \xc3\x97 \xc3\xb7 \xe2\x88\x92 \xcf\x80 \xe2\x88\x9a \xc2\xb2 \xc2\xb3 \xc2\xb0\n"
        "\n"
        "OPERATORS (lowest to highest precedence)\n"
        "  =  +=  -=  *=  /=        assignment\n"
        "  ||   &&                  logical or / and (result 1 or 0)\n"
        "  ==  !=  <  <=  >  >=     comparison (== is tolerant: 0.1+0.2 == 0.3)\n"
        "  |   xor   &              bitwise (64-bit integers)\n"
        "  <<  >>                   shifts\n"
        "  +  -                     add, subtract\n"
        "  *  /  %  mod             multiply, divide, modulo (sign of divisor)\n"
        "  -x  +x  !x  ~x           unary minus, logical not, bitwise not\n"
        "  ^  **                    power (right associative: 2^3^2 = 512)\n"
        "  n!                       factorial (gamma for non-integers)\n"
        "\n"
        "VARIABLES AND FUNCTIONS\n"
        "  x = 5            r = sqrt(x^2 + 1)          x += 2\n"
        "  f(x) = x^2 + 1   g(a, b) = hypot(a, b)      f(3) + g(3, 4)\n"
        "  Recursion: fact2(n) = if(n <= 1, 1, n * fact2(n - 1))\n"
        "  Variables and functions are saved between sessions.\n"
        "\n"
        "CALCULUS\n"
        "  sigma(k^2, k, 1, 10)            product(k, k, 1, 5)\n"
        "  integ(sin(x), x, 0, 180)        integ(exp(-x^2), x, -inf, inf)\n"
        "  deriv(x^3, x, 2)                deriv(x^3, x, 2, 2)   (2nd derivative)\n"
        "  solve(x^2 = 2, x)               solve(cos(x) - x, x, 0, 1)  (interval)\n"
        "\n"
        "UNITS\n"
        "  conv(100, km, mi)    conv(37, C, F)    conv(1, GiB, MB)    (:units)\n"
        "\n"
        "COMMANDS (type them in the input line)\n"
        "  :help  :q              help, quit\n"
        "  :deg  :rad  :grad      angle mode\n"
        "  :dec  :hex  :bin  :oct output base for integer results\n"
        "  :auto  :fix N  :sci N  :eng N  :prec N  :group   number format\n"
        "  :vars  :funcs  :consts  :units                    listings\n"
        "  :del NAME  :reset      delete one / all user variables and functions\n"
        "  :plot EXPR[; EXPR..] [xmin, xmax] [ymin, ymax]    e.g. :plot sin(x); x/50\n"
        "  :factor N  :base N     prime factors, number in all bases\n"
        "  :copy  :clear  :clearhist  :mouse  :save  :version\n"
        "\n"
        "KEYS\n"
        "  Enter evaluate          Tab complete name (again: next match)\n"
        "  Up/Down history         (searches entries starting with the typed text)\n"
        "  Left/Right Home/End     Ctrl+A/E  Ctrl+B/F  Ctrl+Left/Right  Alt+B/F\n"
        "  Backspace/Del           Ctrl+K/U/W delete to end / start / word\n"
        "  Ctrl+Z / Ctrl+Y         undo / redo         Ctrl+L clear screen\n"
        "  PgUp/PgDn               scroll history      Ctrl+O copy last result\n"
        "  Ctrl+C                  clear the line, or cancel a long computation\n"
        "  Ctrl+D (on an empty line), Ctrl+Q or F10    quit\n"
        "  F1 help  F2 angle  F3 base  F4 format  F5 plot the input  F10 quit\n"
        "  Touch / mouse: tap a result to insert it, swipe or wheel to scroll\n"
        "  Plot: arrows pan, + - zoom, a autoscale, t trace, r reset, q back\n"
        "\n"
        "CONSTANTS\n");
    list_consts(out);
    for (int c = 0; c < CAT_COUNT; c++) {
        buf_printf(out, "\n%s\n", g_cat_names[c]);
        for (int i = 0; i < g_nbuiltins; i++) {
            const Builtin *b = &g_builtins[i];
            if (b->cat == c) buf_printf(out, "  %-30s %s\n", b->sig, b->help);
        }
    }
    buf_puts(out, "\nUNITS for conv(x, from, to)\n");
    units_help(out);
}

static void set_digits(const char *arg, int fmt, CmdIO *io)
{
    if (*arg) {
        int d = atoi(arg);
        if (d < 0 || d > 15) {
            say(io, MSG_ERROR, "digits must be 0..15");
            return;
        }
        g_set.digits = d;
    }
    g_set.fmt = fmt;
}

int cmd_execute(const char *line, CmdIO *io, const char **argp)
{
    char word[32], argbuf[1024];
    line = skip_ws(line);
    if (*line == ':') line = skip_ws(line + 1);
    size_t n = strcspn(line, " \t");
    if (n >= sizeof word) n = sizeof word - 1;
    for (size_t i = 0; i < n; i++) word[i] = (char)tolower((unsigned char)line[i]);
    word[n] = 0;
    str_copy(argbuf, sizeof argbuf, skip_ws(line + n));
    char *arg = str_trim(argbuf);
    static char plot_arg[1024];
    char fm[32];
    Buf b = {0};
    num x;

    if (!*word || !strcmp(word, "help") || !strcmp(word, "h") || !strcmp(word, "?")) return CMD_HELP;
    if (!strcmp(word, "q") || !strcmp(word, "quit") || !strcmp(word, "exit")) return CMD_QUIT;
    if (!strcmp(word, "deg") || !strcmp(word, "rad") || !strcmp(word, "grad")) {
        g_set.angle = word[0] == 'd' ? ANG_DEG : word[0] == 'r' ? ANG_RAD : ANG_GRAD;
        note(io, "angle mode: %s", angle_name(g_set.angle));
        return CMD_OK;
    }
    if (!strcmp(word, "dec") || !strcmp(word, "hex") || !strcmp(word, "bin") || !strcmp(word, "oct")) {
        g_set.base = word[0] == 'd' ? 10 : word[0] == 'h' ? 16 : word[0] == 'b' ? 2 : 8;
        note(io, "output base: %s", base_name(g_set.base));
        return CMD_OK;
    }
    if (!strcmp(word, "auto") || !strcmp(word, "norm")) {
        g_set.fmt = FMT_AUTO;
        note(io, "format: %s", fmt_mode_name(fm, sizeof fm));
        return CMD_OK;
    }
    if (!strcmp(word, "fix") || !strcmp(word, "sci") || !strcmp(word, "eng")) {
        set_digits(arg, word[0] == 'f' ? FMT_FIX : word[0] == 's' ? FMT_SCI : FMT_ENG, io);
        note(io, "format: %s", fmt_mode_name(fm, sizeof fm));
        return CMD_OK;
    }
    if (!strcmp(word, "prec")) {
        int p = atoi(arg);
        if (p < 1 || p > 17) {
            say(io, MSG_ERROR, "usage: :prec N   (1..17 significant digits, now %d)", g_set.prec);
            return CMD_ERROR;
        }
        g_set.prec = p;
        g_set.fmt = FMT_AUTO;
        note(io, "format: %s", fmt_mode_name(fm, sizeof fm));
        return CMD_OK;
    }
    if (!strcmp(word, "group")) {
        g_set.group = parse_onoff(arg, g_set.group);
        note(io, "digit grouping: %s", g_set.group ? "on" : "off");
        return CMD_OK;
    }
    if (!strcmp(word, "vars")) { list_vars(&b); say_buf(io, &b); return CMD_OK; }
    if (!strcmp(word, "funcs")) { list_funcs(&b); say_buf(io, &b); return CMD_OK; }
    if (!strcmp(word, "consts")) { list_consts(&b); say_buf(io, &b); return CMD_OK; }
    if (!strcmp(word, "units")) { units_help(&b); say_buf(io, &b); return CMD_OK; }
    if (!strcmp(word, "del") || !strcmp(word, "unset")) {
        if (!*arg) {
            say(io, MSG_ERROR, "usage: :del NAME [NAME...]");
            return CMD_ERROR;
        }
        for (char *tok = strtok(arg, " ,\t"); tok; tok = strtok(NULL, " ,\t")) {
            int r = sym_delete(tok);
            if (r > 0) note(io, "deleted %s", tok);
            else say(io, MSG_ERROR, r < 0 ? "%s is built in" : "%s is not defined", tok);
        }
        return CMD_OK;
    }
    if (!strcmp(word, "reset")) {
        sym_reset_user();
        note(io, "all user variables and functions deleted");
        return CMD_OK;
    }
    if (!strcmp(word, "clear") || !strcmp(word, "cls")) return CMD_CLEAR;
    if (!strcmp(word, "clearhist")) return CMD_CLEARHIST;
    if (!strcmp(word, "plot")) {
        if (!*arg) {
            say(io, MSG_ERROR, "usage: :plot sin(x); cos(x) [-pi, pi] [-2, 2]");
            return CMD_ERROR;
        }
        str_copy(plot_arg, sizeof plot_arg, arg);
        if (argp) *argp = plot_arg;
        return CMD_PLOT;
    }
    if (!strcmp(word, "factor")) {
        if (!arg_value(arg, io, &x)) return CMD_ERROR;
        factor_text(&b, x);
        say_buf(io, &b);
        return CMD_OK;
    }
    if (!strcmp(word, "base")) {
        if (!arg_value(arg, io, &x)) return CMD_ERROR;
        bases_text(&b, x);
        say_buf(io, &b);
        return CMD_OK;
    }
    if (!strcmp(word, "copy")) return CMD_COPY;
    if (!strcmp(word, "mouse")) {
        if (argp) *argp = parse_onoff(arg, io ? io->mouse : 0) ? "on" : "off";
        return CMD_MOUSE;
    }
    if (!strcmp(word, "save")) {
        char path[1024];
        if (state_save(home_path(".tcalc_state", path, sizeof path)) == 0)
            say(io, MSG_INFO, "saved to %s", path);
        else
            say(io, MSG_ERROR, "cannot write %s", path);
        return CMD_OK;
    }
    if (!strcmp(word, "version")) {
        say(io, MSG_INFO, "tcalc %s", TCALC_VERSION);
        return CMD_OK;
    }
    say(io, MSG_ERROR, "unknown command :%s  (try :help)", word);
    return CMD_ERROR;
}

/* ------------------------------------------------------------ files */

const char *home_path(const char *file, char *buf, size_t n)
{
    const char *home = getenv("HOME");
    snprintf(buf, n, "%s/%s", home && *home ? home : ".", file);
    return buf;
}

int state_save(const char *path)
{
    static const char *const fmts[] = {"auto", "fix", "sci", "eng"};
    char tmp[1100], v[64];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "# tcalc state - saved automatically on exit\n");
    fprintf(f, ":%s\n:%s\n", g_set.angle == ANG_DEG ? "deg" : g_set.angle == ANG_GRAD ? "grad" : "rad",
            g_set.base == 16 ? "hex" : g_set.base == 2 ? "bin" : g_set.base == 8 ? "oct" : "dec");
    fprintf(f, ":prec %d\n:%s %d\n", g_set.prec, fmts[g_set.fmt], g_set.digits);
    fprintf(f, ":group %s\n", g_set.group ? "on" : "off");
    for (int i = 0; i < g_nvars; i++) {
        Var *x = &g_vars[i];
        if (!x->defined || x->readonly) continue;
        if (isnan(x->val)) fprintf(f, "%s = 0/0\n", x->name);
        else {
            fmt_exact(v, sizeof v, x->val);
            fprintf(f, "%s = %s\n", x->name, v);
        }
    }
    Buf b = {0};
    list_funcs(&b);
    if (b.len) fputs(b.s, f);
    buf_free(&b);
    int bad = ferror(f);
    if (fclose(f) != 0 || bad) {
        remove(tmp);
        return -1;
    }
    return rename(tmp, path);
}

/* "name(a, b) = ..." -> declare name so definitions may refer to each other */
static void predeclare(const char *line)
{
    Lexer lx;
    lex_init(&lx, line, 0);
    if (lx.t.kind != T_IDENT) return;
    Token name = lx.t;
    int np = 0;
    lex_next(&lx);
    if (lx.t.kind != T_LPAREN) return;
    lex_next(&lx);
    while (lx.t.kind == T_IDENT) {
        np++;
        lex_next(&lx);
        if (lx.t.kind != T_COMMA) break;
        lex_next(&lx);
    }
    if (lx.t.kind != T_RPAREN) return;
    lex_next(&lx);
    if (lx.t.kind != T_ASSIGN) return;
    if (!builtin_find(line + name.pos, name.len) && !func_find(line + name.pos, name.len))
        func_declare(line + name.pos, name.len, np);
}

static int run_file(const char *path, CmdIO *io, int report)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[4096], norm[8192];
    long pos = ftell(f);
    while (fgets(line, sizeof line, f)) predeclare(line);
    fseek(f, pos, SEEK_SET);
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        normalize_input(line, norm, sizeof norm);
        char *s = str_trim(norm);
        if (!*s || *s == '#') continue;
        if (is_command(s)) {
            int r = cmd_execute(s, NULL, NULL);
            if (r == CMD_ERROR && report) say(io, MSG_ERROR, "%s:%d: bad command", path, lineno);
            continue;
        }
        CalcResult res;
        calc_eval(s, 0, &res);
        if (res.kind == RES_ERROR && report) say(io, MSG_ERROR, "%s:%d: %s", path, lineno, res.msg);
    }
    fclose(f);
    return 0;
}

int state_load(const char *path) { return run_file(path, NULL, 0); }

int script_run(const char *path, CmdIO *io) { return run_file(path, io, 1); }
