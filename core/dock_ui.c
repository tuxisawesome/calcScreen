/*
 * dock_ui.c - tiny immediate-mode UI with a built-in 8x8 font.
 *
 * Everything is clipped to the canvas. Layouts are designed for 320x240 (the
 * dock scales that up in hardware) but adapt to any canvas size.
 */
#include "dock_ui.h"

/*
 * font8x8_basic, glyphs U+0020..U+007E: one byte per row, top row first,
 * bit 0 = leftmost pixel.
 * Author: Daniel Hepper <daniel@hepper.net>, based on font8x8.h by Marcel
 * Sondaar / IBM public domain VGA fonts. License: Public Domain.
 * https://github.com/dhepper/font8x8
 */
static const uint8_t font8x8[95][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*   */
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00}, /* ! */
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* " */
    {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00}, /* # */
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00}, /* $ */
    {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00}, /* % */
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00}, /* & */
    {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ' */
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00}, /* ( */
    {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00}, /* ) */
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, /* * */
    {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00}, /* + */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06}, /* , */
    {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00}, /* - */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00}, /* . */
    {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00}, /* / */
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00}, /* 0 */
    {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00}, /* 1 */
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00}, /* 2 */
    {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00}, /* 3 */
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00}, /* 4 */
    {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00}, /* 5 */
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00}, /* 6 */
    {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00}, /* 7 */
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00}, /* 8 */
    {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00}, /* 9 */
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00}, /* : */
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06}, /* ; */
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00}, /* < */
    {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00}, /* = */
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00}, /* > */
    {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00}, /* ? */
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00}, /* @ */
    {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00}, /* A */
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00}, /* B */
    {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00}, /* C */
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00}, /* D */
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00}, /* E */
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00}, /* F */
    {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00}, /* G */
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00}, /* H */
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, /* I */
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00}, /* J */
    {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00}, /* K */
    {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00}, /* L */
    {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00}, /* M */
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00}, /* N */
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00}, /* O */
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00}, /* P */
    {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00}, /* Q */
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00}, /* R */
    {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00}, /* S */
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, /* T */
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00}, /* U */
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, /* V */
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00}, /* W */
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00}, /* X */
    {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00}, /* Y */
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00}, /* Z */
    {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00}, /* [ */
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00}, /* backslash */
    {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00}, /* ] */
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00}, /* ^ */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF}, /* _ */
    {0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ` */
    {0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00}, /* a */
    {0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00}, /* b */
    {0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00}, /* c */
    {0x38, 0x30, 0x30, 0x3e, 0x33, 0x33, 0x6E, 0x00}, /* d */
    {0x00, 0x00, 0x1E, 0x33, 0x3f, 0x03, 0x1E, 0x00}, /* e */
    {0x1C, 0x36, 0x06, 0x0f, 0x06, 0x06, 0x0F, 0x00}, /* f */
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F}, /* g */
    {0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00}, /* h */
    {0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, /* i */
    {0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E}, /* j */
    {0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00}, /* k */
    {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, /* l */
    {0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00}, /* m */
    {0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00}, /* n */
    {0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00}, /* o */
    {0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F}, /* p */
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78}, /* q */
    {0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00}, /* r */
    {0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00}, /* s */
    {0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00}, /* t */
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00}, /* u */
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, /* v */
    {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00}, /* w */
    {0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00}, /* x */
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F}, /* y */
    {0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00}, /* z */
    {0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00}, /* { */
    {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00}, /* | */
    {0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00}, /* } */
    {0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ~ */
};

#define GLYPH 8
#define LINE_H(scale) (10 * (scale)) /* 8 px glyph + 2 px leading */
#define TITLE_H 20
#define FOOTER_H 16
#define MARGIN 12
#define ROW_H 14

/* ---- primitives ---- */

void dock_ui_fill(const dock_canvas_t *c, int x, int y, int w, int h,
                  uint32_t color)
{
    int x0 = x, y0 = y, x1, y1, yy;
    if (w <= 0 || h <= 0)
        return;
    x1 = x + w;
    y1 = y + h;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > (int)c->w)
        x1 = (int)c->w;
    if (y1 > (int)c->h)
        y1 = (int)c->h;
    for (yy = y0; yy < y1; yy++) {
        uint32_t *row = c->px + (uint32_t)yy * c->stride;
        int xx;
        for (xx = x0; xx < x1; xx++)
            row[xx] = color;
    }
}

static void frame(const dock_canvas_t *c, int x, int y, int w, int h,
                  int t, uint32_t color)
{
    dock_ui_fill(c, x, y, w, t, color);
    dock_ui_fill(c, x, y + h - t, w, t, color);
    dock_ui_fill(c, x, y, t, h, color);
    dock_ui_fill(c, x + w - t, y, t, h, color);
}

static void glyph(const dock_canvas_t *c, int x, int y, char ch, uint32_t color,
                  int scale)
{
    const uint8_t *g;
    int r, b;
    if ((unsigned char)ch < 32u || (unsigned char)ch > 126u)
        ch = '?';
    if (x >= (int)c->w || y >= (int)c->h || x + GLYPH * scale <= 0 ||
        y + GLYPH * scale <= 0)
        return;
    g = font8x8[(unsigned char)ch - 32u];
    for (r = 0; r < GLYPH; r++) {
        uint8_t bits = g[r];
        for (b = 0; b < GLYPH; b++)
            if (bits & (1u << b))
                dock_ui_fill(c, x + b * scale, y + r * scale, scale, scale, color);
    }
}

/* Draw at most n characters of one line (stops at '\n' or '\0'). */
static int text_n(const dock_canvas_t *c, int x, int y, const char *s, int n,
                  uint32_t color, int scale)
{
    int i;
    for (i = 0; i < n && s[i] && s[i] != '\n'; i++)
        glyph(c, x + i * GLYPH * scale, y, s[i], color, scale);
    return i * GLYPH * scale;
}

static int fix_scale(int scale)
{
    return scale < 1 ? 1 : scale;
}

static int line_len(const char *s)
{
    int n = 0;
    while (s[n] && s[n] != '\n')
        n++;
    return n;
}

int dock_ui_text(const dock_canvas_t *c, int x, int y, const char *s,
                 uint32_t color, int scale)
{
    int widest = 0;
    scale = fix_scale(scale);
    for (;;) {
        int n = line_len(s);
        int w = text_n(c, x, y, s, n, color, scale);
        if (w > widest)
            widest = w;
        s += n;
        if (*s != '\n')
            break;
        s++;
        y += LINE_H(scale);
    }
    return widest;
}

int dock_ui_text_width(const char *s, int scale)
{
    int widest = 0;
    scale = fix_scale(scale);
    for (;;) {
        int n = line_len(s);
        if (n * GLYPH * scale > widest)
            widest = n * GLYPH * scale;
        s += n;
        if (*s != '\n')
            break;
        s++;
    }
    return widest;
}

void dock_ui_text_centered(const dock_canvas_t *c, int y, const char *s,
                           uint32_t color, int scale)
{
    scale = fix_scale(scale);
    for (;;) {
        int n = line_len(s);
        int w = n * GLYPH * scale;
        text_n(c, ((int)c->w - w) / 2, y, s, n, color, scale);
        s += n;
        if (*s != '\n')
            break;
        s++;
        y += LINE_H(scale);
    }
}

/* Word-wrap s into lines of at most cols characters; draws them when c is
 * not NULL. Returns the number of lines. */
static int wrap(const dock_canvas_t *c, int x, int y, const char *s, int cols,
                uint32_t color)
{
    int lines = 0;
    if (cols < 1)
        cols = 1;
    while (*s) {
        int n = line_len(s), take = n;
        if (n > cols) {
            int i;
            take = cols;
            for (i = cols; i > 0; i--) {
                if (s[i] == ' ') {
                    take = i;
                    break;
                }
            }
        }
        if (c)
            text_n(c, x, y + lines * LINE_H(1), s, take, color, 1);
        lines++;
        s += take;
        while (*s == ' ' && take < n) /* swallow the break's spaces */
            s++;
        if (take >= n && *s == '\n')
            s++;
    }
    return lines;
}

static void clear(const dock_canvas_t *c)
{
    dock_ui_fill(c, 0, 0, c->w, c->h, DOCK_UI_BG);
}

static void title_bar(const dock_canvas_t *c, const char *title)
{
    dock_ui_fill(c, 0, 0, c->w, TITLE_H, DOCK_UI_SEL_BG);
    dock_ui_fill(c, 0, TITLE_H - 2, c->w, 2, DOCK_UI_ACCENT);
    if (title)
        text_n(c, MARGIN, (TITLE_H - 2 - GLYPH) / 2, title,
               ((int)c->w - 2 * MARGIN) / GLYPH, DOCK_UI_FG, 1);
}

static void footer_bar(const dock_canvas_t *c, const char *footer)
{
    int y = (int)c->h - FOOTER_H;
    if (!footer || !footer[0])
        return;
    dock_ui_fill(c, 0, y, c->w, 1, DOCK_UI_DIM);
    text_n(c, MARGIN, y + (FOOTER_H - GLYPH) / 2 + 1, footer,
           ((int)c->w - 2 * MARGIN) / GLYPH, DOCK_UI_DIM, 1);
}

/* ---- screens ---- */

void dock_ui_screen_waiting(const dock_canvas_t *c, const char *status_line,
                            const char *hint)
{
    int h = (int)c->h, w = (int)c->w;
    int ty = h / 4 - 12;
    int tw = dock_ui_text_width("calcScreen", 3);
    clear(c);
    dock_ui_text_centered(c, ty, "calcScreen", DOCK_UI_ACCENT, 3);
    dock_ui_fill(c, (w - tw) / 2, ty + 30, tw, 2, DOCK_UI_SEL_BG);
    dock_ui_text_centered(c, ty + 40, "TI-84 Plus CE dock", DOCK_UI_DIM, 1);
    if (status_line)
        dock_ui_text_centered(c, h / 2 + 8, status_line, DOCK_UI_FG, 1);
    if (hint)
        dock_ui_text_centered(c, h - 28, hint, DOCK_UI_DIM, 1);
}

void dock_ui_screen_join(const dock_canvas_t *c, const char *join_code)
{
    char code[7];
    int i, h = (int)c->h, w = (int)c->w;
    int cw = 6 * GLYPH * 4, ch = GLYPH * 4;
    int cy = h / 2 - ch / 2;
    bool end = join_code == NULL;
    for (i = 0; i < 6; i++) {
        if (!end && join_code[i] == '\0')
            end = true;
        code[i] = end ? '-' : join_code[i];
    }
    code[6] = '\0';
    clear(c);
    dock_ui_text_centered(c, h / 6, "Join code", DOCK_UI_FG, 2);
    frame(c, (w - cw) / 2 - 12, cy - 12, cw + 24, ch + 24, 2, DOCK_UI_ACCENT);
    dock_ui_text_centered(c, cy, code, DOCK_UI_WARN, 4);
    dock_ui_text_centered(c, cy + ch + 28, "Enter this code on your calculator",
                          DOCK_UI_FG, 1);
    dock_ui_text_centered(c, h - 28, "Ctrl+Alt+S: settings", DOCK_UI_DIM, 1);
}

void dock_ui_screen_menu(const dock_canvas_t *c, const char *title,
                         const dock_ui_item_t *items, int n, int selected,
                         const char *footer)
{
    int w = (int)c->w, h = (int)c->h;
    int top = TITLE_H + 6;
    int bottom = h - FOOTER_H - 2;
    int visible = (bottom - top) / ROW_H;
    int first = 0, i;
    if (visible < 1)
        visible = 1;
    if (selected >= visible)
        first = selected - visible + 1;
    if (first > n - visible)
        first = n - visible > 0 ? n - visible : 0;
    clear(c);
    title_bar(c, title);
    for (i = 0; i < visible && first + i < n; i++) {
        const dock_ui_item_t *it = &items[first + i];
        int y = top + i * ROW_H;
        int vw = 0, cols;
        bool sel = first + i == selected;
        if (sel) {
            dock_ui_fill(c, 4, y, w - 8, ROW_H, DOCK_UI_SEL_BG);
            dock_ui_fill(c, 4, y, 3, ROW_H, DOCK_UI_ACCENT);
        }
        if (it->value) {
            vw = dock_ui_text_width(it->value, 1);
            if (vw > w / 2)
                vw = (w / 2 / GLYPH) * GLYPH;
            text_n(c, w - MARGIN - vw, y + (ROW_H - GLYPH) / 2, it->value,
                   vw / GLYPH, sel ? DOCK_UI_FG : DOCK_UI_ACCENT, 1);
        }
        cols = (w - 2 * MARGIN - vw - (vw ? GLYPH : 0)) / GLYPH;
        if (it->label)
            text_n(c, MARGIN, y + (ROW_H - GLYPH) / 2, it->label, cols,
                   DOCK_UI_FG, 1);
    }
    /* scroll hints, in the right margin of the first / last visible row */
    if (first > 0)
        glyph(c, w - MARGIN + 3, top + (ROW_H - GLYPH) / 2, '^', DOCK_UI_DIM, 1);
    if (first + visible < n)
        glyph(c, w - MARGIN + 3, top + (visible - 1) * ROW_H + (ROW_H - GLYPH) / 2,
              'v', DOCK_UI_DIM, 1);
    footer_bar(c, footer);
}

void dock_ui_screen_prompt(const dock_canvas_t *c, const char *title,
                           const char *text, bool masked, const char *footer)
{
    int w = (int)c->w, h = (int)c->h;
    int scale = 2, fh = GLYPH * scale + 12;
    int fx = 16, fy = h / 2 - fh / 2, fw = w - 32;
    int cols = (fw - 12) / (GLYPH * scale) - 1; /* keep room for the cursor */
    int len = text ? (int)strlen(text) : 0;
    int start = len > cols ? len - cols : 0, i, x;
    clear(c);
    title_bar(c, title);
    dock_ui_fill(c, fx, fy, fw, fh, DOCK_UI_SEL_BG);
    frame(c, fx, fy, fw, fh, 1, DOCK_UI_ACCENT);
    x = fx + 6;
    for (i = start; i < len; i++) {
        glyph(c, x, fy + 6, masked ? '*' : text[i], DOCK_UI_FG, scale);
        x += GLYPH * scale;
    }
    /* cursor */
    dock_ui_fill(c, x + 1, fy + 5, 2, GLYPH * scale + 2, DOCK_UI_ACCENT);
    footer_bar(c, footer);
}

void dock_ui_screen_message(const dock_canvas_t *c, const char *title,
                            const char *body)
{
    int w = (int)c->w, h = (int)c->h;
    int cols = (w - 2 * 16) / GLYPH;
    int lines, y;
    clear(c);
    title_bar(c, title);
    if (!body)
        return;
    lines = wrap(NULL, 0, 0, body, cols, 0);
    y = TITLE_H + (h - TITLE_H - lines * LINE_H(1)) / 2;
    if (y < TITLE_H + 8)
        y = TITLE_H + 8;
    wrap(c, 16, y, body, cols, DOCK_UI_FG);
}
