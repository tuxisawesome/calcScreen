;-------------------------------------------------------------------------------
; RESIDIAG - read-only diagnostic for the RESIDOCK installer.
;
; Writes NO flash and installs nothing - completely safe to run. It steps
; through the same preparation the installer does (locate the flash-unlock
; gadget, look the app up by name, walk the app headers to find the free slot)
; and prints a checkpoint before each step, so if the calculator crashes the
; last line on screen tells us which step is at fault. It stops before any
; flash write.
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
APP_NAME_OFFSET := 256 + 3

	jp	start

;-------------------------------------------------------------------------------
start:
	ld	iy, osFlags
	call	ti.ClrScrn
	call	ti.HomeUp
	ld	hl, s_title
	call	ti.PutS
	call	ti.NewLine

	; --- step 1: locate the flash-unlock gadget (read-only) ---
	ld	hl, s_1
	call	ti.PutS
	call	port_setup		; sets flags only; no write
	ld	iy, osFlags
	or	a, a
	jr	z, .s1ok
	ld	hl, s_nogadget
	call	ti.PutS
	jp	finish			; cannot continue safely
.s1ok:
	ld	hl, s_ok
	call	ti.PutS
	call	ti.NewLine

	; --- step 2: look RESIDOCK up by name (read-only) ---
	ld	hl, s_2
	call	ti.PutS
	ld	hl, app_name
	call	FindAppStart
	ld	iy, osFlags
	jr	nz, .s2new
	ld	hl, s_exists
	call	ti.PutS
	jp	finish			; already installed; nothing to probe
.s2new:
	ld	hl, s_ok
	call	ti.PutS
	call	ti.NewLine

	; --- step 3: walk app headers to find the free slot (read-only) ---
	ld	hl, s_3
	call	ti.PutS
	ld	hl, 0x3B0000
.find:
	push	hl
	call	ArcChkNext
	pop	de
	jr	nz, .find
	ld	iy, osFlags
	ld	hl, s_ok
	call	ti.PutS
	call	ti.NewLine

	ld	hl, s_allok
	call	ti.PutS

finish:
	call	ti.NewLine
	ld	hl, s_key
	call	ti.PutS
	call	ti.GetKey
	call	ti.ClrScrn
	jp	ti.HomeUp

;-------------------------------------------------------------------------------
; port_setup - locate the flash-unlock gadget by scanning the OS. Read-only;
; returns A=0 and records nothing we use here, A<>0 if the OS is not recognised.
; Full version WITH the OS-shape guard (confirms the jump table points at
; CheckIfEmulated) so an unexpected OS bails instead of matching garbage.
;-------------------------------------------------------------------------------
port_setup:
	di
	ld	hl, (0x000008 + 5)
	ld	a, (hl)
	cp	a, 0xCD			; a CALL opcode?
	jr	nz, .bad
	inc	hl
	ld	de, (hl)
	ld	hl, (ti.CheckIfEmulated + 1)
	or	a, a
	sbc	hl, de			; does it call CheckIfEmulated?
	jr	nz, .bad
	ld	hl, (ti.KeypadScanFull + 1)
	ld	bc, 10
	add	hl, bc
	push	hl
	ld	b, port_pattern.len
	ld	de, port_pattern
	call	StrCmpre		; is the unlock gadget signature here?
	pop	hl
	jr	nz, .bad
	xor	a, a			; found
	ret
.bad:
	ld	a, 1
	ret

port_pattern:
	db	0xED, 0x79, 0x78, 0xFE, 0xA0, 0x28, 0x01, 0xCF
port_pattern.len := $ - port_pattern

app_name:
	file 'RESIDOCK.bin' : APP_NAME_OFFSET, 8   ; the 8-byte app name

s_title:    db "RESIDOCK diagnostic", 0
s_1:        db "1 unlock scan... ", 0
s_2:        db "2 find app...    ", 0
s_3:        db "3 free slot...   ", 0
s_ok:       db "OK", 0
s_nogadget: db "FAIL: OS not recognised", 0
s_exists:   db "already installed", 0
s_allok:    db "All checks passed.", 0
s_key:      db "Press a key.", 0
