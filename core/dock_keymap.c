/*
 * dock_keymap.c - PC keyboard (USB HID usages) to TI-84 Plus CE keys.
 *
 * Key code sources (core/ must not include TI headers, so the values we need
 * are copied here and cross-checked):
 *   [GK]  CE C toolchain v15, include/ti/getkey.h  (k_* GetKey codes)
 *   [CSC] CE C toolchain v15, include/ti/getcsc.h  (sk_* GetCSC scan codes)
 *   [LT]  libticalcs2 src/keys83p.h (KEY83P_*, TI's ti83plus.inc equates)
 * One-byte codes are identical in [GK] and [LT].
 *
 * Two-byte keys: TI-OS returns a prefix byte (kbdKey = 0xFE k_ExtendEcho,
 * 0xFC k_ExtendEcho2, 0xFB k_ExtendEcho3) plus a second byte (keyExtend).
 * We write them as 0xPPXX with the PREFIX IN THE HIGH BYTE, the [LT]
 * convention (KEY83P_La = 0xFCE2), which is also what the DUSB "execute key"
 * command takes (dusb.c sends keyExtend first, kbdKey second). Note [GK]
 * spells the same keys the other way round: k_La = k_ExtendEcho2 + (0xE2<<8)
 * = 0xE2FC. The calculator-side driver must swap bytes if it compares with
 * the toolchain's k_* constants.
 */
#include "dock_keymap.h"
#include "dock_proto.h"

/* ---- GetKey codes, one byte ([GK] == [LT]) ---- */
#define K_RIGHT 0x01u
#define K_LEFT 0x02u
#define K_UP 0x03u
#define K_DOWN 0x04u
#define K_ENTER 0x05u
#define K_ALPHAENTER 0x06u /* ALPHA+ENTER: SOLVE */
#define K_ALPHAUP 0x07u    /* ALPHA+UP: page up in menus/editors */
#define K_ALPHADOWN 0x08u
#define K_CLEAR 0x09u
#define K_DEL 0x0Au
#define K_INS 0x0Bu
#define K_RECALL 0x0Cu
#define K_LASTENT 0x0Du /* ENTRY */
#define K_BOL 0x0Eu
#define K_EOL 0x0Fu
#define K_APPSMENU 0x2Cu
#define K_PRGM 0x2Du
#define K_ZOOM 0x2Eu
#define K_DRAW 0x2Fu
#define K_STAT 0x31u
#define K_MATH 0x32u
#define K_TEST 0x33u
#define K_VARS 0x35u
#define K_MEM 0x36u
#define K_MATRIX 0x37u
#define K_DIST 0x38u
#define K_ANGLE 0x39u
#define K_LIST 0x3Au
#define K_CALC 0x3Bu
#define K_CATALOG 0x3Eu
#define K_QUIT 0x40u
#define K_LINKIO 0x41u
#define K_GRAPH 0x44u
#define K_MODE 0x45u
#define K_WINDOW 0x48u
#define K_YEQU 0x49u
#define K_TABLE 0x4Au
#define K_TBLSET 0x4Bu
#define K_STATP 0x55u
#define K_FORMAT 0x57u
#define K_TRACE 0x5Au
#define K_ADD 0x80u
#define K_SUB 0x81u
#define K_MUL 0x82u
#define K_DIV 0x83u
#define K_EXPON 0x84u
#define K_LPAREN 0x85u
#define K_RPAREN 0x86u
#define K_LBRACK 0x87u
#define K_RBRACK 0x88u
#define K_STORE 0x8Au
#define K_COMMA 0x8Bu
#define K_CHS 0x8Cu /* (-) */
#define K_DECPNT 0x8Du
#define K_0 0x8Eu   /* k_0..k_9 = 0x8E..0x97 */
#define K_EE 0x98u
#define K_SPACE 0x99u
#define K_CAPA 0x9Au /* k_CapA..k_CapZ = 0x9A..0xB3 */
#define K_VARX 0xB4u /* X,T,theta,n */
#define K_PI 0xB5u
#define K_INV 0xB6u
#define K_SIN 0xB7u
#define K_ASIN 0xB8u
#define K_COS 0xB9u
#define K_ACOS 0xBAu
#define K_TAN 0xBBu
#define K_ATAN 0xBCu
#define K_SQUARE 0xBDu
#define K_SQRT 0xBEu
#define K_LN 0xBFu
#define K_EXP 0xC0u
#define K_LOG 0xC1u
#define K_ALOG 0xC2u
#define K_ANS 0xC5u
#define K_COLON 0xC6u
#define K_QUEST 0xCAu
#define K_QUOTE 0xCBu
#define K_THETA 0xCCu
#define K_LBRACE 0xECu
#define K_RBRACE 0xEDu
#define K_I 0xEEu
#define K_CONSTEA 0xEFu /* e */
#define K_L1A 0xF3u     /* L1..L6 = 0xF3..0xF8 */
#define K_UNA 0xF9u     /* u, v, w = 0xF9..0xFB */

