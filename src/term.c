/* term.c - raw terminal I/O, key decoding and a line-diffing screen buffer */
#include "term.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

Theme T;
Glyphs G;

static struct termios orig_tio;
static int raw_on, mouse_on;
static volatile sig_atomic_t sig_winch, sig_quit;

static unsigned char ibuf[8192];
static int ilen;

static void out(const char *s, size_t n)
{
    while (n) {
        ssize_t w = write(1, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

static void outs(const char *s) { out(s, strlen(s)); }

/* ------------------------------------------------------- setup/teardown */

static void restore_now(void)
{
    if (!raw_on) return;
    if (mouse_on) outs("\x1b[?1006l\x1b[?1000l");
    outs("\x1b[?2004l\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l");
    tcsetattr(0, TCSAFLUSH, &orig_tio);
    raw_on = 0;
}

static void on_winch(int s)
{
    (void)s;
    sig_winch = 1;
}

static void on_quit(int s)
{
    (void)s;
    sig_quit = 1;
}

static void on_fatal(int s)
{
    restore_now();                  /* only write() and tcsetattr(): signal safe */
    signal(s, SIG_DFL);
    raise(s);
}

int term_open(int mouse)
{
    if (!isatty(0) || !isatty(1) || tcgetattr(0, &orig_tio) < 0) return -1;
    struct termios raw = orig_tio;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(tcflag_t)OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSAFLUSH, &raw) < 0) return -1;
    raw_on = 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_winch;           /* no SA_RESTART: wake up poll() */
    sigaction(SIGWINCH, &sa, NULL);
    sa.sa_handler = on_quit;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = on_fatal;
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    atexit(term_close);

    /* alternate screen, bracketed paste, no autowrap, hidden cursor */
    outs("\x1b[?1049h\x1b[?2004h\x1b[?7l\x1b[?25l\x1b[H\x1b[2J");
    term_mouse(mouse);
    return 0;
}

void term_close(void) { restore_now(); }

void term_mouse(int on)
{
    if (on) outs("\x1b[?1000h\x1b[?1006h");
    else outs("\x1b[?1006l\x1b[?1000l");
    mouse_on = on;
}

void term_size(int *w, int *h)
{
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    } else {
        const char *c = getenv("COLUMNS"), *l = getenv("LINES");
        *w = c && atoi(c) > 0 ? atoi(c) : 80;
        *h = l && atoi(l) > 0 ? atoi(l) : 24;
    }
}

/* OSC 52: put text on the system clipboard (Termux, xterm, kitty, tmux...) */
void term_copy(const char *text)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Buf b = {0};
    size_t n = strlen(text);
    buf_puts(&b, "\x1b]52;c;");
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)(unsigned char)text[i] << 16;
        if (i + 1 < n) v |= (uint32_t)(unsigned char)text[i + 1] << 8;
        if (i + 2 < n) v |= (uint32_t)(unsigned char)text[i + 2];
        buf_putc(&b, b64[(v >> 18) & 63]);
        buf_putc(&b, b64[(v >> 12) & 63]);
        buf_putc(&b, i + 1 < n ? b64[(v >> 6) & 63] : '=');
        buf_putc(&b, i + 2 < n ? b64[v & 63] : '=');
    }
    buf_puts(&b, "\a");
    out(b.s, b.len);
    buf_free(&b);
}

/* ---------------------------------------------------------------- input */

int utf8_len(unsigned char c)
{
    return c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
}

static int fill(int timeout_ms)
{
    if (ilen >= (int)sizeof ibuf) return 1;
    struct pollfd p = {0, POLLIN, 0};
    int r = poll(&p, 1, timeout_ms);
    if (r <= 0) return r;
    ssize_t n = read(0, ibuf + ilen, sizeof ibuf - (size_t)ilen);
    if (n == 0) return -2;
    if (n < 0) return -1;
    ilen += (int)n;
    return 1;
}

static void consume(int n)
{
    memmove(ibuf, ibuf + n, (size_t)(ilen - n));
    ilen -= n;
}

static int csi_mods(int p) { return p > 1 ? p - 1 : 0; }   /* 2 shift, 3 alt, 5 ctrl */

