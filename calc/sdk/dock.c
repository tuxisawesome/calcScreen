/*
 * dock.c - SDK glue: wires the portable device engine (core/dock_dev.c) to the
 * embedded CE USB/LCD/timer HAL (calc/hal_ce) and persists the join code.
 *
 * One static session lives for the life of the program. core/dock_api.c calls
 * the dock_dev_* engine entry points directly (they are real functions on this
 * platform), so this file only has to init/tear-down the HAL and the engine.
 */
#include "dock.h"
#include "hal_ce.h"

static dock_t g_dev;
static bool g_open;

dock_t *dock_open(void)
{
    char code[DOCK_JOIN_LEN + 1];
    if (g_open)
        return &g_dev;
    if (!halce_init())
        return NULL;
    dock_dev_init(&g_dev, halce_hal(), halce_calc_id());
    if (halce_cfg_load_join(code, sizeof code))
        dock_dev_set_join_code(&g_dev, code);
    g_open = true;
    return &g_dev;
}

void dock_set_join_code(dock_t *d, const char *code)
{
    dock_dev_set_join_code(d, code);
    halce_cfg_save_join(code);
}

void dock_close(dock_t *d)
{
    if (!g_open)
        return;
    dock_dev_bye(d);
    halce_cleanup();
    g_open = false;
}
