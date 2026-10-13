/* test_keymap.c - HID -> TI key translation, hotkeys and ASCII. */
#include "../core/dock_keymap.h"
#include "../core/dock_proto.h"
#include "test.h"

#define SH DOCK_MOD_LSHIFT
#define CT DOCK_MOD_LCTRL
#define AL DOCK_MOD_LALT

/* HID usages used below */
enum {
    U_A = 0x04, U_N = 0x11, U_R = 0x15, U_S = 0x16, U_Z = 0x1D,
    U_1 = 0x1E, U_6 = 0x23, U_8 = 0x25, U_9 = 0x26, U_0 = 0x27,
    U_ENTER = 0x28, U_ESC = 0x29, U_BS = 0x2A, U_TAB = 0x2B, U_SPACE = 0x2C,
    U_MINUS = 0x2D, U_EQUAL = 0x2E, U_LBR = 0x2F, U_RBR = 0x30, U_BSL = 0x31,
    U_SEMI = 0x33, U_APOS = 0x34, U_GRAVE = 0x35, U_COMMA = 0x36, U_DOT = 0x37,
    U_SLASH = 0x38, U_CAPS = 0x39, U_F1 = 0x3A, U_F6 = 0x3F, U_F11 = 0x44,
    U_F12 = 0x45, U_PAUSE = 0x48, U_INS = 0x49, U_HOME = 0x4A, U_PGUP = 0x4B,
    U_DEL = 0x4C, U_END = 0x4D, U_PGDN = 0x4E, U_RIGHT = 0x4F, U_LEFT = 0x50,
    U_DOWN = 0x51, U_UP = 0x52, U_KPSLASH = 0x54, U_KPPLUS = 0x57,
    U_KPENTER = 0x58, U_KP1 = 0x59, U_KP5 = 0x5D, U_KP0 = 0x62, U_KPDOT = 0x63
};

static void expect(uint8_t usage, uint8_t mods, uint16_t tikey, uint8_t sc,
                   int line)
{
    dock_ti_key_t k;
    bool ok;
    memset(&k, 0xAA, sizeof k);
    ok = dock_keymap_translate(usage, mods, &k);
    t_checks++;
    if (!ok || k.tikey != tikey || k.scancode != sc || k.flags != 0) {
        t_failures++;
        fprintf(stderr, "test_keymap.c:%d: usage 0x%02x mods 0x%02x -> %s "
                "tikey 0x%04x sc 0x%02x flags %u, want 0x%04x 0x%02x\n",
                line, usage, mods, ok ? "true" : "false", k.tikey, k.scancode,
                k.flags, tikey, sc);
    }
}
#define EXPECT(u, m, t, s) expect((uint8_t)(u), (uint8_t)(m), (uint16_t)(t), (uint8_t)(s), __LINE__)

static void expect_none(uint8_t usage, uint8_t mods, int line)
{
    dock_ti_key_t k;
    t_checks++;
    if (dock_keymap_translate(usage, mods, &k)) {
        t_failures++;
        fprintf(stderr, "test_keymap.c:%d: usage 0x%02x mods 0x%02x should not "
                "map (got 0x%04x)\n", line, usage, mods, k.tikey);
    }
}
#define NONE(u, m) expect_none((uint8_t)(u), (uint8_t)(m), __LINE__)

static void t_letters(void)
{
    /* Scan code of the key carrying each ALPHA letter (getcsc.h sk_*). */
    static const uint8_t sk[26] = {
        0x2F, 0x27, 0x1F, 0x2E, 0x26, 0x1E, 0x16, 0x0E, 0x2D, 0x25, 0x1D, 0x15, 0x0D,
        0x2C, 0x24, 0x1C, 0x14, 0x0C, 0x2B, 0x23, 0x1B, 0x13, 0x0B, 0x2A, 0x22, 0x1A};
    unsigned i, j;
    for (i = 0; i < 26; i++) {
        EXPECT(U_A + i, 0, 0x9A + i, sk[i]);                 /* k_CapA.. */
        EXPECT(U_A + i, DOCK_MOD_RALT, 0x9A + i, sk[i]);     /* ALPHA fn */
        EXPECT(U_A + i, SH, 0xFCE2 + i, sk[i]);              /* KEY83P_La.. */
        EXPECT(U_A + i, DOCK_MOD_RSHIFT, 0xFCE2 + i, sk[i]);
        for (j = 0; j < i; j++)
            CHECK(sk[i] != sk[j]);
    }
    EXPECT(U_N, SH, 0xFCEF, 0x2C); /* KEY83P_Lsmalln */
    EXPECT(U_R, SH, 0xFCF3, 0x0C); /* KEY83P_Lsmallr */
    EXPECT(U_Z, SH, 0xFCFB, 0x1A); /* KEY83P_Lz */
    /* Ctrl+letter = 2nd function of the key under the letter */
    EXPECT(U_A, CT, 0x33, 0x2F);       /* TEST */
    EXPECT(U_A + 1, CT, 0x39, 0x27);   /* ANGLE */
    EXPECT(U_A + 2, CT, 0x2F, 0x1F);   /* DRAW */
    EXPECT(U_A + 3, CT, 0x37, 0x2E);   /* MATRIX */
    EXPECT(U_A + 7, CT, 0xB5, 0x0E);   /* pi */
    EXPECT(U_A + 8, DOCK_MOD_RCTRL, 0xBE, 0x2D); /* sqrt */
    EXPECT(U_S, CT, 0xC0, 0x2B);       /* e^x */
    EXPECT(U_A + 23, CT, 0x0C, 0x2A);  /* X: STO -> RCL */
}