/* ---- GetKey codes, two bytes (prefix in the high byte) ---- */
/* 0xFE (k_ExtendEcho): [GK] k_E1BT+(n<<8), [LT] 0xFE00+n */
#define K_TEQU 0xFE0Au       /* =  (TEST menu)  KEY83P_TEqu */
#define K_TNOTEQ 0xFE0Bu     /* != */
#define K_TGT 0xFE0Cu        /* >  */
#define K_TGTE 0xFE0Du       /* >= */
#define K_TLT 0xFE0Eu        /* <  */
#define K_TLTE 0xFE0Fu       /* <= */
#define K_XFACTORIAL 0xFE24u /* !  KEY83P_XFactorial */
/* 0xFC (k_ExtendEcho2): lowercase letters, [GK] k_La..k_Lz, [LT] KEY83P_La
 * (0xFCE2) .. KEY83P_Lz (0xFCFB), contiguous (n is k_Lsmalln, r k_Lsmallr). */
#define K_LA 0xFCE2u
/* 0xFB (k_ExtendEcho3): ASCII punctuation added for the TI keyboard,
 * [GK] k_E2BT3+(n<<8), [LT] "key codes added in OS 1.15" 0xFB00+n. */
#define K_SINGLEQUOTE 0xFB01u /* [GK] k_SingleQuote; [LT] calls 0xFB01 "Reserved" */
#define K_AT 0xFB02u
#define K_HASH 0xFB03u
#define K_DOLLAR 0xFB04u
#define K_AMP 0xFB05u
#define K_BACKQUOTE 0xFB06u
#define K_SEMICOLON 0xFB07u
#define K_BACKSLASH 0xFB08u
#define K_BAR 0xFB09u
#define K_TILDE 0xFB0Bu
#define K_PERCENT 0xFB0Cu

/* ---- GetCSC scan codes ([CSC]) ---- */
#define SK_DOWN 0x01u
#define SK_LEFT 0x02u
#define SK_RIGHT 0x03u
#define SK_UP 0x04u
#define SK_ENTER 0x09u
#define SK_ADD 0x0Au
#define SK_SUB 0x0Bu
#define SK_MUL 0x0Cu
#define SK_DIV 0x0Du
#define SK_POWER 0x0Eu
#define SK_CLEAR 0x0Fu
#define SK_CHS 0x11u
#define SK_3 0x12u
#define SK_6 0x13u
#define SK_9 0x14u
#define SK_RPAREN 0x15u
#define SK_TAN 0x16u
#define SK_VARS 0x17u
#define SK_DECPNT 0x19u
#define SK_2 0x1Au
#define SK_5 0x1Bu
#define SK_8 0x1Cu
#define SK_LPAREN 0x1Du
#define SK_COS 0x1Eu
#define SK_PRGM 0x1Fu
#define SK_STAT 0x20u
#define SK_0 0x21u
#define SK_1 0x22u
#define SK_4 0x23u
#define SK_7 0x24u
#define SK_COMMA 0x25u
#define SK_SIN 0x26u
#define SK_APPS 0x27u
#define SK_GRAPHVAR 0x28u /* X,T,theta,n */
#define SK_STORE 0x2Au
#define SK_LN 0x2Bu
#define SK_LOG 0x2Cu
#define SK_SQUARE 0x2Du
#define SK_RECIP 0x2Eu
#define SK_MATH 0x2Fu
#define SK_ALPHA 0x30u
#define SK_GRAPH 0x31u
#define SK_TRACE 0x32u
#define SK_ZOOM 0x33u
#define SK_WINDOW 0x34u
#define SK_YEQU 0x35u
#define SK_2ND 0x36u
#define SK_MODE 0x37u
#define SK_DEL 0x38u

