/* test_ui.c - renders the dock screens to PPM (tests/out/) and checks
 * invariants: clipping, palette, glyph bits, layout regions. */
#include "../core/dock_ui.h"
#include "test.h"

#define W 320
#define H 240
#define PAD 16 /* guard pixels around the canvas */
#define STRIDE (W + 2 * PAD)
#define CANARY 0xDEADBEEFu

static uint32_t mem[(H + 2 * PAD) * STRIDE];
static dock_canvas_t cv;

static void canvas_init(uint16_t w, uint16_t h)
{
    size_t i;
    for (i = 0; i < sizeof mem / sizeof mem[0]; i++)
        mem[i] = CANARY;
    cv.px = mem + PAD * STRIDE + PAD;
    cv.w = w;
    cv.h = h;
    cv.stride = STRIDE;
}

static uint32_t px(int x, int y)
{
    return cv.px[(size_t)y * cv.stride + (size_t)x];
}

/* Every pixel outside the w x h canvas must still hold the canary. */
static bool canaries_ok(void)
{
    int x, y;
    for (y = -PAD; y < H + PAD; y++) {
        for (x = -PAD; x < W + PAD; x++) {
            bool inside = x >= 0 && y >= 0 && x < (int)cv.w && y < (int)cv.h;
            uint32_t v = mem[(size_t)(y + PAD) * STRIDE + (size_t)(x + PAD)];
            if (!inside && v != CANARY)
                return false;
        }
    }
    return true;
}

/* Every canvas pixel is one of the UI colours. */
static bool palette_ok(void)
{
    static const uint32_t ok[] = {DOCK_UI_BG,     DOCK_UI_FG,   DOCK_UI_DIM,
                                  DOCK_UI_ACCENT, DOCK_UI_WARN, DOCK_UI_SEL_BG};
    int x, y;
    size_t i;
    for (y = 0; y < cv.h; y++) {
        for (x = 0; x < cv.w; x++) {
            uint32_t v = px(x, y);
            bool found = false;
            for (i = 0; i < sizeof ok / sizeof ok[0]; i++)
                found = found || v == ok[i];
            if (!found)
                return false;
        }
    }
    return true;
}

static unsigned count(int x0, int y0, int x1, int y1, uint32_t color)
{
    int x, y;
    unsigned n = 0;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            if (px(x, y) == color)
                n++;
    return n;
}

static void save_ppm(const char *name)
{
    char path[128];
    FILE *f;
    int x, y;
    snprintf(path, sizeof path, "out/%s.ppm", name);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "  (could not write %s; run from tests/)\n", path);
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", cv.w, cv.h);
    for (y = 0; y < cv.h; y++) {
        for (x = 0; x < cv.w; x++) {
            uint32_t v = px(x, y);
            fputc((int)((v >> 16) & 0xFF), f);
            fputc((int)((v >> 8) & 0xFF), f);
            fputc((int)(v & 0xFF), f);
        }
    }
    fclose(f);
}

/* ---- tests ---- */

static void t_glyphs(void)
{
    static const uint8_t A[8] = {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00};
    int x, y, w;
    canvas_init(W, H);
    dock_ui_fill(&cv, 0, 0, W, H, 0);
    w = dock_ui_text(&cv, 0, 0, "A", 0xFFFFFFFFu, 1);
    CHECK_EQ(w, 8);
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++)
            CHECK_EQ(px(x, y) != 0, (A[y] >> x) & 1);
    /* scale 3 = pixel replication */
    w = dock_ui_text(&cv, 100, 100, "A", 0xFFFFFFFFu, 3);
    CHECK_EQ(w, 24);
    for (y = 0; y < 24; y++)
        for (x = 0; x < 24; x++)
            CHECK_EQ(px(100 + x, 100 + y) != 0, (A[y / 3] >> (x / 3)) & 1);
    /* '\n' starts a new line, width = longest line */
    CHECK_EQ(dock_ui_text_width("ab\ncdef\ng", 2), 64);
    CHECK_EQ(dock_ui_text_width("", 1), 0);
    CHECK_EQ(dock_ui_text_width("abc", 0), 24); /* scale < 1 -> 1 */
    w = dock_ui_text(&cv, 0, 50, "xy\nlonger", 0xFFFFFFFFu, 1);
    CHECK_EQ(w, 48);
    CHECK(canaries_ok());
}