static void t_digits_ops(void)
{
    static const uint8_t sk[10] = {0x21, 0x22, 0x1A, 0x12, 0x23,
                                   0x1B, 0x13, 0x24, 0x1C, 0x14};
    unsigned d;
    for (d = 1; d <= 9; d++) {
        EXPECT(U_1 + d - 1, 0, 0x8E + d, sk[d]);
        EXPECT(U_KP1 + d - 1, 0, 0x8E + d, sk[d]);
    }
    EXPECT(U_0, 0, 0x8E, 0x21);
    EXPECT(U_KP0, 0, 0x8E, 0x21);
    EXPECT(U_0, CT, 0x3E, 0x21);       /* CATALOG */
    EXPECT(U_0, AL, 0x99, 0x21);       /* space */
    EXPECT(U_1, CT, 0xF3, 0x22);       /* L1 */
    EXPECT(U_6 - 2, CT, 0xF6, 0x23);   /* '4' -> L4 */
    EXPECT(U_1 + 6, CT, 0xF9, 0x24);   /* '7' -> u */
    EXPECT(U_1, AL, 0xB2, 0x22);       /* Y */
    EXPECT(U_1 + 2, AL, 0xCC, 0x12);   /* '3' -> theta */

    EXPECT(U_EQUAL, SH, 0x80, 0x0A);   /* + */
    EXPECT(U_KPPLUS, 0, 0x80, 0x0A);
    EXPECT(U_MINUS, 0, 0x81, 0x0B);    /* - */
    EXPECT(U_8, SH, 0x82, 0x0C);       /* * */
    EXPECT(U_SLASH, 0, 0x83, 0x0D);    /* / */
    EXPECT(U_KPSLASH, 0, 0x83, 0x0D);
    EXPECT(U_6, SH, 0x84, 0x0E);       /* ^ */
    EXPECT(U_9, SH, 0x85, 0x1D);       /* ( */
    EXPECT(U_0, SH, 0x86, 0x15);       /* ) */
    EXPECT(U_COMMA, 0, 0x8B, 0x25);
    EXPECT(U_DOT, 0, 0x8D, 0x19);
    EXPECT(U_KPDOT, 0, 0x8D, 0x19);
    EXPECT(U_MINUS, SH, 0x8C, 0x11);   /* (-) */
    EXPECT(U_MINUS, SH | CT, 0xC5, 0x11); /* ANS */
    EXPECT(U_6, SH | CT, 0xB5, 0x0E);  /* 2nd ^ = pi */
    EXPECT(U_EQUAL, SH | CT, 0x36, 0x0A); /* MEM */
}