/* ---- the physical keypad: scan code + GetKey code per layer ---- */

enum { L_PRI = 0, L_2ND = 1, L_ALPHA = 2 };

typedef struct phys_key {
    uint8_t sk;
    uint16_t code[3]; /* primary, 2nd, ALPHA (0 = nothing useful) */
} phys_key_t;

enum {
    P_NONE = 0,
    P_YEQU, P_WINDOW, P_ZOOM, P_TRACE, P_GRAPH,
    P_MODE, P_DEL, P_ALPHA, P_XTON, P_STAT,
    P_MATH, P_APPS, P_PRGM, P_VARS, P_CLEAR,
    P_RECIP, P_SIN, P_COS, P_TAN, P_POWER,
    P_SQUARE, P_COMMA, P_LPAREN, P_RPAREN, P_DIV,
    P_LOG, P_7, P_8, P_9, P_MUL,
    P_LN, P_4, P_5, P_6, P_SUB,
    P_STO, P_1, P_2, P_3, P_ADD,
    P_0, P_DOT, P_CHS, P_ENTER,
    P_UP, P_DOWN, P_LEFT, P_RIGHT,
    P_COUNT
};

#define CAP(c) ((uint16_t)(K_CAPA + (uint16_t)((c) - 'A')))

/* Labels as printed on the TI-84 Plus CE keypad. The ALPHA functions of
 * Y=..GRAPH (CE OS 5.2+ "f1..f5" shortcut menus) have no GetKey code. */
