/*
 * DOCK - the calcScreen foreground connection manager and live screen mirror.
 *
 * What DOCK is
 * ------------
 *   1. Connection manager: it takes over the USB port (dock_open), talks to
 *      the Raspberry Pi dock, and if the dock asks for a join code it prompts
 *      for the 6 digits and sends them (dock_set_join_code, which also saves
 *      them to the archived DOCKCFG AppVar).
 *   2. Fast screen mirror: it calls dock_service(d) continuously, so whatever
 *      is on the calculator's LCD (including the little status page DOCK itself
 *      draws) is mirrored to the Pi in near real time.
 *   3. Exits on CLEAR, handing the USB port back to TI-OS (dock_close).
 *
 * What DOCK is NOT
 * ----------------
 *   DOCK does NOT inject the dock's keyboard events back into TI-OS. A running
 *   foreground program cannot feed keys into the OS key loop (that loop is not
 *   running while we are). Driving the full TI-OS from the Pi's keyboard is the
 *   job of the Pi-side DUSB fallback, not this program. DOCK therefore just
 *   drains any HID key events the dock sends (so the queue never fills) and
 *   otherwise serves as a connection manager + mirror. Run a dock-aware app
 *   (e.g. BADAPPLE) for programs that actually use the dock's APIs.
 */
#include <stdbool.h>
#include <stdint.h>

#include <ti/getcsc.h>
#include <ti/screen.h>
#include <tice.h>

#include "dock.h"

/* Map a GetCSC scancode to an ASCII digit, or 0 if it is not a digit key. */
static char csc_digit(uint8_t k)
{
    switch (k) {
    case sk_0: return '0';
    case sk_1: return '1';
    case sk_2: return '2';
    case sk_3: return '3';
    case sk_4: return '4';
    case sk_5: return '5';
    case sk_6: return '6';
    case sk_7: return '7';
    case sk_8: return '8';
    case sk_9: return '9';
    default:   return 0;
    }
}

static void draw(int status, const char *join, uint8_t joinlen)
{
    os_ClrHome();
    os_PutStrFull("calcScreen DOCK");
    os_NewLine();
    switch (status) {
    case DOCK_OK:
        os_PutStrFull("Connected to dock.");
        break;
    case DOCK_E_JOIN_REQUIRED:
        os_PutStrFull("Join code needed:");
        os_NewLine();
        /* echo the digits entered so far */
        {
            char buf[DOCK_JOIN_LEN + 1];
            uint8_t i;
            for (i = 0; i < joinlen && i < DOCK_JOIN_LEN; i++)
                buf[i] = join[i];
            for (; i < DOCK_JOIN_LEN; i++)
                buf[i] = '_';
            buf[DOCK_JOIN_LEN] = 0;
            os_PutStrFull(buf);
        }
        break;
    case DOCK_E_JOIN_BAD:
        os_PutStrFull("Join code rejected.");
        break;
    default:
        os_PutStrFull("Waiting for dock...");
        break;
    }
    os_NewLine();
    os_NewLine();
    os_PutStrFull("Live mirror active.");
    os_NewLine();
    os_PutStrFull("[CLEAR] quits.");
}

int main(void)
{
    dock_t *d;
    char join[DOCK_JOIN_LEN];
    uint8_t joinlen = 0;
    int last_status = -1;
    bool redraw = true;

    d = dock_open();
    if (!d) {
        os_ClrHome();
        os_PutStrFull("USB init failed.");
        os_NewLine();
        os_PutStrFull("Press any key.");
        while (!os_GetCSC())
            ;
        return 1;
    }

    for (;;) {
        int status;
        uint8_t key;
        dock_hidkey_t hk;

        /* Keep the link and the live screen mirror running. */
        dock_service(d);

        /* Drain (and discard) dock keyboard events; see the file header. */
        while (dock_key_get(d, &hk))
            ;

        status = dock_status(d);
        if (status != last_status) {
            last_status = status;
            joinlen = 0;
            redraw = true;
        }

        key = os_GetCSC();
        if (key == sk_Clear)
            break;

        if (status == DOCK_E_JOIN_REQUIRED && key) {
            char c = csc_digit(key);
            if (c && joinlen < DOCK_JOIN_LEN) {
                join[joinlen++] = c;
                redraw = true;
                if (joinlen == DOCK_JOIN_LEN) {
                    char code[DOCK_JOIN_LEN + 1];
                    uint8_t i;
                    for (i = 0; i < DOCK_JOIN_LEN; i++)
                        code[i] = join[i];
                    code[DOCK_JOIN_LEN] = 0;
                    dock_set_join_code(d, code);
                    joinlen = 0;
                }
            } else if (key == sk_Del && joinlen) {
                joinlen--;
                redraw = true;
            }
        }

        if (redraw) {
            draw(status, join, joinlen);
            redraw = false;
        }
    }

    dock_close(d);
    os_ClrHomeFull();
    return 0;
}
