;-------------------------------------------------------------------------------
; hook.S - the calcScreen resident GetCSC hook and its (un)installers.
;
; EXPERIMENTAL. The ABI here is reconstructed from working CE projects (Cesium,
; ScreenSaver-CE, capnhook, CE-Menu-Locker) and graded in docs/TIOS_HOOKS_SPEC.md;
; it is NOT verified on hardware. Validate in CEmu before flashing a real calc.
;
; The GetCSC hook is called from the OS key-scan loop while the calculator sits
; idle at the homescreen (and in menus, editors, TI-BASIC Input, ...). On entry:
;   A  = event: 0x1A pre-scan (fires every loop pass, our mirror tick) or
;               0x1B post-scan (B = scan code just read, 0 if no key)
;   IY = osFlags (0xD00080); IX = hook pointer + 1
; The first byte at the hook pointer must be 0x83 and execution starts at +1.
; We preserve everything the OS expects (AF except our output, BC, DE, HL, IX,
; IY), service the dock on every call, then apply the GetCSC return convention:
;   return A = scan code (NZ) to deliver/inject a key, or A = 0 (Z) for none;
;   for the pre-scan event just return A = 0x1A unchanged (NZ).
;
; Assembled like dma_ce.S: ez80-clang -E (so C-preprocessor #defines work) then
; z80-none-elf-as -march=ez80+full. C symbol foo == asm _foo.
;-------------------------------------------------------------------------------

	.assume	adl=1

; OS entry points and addresses (docs/TIOS_HOOKS_SPEC.md section 2.3).
#define SetGetCSCHook        0x0213E0
#define ClrGetKeyHook        0x0213E4   /* clears the GetCSC hook (OS misnomer) */
#define osFlags              0xD00080
#define EVENT_POSTSCAN       0x1B
#define RESIDENT_INJECTED_KEY 0xD13FD8  /* must equal resident.h RESIDENT_INJECTED_KEY */

	.text

;-------------------------------------------------------------------------------
; The hook itself. SetGetCSCHook is given the address of the 0x83 byte.
;-------------------------------------------------------------------------------
	.global	_resident_hook
	.type	_resident_hook, @function
_resident_hook:
	.byte	0x83
	; Save all registers the OS relies on (IY = osFlags in particular), then
	; service. resident_tick() is ordinary C and may clobber anything.
	push	ix
	push	iy
	push	hl
	push	de
	push	bc			; keep the scan code in B across the call
	push	af			; keep the event in A across the call
	call	_resident_tick
	pop	af
	pop	bc
	pop	de
	pop	hl
	pop	iy
	pop	ix
	; Return convention.
	cp	a, EVENT_POSTSCAN
	ret	nz			; pre-scan / other: A unchanged, NZ -> no key
	ld	a, b
	or	a, a
	ret	nz			; a physical key is down: pass it through
	; No physical key: deliver an injected scan code if the servicer left one.
	ld	a, (RESIDENT_INJECTED_KEY)
	or	a, a
	ret	z			; nothing to inject -> A=0, Z
	push	hl
	ld	hl, RESIDENT_INJECTED_KEY
	ld	(hl), 0			; one-shot: consume it
	pop	hl
	or	a, a			; A = scan code, NZ
	ret

;-------------------------------------------------------------------------------
; install / uninstall. Both OS routines need IY = osFlags; the ez80 C ABI treats
; IY as callee-saved, so we preserve the caller's IY.
;-------------------------------------------------------------------------------
	.global	_resident_install_hook
	.type	_resident_install_hook, @function
_resident_install_hook:
	push	iy
	ld	iy, osFlags
	ld	hl, _resident_hook
	call	SetGetCSCHook
	pop	iy
	ret

	.global	_resident_uninstall_hook
	.type	_resident_uninstall_hook, @function
_resident_uninstall_hook:
	push	iy
	ld	iy, osFlags
	call	ClrGetKeyHook
	pop	iy
	ret
