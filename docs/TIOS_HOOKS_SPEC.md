# TI-OS (TI-84 Plus CE, OS 5.x) hook spec for a resident key-injection and background driver

Status: implementation-ready, with every claim graded.
Target: TI-84 Plus CE (eZ80, ADL mode), TI-OS 5.x, native code via arTIfiCE.
Date researched: 2026-10-13.

## 0. How to read this document; sources; confidence levels

Each claim is tagged:

- **[VERIFIED-SRC]**: read directly in the source code or equates of a cited project (path and line given).
- **[CONSISTENT]**: several independent working projects rely on it, but no primary documentation was available.
- **[REPORTED]**: a second-hand statement from a project document (for example, "observed in CEmu").
- **[UNSURE]**: my best inference. Check it in CEmu before relying on it. Section 10 lists the tests to run.

**WikiTI and Cemetech were not reachable from this environment** (the egress proxy blocked
wikiti.brandonw.net, cemetech.net, web.archive.org, tiplanet.org and education.ti.com). As a result, no claim
below cites WikiTI text directly. Everything is grounded in source code that working CE projects ship, plus
toolchain equates. Wherever I name a WikiTI page, it is as a pointer for you to check, not as a source I read.

Local clones (all under `/tmp/claude-0/ext/`):

| Clone dir | Upstream | What it proves |
|---|---|---|
| `toolchain/` (v15.0, commit a2e96f9, 2026-10-06) | github.com/CE-Programming/toolchain | equates (`src/include/ti84pceg.inc`), C ABI doc (`docs/static/asm.rst`), crt0, usbdrvce |
| `capnhook/` (branch `capnhook`) | CE-Programming/toolchain@capnhook | hook library: setter, clearer and pointer tables, "no-op" tails per hook type |
| `cesium/` (commit bc7a9aa, 2026-09-15) | github.com/mateoconlechuga/cesium | GetCSC hook (shortcuts), parser hook chaining |
| `RoccoLoxPrograms_CEaShell/` (7d9cc8d, 2026-09-11) | github.com/RoccoLoxPrograms/CEaShell | GetCSC hook ($1A/$1B use), RawKey hook during TI-BASIC `Input` |
| `EzCE_ScreenSaver-CE/` | github.com/EzCE/ScreenSaver-CE | GetCSC hook called with B=0 (idle), APD manipulation, C code run from a hook, inject-then-swallow pattern |
| `jacobly0_asmhook/` | github.com/jacobly0/asmhook | direct pointer/flag install, **IX = hook entry** convention |
| `RoccoLoxPrograms_AsmHook2/` | github.com/RoccoLoxPrograms/AsmHook2 | OS 5.8.3+ workaround: the hook must live in a Flash app |
| `Lord-Funion_CE-Menu-Locker/` | github.com/Lord-Funion/CE-Menu-Locker | `DESIGN.md`: CEmu-verified hook ABI notes (OS 5.3.0 and 5.8.3) |
| `EzCE_ez80-docs/`, `EzCE_hook-template/` | github.com/EzCE/ez80-docs, EzCE/hook-template | hook overview; **5.8.3 hook restriction** |
| `TIny-Hacker_*`, `juhapark21_TI84-F5menu/` | GitHub | appData usage by other hooks; RAM-resident hook on older OS |
| `YvanTT_arTIfiCE/` | github.com/YvanTT/arTIfiCE | jailbreak/OS-version compatibility matrix |
| (raw fetch) | github.com/jacobly0/llvm-project `z80` branch, `Z80CallingConv.td`, `Z80RegisterInfo.cpp` | ez80-clang callee-saved registers |

---

## 1. Critical blocker first: where the hook code may live (OS version gate)

**[VERIFIED-SRC]** Hook code location rules by OS version:

- `EzCE_ez80-docs/content/other/hooks/_index.md`: "As of TI-OS version 5.8.3, TI-OS prevents hook pointer
  locations outside of apps and the OS itself. In order for your hooks to work on all OS versions, it is
  recommended to use an app to store your hook code."
- `EzCE_hook-template/README.md`: "as of TI-OS 5.8.3, hooks are blocked outside of apps and the OS itself. As
  this template uses an AppVar to store hook code, it is only compatible with OS versions Pre-5.8.3."