static int decode_csi(Key *k, int final)
{
    int i = 2, np = 0, par[8] = {0}, priv = 0;
    if (i < ilen && (ibuf[i] == '<' || ibuf[i] == '?')) priv = ibuf[i++];
    for (; i < ilen; i++) {
        unsigned char c = ibuf[i];
        if (c >= '0' && c <= '9') {
            if (np == 0) np = 1;
            par[np - 1] = par[np - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (np == 0) np = 1;
            if (np < 8) np++;
        } else if (c >= 0x40 && c <= 0x7E) {
            break;
        } else if (c < 0x20 || c > 0x7E) {
            k->code = K_ESC;       /* garbage: treat as ESC */
            return 1;
        }
    }
    if (i >= ilen) {
        if (!final) return 0;
        k->code = K_ESC;
        return 1;
    }
    unsigned char fin = ibuf[i];
    int used = i + 1, mods = np >= 2 ? csi_mods(par[1]) : 0;
    k->mods = mods;
    if (priv == '<' && (fin == 'M' || fin == 'm')) {      /* SGR mouse */
        k->code = K_MOUSE;
        k->mbtn = par[0] & ~(4 | 8 | 16 | 32);
        k->mods = (par[0] & 16) ? M_CTRL : 0;
        k->mx = par[1] - 1;
        k->my = par[2] - 1;
        k->mpress = fin == 'M' && !(par[0] & 32);
        return used;
    }
    if (fin == 'M' && np == 0) {                          /* X10 mouse fallback */
        if (ilen < 6) return final ? (k->code = K_ESC, 1) : 0;
        int b = ibuf[3] - 32;
        k->code = K_MOUSE;
        k->mbtn = (b & 3) == 3 ? 0 : (b & ~(4 | 8 | 16 | 32));
        k->mpress = (b & 3) != 3;
        k->mx = ibuf[4] - 33;
        k->my = ibuf[5] - 33;
        return 6;
    }
    switch (fin) {
    case 'A': k->code = K_UP; break;
    case 'B': k->code = K_DOWN; break;
    case 'C': k->code = K_RIGHT; break;
    case 'D': k->code = K_LEFT; break;
    case 'H': k->code = K_HOME; break;
    case 'F': k->code = K_END; break;
    case 'Z': k->code = K_BTAB; break;
    case 'P': case 'Q': case 'R': case 'S': k->code = K_F1 + (fin - 'P'); break;
    case '~':
        switch (par[0]) {
        case 1: case 7: k->code = K_HOME; break;
        case 4: case 8: k->code = K_END; break;
        case 2: k->code = K_INSERT; break;
        case 3: k->code = K_DELETE; break;
        case 5: k->code = K_PGUP; break;
        case 6: k->code = K_PGDN; break;
        case 11: case 12: case 13: case 14: case 15: k->code = K_F1 + par[0] - 11; break;
        case 17: case 18: case 19: case 20: case 21: k->code = K_F1 + 5 + par[0] - 17; break;
        case 23: case 24: k->code = K_F1 + 10 + par[0] - 23; break;
        case 200: k->code = K_PASTE_BEGIN; break;
        case 201: k->code = K_PASTE_END; break;
        default: k->code = K_NONE;
        }
        break;
    default: k->code = K_NONE;
    }
    return used;
}

static int decode_plain(Key *k, int off, int final)
{
    unsigned char c = ibuf[off];
    if (c == '\r' || c == '\n') k->code = K_ENTER;
    else if (c == '\t') k->code = K_TAB;
    else if (c == 0x7f || c == 0x08) k->code = K_BACKSPACE;
    else if (c < 0x20) {
        k->code = K_CHAR;
        k->mods |= M_CTRL;
        k->ch = c == 0 ? ' ' : c <= 26 ? 'a' + c - 1 : "\\]^_"[c - 28];
    } else if (c < 0x80) {
        k->code = K_CHAR;
        k->ch = c;
    } else {
        int n = utf8_len(c);
        if (off + n > ilen) {
            if (!final) return 0;
            n = ilen - off;
        }
        char tmp[8] = {0};
        memcpy(tmp, ibuf + off, (size_t)n);
        k->code = K_CHAR;
        utf8_decode(tmp, &k->ch);
        return off + n;
    }
    return off + 1;
}

static int decode(Key *k, int final)
{
    memset(k, 0, sizeof *k);
    if (ibuf[0] != 0x1b) return decode_plain(k, 0, final);
    if (ilen == 1) {
        if (!final) return 0;
        k->code = K_ESC;
        return 1;
    }
    unsigned char c1 = ibuf[1];
    if (c1 == '[') return decode_csi(k, final);
    if (c1 == 'O') {
        if (ilen < 3) return final ? (k->code = K_ESC, 1) : 0;
        switch (ibuf[2]) {
        case 'A': k->code = K_UP; break;
        case 'B': k->code = K_DOWN; break;
        case 'C': k->code = K_RIGHT; break;
        case 'D': k->code = K_LEFT; break;
        case 'H': k->code = K_HOME; break;
        case 'F': k->code = K_END; break;
        case 'M': k->code = K_ENTER; break;
        case 'P': case 'Q': case 'R': case 'S': k->code = K_F1 + (ibuf[2] - 'P'); break;
        default: k->code = K_NONE;
        }
        return 3;
    }
    if (c1 == 0x1b) {
        k->code = K_ESC;
        return 1;
    }
    k->mods = M_ALT;                                   /* Alt+key */
    return decode_plain(k, 1, final);
}

int term_read_key(Key *k, int timeout_ms)
{
    for (;;) {
        if (sig_quit) {
            memset(k, 0, sizeof *k);
            k->code = K_QUIT;
            return 1;
        }
        if (sig_winch) {
            sig_winch = 0;
            memset(k, 0, sizeof *k);
            k->code = K_RESIZE;
            return 1;
        }
        if (ilen > 0) {
            int used = decode(k, 0);
            if (used == 0) {                          /* incomplete sequence */
                int r = fill(40);
                if (r == 1) continue;
                used = decode(k, 1);
            }
            consume(used);
            return 1;
        }
        int r = fill(timeout_ms);
        if (r == 0) return 0;
        if (r == -2 || (r < 0 && errno != EINTR)) {
            memset(k, 0, sizeof *k);
            k->code = K_QUIT;
            return 1;
        }
    }
}

int term_pending(void)
{
    if (ilen > 0) return 1;
    struct pollfd p = {0, POLLIN, 0};
    return poll(&p, 1, 0) > 0;
}

/* Called every few thousand evaluation steps: was Ctrl+C pressed? */
int term_check_interrupt(void)
{
    struct pollfd p = {0, POLLIN, 0};
    while (ilen < (int)sizeof ibuf && poll(&p, 1, 0) > 0) {
        ssize_t n = read(0, ibuf + ilen, sizeof ibuf - (size_t)ilen);
        if (n <= 0) break;
        ilen += (int)n;
    }
    for (int i = 0; i < ilen; i++)
        if (ibuf[i] == 3) {
            memmove(ibuf + i, ibuf + i + 1, (size_t)(ilen - i - 1));
            ilen--;
            return 1;
        }
    return sig_quit != 0;
}

/* --------------------------------------------------------------- screen */

static Line *rows;
static Buf *prev;
static int sw, sh, full_redraw = 1;

void scr_resize(int w, int h)
{
    if (w == sw && h == sh && rows) return;
    for (int i = 0; i < sh; i++) {
        buf_free(&rows[i].b);
        buf_free(&prev[i]);
    }
    free(rows);
    free(prev);
    rows = xmalloc(sizeof *rows * (size_t)h);
    prev = xmalloc(sizeof *prev * (size_t)h);
    memset(rows, 0, sizeof *rows * (size_t)h);
    memset(prev, 0, sizeof *prev * (size_t)h);
    sw = w;
    sh = h;
    full_redraw = 1;
}

void scr_invalidate(void) { full_redraw = 1; }

Line *scr_line(int y)
{
    Line *l = &rows[y];
    l->b.len = 0;
    if (l->b.s) l->b.s[0] = 0;
    l->col = 0;
    l->width = sw;
    return l;
}

void ln_sgr(Line *l, const char *sgr)
{
    if (*sgr) buf_puts(&l->b, sgr);
}

int ln_text(Line *l, const char *s, int n)
{
    const char *p = s, *end = s + (n < 0 ? (int)strlen(s) : n);
    int cols = 0;
    while (p < end && l->col < l->width) {
        int len = utf8_len((unsigned char)*p);
        if (p + len > end) break;
        if ((unsigned char)*p < 0x20) buf_putc(&l->b, ' ');
        else buf_putn(&l->b, p, (size_t)len);
        l->col++;
        cols++;
        p += len;
    }
    return cols;
}

void ln_spaces(Line *l, int n)
{
    while (n-- > 0 && l->col < l->width) {
        buf_putc(&l->b, ' ');
        l->col++;
    }
}

void ln_repeat(Line *l, const char *glyph, int n)
{
    size_t gl = strlen(glyph);
    while (n-- > 0 && l->col < l->width) {
        buf_putn(&l->b, glyph, gl);
        l->col++;
    }
}

void ln_pad(Line *l) { ln_spaces(l, l->width - l->col); }

void scr_present(int cy, int cx, int show_cursor)
{
    Buf o = {0};
    buf_puts(&o, "\x1b[?2026h\x1b[?25l");
    if (full_redraw) buf_puts(&o, "\x1b[0m\x1b[2J");
    for (int y = 0; y < sh; y++) {
        Line *l = &rows[y];
        if (!l->b.s) buf_putc(&l->b, 0), l->b.len = 0;
        if (!full_redraw && prev[y].s && prev[y].len == l->b.len && !memcmp(prev[y].s, l->b.s, l->b.len))
            continue;
        buf_printf(&o, "\x1b[%d;1H\x1b[0m", y + 1);
        buf_putn(&o, l->b.s, l->b.len);
        buf_puts(&o, "\x1b[0m");
        prev[y].len = 0;
        buf_putn(&prev[y], l->b.s, l->b.len);
    }
    buf_printf(&o, "\x1b[%d;%dH", cy + 1, cx + 1);
    if (show_cursor) buf_puts(&o, "\x1b[?25h");
    buf_puts(&o, "\x1b[?2026l");
    out(o.s, o.len);
    buf_free(&o);
    full_redraw = 0;
}

/* ---------------------------------------------------------------- theme */

void theme_init(int color)
{
    static const char *const R = "\x1b[0m";
    if (!color) {
        Theme t = {R, "\x1b[0;7m", "\x1b[0;7;1m", "\x1b[0;7m", R, R, R, R, R, R, R, "\x1b[0;7m",
                   "\x1b[0;1m", "\x1b[0;7m", R, "\x1b[0;1m", "\x1b[0;1m", R, R, R, "\x1b[0;7m",
                   "\x1b[0;4m", R, R, {R, R, R, R, R, R}};
        T = t;
        return;
    }
    Theme t = {
        R,
        "\x1b[0;48;5;24;38;5;255m",       /* bar */
        "\x1b[0;1;48;5;31;38;5;231m",     /* bar_hl */
        "\x1b[0;48;5;24;38;5;153m",       /* bar_dim */
        "\x1b[0;38;5;117m",               /* num */
        "\x1b[0;38;5;221m",               /* func */
        "\x1b[0;38;5;150m",               /* var */
        "\x1b[0;38;5;213m",               /* const */
        "\x1b[0;38;5;209m",               /* keyword */
        "\x1b[0m",                        /* operator */
        "\x1b[0;38;5;245m",               /* paren */
        "\x1b[0;1;48;5;240;38;5;231m",    /* matching paren */
        "\x1b[0;38;5;203m",               /* error */
        "\x1b[0;48;5;124;38;5;231m",      /* error position */
        "\x1b[0;38;5;244m",               /* dim */
        "\x1b[0;1m",                      /* result */
        "\x1b[0;1;38;5;81m",              /* prompt */
        "\x1b[0;38;5;241m",               /* ghost text */
        "\x1b[0;38;5;109m",               /* info */
        "\x1b[0;38;5;239m",               /* border */
        "\x1b[0;48;5;238m",               /* selection */
        "\x1b[0;4;38;5;210m",             /* unknown name */
        "\x1b[0;38;5;114m",               /* ok */
        "\x1b[0;38;5;240m",               /* plot axis */
        {"\x1b[0;38;5;81m", "\x1b[0;38;5;221m", "\x1b[0;38;5;150m", "\x1b[0;38;5;213m",
         "\x1b[0;38;5;209m", "\x1b[0;38;5;141m"},
    };
    T = t;
}

void glyphs_init(int ascii)
{
    if (ascii) {
        Glyphs g = {"-", "|", ">", "~", ">", "<", "^", "v", "."};
        G = g;
    } else {
        Glyphs g = {"\xe2\x94\x80", "\xe2\x94\x82", "\xc2\xbb", "\xe2\x89\x88", "\xe2\x80\xa6",
                    "\xe2\x80\xa6", "\xe2\x86\x91", "\xe2\x86\x93", "\xc2\xb7"};
        G = g;
    }
}