static void t_symbols(void)
{
    EXPECT(U_EQUAL, 0, 0xFE0A, 0);     /* = KEY83P_TEqu */
    EXPECT(U_COMMA, SH, 0xFE0E, 0);    /* < */
    EXPECT(U_DOT, SH, 0xFE0C, 0);      /* > */
    EXPECT(U_1, SH, 0xFE24, 0);        /* ! KEY83P_XFactorial */
    EXPECT(U_LBR, 0, 0x87, 0x0C);      /* [ (2nd x) */
    EXPECT(U_RBR, 0, 0x88, 0x0B);      /* ] (2nd -) */
    EXPECT(U_LBR, SH, 0xEC, 0x1D);     /* { */
    EXPECT(U_RBR, SH, 0xED, 0x15);     /* } */
    EXPECT(U_APOS, SH, 0xCB, 0x0A);    /* " (ALPHA +) */
    EXPECT(U_SEMI, SH, 0xC6, 0x19);    /* : (ALPHA .) */
    EXPECT(U_SLASH, SH, 0xCA, 0x11);   /* ? (ALPHA (-)) */
    EXPECT(U_SPACE, 0, 0x99, 0x21);    /* space (ALPHA 0) */
    EXPECT(U_SPACE, CT, 0x99, 0x21);   /* modifiers ignored */
    EXPECT(U_1 + 1, SH, 0xFB02, 0);    /* @ */
    EXPECT(U_1 + 2, SH, 0xFB03, 0);    /* # */
    EXPECT(U_1 + 3, SH, 0xFB04, 0);    /* $ */
    EXPECT(U_1 + 4, SH, 0xFB0C, 0);    /* % */
    EXPECT(U_1 + 6, SH, 0xFB05, 0);    /* & */
    EXPECT(U_SEMI, 0, 0xFB07, 0);      /* ; */
    EXPECT(U_BSL, 0, 0xFB08, 0);       /* \ */
    EXPECT(U_BSL, SH, 0xFB09, 0);      /* | */
    EXPECT(U_GRAVE, 0, 0xFB06, 0);     /* ` */
    EXPECT(U_GRAVE, SH, 0xFB0B, 0);    /* ~ */
    EXPECT(U_APOS, 0, 0xFB01, 0);      /* ' */
}

static void t_function_keys(void)
{
    static const uint16_t code[11] = {0x49, 0x48, 0x2E, 0x5A, 0x44, 0x45,
                                      0x31, 0x32, 0x2C, 0x2D, 0x35};
    static const uint8_t sk[11] = {0x35, 0x34, 0x33, 0x32, 0x31, 0x37,
                                   0x20, 0x2F, 0x27, 0x1F, 0x17};
    static const uint16_t second[11] = {0x55, 0x4B, 0x57, 0x3B, 0x4A, 0x40,
                                        0x3A, 0x33, 0x39, 0x2F, 0x38};
    unsigned i;
    for (i = 0; i < 11; i++) {
        EXPECT(U_F1 + i, 0, code[i], sk[i]);
        EXPECT(U_F1 + i, CT, second[i], sk[i]);
    }
    EXPECT(U_F6, SH, 0x45, 0x37); /* shift ignored */
    EXPECT(U_F1 + 7, AL, 0x9A, 0x2F); /* Alt+F8 = ALPHA MATH = A */
    NONE(U_F1, AL);   /* Y= has no ALPHA GetKey code */
    NONE(U_F11, AL);
}

static void t_editing_keys(void)
{
    dock_ti_key_t k;
    EXPECT(U_ENTER, 0, 0x05, 0x09);
    EXPECT(U_KPENTER, 0, 0x05, 0x09);
    EXPECT(U_ENTER, CT, 0x0D, 0x09);  /* ENTRY */
    EXPECT(U_ENTER, AL, 0x06, 0x09);  /* SOLVE */
    EXPECT(U_BS, 0, 0x0A, 0x38);
    EXPECT(U_DEL, 0, 0x0A, 0x38);
    EXPECT(U_INS, 0, 0x0B, 0x38);
    EXPECT(U_BS, CT, 0x0B, 0x38);
    EXPECT(U_ESC, 0, 0x09, 0x0F);
    EXPECT(U_RIGHT, 0, 0x01, 0x03);
    EXPECT(U_LEFT, 0, 0x02, 0x02);
    EXPECT(U_UP, 0, 0x03, 0x04);
    EXPECT(U_DOWN, 0, 0x04, 0x01);
    EXPECT(U_HOME, 0, 0x0E, 0x02);
    EXPECT(U_END, 0, 0x0F, 0x03);
    EXPECT(U_LEFT, CT, 0x0E, 0x02);
    EXPECT(U_PGUP, 0, 0x07, 0x04);
    EXPECT(U_PGDN, 0, 0x08, 0x01);
    EXPECT(U_UP, AL, 0x07, 0x04);
    NONE(U_UP, CT);   /* 2nd+UP: brightness, no key code */
    NONE(U_ESC, CT);
    /* Tab = the ALPHA key itself: scan code only */
    EXPECT(U_TAB, 0, 0, 0x30);
    EXPECT(U_TAB, CT, 0, 0x30);
    /* Pause = ON */
    memset(&k, 0, sizeof k);
    CHECK(dock_keymap_translate(U_PAUSE, 0, &k));
    CHECK_EQ(k.flags, DOCK_KEYF_ON);
    CHECK_EQ(k.tikey, 0);
    CHECK_EQ(k.scancode, 0);
}