- jacobly's ASMHOOK (code in an archived program) stopped working on 5.8.3. That is why AsmHook2 exists:
  `RoccoLoxPrograms_AsmHook2/src/main.asm` builds a Flash app (`app_start 'AsmHook2'`, `include/app.inc`
  `app_create`), and the app installs `ld hl, parser - 1 / call ti.SetParserHook`.
- arTIfiCE compatibility (`YvanTT_arTIfiCE/index.html`):
  - "arTIfiCE v2 supports CE OS until v5.8.4".
  - "arTIfiCE v3 supports CE OS v5.8.5 only".
  - "In May 2025, TI fixed the security flaws with the 5.8.3 (CE)".
  - v3 installs AsmHook2 as a Flash app.

Consequences for this driver:

| OS | Hook code in RAM (appData 0xD00429, usbArea 0xD13FD8, pixelShadow…) | Hook code in archived AppVar/program | Hook code in Flash **app** |
|---|---|---|---|
| ≤ 5.8.2 | Works [CONSISTENT]. For example, `juhapark21_TI84-F5menu/src/main.asm` copies the hook to `pixelshadow2+6000` and calls `_SetGetCSCHook`. | Works, but dangles after a Garbage Collect (hook-template README). | Works. |
| ≥ 5.8.3 | **Blocked** [VERIFIED-SRC, docs plus two projects] | **Blocked** | Works: CE-Menu-Locker (CEmu, 5.8.3.0048), AsmHook2, ScreenSaver-CE. |

- **[UNSURE]** I could not find how 5.8.3 enforces this: whether it checks the pointer range in the
  dispatcher or in `Set*Hook`, and which range counts as "app". ASMHOOK writes the pointer and flags directly,
  bypassing `Set*Hook` (`jacobly0_asmhook/ASMHOOK.asm:59-69`), and it still broke. That suggests the check
  happens at dispatch time.
