/* term.h - raw terminal I/O, key decoding and screen rendering */
#ifndef TCALC_TERM_H
#define TCALC_TERM_H

#include "calc.h"

enum {
    K_NONE, K_CHAR, K_ENTER, K_TAB, K_BTAB, K_BACKSPACE, K_DELETE,
    K_LEFT, K_RIGHT, K_UP, K_DOWN, K_HOME, K_END, K_PGUP, K_PGDN, K_INSERT,
    K_ESC, K_MOUSE, K_PASTE_BEGIN, K_PASTE_END, K_RESIZE, K_QUIT,
    K_F1 = 100      /* K_F1 + n - 1 = Fn */
};
enum { M_SHIFT = 1, M_ALT = 2, M_CTRL = 4 };

typedef struct {
    int code;          /* K_* */
    uint32_t ch;       /* K_CHAR: code point; Ctrl+X gives ch 'x' + M_CTRL */
    int mods;
    int mx, my;        /* mouse cell, 0-based */
    int mbtn, mpress;  /* button: 0 left, 1 middle, 2 right, 64/65 wheel */
} Key;

int term_open(int mouse);
void term_close(void);
void term_size(int *w, int *h);
int term_read_key(Key *k, int timeout_ms);
int term_pending(void);
int term_check_interrupt(void);
void term_mouse(int on);
void term_copy(const char *text);

/* line-diffing screen */
typedef struct {
    Buf b;
    int col, width;
} Line;

void scr_resize(int w, int h);
void scr_invalidate(void);
Line *scr_line(int y);
void ln_sgr(Line *l, const char *sgr);
int ln_text(Line *l, const char *s, int n);
void ln_spaces(Line *l, int n);
void ln_repeat(Line *l, const char *glyph, int n);
void ln_pad(Line *l);
void scr_present(int cy, int cx, int show_cursor);

typedef struct {
    const char *reset, *bar, *bar_hl, *bar_dim, *num, *func, *var, *cst, *kw, *op,
        *paren, *paren_hl, *err, *err_hl, *dim, *result, *prompt, *ghost, *info,
        *border, *sel, *unknown, *ok, *axis, *plot[PLOT_MAX_FN];
} Theme;
extern Theme T;
void theme_init(int color);

typedef struct {
    const char *h, *v, *prompt, *approx, *ell, *lell, *up, *down, *dot;
} Glyphs;
extern Glyphs G;
void glyphs_init(int ascii);

int utf8_len(unsigned char c);

#endif
