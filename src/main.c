/* main.c - command line interface: full-screen UI, one-shot, pipe and plain modes */
#include "term.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int g_opt_ascii, g_opt_color = 1, g_opt_mouse = 1;

static void usage(FILE *f)
{
    fprintf(f,
            "tcalc %s - advanced terminal calculator (Termux friendly)\n"
            "\n"
            "usage: tcalc                    start the full-screen calculator\n"
            "       tcalc 'EXPR'             evaluate and print the result\n"
            "       echo 'EXPR' | tcalc      evaluate each line of stdin\n"
            "       tcalc ':plot sin(x)'     draw a plot in the terminal\n"
            "\n"
            "options:\n"
            "  -e EXPR         evaluate EXPR (can be repeated)\n"
            "  -d | -r | -g    angle mode: degrees, radians, gradians\n"
            "  -p N            significant digits (1-17, default 15)\n"
            "  -x | --bin | --oct   integer output in hex, binary or octal\n"
            "  -n, --norc      ignore ~/.tcalcrc and the saved session\n"
            "      --plain     simple line mode instead of the full-screen UI\n"
            "      --ascii     ASCII drawing only (no Unicode box/braille)\n"
            "      --no-color  no colors (the NO_COLOR variable works too)\n"
            "      --no-mouse  no mouse / touch support in the UI\n"
            "  -h, --help      this help;  -v, --version  show version\n"
            "\n"
            "Inside the calculator press F1 or type :help.\n",
            TCALC_VERSION);
}

static void cli_print(void *ctx, int kind, const char *text)
{
    (void)ctx;
    if (kind == MSG_ERROR) fprintf(stderr, "tcalc: %s\n", text);
    else puts(text);
}

static int cli_plot(const char *spec)
{
    Plot pl;
    char err[256];
    if (plot_parse(&pl, spec, err, sizeof err) < 0) {
        fprintf(stderr, "tcalc: %s\n", err);
        return 1;
    }
    int w = 80, h = 24, tty = isatty(1);
    if (tty) term_size(&w, &h);
    h = h - 4 < 8 ? 8 : h - 4 > 30 ? 30 : h - 4;
    plot_print(&pl, stdout, w, h, g_opt_color && tty, g_opt_ascii);
    plot_free(&pl);
    return 0;
}

/* returns 0 ok, 1 error, 2 quit */
static int run_line(const char *line, int interactive)
{
    char norm[8192];
    normalize_input(line, norm, sizeof norm);
    char *s = str_trim(norm);
    if (!*s || *s == '#') return 0;
    if (is_command(s)) {
        CmdIO io = {.print = cli_print, .verbose = interactive};
        const char *arg = NULL;
        Buf b = {0};
        switch (cmd_execute(s, &io, &arg)) {
        case CMD_HELP:
            help_text(&b);
            fputs(b.s, stdout);
            buf_free(&b);
            return 0;
        case CMD_PLOT: return cli_plot(arg);
        case CMD_QUIT: return 2;
        case CMD_ERROR: return 1;
        default: return 0;
        }
    }
    CalcResult r;
    calc_eval(s, 0, &r);
    if (r.kind == RES_ERROR) {
        fprintf(stderr, "tcalc: error: %s\n", r.msg);
        if (r.err_pos >= 0 && strlen(s) < 200) fprintf(stderr, "  %s\n  %*s^\n", s, r.err_pos, "");
        return 1;
    }
    if (r.kind == RES_VALUE && (!r.assigned || interactive)) {
        char buf[512];
        fmt_number(buf, sizeof buf, r.value);
        printf("%s%s\n", interactive ? "= " : "", buf);
    } else if (r.kind == RES_FUNC && interactive) {
        printf("%s\n", r.msg);
    }
    return 0;
}

static int run_stream(FILE *in, int interactive)
{
    char line[8192];
    int status = 0;
    for (;;) {
        if (interactive) {
            fputs("> ", stdout);
            fflush(stdout);
        }
        if (!fgets(line, sizeof line, in)) break;
        int r = run_line(line, interactive);
        if (r == 2) break;
        if (r) status = 1;
        fflush(stdout);
    }
    if (interactive) putchar('\n');
    return status;
}

