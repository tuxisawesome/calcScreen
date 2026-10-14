;-------------------------------------------------------------------------------
; RESIDWR - minimal flash-write test for the RESIDOCK installer.
;
; The read-only diagnostics pass but the real installer crashes before writing
; the image (dest reads FFFFFF afterwards), so the fault is in the first flash
; operation: port_unlock or the _WriteFlash call. This isolates which.
;
; It writes just 4 bytes (0x11,0x22,0x33,0x44) into the confirmed-erased free
; app slot, printing a checkpoint before each risky step so a crash's last line
; names the culprit:
;   "A unlock"  shown, then crash  -> port_unlock is at fault
;   "B write"   shown, then crash  -> _WriteFlash is at fault
;   "C ok <hx>" shown              -> the write path works; fault is elsewhere
;
; Low risk (4 bytes into verified-empty flash) but it DOES write flash; recover
; with a RAM clear if it misbehaves. Build: fasmg wtest.asm RESIDWR.8xp
;-------------------------------------------------------------------------------

include 'ez80.inc'
include 'tiformat.inc'
format ti executable program 'RESIDWR'
include 'ti84pceg.inc'

StrCmpre     := 0x021DB0
ArcChkNext   := 0x022044
WriteFlash   := 0x0002E0
osFlags      := 0xD00080

	jp	start

start:
	ld	iy, osFlags
	call	ti.ClrScrn
	call	ti.HomeUp
	ld	hl, s_title
	call	msg
	call	nl

	call	port_setup		; patches port_unlock.target
	ld	iy, osFlags
	or	a, a
	jr	z, .ok
	ld	hl, s_osfail
	call	msg
	jp	finish
.ok:
	; free slot -> dest (same walk the installer uses)
	ld	hl, 0x3B0000
.find:
	push	hl
	call	ArcChkNext
	pop	de
	jr	nz, .find
	ex	de, hl
	ld	de, 4			; we only write 4 bytes for the test
	or	a, a
	sbc	hl, de
	ld	(dest_var), hl
	ld	iy, osFlags
	ld	hl, s_dest
	call	msg
	ld	hl, (dest_var)
	call	put_hex24
	call	nl

	ld	hl, s_a			; checkpoint A: about to unlock
	call	msg
	call	nl
	call	port_unlock

	ld	iy, osFlags
	ld	hl, s_b			; checkpoint B: unlock survived, about to write
	call	msg
	call	nl
	ld	hl, testbytes
	ld	de, (dest_var)
	ld	bc, 4
	call	WriteFlash
	call	port_lock

	ld	iy, osFlags
	ld	hl, s_c			; checkpoint C: write survived
	call	msg
	ld	hl, (dest_var)
	ld	hl, (hl)		; read back (expect 332211)
	call	put_hex24

finish:
	call	nl
	ld	hl, s_key
	call	msg
	call	ti.GetKey
	call	ti.ClrScrn
	jp	ti.HomeUp

; --- helpers ---
msg:
	ld	iy, osFlags
	jp	ti.PutS
nl:
	ld	iy, osFlags
	jp	ti.NewLine

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
; flash port control (same as installer.asm)
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
	ld	(port_unlock.target), hl
	xor	a, a
	ret
.bad:
	ld	a, 1
	ret

port_pattern:
	db	0xED, 0x79, 0x78, 0xFE, 0xA0, 0x28, 0x01, 0xCF
port_pattern.len := $ - port_pattern

port_unlock:
	push	iy, de, bc, hl
	call	ti._frameset0
	ld	iy, .finish
	ld	sp, ti._indcall + 7
	ld	bc, 0x22
	xor	a, a
.target := $ + 1
	jp	0
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

testbytes:  db 0x11, 0x22, 0x33, 0x44
hx_val:     rb 3
hx_buf:     rb 8
dest_var:   rb 3

s_title:  db "flash write test", 0
s_dest:   db "dest=", 0
s_a:      db "A unlock", 0
s_b:      db "B write", 0
s_c:      db "C ok ", 0
s_osfail: db "OS not recognised", 0
s_key:    db "Press a key.", 0