static const phys_key_t phys[P_COUNT] = {
    /* P_NONE   */ {0, {0, 0, 0}},
    /* P_YEQU   */ {SK_YEQU, {K_YEQU, K_STATP, 0}},
    /* P_WINDOW */ {SK_WINDOW, {K_WINDOW, K_TBLSET, 0}},
    /* P_ZOOM   */ {SK_ZOOM, {K_ZOOM, K_FORMAT, 0}},
    /* P_TRACE  */ {SK_TRACE, {K_TRACE, K_CALC, 0}},
    /* P_GRAPH  */ {SK_GRAPH, {K_GRAPH, K_TABLE, 0}},
    /* P_MODE   */ {SK_MODE, {K_MODE, K_QUIT, 0}},
    /* P_DEL    */ {SK_DEL, {K_DEL, K_INS, 0}},
    /* P_ALPHA  */ {SK_ALPHA, {0, 0, 0}}, /* shift key: scan code only */
    /* P_XTON   */ {SK_GRAPHVAR, {K_VARX, K_LINKIO, 0}},
    /* P_STAT   */ {SK_STAT, {K_STAT, K_LIST, 0}},
    /* P_MATH   */ {SK_MATH, {K_MATH, K_TEST, CAP('A')}},
    /* P_APPS   */ {SK_APPS, {K_APPSMENU, K_ANGLE, CAP('B')}},
    /* P_PRGM   */ {SK_PRGM, {K_PRGM, K_DRAW, CAP('C')}},
    /* P_VARS   */ {SK_VARS, {K_VARS, K_DIST, 0}},
    /* P_CLEAR  */ {SK_CLEAR, {K_CLEAR, 0, 0}},
    /* P_RECIP  */ {SK_RECIP, {K_INV, K_MATRIX, CAP('D')}},
    /* P_SIN    */ {SK_SIN, {K_SIN, K_ASIN, CAP('E')}},
    /* P_COS    */ {SK_COS, {K_COS, K_ACOS, CAP('F')}},
    /* P_TAN    */ {SK_TAN, {K_TAN, K_ATAN, CAP('G')}},
    /* P_POWER  */ {SK_POWER, {K_EXPON, K_PI, CAP('H')}},
    /* P_SQUARE */ {SK_SQUARE, {K_SQUARE, K_SQRT, CAP('I')}},
    /* P_COMMA  */ {SK_COMMA, {K_COMMA, K_EE, CAP('J')}},
    /* P_LPAREN */ {SK_LPAREN, {K_LPAREN, K_LBRACE, CAP('K')}},
    /* P_RPAREN */ {SK_RPAREN, {K_RPAREN, K_RBRACE, CAP('L')}},
    /* P_DIV    */ {SK_DIV, {K_DIV, K_CONSTEA, CAP('M')}},
    /* P_LOG    */ {SK_LOG, {K_LOG, K_ALOG, CAP('N')}},
    /* P_7      */ {SK_7, {K_0 + 7u, K_UNA, CAP('O')}},
    /* P_8      */ {SK_8, {K_0 + 8u, K_UNA + 1u, CAP('P')}},
    /* P_9      */ {SK_9, {K_0 + 9u, K_UNA + 2u, CAP('Q')}},
    /* P_MUL    */ {SK_MUL, {K_MUL, K_LBRACK, CAP('R')}},
    /* P_LN     */ {SK_LN, {K_LN, K_EXP, CAP('S')}},
    /* P_4      */ {SK_4, {K_0 + 4u, K_L1A + 3u, CAP('T')}},
    /* P_5      */ {SK_5, {K_0 + 5u, K_L1A + 4u, CAP('U')}},
    /* P_6      */ {SK_6, {K_0 + 6u, K_L1A + 5u, CAP('V')}},
    /* P_SUB    */ {SK_SUB, {K_SUB, K_RBRACK, CAP('W')}},
    /* P_STO    */ {SK_STORE, {K_STORE, K_RECALL, CAP('X')}},
    /* P_1      */ {SK_1, {K_0 + 1u, K_L1A, CAP('Y')}},
    /* P_2      */ {SK_2, {K_0 + 2u, K_L1A + 1u, CAP('Z')}},
    /* P_3      */ {SK_3, {K_0 + 3u, K_L1A + 2u, K_THETA}},
    /* P_ADD    */ {SK_ADD, {K_ADD, K_MEM, K_QUOTE}},
    /* P_0      */ {SK_0, {K_0, K_CATALOG, K_SPACE}},
    /* P_DOT    */ {SK_DECPNT, {K_DECPNT, K_I, K_COLON}},
    /* P_CHS    */ {SK_CHS, {K_CHS, K_ANS, K_QUEST}},
    /* P_ENTER  */ {SK_ENTER, {K_ENTER, K_LASTENT, K_ALPHAENTER}},
    /* 2nd+UP/DOWN change the brightness inside TI-OS: no GetKey code. */
    /* P_UP     */ {SK_UP, {K_UP, 0, K_ALPHAUP}},
    /* P_DOWN   */ {SK_DOWN, {K_DOWN, 0, K_ALPHADOWN}},
    /* P_LEFT   */ {SK_LEFT, {K_LEFT, K_BOL, 0}},
    /* P_RIGHT  */ {SK_RIGHT, {K_RIGHT, K_EOL, 0}}
};

/* Physical key under the ALPHA letter A..Z. */
static const uint8_t letter_phys[26] = {
    P_MATH, P_APPS, P_PRGM, P_RECIP, P_SIN, P_COS, P_TAN, P_POWER, P_SQUARE,
    P_COMMA, P_LPAREN, P_RPAREN, P_DIV, P_LOG, P_7, P_8, P_9, P_MUL, P_LN,
    P_4, P_5, P_6, P_SUB, P_STO, P_1, P_2
};

/* ---- HID usage -> key ---- */

