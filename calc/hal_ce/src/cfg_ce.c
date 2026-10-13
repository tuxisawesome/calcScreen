/*
 * cfg_ce.c - persist the 6-digit join code in an archived AppVar ("DOCKCFG").
 *
 * Archiving (moving the var to flash) is what lets the code survive a RAM
 * clear. We use only OS entry points:
 *   os_GetAppVarData - read (works whether the var is in RAM or archive)
 *   os_CreateAppVar  - (re)create in RAM
 *   os_DelAppVar     - delete from RAM
 *   Arc_Unarc        - the OS routine at 0x021448 that TOGGLES a var between
 *                      RAM and archive (its name is in OP1). crt0.S references
 *                      it as ti.Arc_Unarc = 0x021448.
 *   os_ArcChk        - refresh the free-archive accounting before archiving.
 *
 * Layout stored: { magic[4], code[6] }. Loads tolerate a missing, short or
 * wrong-magic var. Saving is best-effort: if the archive is full the OS may
 * refuse to archive, in which case the code still persists until the next RAM
 * clear (we do not force a garbage collect).
 *
 * This writes only an OS variable; it never writes flash directly and installs
 * nothing resident.
 */
#include "hal_ce.h"

#include <stdint.h>
#include <string.h>
#include <ti/vars.h>

#include "dock_dev.h" /* DOCK_JOIN_LEN */

#define CFG_NAME "DOCKCFG"
#define AppVarObj 0x15 /* TI-OS variable type for an AppVar (crt0.S ti.AppVarObj) */

/* OS floating-point accumulator OP1 (ti/vars.h uses 0xD005F8; crt0.S ti.OP1). */
static uint8_t *const OP1 = (uint8_t *)0xD005F8;

/* ti.Arc_Unarc: toggle the archive status of the var named in OP1. */
static void arc_unarc_toggle(const char *name)
{
    size_t i;
    OP1[0] = AppVarObj;
    for (i = 0; i < 8u && name[i]; i++)
        OP1[i + 1u] = (uint8_t)name[i];
    OP1[i + 1u] = 0;
    os_ArcChk(); /* refresh free-archive bookkeeping first */
    ((void (*)(void))0x021448)();
}

static const uint8_t CFG_MAGIC[4] = {'D', 'K', 'C', '1'};

struct cfg {
    uint8_t magic[4];
    uint8_t code[DOCK_JOIN_LEN];
};

bool halce_cfg_load_join(char *out, size_t cap)
{
    int archived;
    var_t *v;
    const struct cfg *c;
    size_t n;

    if (!out || cap == 0)
        return false;
    out[0] = 0;

    v = os_GetAppVarData(CFG_NAME, &archived);
    if (!v || v->size < sizeof(struct cfg))
        return false;
    c = (const struct cfg *)v->data;
    if (memcmp(c->magic, CFG_MAGIC, sizeof CFG_MAGIC) != 0)
        return false;

    n = DOCK_JOIN_LEN < (cap - 1u) ? DOCK_JOIN_LEN : (cap - 1u);
    memcpy(out, c->code, n);
    out[n] = 0;
    return out[0] != 0;
}

void halce_cfg_save_join(const char *code)
{
    int archived;
    var_t *v;
    struct cfg c;

    memcpy(c.magic, CFG_MAGIC, sizeof CFG_MAGIC);
    memset(c.code, 0, sizeof c.code);
    if (code) {
        size_t n = strlen(code);
        memcpy(c.code, code, n < DOCK_JOIN_LEN ? n : DOCK_JOIN_LEN);
    }

    /* Drop any existing copy. Bring it back to RAM first if archived, so
     * os_DelAppVar can remove it. */
    v = os_GetAppVarData(CFG_NAME, &archived);
    if (v) {
        if (archived)
            arc_unarc_toggle(CFG_NAME); /* archive -> RAM */
        os_DelAppVar(CFG_NAME);
    }

    v = os_CreateAppVar(CFG_NAME, (uint16_t)sizeof(struct cfg));
    if (!v)
        return;
    memcpy(v->data, &c, sizeof c);

    /* RAM -> archive so it survives a RAM clear (best effort). */
    arc_unarc_toggle(CFG_NAME);
}