static void t_clipping(void)
{
    canvas_init(W, H);
    dock_ui_fill(&cv, 0, 0, W, H, DOCK_UI_BG);
    dock_ui_fill(&cv, -50, -50, 1000, 1000, DOCK_UI_FG);
    dock_ui_fill(&cv, 10, 10, -5, 5, DOCK_UI_WARN);
    dock_ui_text(&cv, -20, -4, "clipped text", DOCK_UI_ACCENT, 3);
    dock_ui_text(&cv, 300, 230, "edge", DOCK_UI_ACCENT, 4);
    dock_ui_text(&cv, -100000, 5, "far", DOCK_UI_ACCENT, 1);
    dock_ui_text_centered(&cv, 200, "a very very very very very long line that "
                          "is far wider than the canvas", DOCK_UI_ACCENT, 2);
    dock_ui_text(&cv, 0, 0, "\x01\x7f\xff", DOCK_UI_ACCENT, 1); /* non-ASCII */
    CHECK(canaries_ok());
    CHECK_EQ(count(0, 0, W, H, DOCK_UI_WARN), 0);

    /* A small canvas: all screens must stay inside. */
    {
        static const dock_ui_item_t items[3] = {
            {"Wi-Fi", "On"}, {"A label that is much too long", "Value too"},
            {"Third", NULL}};
        canvas_init(100, 40);
        dock_ui_screen_waiting(&cv, "status line that is long", "hint");
        CHECK(canaries_ok());
        dock_ui_screen_join(&cv, "123456");
        CHECK(canaries_ok());
        dock_ui_screen_menu(&cv, "Settings", items, 3, 2, "footer");
        CHECK(canaries_ok());
        dock_ui_screen_prompt(&cv, "Password", "secret", true, "Enter");
        CHECK(canaries_ok());
        dock_ui_screen_message(&cv, "Note", "word wrapped body text here");
        CHECK(canaries_ok());
        canvas_init(1, 1);
        dock_ui_screen_menu(&cv, "x", items, 3, 1, NULL);
        CHECK(canaries_ok());
    }
}

static void t_waiting(void)
{
    canvas_init(W, H);
    dock_ui_screen_waiting(&cv, "Waiting for a calculator...",
                           "Plug in a TI-84 Plus CE");
    save_ppm("waiting");
    CHECK(canaries_ok());
    CHECK(palette_ok());
    CHECK(count(0, 0, W, H / 2, DOCK_UI_ACCENT) > 200); /* big title */
    CHECK(count(0, H / 2, W, H / 2 + 20, DOCK_UI_FG) > 50); /* status */
    CHECK(count(0, H - 30, W, H, DOCK_UI_DIM) > 50);      /* hint */
    CHECK_EQ(px(0, 0), DOCK_UI_BG);
    CHECK_EQ(px(W - 1, H - 1), DOCK_UI_BG);
    dock_ui_screen_waiting(&cv, NULL, NULL);
    CHECK(canaries_ok());
}

static void t_join(void)
{
    int x0 = (W - 192) / 2, y0 = H / 2 - 16;
    canvas_init(W, H);
    dock_ui_screen_join(&cv, "042917");
    save_ppm("join");
    CHECK(canaries_ok());
    CHECK(palette_ok());
    /* the 6 digits at scale 4: a 192x32 block in the middle */
    CHECK(count(x0, y0, x0 + 192, y0 + 32, DOCK_UI_WARN) > 600);
    CHECK_EQ(count(0, 0, W, y0 - 1, DOCK_UI_WARN), 0);
    CHECK(count(0, y0 + 40, W, y0 + 72, DOCK_UI_FG) > 100); /* instruction */
    /* each digit cell has ink */
    {
        int i;
        for (i = 0; i < 6; i++)
            CHECK(count(x0 + 32 * i, y0, x0 + 32 * i + 32, y0 + 32, DOCK_UI_WARN) > 40);
    }
    dock_ui_screen_join(&cv, NULL); /* "------" */
    CHECK(count(x0, y0, x0 + 192, y0 + 32, DOCK_UI_WARN) > 0);
    dock_ui_screen_join(&cv, "12"); /* short code: padded, no overread */
    CHECK(canaries_ok());
}