/* HID usage IDs (USB HID Usage Tables, keyboard page 0x07). */
#define HID_A 0x04u
#define HID_Z 0x1Du
#define HID_1 0x1Eu
#define HID_0 0x27u
#define HID_ENTER 0x28u
#define HID_ESC 0x29u
#define HID_BACKSPACE 0x2Au
#define HID_TAB 0x2Bu
#define HID_SPACE 0x2Cu
#define HID_MINUS 0x2Du
#define HID_EQUAL 0x2Eu
#define HID_LBRACKET 0x2Fu
#define HID_RBRACKET 0x30u
#define HID_BACKSLASH 0x31u
#define HID_NONUS_HASH 0x32u
#define HID_SEMICOLON 0x33u
#define HID_APOSTROPHE 0x34u
#define HID_GRAVE 0x35u
#define HID_COMMA 0x36u
#define HID_DOT 0x37u
#define HID_SLASH 0x38u
#define HID_F1 0x3Au
#define HID_F11 0x44u
#define HID_F12 0x45u
#define HID_PAUSE 0x48u
#define HID_INSERT 0x49u
#define HID_HOME 0x4Au
#define HID_PGUP 0x4Bu
#define HID_DELETE 0x4Cu
#define HID_END 0x4Du
#define HID_PGDN 0x4Eu
#define HID_RIGHT 0x4Fu
#define HID_LEFT 0x50u
#define HID_DOWN 0x51u
#define HID_UP 0x52u
#define HID_KP_SLASH 0x54u
#define HID_KP_STAR 0x55u
#define HID_KP_MINUS 0x56u
#define HID_KP_PLUS 0x57u
#define HID_KP_ENTER 0x58u
#define HID_KP_1 0x59u
#define HID_KP_0 0x62u
#define HID_KP_DOT 0x63u
#define HID_KP_EQUAL 0x67u
#define HID_R 0x15u
#define HID_S 0x16u

/* A mapping: either a physical key on a layer, or a direct GetKey code with
 * no physical key (scan code 0). */
typedef struct key_map {
    uint8_t usage;
    uint8_t phys;  /* P_NONE for a direct code */
    uint8_t layer;
    uint16_t direct;
} key_map_t;

#define PK(u, p) {u, p, L_PRI, 0}
#define PL(u, p, l) {u, p, l, 0}
#define DK(u, k) {u, P_NONE, L_PRI, k}

/* Unshifted keys (letters and digits are handled in code). */
static const key_map_t base_map[] = {
    PK(HID_ENTER, P_ENTER),
    PK(HID_ESC, P_CLEAR),
    PK(HID_BACKSPACE, P_DEL),
    PK(HID_TAB, P_ALPHA),
    PL(HID_SPACE, P_0, L_ALPHA),
    PK(HID_MINUS, P_SUB),
    DK(HID_EQUAL, K_TEQU),
    PL(HID_LBRACKET, P_MUL, L_2ND),
    PL(HID_RBRACKET, P_SUB, L_2ND),
    DK(HID_BACKSLASH, K_BACKSLASH),
    DK(HID_NONUS_HASH, K_BACKSLASH),
    DK(HID_SEMICOLON, K_SEMICOLON),
    DK(HID_APOSTROPHE, K_SINGLEQUOTE),
    DK(HID_GRAVE, K_BACKQUOTE),
    PK(HID_COMMA, P_COMMA),
    PK(HID_DOT, P_DOT),
    PK(HID_SLASH, P_DIV),
    PK(HID_F1 + 0u, P_YEQU),
    PK(HID_F1 + 1u, P_WINDOW),
    PK(HID_F1 + 2u, P_ZOOM),
    PK(HID_F1 + 3u, P_TRACE),
    PK(HID_F1 + 4u, P_GRAPH),
    PK(HID_F1 + 5u, P_MODE),
    PK(HID_F1 + 6u, P_STAT),
    PK(HID_F1 + 7u, P_MATH),
    PK(HID_F1 + 8u, P_APPS),
    PK(HID_F1 + 9u, P_PRGM),
    PK(HID_F11, P_VARS),
    PL(HID_INSERT, P_DEL, L_2ND),    /* INS */
    PL(HID_HOME, P_LEFT, L_2ND),     /* beginning of line */
    PL(HID_END, P_RIGHT, L_2ND),     /* end of line */
    PL(HID_PGUP, P_UP, L_ALPHA),     /* page up */
    PL(HID_PGDN, P_DOWN, L_ALPHA),   /* page down */
    PK(HID_DELETE, P_DEL),
    PK(HID_RIGHT, P_RIGHT),
    PK(HID_LEFT, P_LEFT),
    PK(HID_DOWN, P_DOWN),
    PK(HID_UP, P_UP),
    PK(HID_KP_SLASH, P_DIV),
    PK(HID_KP_STAR, P_MUL),
    PK(HID_KP_MINUS, P_SUB),
    PK(HID_KP_PLUS, P_ADD),
    PK(HID_KP_ENTER, P_ENTER),
    PK(HID_KP_DOT, P_DOT),
    DK(HID_KP_EQUAL, K_TEQU)
};

