/*
 * dock_ui.h - tiny immediate-mode UI drawn into any XRGB8888 buffer, with a
 * built-in 8x8 bitmap font. Used for the dock's own screens (waiting, join
 * code, settings) so no GUI toolkit is needed.
 */
#ifndef DOCK_UI_H
#define DOCK_UI_H

#include "dock_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dock_canvas {
    uint32_t *px;
    uint16_t w, h;
    uint32_t stride; /* pixels per row */
} dock_canvas_t;

#define DOCK_UI_BG 0xFF101418u
#define DOCK_UI_FG 0xFFE8ECEFu
#define DOCK_UI_DIM 0xFF7A8590u
#define DOCK_UI_ACCENT 0xFF3FA7FFu
#define DOCK_UI_WARN 0xFFFFB13Fu
#define DOCK_UI_SEL_BG 0xFF24486Bu

void dock_ui_fill(const dock_canvas_t *c, int x, int y, int w, int h,
                  uint32_t color);
/* Draw text (printable ASCII; '\n' starts a new line). Returns the width in
 * pixels of the longest line. */
int dock_ui_text(const dock_canvas_t *c, int x, int y, const char *s,
                 uint32_t color, int scale);
int dock_ui_text_width(const char *s, int scale);
void dock_ui_text_centered(const dock_canvas_t *c, int y, const char *s,
                           uint32_t color, int scale);

/* Full screens. */
void dock_ui_screen_waiting(const dock_canvas_t *c, const char *status_line,
                            const char *hint);
void dock_ui_screen_join(const dock_canvas_t *c, const char *join_code);

typedef struct dock_ui_item {
    const char *label;
    const char *value; /* right-aligned, may be NULL */
} dock_ui_item_t;

void dock_ui_screen_menu(const dock_canvas_t *c, const char *title,
                         const dock_ui_item_t *items, int n, int selected,
                         const char *footer);
/* Text entry; masked shows '*' per character. */
void dock_ui_screen_prompt(const dock_canvas_t *c, const char *title,
                           const char *text, bool masked, const char *footer);
void dock_ui_screen_message(const dock_canvas_t *c, const char *title,
                            const char *body);

#ifdef __cplusplus
}
#endif

#endif /* DOCK_UI_H */