static void t_menu(void)
{
    static const dock_ui_item_t items[] = {
        {"Wi-Fi", "On"},           {"Choose network", "home-net"},
        {"Network API", "Off"},    {"Require join code", "Off"},
        {"Join code", "042917"},   {"Regenerate join code", NULL},
        {"Audio output", "HDMI"},  {"Admin password", "Not set"},
        {"Drop to shell", NULL},   {"Reboot", NULL},
    };
    int n = (int)(sizeof items / sizeof items[0]);
    int top = 20 + 6, row = 14;
    canvas_init(W, H);
    dock_ui_screen_menu(&cv, "Dock settings", items, n, 2, "Up/Down Enter Esc");
    save_ppm("menu");
    CHECK(canaries_ok());
    CHECK(palette_ok());
    /* title bar */
    CHECK(count(0, 0, W, 18, DOCK_UI_SEL_BG) > W * 10);
    CHECK(count(0, 0, W, 18, DOCK_UI_FG) > 50);
    /* selected row 2 is highlighted, row 1 is not */
    CHECK(count(0, top + 2 * row, W, top + 3 * row, DOCK_UI_SEL_BG) > W * 8);
    CHECK_EQ(count(8, top + row, W - 8, top + 2 * row, DOCK_UI_SEL_BG), 0);
    /* values are right-aligned: ink near the right edge on row 0 */
    CHECK(count(W - 40, top, W - 12, top + row, DOCK_UI_ACCENT) > 10);
    CHECK(count(W - 11, top, W, top + row, DOCK_UI_ACCENT) == 0);
    /* footer */
    CHECK(count(0, H - 14, W, H, DOCK_UI_DIM) > 50);

    /* Many items: the selection scrolls into view. */
    {
        static dock_ui_item_t many[40];
        static char labels[40][24];
        int i, visible = (H - 16 - 2 - top) / row;
        for (i = 0; i < 40; i++) {
            snprintf(labels[i], sizeof labels[i], "Item %d", i);
            many[i].label = labels[i];
            many[i].value = (i % 3) ? "v" : NULL;
        }
        dock_ui_screen_menu(&cv, "Long list", many, 40, 35, NULL);
        save_ppm("menu_scrolled");
        CHECK(canaries_ok());
        /* selected item is the last visible row */
        CHECK(count(0, top + (visible - 1) * row, W, top + visible * row,
                    DOCK_UI_SEL_BG) > W * 8);
        dock_ui_screen_menu(&cv, "Empty", many, 0, 0, NULL);
        CHECK(canaries_ok());
    }
}

static void t_prompt(void)
{
    static uint32_t a[W * H];
    int y;
    canvas_init(W, H);
    dock_ui_screen_prompt(&cv, "Admin password", "hunter2", true,
                          "Enter: OK  Esc: cancel");
    save_ppm("prompt_masked");
    CHECK(canaries_ok());
    CHECK(palette_ok());
    for (y = 0; y < H; y++)
        memcpy(a + y * W, cv.px + (size_t)y * cv.stride, W * sizeof(uint32_t));
    /* masking hides the content: same length, same picture */
    dock_ui_screen_prompt(&cv, "Admin password", "abcdefg", true,
                          "Enter: OK  Esc: cancel");
    {
        bool same = true;
        for (y = 0; y < H; y++)
            same = same && memcmp(a + y * W, cv.px + (size_t)y * cv.stride,
                                  W * sizeof(uint32_t)) == 0;
        CHECK(same);
    }
    dock_ui_screen_prompt(&cv, "Wi-Fi password", "hunter2", false, NULL);
    save_ppm("prompt_plain");
    {
        bool same = true;
        for (y = 0; y < H; y++)
            same = same && memcmp(a + y * W, cv.px + (size_t)y * cv.stride,
                                  W * sizeof(uint32_t)) == 0;
        CHECK(!same);
    }
    /* cursor present in the field */
    CHECK(count(16, H / 2 - 14, W - 16, H / 2 + 14, DOCK_UI_ACCENT) > 30);
    /* a very long text scrolls (no overflow) */
    dock_ui_screen_prompt(&cv, "Long", "0123456789012345678901234567890123456789",
                          false, NULL);
    CHECK(canaries_ok());
    dock_ui_screen_prompt(&cv, "Empty", "", false, NULL);
    CHECK(canaries_ok());
    dock_ui_screen_prompt(&cv, "Null", NULL, true, NULL);
    CHECK(canaries_ok());
}

static void t_message(void)
{
    canvas_init(W, H);
    dock_ui_screen_message(&cv, "Shell",
                           "The root filesystem is a read-only overlay. Run "
                           "dock-unlock to make permanent changes.\n\nType exit "
                           "to return to the dock. AVeryLongWordWithoutAnySpaces"
                           "ThatMustBeHardWrapped.");
    save_ppm("message");
    CHECK(canaries_ok());
    CHECK(palette_ok());
    CHECK(count(0, 24, W, H, DOCK_UI_FG) > 300);
    /* left margin respected */
    CHECK_EQ(count(0, 24, 16, H, DOCK_UI_FG), 0);
    CHECK_EQ(count(W - 15, 24, W, H, DOCK_UI_FG), 0);
    dock_ui_screen_message(&cv, NULL, NULL);
    CHECK(canaries_ok());
}

void suite_ui(void)
{
    RUN(t_glyphs);
    RUN(t_clipping);
    RUN(t_waiting);
    RUN(t_join);
    RUN(t_menu);
    RUN(t_prompt);
    RUN(t_message);
}