/* US layout shifted symbols. Shift+'-' is the TI negation key (-), which is
 * far more useful on a calculator than '_' (which TI-OS has no key for). */
static const key_map_t shift_map[] = {
    DK(HID_1 + 0u, K_XFACTORIAL), /* ! */
    DK(HID_1 + 1u, K_AT),         /* @ */
    DK(HID_1 + 2u, K_HASH),       /* # */
    DK(HID_1 + 3u, K_DOLLAR),     /* $ */
    DK(HID_1 + 4u, K_PERCENT),    /* % */
    PK(HID_1 + 5u, P_POWER),      /* ^ */
    DK(HID_1 + 6u, K_AMP),        /* & */
    PK(HID_1 + 7u, P_MUL),        /* * */
    PK(HID_1 + 8u, P_LPAREN),     /* ( */
    PK(HID_0, P_RPAREN),          /* ) */
    PK(HID_MINUS, P_CHS),         /* (-) */
    PK(HID_EQUAL, P_ADD),         /* + */
    PL(HID_LBRACKET, P_LPAREN, L_2ND), /* { */
    PL(HID_RBRACKET, P_RPAREN, L_2ND), /* } */
    DK(HID_BACKSLASH, K_BAR),     /* | */
    DK(HID_NONUS_HASH, K_BAR),
    PL(HID_SEMICOLON, P_DOT, L_ALPHA),  /* : */
    PL(HID_APOSTROPHE, P_ADD, L_ALPHA), /* " */
    DK(HID_GRAVE, K_TILDE),       /* ~ */
    DK(HID_COMMA, K_TLT),         /* < */
    DK(HID_DOT, K_TGT),           /* > */
    PL(HID_SLASH, P_CHS, L_ALPHA) /* ? */
};

static const uint8_t digit_phys[10] = {P_0, P_1, P_2, P_3, P_4,
                                       P_5, P_6, P_7, P_8, P_9};

static const key_map_t *find(const key_map_t *t, size_t n, uint8_t usage)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (t[i].usage == usage)
            return &t[i];
    return NULL;
}

int dock_keymap_hotkey(uint8_t hid_usage, uint8_t mods)
{
    bool ctrl_alt = (mods & DOCK_MOD_CTRL) && (mods & DOCK_MOD_ALT);
    if (hid_usage == HID_F12 || (ctrl_alt && hid_usage == HID_S))
        return DOCK_HOTKEY_SETTINGS;
    if (ctrl_alt && hid_usage == HID_R)
        return DOCK_HOTKEY_REFRESH;
    return DOCK_HOTKEY_NONE;
}

static bool emit_phys(uint8_t p, uint8_t layer, dock_ti_key_t *out)
{
    const phys_key_t *k = &phys[p];
    uint16_t code = k->code[layer];
    /* The ALPHA key has no GetKey code of its own: scan code only. */
    if (!code && !(p == P_ALPHA && layer == L_PRI))
        return false;
    out->tikey = code;
    out->scancode = k->sk;
    out->flags = 0;
    return true;
}