int main(int argc, char **argv)
{
    const char *exprs[64];
    int nexpr = 0, norc = 0, plain = 0, angle = -1, base = 0, prec = 0, read_stdin = 0;
    Buf joined = {0};

    const char *nc = getenv("NO_COLOR"), *term = getenv("TERM");
    if ((nc && *nc) || (term && !strcmp(term, "dumb"))) g_opt_color = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int is_opt = a[0] == '-' && a[1] && !strchr("0123456789.(", a[1]);
        if (!strcmp(a, "--")) {
            for (i++; i < argc; i++) {
                if (joined.len) buf_putc(&joined, ' ');
                buf_puts(&joined, argv[i]);
            }
            break;
        }
        if (!is_opt) {
            if (!strcmp(a, "-")) {
                read_stdin = 1;
                continue;
            }
            if (joined.len) buf_putc(&joined, ' ');
            buf_puts(&joined, a);
            continue;
        }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-v") || !strcmp(a, "--version")) {
            printf("tcalc %s\n", TCALC_VERSION);
            return 0;
        } else if (!strcmp(a, "-e") && i + 1 < argc) {
            if (nexpr < 64) exprs[nexpr++] = argv[++i];
        } else if (!strcmp(a, "-d") || !strcmp(a, "--deg")) {
            angle = ANG_DEG;
        } else if (!strcmp(a, "-r") || !strcmp(a, "--rad")) {
            angle = ANG_RAD;
        } else if (!strcmp(a, "-g") || !strcmp(a, "--grad")) {
            angle = ANG_GRAD;
        } else if (!strcmp(a, "-p") && i + 1 < argc) {
            prec = atoi(argv[++i]);
            if (prec < 1 || prec > 17) {
                fprintf(stderr, "tcalc: -p needs 1..17\n");
                return 2;
            }
        } else if (!strcmp(a, "-x") || !strcmp(a, "--hex")) {
            base = 16;
        } else if (!strcmp(a, "--bin")) {
            base = 2;
        } else if (!strcmp(a, "--oct")) {
            base = 8;
        } else if (!strcmp(a, "-n") || !strcmp(a, "--norc")) {
            norc = 1;
        } else if (!strcmp(a, "--plain")) {
            plain = 1;
        } else if (!strcmp(a, "--ascii")) {
            g_opt_ascii = 1;
        } else if (!strcmp(a, "--no-color") || !strcmp(a, "--no-colour")) {
            g_opt_color = 0;
        } else if (!strcmp(a, "--no-mouse")) {
            g_opt_mouse = 0;
        } else {
            fprintf(stderr, "tcalc: unknown option %s (try --help)\n", a);
            return 2;
        }
    }

    rng_seed((uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32) ^ (uint64_t)clock());
    calc_init();
    char path[1024];
    if (!norc) {
        CmdIO io = {.print = cli_print};
        script_run(home_path(".tcalcrc", path, sizeof path), &io);
        state_load(home_path(".tcalc_state", path, sizeof path));
    }
    if (angle >= 0) g_set.angle = angle;
    if (base) g_set.base = base;
    if (prec) g_set.prec = prec, g_set.fmt = FMT_AUTO;

    int status = 0;
    if (nexpr || joined.len) {
        for (int i = 0; i < nexpr; i++)
            if (run_line(exprs[i], 0)) status = 1;
        if (joined.len && run_line(joined.s, 0)) status = 1;
        buf_free(&joined);
        if (read_stdin) status |= run_stream(stdin, 0);
        return status;
    }
    if (read_stdin || !isatty(0)) return run_stream(stdin, 0);

    if (plain || (term && !strcmp(term, "dumb")) || !isatty(1) || tui_run() < 0) {
        status = run_stream(stdin, 1);
    }
    if (!norc && state_save(home_path(".tcalc_state", path, sizeof path)) != 0)
        fprintf(stderr, "tcalc: could not save %s\n", path);
    return status;
}
