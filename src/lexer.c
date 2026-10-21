/* lexer.c - tokenizer and Unicode input normalization */
#include "calc.h"

#include <stdlib.h>
#include <string.h>

static int is_digit(int c) { return c >= '0' && c <= '9'; }

static void lex_number(Lexer *lx, int p)
{
    const char *s = lx->src;
    Token *t = &lx->t;
    t->kind = T_NUM;
    t->pos = p;
    t->val = 0;

    int pfx = s[p] == '0' ? (s[p + 1] | 32) : 0;
    if (pfx == 'x' || pfx == 'b' || pfx == 'o') {
        int base = pfx == 'x' ? 16 : pfx == 'b' ? 2 : 8;
        int q = p + 2, nd = 0, bad = 0;
        uint64_t v = 0;
        for (;; q++) {
            int c = (unsigned char)s[q], d;
            if (c == '_') continue;
            if (is_digit(c)) d = c - '0';
            else if ((c | 32) >= 'a' && (c | 32) <= 'f') d = (c | 32) - 'a' + 10;
            else break;
            if (d >= base) break;
            if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) bad = 1;
            v = v * (uint64_t)base + (uint64_t)d;
            nd++;
        }
        if (is_ident_char((unsigned char)s[q])) {
            bad = 1;
            while (is_ident_char((unsigned char)s[q])) q++;
        }
        t->kind = (!nd || bad) ? T_ERROR : T_NUM;
        t->val = (num)v;
        t->len = q - p;
        lx->pos = q;
        return;
    }

    char tmp[128];
    int n = 0, q = p, overflow = 0;
#define PUT(ch) do { if (n < (int)sizeof tmp - 1) tmp[n++] = (char)(ch); else overflow = 1; } while (0)
    while (is_digit(s[q]) || (s[q] == '_' && q > p)) {
        if (s[q] != '_') PUT(s[q]);
        q++;
    }
    if (s[q] == '.') {
        PUT('.');
        q++;
        while (is_digit(s[q]) || (s[q] == '_' && is_digit(s[q - 1]))) {
            if (s[q] != '_') PUT(s[q]);
            q++;
        }
    }
    if ((s[q] == 'e' || s[q] == 'E') &&
        (is_digit(s[q + 1]) || ((s[q + 1] == '+' || s[q + 1] == '-') && is_digit(s[q + 2])))) {
        PUT('e');
        q++;
        if (s[q] == '+' || s[q] == '-') PUT(s[q++]);
        while (is_digit(s[q])) PUT(s[q++]);
    }
#undef PUT
    tmp[n] = 0;
    t->val = strtod(tmp, NULL);
    t->len = q - p;
    if (overflow) t->kind = T_ERROR;
    lx->pos = q;
}

void lex_next(Lexer *lx)
{
    const char *s = lx->src;
    int p = lx->pos;
    for (;;) {
        while (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n') p++;
        if (s[p] != '#') break;
        while (s[p] && s[p] != '\n') p++;
    }
    Token *t = &lx->t;
    t->pos = p;
    t->val = 0;
    t->len = 1;
    int c = (unsigned char)s[p];
    if (!c) {
        t->kind = T_EOF;
        t->len = 0;
        lx->pos = p;
        return;
    }
    if (is_digit(c) || (c == '.' && is_digit(s[p + 1]))) {
        lex_number(lx, p);
        return;
    }
    if (is_ident_start(c)) {
        int q = p + 1;
        while (is_ident_char((unsigned char)s[q])) q++;
        t->kind = T_IDENT;
        t->len = q - p;
        lx->pos = q;
        return;
    }
    int c2 = (unsigned char)s[p + 1];
    int k = T_ERROR, len = 1;
    switch (c) {
    case '+': k = c2 == '=' ? (len = 2, T_PLUSEQ) : T_PLUS; break;
    case '-': k = c2 == '=' ? (len = 2, T_MINUSEQ) : T_MINUS; break;
    case '*':
        if (c2 == '*') k = T_CARET, len = 2;
        else if (c2 == '=') k = T_STAREQ, len = 2;
        else k = T_STAR;
        break;
    case '/': k = c2 == '=' ? (len = 2, T_SLASHEQ) : T_SLASH; break;
    case '%': k = T_PERCENT; break;
    case '^': k = T_CARET; break;
    case '(': k = T_LPAREN; break;
    case ')': k = T_RPAREN; break;
    case '[': k = T_LBRACK; break;
    case ']': k = T_RBRACK; break;
    case ',': k = T_COMMA; break;
    case ';': k = T_SEMI; break;
    case '=': k = c2 == '=' ? (len = 2, T_EQ) : T_ASSIGN; break;
    case '!': k = c2 == '=' ? (len = 2, T_NE) : T_BANG; break;
    case '<':
        if (c2 == '=') k = T_LE, len = 2;
        else if (c2 == '<') k = T_SHL, len = 2;
        else k = T_LT;
        break;
    case '>':
        if (c2 == '=') k = T_GE, len = 2;
        else if (c2 == '>') k = T_SHR, len = 2;
        else k = T_GT;
        break;
    case '&': k = c2 == '&' ? (len = 2, T_ANDAND) : T_AMP; break;
    case '|': k = c2 == '|' ? (len = 2, T_OROR) : T_PIPE; break;
    case '~': k = T_TILDE; break;
    default:
        if (c >= 0x80) {          /* swallow a whole UTF-8 sequence */
            uint32_t cp;
            len = utf8_decode(s + p, &cp);
        }
        break;
    }
    t->kind = k;
    t->len = len;
    lx->pos = p + len;
}

void lex_init(Lexer *lx, const char *src, int start)
{
    lx->src = src;
    lx->pos = start;
    lex_next(lx);
}

/* ------------------------------------------------------------ UTF-8 */

int utf8_decode(const char *str, uint32_t *cp)
{
    const unsigned char *s = (const unsigned char *)str;
    uint32_t c = s[0];
    int n;
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0) n = 1, c &= 0x1F;
    else if ((c & 0xF0) == 0xE0) n = 2, c &= 0x0F;
    else if ((c & 0xF8) == 0xF0) n = 3, c &= 0x07;
    else { *cp = 0xFFFD; return 1; }
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *cp = c;
    return n + 1;
}