bool dock_keymap_translate(uint8_t hid_usage, uint8_t mods, dock_ti_key_t *out)
{
    bool ctrl = (mods & DOCK_MOD_CTRL) != 0;
    bool alt = (mods & DOCK_MOD_ALT) != 0;
    bool shift = (mods & DOCK_MOD_SHIFT) != 0;
    const key_map_t *m = NULL;
    uint8_t p, layer;

    if (dock_keymap_hotkey(hid_usage, mods) != DOCK_HOTKEY_NONE)
        return false;

    if (hid_usage == HID_PAUSE) {
        out->tikey = 0;
        out->scancode = 0;
        out->flags = DOCK_KEYF_ON;
        return true;
    }

    /* Letters: ALPHA function of the key under the letter. */
    if (hid_usage >= HID_A && hid_usage <= HID_Z) {
        uint8_t i = (uint8_t)(hid_usage - HID_A);
        p = letter_phys[i];
        if (ctrl)
            return emit_phys(p, L_2ND, out);
        if (shift && !alt) {
            out->tikey = (uint16_t)(K_LA + i);
            out->scancode = phys[p].sk;
            out->flags = 0;
            return true;
        }
        return emit_phys(p, L_ALPHA, out);
    }

    if (shift)
        m = find(shift_map, sizeof shift_map / sizeof shift_map[0], hid_usage);
    if (!m && hid_usage >= HID_1 && hid_usage <= HID_0) {
        p = digit_phys[(hid_usage - HID_1 + 1u) % 10u];
        layer = ctrl ? L_2ND : alt ? L_ALPHA : L_PRI;
        return emit_phys(p, layer, out);
    }
    if (!m)
        m = find(base_map, sizeof base_map / sizeof base_map[0], hid_usage);
    if (!m && hid_usage >= HID_KP_1 && hid_usage <= HID_KP_0) {
        p = digit_phys[(hid_usage - HID_KP_1 + 1u) % 10u];
        layer = ctrl ? L_2ND : alt ? L_ALPHA : L_PRI;
        return emit_phys(p, layer, out);
    }
    if (!m)
        return false;

    if (m->phys == P_NONE) {
        out->tikey = m->direct;
        out->scancode = 0;
        out->flags = 0;
        return true;
    }
    /* Ctrl/Alt select the 2nd/ALPHA function of keys that map to a primary
     * function; keys already bound to a 2nd/ALPHA function ignore them. */
    layer = m->layer;
    if (layer == L_PRI && m->phys != P_ALPHA)
        layer = ctrl ? L_2ND : alt ? L_ALPHA : L_PRI;
    return emit_phys(m->phys, layer, out);
}

/* ---- ASCII (US layout) ---- */

static const char ascii_digits[2][10] = {
    {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
    {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'}
};

/* HID 0x2D..0x38 */
static const char ascii_punct[2][12] = {
    {'-', '=', '[', ']', '\\', '\\', ';', '\'', '`', ',', '.', '/'},
    {'_', '+', '{', '}', '|', '|', ':', '"', '~', '<', '>', '?'}
};

/* HID 0x54..0x63 (keypad) */
static const char ascii_kp[16] = {'/', '*', '-', '+', 13,  '1', '2', '3',
                                  '4', '5', '6', '7', '8', '9', '0', '.'};

uint8_t dock_keymap_ascii(uint8_t hid_usage, uint8_t mods)
{
    uint8_t sh = (mods & DOCK_MOD_SHIFT) ? 1u : 0u;
    if (hid_usage >= HID_A && hid_usage <= HID_Z) {
        uint8_t i = (uint8_t)(hid_usage - HID_A);
        if (mods & DOCK_MOD_CTRL)
            return (uint8_t)(i + 1u); /* ^A = 1 .. ^Z = 26 */
        return (uint8_t)((sh ? 'A' : 'a') + i);
    }
    if (hid_usage >= HID_1 && hid_usage <= HID_0)
        return (uint8_t)ascii_digits[sh][hid_usage - HID_1];
    if (hid_usage >= HID_MINUS && hid_usage <= HID_SLASH)
        return (uint8_t)ascii_punct[sh][hid_usage - HID_MINUS];
    if (hid_usage >= HID_KP_SLASH && hid_usage <= HID_KP_DOT)
        return (uint8_t)ascii_kp[hid_usage - HID_KP_SLASH];
    switch (hid_usage) {
    case HID_ENTER:
        return 13;
    case HID_ESC:
        return 27;
    case HID_BACKSPACE:
        return 8;
    case HID_TAB:
        return 9;
    case HID_SPACE:
        return ' ';
    case HID_DELETE:
        return 127;
    case HID_KP_EQUAL:
        return '=';
    default:
        return 0;
    }
}
