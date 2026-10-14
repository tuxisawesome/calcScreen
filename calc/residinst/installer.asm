;-------------------------------------------------------------------------------
; RESIDINS - installer for the calcScreen resident fast-mirror flash app.
;
; *** EXPERIMENTAL - CEmu-ONLY until validated. This writes flash. ***
; A bug here can corrupt flash and force a boot-code OS reinstall to recover.
; Do NOT run this on a physical calculator until the full install -> run ->
; uninstall cycle is proven in the CEmu emulator. See docs/RESIDENT.md.
;
; What it does: embeds RESIDOCK.8ek (the resident app, built by calc/resident),
; finds a free flash application slot, unlocks flash, writes the app image,
; applies its relocation table, and re-locks flash - so RESIDOCK appears in the
; [apps] menu. Run RESIDOCK once from there to arm the resident mirror.
;
; This is an independent reimplementation of the well-known CE flash-app install
; technique that Cesium and AsmHook2 use (MateoConLechuga / RoccoLox / jacobly):
;   - TI removed the clean flash-write-enable call, so the unlock routine is
;     found by scanning the running OS for a known gadget (port_setup), then
;     invoked (port_unlock) with the flash-controller ports.
;   - _WriteFlash (0x0002E0) writes the bytes; the OS routine tolerates being
;     called while flash is busy.
;   - app images carry a relocation table that must be fixed up for the final
;     flash address.
; The flash image and its relocation table are produced by the CE C toolchain
; (calc/resident -> RESIDOCK.8ek), not assembled here; this installer only
; places them. Build with fasmg and the standard CE asm includes (ez80.inc,
; tiformat.inc, ti84pceg.inc) on the include path; see the makefile.
;-------------------------------------------------------------------------------

include 'ez80.inc'
include 'tiformat.inc'
format ti executable program 'RESIDINS'
include 'ti84pceg.inc'

; The embedded app image is the TIFL payload of RESIDOCK.8ek: the file minus its
; 78-byte TIFL header. Inside it the eZ80 app header 'eZ8' sits at offset 256;
; the relocation table is 0x2A bytes after that (298), and the 3-byte field at
; header+0x12 gives the code offset (read at runtime so it tracks the build).
APP_IMAGE_HEADER := 256         ; offset of 'eZ8' within the image
APP_TABLE_OFFSET := APP_IMAGE_HEADER + 0x2A   ; = 298, start of relocation table
APP_NAME_OFFSET  := APP_IMAGE_HEADER + 3      ; = 259, the 8-byte app name
APP_CODEFIELD    := APP_IMAGE_HEADER + 0x12   ; u24 code offset, relative to header

_WriteFlash      := 0x0002E0
FindAppStart     := 0x021100    ; HL = ptr to 8-byte name; NZ if the app exists
ArcChkNext       := 0x022044    ; walk to the next application header

	jp	start

;-------------------------------------------------------------------------------
start:
	call	ti.RunIndicOff
	call	clear_screen
	ld	hl, str_installing
	call	ti.PutS

	call	port_setup		; locate the flash-unlock gadget
	or	a, a
	jr	z, .os_ok
	ld	hl, str_bad_os
	jp	message
.os_ok:
	; Already installed? (find our app by name)
	ld	hl, app_image + APP_NAME_OFFSET
	call	FindAppStart
	jr	nz, .exists		; NZ = an app with this name already exists

	call	install_app		; the flash write + relocation
	ld	hl, str_installed
	jp	message
.exists:
	ld	hl, str_exists
	jp	message

;-------------------------------------------------------------------------------
; install_app - place app_image into a free flash slot and relocate it.
; Faithful to the app_create flow, using the embedded image's real size and
; its relocation table instead of assembly-time symbols.
;-------------------------------------------------------------------------------
install_app:
	di
	; --- find the free slot: walk app headers from 0x3B0000 ---
	ld	hl, 0x3B0000
.find:
	push	hl
	call	ArcChkNext		; -> next app header; NZ while apps remain
	pop	de
	jr	nz, .find
	ex	de, hl			; hl = end of the app list (free boundary)
	; --- dest = free_boundary - image_size ---
	ld	de, app_image_len
	or	a, a
	sbc	hl, de
	ld	(dest), hl		; flash destination base

	; --- derive code offset, relocation base and table end ---
	ld	hl, (app_image + APP_CODEFIELD)  ; code offset, relative to header
	ld	de, APP_IMAGE_HEADER
	add	hl, de			; hl = code offset, relative to the image
	push	hl			; (code_off)
	ld	de, app_image
	add	hl, de			; hl = app_image + code_off = end of reloc table
	ld	(tbl_end), hl
	pop	hl			; code_off
	ld	de, (dest)
	add	hl, de			; hl = dest + code_off = relocation base
	ld	(reloc_base), hl

	; --- write the whole image: WriteFlash(dest <- app_image, len) ---
	ld	hl, app_image
	ld	de, (dest)
	ld	bc, app_image_len
	call	port_unlock
	call	_WriteFlash
	call	port_lock

	; --- walk the 6-byte relocation entries in the source table ---
	ld	hl, app_image + APP_TABLE_OFFSET
	ld	(tbl_ptr), hl