/* Map handy Unicode input (Persian/Arabic digits, math symbols typed on a
 * phone keyboard, ...) to the ASCII syntax the parser understands. */
const char *unicode_alias(uint32_t cp)
{
    static const char *const dig[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    static const char *const sup[10] = {"^0", "^1", "^2", "^3", "^4", "^5", "^6", "^7", "^8", "^9"};
    static char fw[95][2];

    if (cp >= 0x06F0 && cp <= 0x06F9) return dig[cp - 0x06F0];     /* Persian */
    if (cp >= 0x0660 && cp <= 0x0669) return dig[cp - 0x0660];     /* Arabic-Indic */
    if (cp >= 0x2074 && cp <= 0x2079) return sup[cp - 0x2070];
    if (cp >= 0xFF01 && cp <= 0xFF5E) {                            /* fullwidth ASCII */
        char *f = fw[cp - 0xFF01];
        f[0] = (char)(cp - 0xFEE0);
        f[1] = 0;
        return f;
    }
    if ((cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069)) return "";
    switch (cp) {
    case 0x066B: return ".";                 /* Arabic decimal separator */
    case 0x066C: return "_";                 /* Arabic thousands separator */
    case 0x060C: return ",";                 /* Arabic comma */
    case 0x061B: return ";";                 /* Arabic semicolon */
    case 0x00D7: case 0x00B7: case 0x22C5: case 0x2219: case 0x2715: case 0x2716: return "*";
    case 0x00F7: case 0x2215: case 0x2044: case 0x2797: return "/";
    case 0x2212: case 0x2012: case 0x2013: case 0x2014: case 0xFE63: case 0x2796: return "-";
    case 0x2795: return "+";
    case 0x03C0: return "pi";
    case 0x03C4: return "tau";
    case 0x03C6: case 0x03D5: return "phi";
    case 0x212F: return "e";
    case 0x221E: return "inf";
    case 0x221A: return "sqrt ";
    case 0x221B: return "cbrt ";
    case 0x00B9: return "^1";
    case 0x00B2: return "^2";
    case 0x00B3: return "^3";
    case 0x2070: return "^0";
    case 0x00B0: return "deg";
    case 0x2264: return "<=";
    case 0x2265: return ">=";
    case 0x2260: return "!=";
    case 0x2211: return "sigma";
    case 0x220F: return "product";
    case 0x222B: return "integ";
    case 0x00A0: case 0x2009: case 0x200A: case 0x202F: case 0x3000: return " ";
    case 0x200B: case 0x200C: case 0x200D: case 0x200E: case 0x200F: case 0xFEFF: return "";
    }
    return NULL;
}

size_t normalize_input(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    if (!outsz) return 0;
    while (*in) {
        unsigned char c = (unsigned char)*in;
        const char *rep;
        char one[2];
        if (c < 0x80) {
            one[0] = (char)c;
            one[1] = 0;
            rep = one;
            in++;
        } else {
            uint32_t cp;
            in += utf8_decode(in, &cp);
            rep = unicode_alias(cp);
            if (!rep) rep = "?";
            if (!strcmp(rep, "deg") && o > 0 &&
                (is_ident_char((unsigned char)out[o - 1]) || out[o - 1] == '.') &&
                o + 1 < outsz)
                out[o++] = ' ';
        }
        size_t l = strlen(rep);
        if (o + l >= outsz) break;
        memcpy(out + o, rep, l);
        o += l;
    }
    out[o] = 0;
    return o;
}