- **Recommendation:** put all hook entry code, and any C code it calls, in a Flash application.
  - Build it with toolchain v15 `APPLICATION=YES` (`toolchain/src/makefile.mk:25`, changelog "direct 8ek
    app output").
  - Install it on-calc with an installer that writes Flash and applies relocations. Two patterns exist:
    the CE `app_tools` pattern cited in `Lord-Funion_CE-Menu-Locker/DESIGN.md` "Packaging choice", or
    AsmHook2's `include/app.inc` `app_create`.
- Use RAM only for state. A RAM-resident hook is acceptable only if you explicitly limit support to ≤ 5.8.2.

---

## 2. Q1: Hook format, calling convention, install and uninstall

### 2.1 Format

- **[VERIFIED-SRC]** The first byte at the hook pointer must be `$83`.
  - `capnhook/src/capnhook/capnhook.h`: "A hook is valid if the first byte is 0x83".
  - `cesium/src/hooks.asm:94-95` has `db $83` before `hook_get_key`.
  - `EzCE_ez80-docs/.../hooks/_index.md`: "the first byte of it must be `$83`".
- **[VERIFIED-SRC]** The pointer stored in `xxxHookPtr` is the address of the `$83` byte itself.
  - Cesium: `ld hl,hook_get_key / jp ti.SetGetCSCHook` (`cesium/src/util.asm:295-296`). The label is on the
    `db $83`.
  - ASMHOOK stores `menu - 1` and `parser - 1`, where `menu:` and `parser:` follow `db $83`
    (`jacobly0_asmhook/ASMHOOK.asm:59-66`).
- **[VERIFIED-SRC]** Execution starts at pointer+1. Chaining code jumps to pointer+1 explicitly:
  - Cesium `hook_chain_parser` stores `ptr+1` and later does `ld ix,(hook_chain+1) / jp (ix)` (`cesium/src/hooks.asm:40`, `:538-545`).
  - CEaShell does the same (`hooks.s:85`).
  - ez80-docs: "make sure to jump to the byte after the `$83`".
- **[UNSURE]** If the byte is not `$83`, the OS presumably does not run the hook. I don't know whether it
  then behaves as "no hook" or clears the flag. Never leave a flag set over bytes that may stop being your hook.

### 2.2 How the OS calls the hook (registers, stack, interrupts)

| Item | Value | Confidence and source |
|---|---|---|
| Mechanism | A `call`-style transfer. The hook ends with `ret`, and the OS continues from the flags and registers you return. | [VERIFIED-SRC] All hooks in the cited projects end in `ret`. |
| CPU mode | ADL (24-bit) | [VERIFIED-SRC] All CE hooks are ADL code. |
| **IY** | `= ti.flags` (0xD00080) on entry. **You must return with IY = flags.** | [CONSISTENT] Every hook uses `(iy+…)` without loading IY first (ScreenSaver `hook.s:35-36`, CEaShell `hooks.s:209`, F5menu). OS code addresses flags only through IY. |
| **IX** | = hook pointer + 1 (address of the first instruction after `$83`) | [VERIFIED-SRC for menu and parser hooks] ASMHOOK computes data addresses relative to IX: `lea hl, inst.name + ix - menu` (`ASMHOOK.asm:87`, menu hook), `lea hl, clean + ix - parser` (`:213`, parser hook). capnhook's dispatcher reproduces it ("ix register is set to the address of the hook being called", `capnhook.asm` main_executor). [UNSURE] that every hook type, GetCSC and RawKey included, gets the same IX. Don't depend on IX in your hook: use absolute addresses, which is fine for a Flash app at a fixed relocated address. Preserve IX. |
| Stack | You run on the OS stack (stackBot 0xD1987E to stackTop 0xD1A87E, 4096 bytes total, `ti84pceg.inc:3049-3051`). (SP) = return address into the OS dispatcher. | [VERIFIED-SRC] for addresses. Keep stack use modest: the OS is already some frames deep. |
| MB | Probably $D0 | [UNSURE] Cesium sets `ld a,$d0 / ld mb,a` before `.sis` accesses inside hook-called code (`hooks.asm` `helper_vputs_toolbar`). If you use `.sis`/`.lis` short-mode addressing, set MB yourself. |
| Interrupts | Usually **enabled** while the OS waits in GetKey | [UNSURE] `GetCSC` "will also enable interrupts" (`EzCE_ez80-docs/content/syscalls/all/GetCSC.md`). ScreenSaver does `di … ei` inside its hook (`hook.s` around the APD code), which suggests they were on. Save the state with `ld a,i` (P/V = IEF2) and restore it if you need `di`. |
| Registers to preserve | Treat everything that isn't a documented output as callee-saved: AF (except outputs), BC, DE, HL, IX, IY, and the shadow set if you touch it. | [CONSISTENT] capnhook restores AF/BC/DE/HL/IY between chained hooks (`main_executor`). CE-Menu-Locker preserves both register sets, IX and IY (`DESIGN.md`). The OS dispatcher may expect BC to still hold the scan code. |
| Leaving without returning | Only through OS routines that reset the stack (`JForceCmd`, `JForceCmdNoChar`, `SendKPress`/`MonForceKey`), as Cesium's `hook_execute_cesium` does | [VERIFIED-SRC] `cesium/src/hooks.asm` ends with `jp ti.JForceCmd` |

### 2.3 Install, uninstall and query (all routines need IY = flags)

All addresses are from `toolchain/src/include/ti84pceg.inc`. The pairings are confirmed by capnhook's tables
(`capnhook/src/capnhook/capnhook.asm:1466-1537`).

| Hook | Install (HL = ptr to `$83`) | Uninstall | Pointer var | Active flag |
|---|---|---|---|---|
| **GetCSC hook** | `SetGetCSCHook` = **0x0213E0** | **`ClrGetKeyHook` = 0x0213E4** (historical misnomer) | **`getKeyHookPtr` = 0xD025DE** (misnomer) | `getCSCHookActive` = bit 0 of `(iy+hookflags2)`; hookflags2 = $34, i.e. byte **0xD000B4** |
| **Raw key hook** | **`SetGetKeyHook` = 0x0213CC** (misnomer) | `ClrRawKeyHook` = 0x0213D8 | `rawKeyHookPtr` = 0xD025DB | `rawKeyHookActive` = bit 5 of `(iy+$34)` |
| App change | `SetAppChangeHook` 0x0214A4 | `ClrAppChangeHook` 0x0214A8 | `appChangeHookPtr` 0xD025FC | bit 2 of `(iy+hookflags4=$36)` |
| Parser | `SetParserHook` 0x02149C | `ClrParserHook` 0x0214A0 | `parserHookPtr` 0xD025F9 | bit 1 of `(iy+$36)` |
| Cursor | `SetCursorHook` 0x0213C4 | `ClrCursorHook` 0x0213D0 | `cursorHookPtr` 0xD025D5 | bit 7 of `(iy+$34)` |
| Homescreen | `SetHomescreenHook` 0x021410 | `ClrHomescreenHook` 0x021414 | `homescreenHookPtr` 0xD025E1 | bit 4 of `(iy+$34)` |
| USB activity | `SetUSBActivityHook` 0x02171C | `ClrUSBActivityHook` 0x021720 | `USBActivityHookPtr` 0xD0261A | bit 0 of `(iy+hookflags5=$3A)` |

Other details:

- **[VERIFIED-SRC]** Cesium installs with `SetGetCSCHook` and removes with `ClrGetKeyHook` (`cesium/src/util.asm:296`, `main.asm:145`).
- ScreenSaver installs a raw key hook with `SetGetKeyHook` and removes it with `ClrRawKeyHook` (`hook.s:258`, `:279`).
- Pointer memory: `hookExecTemp` = 0xD025D2. The OS uses it during dispatch (inferred from the name). Don't touch it.
- Installing without the routines: ASMHOOK writes the pointer and ORs the flag bits itself (`ASMHOOK.asm:59-69`). The routines are equivalent to "store HL into the pointer, set the bit". **[UNSURE]** whether `Clr*` also zeroes the pointer. CE-Menu-Locker describes `ClrGetKeyHook` as "Clear the GetCSC hook-active flag".
- Ownership check, used everywhere:

```asm
    bit  ti.getCSCHookActive,(iy+ti.hookflags2)
    jr   z,.notMine
    ld   hl,(ti.getKeyHookPtr)
    ld   de,my_getcsc_hook
    or   a,a
    sbc  hl,de
    jr   nz,.notMine
```

- The `Set*` and `Clr*` routines can be called from any context (program, app, or another hook), provided the
  pointer target is allowed (ez80-docs hooks page).
- `hookflags2Override` .. `hookflags4Override` (flags+$37..$39): setting a bit there makes `AppSetup` kill the
  corresponding hook when an app starts (`EzCE_ez80-docs/.../hookflags2Override.md`; `ti84pceg.inc:7109-7113`).
  Leave them clear.

---

## 3. Q2: GetCSC hook semantics

### 3.1 Inputs

| A on entry | Meaning | Other inputs |
|---|---|---|
| **$1A** | Pre-scan event, raised every time the OS key loop is about to read the keyboard. **Fires while idle.** | none |
| **$1B** | Post-scan event | **B = scan code (sk\*), or B = 0 if no key is pressed** |

- **[VERIFIED-SRC]** for both values. Every GetCSC hook tests `cp a,$1B` (Cesium `hooks.asm:97`, CEaShell
  `hooks.s:189-195`, ScreenSaver `hook.s:37`, capnhook's no-op tail, F5menu).
- CEaShell uses `$1A` as a periodic idle tick to redraw menu decorations (`hooks.s:200-213`: `cp a,$1A`, then
  it checks `menuCurrent`).
- **[VERIFIED-SRC]** The hook is called with **A=$1B and B=0 when no key is down.** ScreenSaver's
  `continueHook` path runs exactly when `B=0` (`hook.s:40-42`: `ld a,b / or a,a / jr z,continueHook`) and then
  checks `apdTimer` to decide whether the idle timeout expired. That only works if the OS calls the hook
  repeatedly with no key pressed.
- CE-Menu-Locker disassembled OS 5.3.0.0037 and states "pre-scan event with A=$1A… after scanning… A=$1B and
  the raw scan code in B" [REPORTED].
- Note the trap: `sk2 = $1A` and `sk5 = $1B` (`ti84pceg.inc` scan-code table). Never confuse the event in A
  with the scan code in B.

### 3.2 Outputs and return convention

| Case | Return | Confidence |
|---|---|---|
| A=$1A (pre-scan): do nothing | A = $1A unchanged, **NZ** (`cp a,$1B / ret nz` produces exactly this) | [VERIFIED-SRC] capnhook's "behaves as though no hook was called" tail `getkey_tail: cp a,$1b / ret nz / or a,a / ld a,b / ret` (`capnhook.asm:1384-1391`); CEaShell `getCSCHook.return` |
| A=$1B: pass through | **A = B, NZ** | [VERIFIED-SRC] same tail; CEaShell; CE-Menu-Locker `hook.S:259-264` |
| A=$1B: substitute or inject a key | **A = new scan code (≠0), NZ** | [CONSISTENT] Pass-through is the special case A=B, so the OS takes A as the scan code. ScreenSaver returns `xor a / inc a` (A=1=skDown, NZ) **while B was 0**, then uses a one-shot RawKey hook to swallow the key that results (`hook.s:270-285`). That is a working inject-from-nothing followed by a RawKey rewrite. |
| A=$1B: swallow the key | **A = 0.** Both flag states are observed: `xor a,a / ret` (Z) in CE-Menu-Locker `hook.S:281` and Cesium `hook_get_key_none` (`hooks.asm:574-578`); `or a,1 / ld a,0` (NZ, A=0) in ScreenSaver `hook.s:42-49`. | [VERIFIED-SRC] |
| A≠0 with **Z** set | Unknown. CE-Menu-Locker claims "zero flag set and the replacement scan code in A" [REPORTED]. | [UNSURE]: avoid |

**Canonical exit:**

```asm
    ld   a,<scan code or 0>
    or   a,a
    ret
```

This makes NZ ⇔ A≠0, which satisfies every observed convention.

### 3.3 When the OS calls it

- **[CONSISTENT]** It is called from the OS key-wait loop (GetKey and the OS main loop "Mon", which waits in GetKey). That covers:
  - homescreen idle;
  - menus (CEaShell decorates the PRGM and APPS menus via $1A);
  - Y=, window, table and the graph screen while idle;
  - editors (Cesium's label jumper runs from the program editor, `cxPrgmEdit`);
  - TI-BASIC commands that wait in GetKey (`Input`, `Prompt`, `Pause`, `Menu(`). ScreenSaver explicitly
    bails out when `progExecuting` is set (`hook.s:36`, `bit ti.progExecuting,(iy+ti.newDispF)`), which
    implies the hook does fire during program execution.
- **[UNSURE]** It is probably **not** called by the plain `GetCSC` syscall 0x02014C. That syscall just
  reads and clears `kbdScanCode` (ez80-docs `GetCSC.md`), and WikiTI-era lore says the GetCSC hook belongs to
  GetKey. Defensively, clear your own GetCSC hook before calling `ti.GetCSC` in a loop inside the hook
  (CE-Menu-Locker does this) or tolerate re-entry.
- **Not called** (no OS key loop runs) during:
  - TI-BASIC execution between statements, including busy loops built on the BASIC `getKey` function
    [UNSURE whether BASIC `getKey` reaches the hook: assume it does not];
  - graph plotting and computation;
  - any asm or C program (unless it calls `_GetKey`);
  - link transfers;
  - Flash apps with their own key loops (Cesium, CEaShell, Python, CabriJr…);
  - while the calculator is off.
- **Frequency:** once or twice per OS key-loop iteration ($1A, then $1B). **[UNSURE]** of the rate. Expect
  many calls per second while idle. Measure it in CEmu (section 10) and never assume a fixed period.
- **Conclusion:** the GetCSC hook gives you a frequent idle tick, not a real timer. It stops whenever TI-OS
  is busy.
- Supplementary ticks:
  - parser hook (fires per token while TI-BASIC runs: A=0 at program start, A=2 with B=command token; Cesium
    `hooks.asm:29-50`, ASMHOOK `parser:`);
  - app-change hook (context switches);
  - cursor hook (cursor blink in editors). [UNSURE] of the exact semantics, but capnhook's no-op tail is
    `cp a,$24 / ret nz / ld a,b / or a,a / ret`.

---

## 4. Q3: Raw key hook semantics

- **[VERIFIED-SRC]** Input: **A = translated GetKey key code** (`k*`, already combined with the 2nd/ALPHA
  state), for example `kPrgm`=$2D, `kAppsMenu`=$2C, `kQuit`=$40.
  - Examples: CEaShell `hooks_basicKeyHook` (`hooks.s:1013-1034`), CE-Menu-Locker `hook.S:46-143`,
    TI-84 Addition Edition `hook.asm`.
  - Because translation already happened, the hook sees `kQuit` (2nd+MODE), not MODE.
- **[UNSURE]** 2-byte keys: A = prefix `kExtendEcho`=$FE, `kExtendEcho2`=$FC or `kExtendEcho3`=$FB
  (`ti84pceg.inc:4898`, `:5189`, `:5454`), with the second byte in **`keyExtend` = 0xD0058E**
  (`os_KeyExtend` in `toolchain/src/ce/include/ti/getkey.h`).
  - The toolchain packs these as `prefix | (keyExtend<<8)`. For example `k_Y1 = 0xFC + (3<<8)`
    (`getkey.h:586-596`).
  - **Note for calcScreen:** `core/dock_keymap.h:9` says "0xFC/0xFD prefix in the high byte". The
    toolchain uses prefixes **$FE/$FC/$FB** (no $FD) in the **low** byte. Make sure the two representations
    agree.
- Output convention:
  - **Pass:** A = key, NZ. CEaShell `ld b,a / inc b / cp a,b / ret` and Addition Edition `or a,1 / ld a,b`.
    [VERIFIED-SRC]
  - **Swallow:** `xor a,a / ret` (A=0, Z). ScreenSaver `hook.s:284`, CEaShell `:1026`. [VERIFIED-SRC]
  - **Replace:** A = new key code, NZ. [CONSISTENT] This follows from the pass-through convention.
  - For a 2-byte replacement, also write `(keyExtend)`. [UNSURE] that the OS re-reads `keyExtend` after the
    hook. Test it.
- Where it is called:
  - [VERIFIED-SRC] in the OS monitor/Mon path. ez80-docs `SendKPress.md`: "`MonForceKey`… handles special
    keys and runs through the raw key hook".
  - [VERIFIED-SRC] in TI-BASIC `Input`/`Prompt`: CEaShell's raw key hook acts only when
    `cxCurApp == kPrgmInput ($50)` (`hooks.s:1016-1022`).
  - [REPORTED] **not** re-entered by `GetCSC` (CE-Menu-Locker: "GetCSC does not dispatch RawKey").
  - **[UNSURE]** whether the `_GetKey` syscall used by asm programs runs it.
- Lifetime: [REPORTED] "TI-OS clears the RawKey hook when a Flash application exits"
  (CE-Menu-Locker `DESIGN.md` "Hook lifetime", observed in CEmu). That is why they re-arm RawKey from the
  GetCSC hook on every `$1B`. Do the same.

### 4.1 Making GetKey return a key when nothing was pressed

**Primary method (works anywhere the GetCSC hook runs):** emulate physical key sequences.

1. Queue the scan codes. A modifier is just another scan code: `sk2nd`=$36, `skAlpha`=$30.
   For example, "Y1" = VARS, →, ENTER… and "A" = skAlpha, skMath.
2. On `A=$1B` with **B=0** (no physical key), pop **one** code and return `A=code, NZ`.
3. On the next `$1B` call, return `A=0` (or pass the real B) **at least once before the next injected
   code**. That gives GetKey a "release", so two identical consecutive keys are not merged and auto-repeat
   is not confused. [UNSURE] Measure whether a single B=0 gap is enough.
4. While a physical key is down (B≠0), pass it through and keep the queue. Never mix the two.

The OS processes the injected `sk2nd`/`skAlpha` exactly like real presses: it toggles `shift2nd`/`shiftAlpha`
in `(iy+shiftFlags)` ($12; bits 3, 4, 5=lowercase, 6=alpha-lock, 7=keep-alpha; `ti84pceg.inc:6959-6964`),
updates the cursor, and translates the next key. [CONSISTENT; this is how the keypad works]

**Modifier state hazard:** before injecting a sequence, read `(iy+$12)`.

- If `shiftALock` (6) or `shift2nd`/`shiftAlpha` are already set (from the user or a previous partial
  sequence), either inject compensating presses or clear the bits first (`res`).
- [UNSURE] Setting `shiftAlpha`/`shift2nd` directly in the hook and returning the base scan code would
  probably translate as the shifted key in one step, but the cursor indicator may not update. Test it before
  use.

**Secondary method (key codes with no scan-code path, or exact 2-byte keys):** inject a carrier and rewrite.

- The GetCSC hook returns a harmless carrier scan code. The raw key hook then sees the carrier's translated
  code, replaces it with the target (A = target; for 2-byte keys A=$FC/$FE/$FB and `(keyExtend)`=second
  byte), returns NZ, and stays armed.
- ScreenSaver-CE does exactly this with A=1 as carrier and a swallow instead of a replace
  (`hook.s:270-285`).
- Limits: it works only where the raw key hook runs (Mon/TI-OS UI and BASIC `Input`/`Prompt`). In contexts
  without RawKey dispatch, the carrier key arrives unmodified, so choose a carrier whose effect is benign
  there.
  - [UNSURE] Which scan code is benign everywhere. Avoid A=1 (skDown), which moves things. Candidates are a
    rarely meaningful key; test it.

**Keys GetKey can never return this way:** ON. See section 5.

---

## 5. Q4: Reaching every context; ON/break

| Context | GetCSC-hook injection | RawKey rewrite | Notes |
|---|---|---|---|
| Homescreen, menus, editors, Y=, window, table, graph and trace idle, mode/format screens | Yes [CONSISTENT] | Yes | OS Mon/GetKey loop |
| TI-BASIC `Input`, `Prompt`, `Pause`, `Menu(` | Yes [CONSISTENT] | Yes for `Input`/`Prompt` [VERIFIED-SRC CEaShell] | waits in GetKey |
| TI-BASIC `getKey` function in a loop | **No hook call** [UNSURE] | No | Workaround (sketched below) |
| asm/C programs using `os_GetKey`/`_GetKey` | Probably yes [UNSURE] | Unknown | |
| asm/C programs using `os_GetCSC` | No hook call | No | No hook runs while they execute, so there is no injection point without an interrupt hijack. |
| Programs reading keypad hardware directly (keypadc `kb_Scan`, most games) | **Impossible** | Impossible | They poll 0xF50010.. directly |
| Flash apps with their own input (Cesium, CEaShell, Python, CabriJr, Inequalz…) | Generally no | No | Cesium also **clears the GetCSC hook** on start (`cesium/src/main.asm:145`). Use the app-change hook to re-arm afterwards (CE-Menu-Locker keeper pattern). |

Workaround for the BASIC `getKey` row: use a parser hook (fires per token during BASIC execution) to write
`kbdScanCode` (0xD00587) and set `kbdSCR` (bit 3 of `(iy+kbdFlags=$00)`) just before `getKey` runs.
`GetCSC` "loads… `kbdScanCode` into `a`, then clears `kbdScanCode`… kbdSCR flag reset" (ez80-docs
`GetCSC.md`, `kbdFlags.md`). [UNSURE] that BASIC `getKey` reads through that path. Test it.

**ON / break:**

- There is no ON scan code. ON is an interrupt (`intOn`, bit 0 of the interrupt controller) whose OS handler
  sets **`onInterrupt` = bit 4 of `(iy+onFlags=$09)`**, i.e. byte 0xD00089 (`ti84pceg.inc:6882-6885`).
- TI-BASIC and graphing poll that flag and raise ERR:BREAK / abort.
- To inject a break, `set 4,(iy+9)` from a hook that runs while the program executes. A parser hook is the
  natural place; a GetCSC hook works while the program waits in `Input`/`Pause`.
- Evidence that the flag is the mechanism: toolchain crt0 clears it on exit (`res 4,(iy+0x09)`,
  `toolchain/src/crt/crt0.S:477`), and CEaShell clears it before `JForceCmd` (`hooks.s:401`).
- **[UNSURE]** how GetKey reports ON (kOn/0/break), and how to emulate 2nd+ON (power off).
- An **untested, risky** hardware trick: toggle the ON line polarity via `mpIntInvert` (0xF00010, bit 0) to
  raise a genuine ON interrupt. Do not ship this without testing in CEmu and on hardware.

---

## 6. Q5: RAM placement, what the OS clears, USB and timer interaction

### 6.1 Can the hook pointer point into RAM?

- OS ≤ 5.8.2: yes ([CONSISTENT], F5menu uses pixelShadow2).
- OS ≥ 5.8.3: no ([VERIFIED-SRC], section 1).
- Even on old OSes, these candidate regions are poor homes for code:
  - **appData 0xD00429 (256 bytes)**:
    - The toolchain calls it "Generally unused by the OS" (`toolchain/src/ce/include/ti/vars.h:33-34`), and
      `ti84pceg.inc:2561` says "formerly used for OFFSCRPT and ONSCRPT".
    - Other hooks use the last bytes: Always Dark `appData+250..252` (`TIny-Hacker_always-dark/src/darkHook.asm:17`),
      TI-84 Addition Edition `appData+253..255` (`src/hook.asm:16`).
    - CE-Menu-Locker [REPORTED]: "emulator testing showed that a magic-byte guard there can collide with
      leg