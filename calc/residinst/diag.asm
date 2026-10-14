;-------------------------------------------------------------------------------
; RESIDIAG - read-only diagnostic for the RESIDOCK installer.
;
; Writes NO flash and installs nothing - completely safe to run. It runs the
; same preparation the installer does (locate the flash-unlock gadget, look the
; app up by name, walk the app headers to find the free slot) and prints a
; checkpoint + the key addresses before each step, so a crash's last line names
; the failing step and we can see where the installer would place the app. It
; stops before any flash write.
;
; Build: fasmg diag.asm RESIDIAG.8xp   (needs the CE asm includes on INCLUDE)
;-------------------------------------------------------------------------------

include 'ez80.inc'
include 'tiformat.inc'
format ti executable program 'RESIDIAG'
include 'ti84pceg.inc'

StrCmpre     := 0x021DB0
FindAppStart := 0x021100
ArcChkNext   := 0x022044
osFlags      := 0xD00080
APP_IMAGE_HEADER := 256
APP_NAME_OFFSET  := APP_IMAGE_HEADER + 3

	jp	start

;-------------------------------------------------------------------------------
start:
	ld	iy, osFlags
	call	ti.ClrScrn
	call	ti.HomeUp
	ld	hl, s_title
	call	ti.PutS
	call	ti.NewLine

	; --- step 1: locate the flash-unlock gadget ---
	ld	hl, s_1
	call	msg
	call	port_setup
	ld	iy, osFlags
	or	a, a
	jr	z, .s1ok
	ld	hl, s_fail
	call	msg
	jp	finish
.s1ok:
	ld	hl, s_ok
	call	msg
	ld	hl, s_gad		; " gadget@"
	call	msg
	ld	hl, (gadget_addr)
	call	put_hex24		; the flash-unlock gadget address
	call	nl

	; --- step 2: find RESIDOCK by name; show where (0 = not present) ---
	ld	hl, s_2
	call	msg
	ld	hl, app_image + APP_NAME_OFFSET
	call	FindAppStart
	ld	iy, osFlags
	push	af			; preserve the found/not-found flag
	jr	nz, .s2none
	call	put_hex24		; HL = app location
	jr	.s2done
.s2none:
	ld	hl, s_none
	call	msg
.s2done:
	pop	af
	call	nl

	; --- step 3: walk app headers -> free boundary + computed dest ---
	ld	hl, s_3
	call	msg
	ld	hl, 0x3B0000
.find:
	push	hl
	call	ArcChkNext
	pop	de
	jr	nz, .find
	ex	de, hl			; hl = free boundary
	ld	iy, osFlags
	push	hl
	call	put_hex24		; show the free boundary
	ld	hl, s_dest
	call	msg
	pop	hl
	ld	de, app_image_len
	or	a, a
	sbc	hl, de			; dest = boundary - image size
	ld	(dest_var), hl
	call	put_hex24		; show the computed destination
	call	nl
	; --- step 4: read back the 3 bytes already in flash at dest ---
	; FFFFFF = erased/free (good); 00 0F 81... = a previous write is there.
	ld	hl, s_4
	call	msg
	ld	hl, (dest_var)
	ld	hl, (hl)		; read 3 bytes at dest
	call	put_hex24
	call	nl
	ld	hl, s_allok
	call	msg

finish:
	call	nl
	ld	hl, s_key
	call	msg
	call	ti.GetKey
	call	ti.ClrScrn
	jp	ti.HomeUp

; --- helpers ---
msg:					; hl = string
	ld	iy, osFlags
	jp	ti.PutS
nl:
	ld	iy, osFlags
	jp	ti.NewLine

; print HL as 6 hex digits (preserves iy=flags for the PutS)
put_hex24:
	ld	(hx_val), hl
	ld	hl, hx_buf
	ld	a, (hx_val + 2)
	call	.byte
	ld	a, (hx_val + 1)
	call	.byte
	ld	a, (hx_val + 0)
	call	.byte
	ld	(hl), 0
	ld	iy, osFlags
	ld	hl, hx_buf
	jp	ti.PutS
.byte:
	push	af
	rra
	rra
	rra
	rra
	call	.nib
	pop	af
.nib:
	and	a, 0x0F
	add	a, 0x90
	daa
	adc	a, 0x40
	daa
	ld	(hl), a
	inc	hl
	ret

;-------------------------------------------------------------------------------
; port_setup - read-only; A=0 found, A<>0 OS not recognised. Includes the
; OS-shape guard so an unexpected layout bails instead of matching garbage.
;-------------------------------------------------------------------------------
port_setup:
	di
	ld	hl, (0x000008 + 5)
	ld	a, (hl)
	cp	a, 0xCD
	jr	nz, .bad
	inc	hl
	ld	de, (hl)
	ld	hl, (ti.CheckIfEmulated + 1)
	or	a, a
	sbc	hl, de
	jr	nz, .bad
	ld	hl, (ti.KeypadScanFull + 1)
	ld	bc, 10
	add	hl, bc
	push	hl
	ld	b, port_pattern.len
	ld	de, port_pattern
	call	StrCmpre
	pop	hl
	jr	nz, .bad
	ld	(gadget_addr), hl	; record it so we can display/inspect it
	xor	a, a
	ret
.bad:
	ld	a, 1
	ret

port_pattern:
	db	0xED, 0x79, 0x78, 0xFE, 0xA0, 0x28, 0x01, 0xCF
port_pattern.len := $ - port_pattern

app_image:
	file 'RESIDOCK.bin'
app_image_end:
app_image_len := app_image_end - app_image

hx_val:  rb 3
hx_buf:  rb 8
dest_var: rb 3
gadget_addr: rb 3

s_title: db "RESIDOCK diag 2", 0
s_1:     db "1 unlock... ", 0
s_2:     db "2 app@", 0
s_3:     db "3 free@", 0
s_dest:  db " dest=", 0
s_4:     db "4 at dest: ", 0
s_ok:    db "OK", 0
s_gad:   db " gadget@", 0
s_none:  db "none", 0
s_fail:  db "FAIL: OS?", 0
s_allok: db "slot walk OK", 0
s_key:   db "Press a key.", 0