static void t_hotkeys_and_unmapped(void)
{
    unsigned u, m;
    CHECK_EQ(dock_keymap_hotkey(U_F12, 0), DOCK_HOTKEY_SETTINGS);
    CHECK_EQ(dock_keymap_hotkey(U_F12, SH), DOCK_HOTKEY_SETTINGS);
    CHECK_EQ(dock_keymap_hotkey(U_S, CT | AL), DOCK_HOTKEY_SETTINGS);
    CHECK_EQ(dock_keymap_hotkey(U_S, DOCK_MOD_RCTRL | DOCK_MOD_RALT), DOCK_HOTKEY_SETTINGS);
    CHECK_EQ(dock_keymap_hotkey(U_R, CT | AL), DOCK_HOTKEY_REFRESH);
    CHECK_EQ(dock_keymap_hotkey(U_S, CT), DOCK_HOTKEY_NONE);
    CHECK_EQ(dock_keymap_hotkey(U_S, AL), DOCK_HOTKEY_NONE);
    CHECK_EQ(dock_keymap_hotkey(U_A, 0), DOCK_HOTKEY_NONE);
    NONE(U_F12, 0);
    NONE(U_S, CT | AL);
    NONE(U_R, CT | AL | SH);
    NONE(U_CAPS, 0);
    NONE(0, 0);
    NONE(0xE0, 0); /* modifier keys themselves */
    NONE(0xFF, 0);

    /* Exhaustive: every mapped key yields something sendable. */
    for (u = 0; u < 256; u++) {
        for (m = 0; m < 256; m += 1) {
            dock_ti_key_t k;
            if (dock_keymap_translate((uint8_t)u, (uint8_t)m, &k)) {
                t_checks++;
                if (!k.tikey && !k.scancode && !k.flags) {
                    t_failures++;
                    fprintf(stderr, "empty mapping for usage 0x%02x mods 0x%02x\n",
                            u, m);
                }
                if (dock_keymap_hotkey((uint8_t)u, (uint8_t)m) != DOCK_HOTKEY_NONE) {
                    t_failures++;
                    fprintf(stderr, "hotkey 0x%02x/0x%02x also translated\n", u, m);
                }
            }
        }
    }
}

static void t_ascii(void)
{
    static const char *row = "1234567890";
    static const char *srow = "!@#$%^&*()";
    static const char *punct = "-=[]\\\\;'`,./";
    static const char *spunct = "_+{}||:\"~<>?";
    unsigned i;
    for (i = 0; i < 26; i++) {
        CHECK_EQ(dock_keymap_ascii((uint8_t)(U_A + i), 0), 'a' + i);
        CHECK_EQ(dock_keymap_ascii((uint8_t)(U_A + i), SH), 'A' + i);
        CHECK_EQ(dock_keymap_ascii((uint8_t)(U_A + i), CT), i + 1);
    }
    for (i = 0; i < 10; i++) {
        CHECK_EQ(dock_keymap_ascii((uint8_t)(U_1 + i), 0), row[i]);
        CHECK_EQ(dock_keymap_ascii((uint8_t)(U_1 + i), DOCK_MOD_RSHIFT), srow[i]);
    }
    for (i = 0; i < 12; i++) {
        CHECK_EQ(dock_keymap_ascii((uint8_t)(U_MINUS + i), 0), punct[i]);
        CHECK_EQ(dock_keymap_ascii((uint8_t)(U_MINUS + i), SH), spunct[i]);
    }
    CHECK_EQ(dock_keymap_ascii(U_ENTER, 0), 13);
    CHECK_EQ(dock_keymap_ascii(U_KPENTER, 0), 13);
    CHECK_EQ(dock_keymap_ascii(U_BS, 0), 8);
    CHECK_EQ(dock_keymap_ascii(U_ESC, 0), 27);
    CHECK_EQ(dock_keymap_ascii(U_TAB, 0), 9);
    CHECK_EQ(dock_keymap_ascii(U_SPACE, SH), ' ');
    CHECK_EQ(dock_keymap_ascii(U_DEL, 0), 127);
    CHECK_EQ(dock_keymap_ascii(U_KP5, 0), '5');
    CHECK_EQ(dock_keymap_ascii(U_KP0, 0), '0');
    CHECK_EQ(dock_keymap_ascii(U_KPDOT, 0), '.');
    CHECK_EQ(dock_keymap_ascii(U_KPSLASH, 0), '/');
    CHECK_EQ(dock_keymap_ascii(U_F1, 0), 0);
    CHECK_EQ(dock_keymap_ascii(U_UP, 0), 0);
    CHECK_EQ(dock_keymap_ascii(0xE1, 0), 0);
}

void suite_keymap(void)
{
    RUN(t_letters);
    RUN(t_digits_ops);
    RUN(t_symbols);
    RUN(t_function_keys);
    RUN(t_editing_keys);
    RUN(t_hotkeys_and_unmapped);
    RUN(t_ascii);
}