.reloc:
	ld	hl, (tbl_ptr)
	ld	de, (tbl_end)
	or	a, a
	sbc	hl, de
	jr	nc, .done		; tbl_ptr >= tbl_end -> finished
	; patch location = reloc_base + u24(entry[0..2])
	ld	hl, (tbl_ptr)
	ld	hl, (hl)
	ld	de, (reloc_base)
	add	hl, de
	ld	(patch_loc), hl
	; relocated value = reloc_base + u24(entry[3..5])
	ld	hl, (tbl_ptr)
	inc	hl
	inc	hl
	inc	hl
	ld	hl, (hl)
	ld	de, (reloc_base)
	add	hl, de
	ld	(patch_val), hl
	; WriteFlash(patch_loc <- patch_val, 3)
	ld	hl, patch_val
	ld	de, (patch_loc)
	ld	bc, 3
	call	port_unlock
	call	_WriteFlash
	call	port_lock
	; advance to the next 6-byte entry
	ld	hl, (tbl_ptr)
	ld	de, 6
	add	hl, de
	ld	(tbl_ptr), hl
	jr	.reloc
.done:
	ret

;-------------------------------------------------------------------------------
; port_setup / port_unlock / port_lock - flash write-protect control.
; Reimplementation of the public Cesium/AsmHook2 technique (see file header):
; TI left no flash-write-enable call, so port_setup scans the OS for the unlock
; gadget and port_unlock jumps to it before poking the flash-controller ports.
; port_setup returns A=0 on success, nonzero if this OS is not understood.
;-------------------------------------------------------------------------------
port_pattern:
	db	0xED, 0x79, 0x78, 0xFE, 0xA0, 0x28, 0x01, 0xCF
port_pattern.len := $ - port_pattern

port_setup:
	di
	ld	hl, (0x000008 + 5)	; validate the OS call table shape
	ld	a, (hl)
	cp	a, 0xCD
	jr	nz, .unsupported
	ld	hl, (ti.KeypadScanFull + 1)
	ld	bc, 10
	add	hl, bc
	push	hl
	ld	b, port_pattern.len
	ld	de, port_pattern
	call	ti.StrCmpre		; compare the gadget signature
	pop	hl
	jr	nz, .unsupported
	ld	(port_unlock.target), hl
	xor	a, a
	ret
.unsupported:
	ld	a, 1
	ret

port_unlock:
	push	iy, de, bc, hl
	call	ti._frameset0
	ld	iy, .finish
	ld	sp, ti._indcall + 7
	ld	bc, 0x22
	xor	a, a
.target := $ + 1
	jp	0			; patched to the unlock gadget by port_setup
.finish:
	ld	sp, ix
	pop	ix
	ld	a, 0x8C
	out0	(0x24), a
	in0	a, (0x06)
	or	a, 4
	out0	(0x06), a
	jr	port_lock.pop

port_lock:
	push	iy, de, bc, hl
	xor	a, a
	out0	(0x28), a
	in0	a, (0x06)
	res	2, a
	out0	(0x06), a
	ld	a, 0x88
	out0	(0x24), a
	ld	a, 0xD1
	out0	(0x22), a
.pop:
	pop	hl, bc, de, iy
	ret

;-------------------------------------------------------------------------------
; UI helpers
;-------------------------------------------------------------------------------
clear_screen:
	call	ti.ClrScrn
	jp	ti.HomeUp

message:				; hl = string; show it and wait, then exit
	push	hl
	call	clear_screen
	pop	hl
	call	ti.PutS
	call	ti.GetKey
	call	ti.ClrScrn
	jp	ti.HomeUp

str_installing:	db "Installing RESIDOCK...", 0
str_installed:	db "Installed. Open RESIDOCK in the [apps] menu.", 0
str_exists:	db "RESIDOCK already installed. Delete it first.", 0
str_bad_os:	db "This OS is not supported by the installer.", 0

;-------------------------------------------------------------------------------
; scratch (program RAM) and the embedded app image
;-------------------------------------------------------------------------------
dest:		rb 3
reloc_base:	rb 3
tbl_ptr:	rb 3
tbl_end:	rb 3
patch_loc:	rb 3
patch_val:	rb 3

app_image:
	file 'RESIDOCK.bin'
app_image_end:
app_image_len := app_image_end - app_image
