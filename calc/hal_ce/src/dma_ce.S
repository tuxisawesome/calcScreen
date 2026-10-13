;-------------------------------------------------------------------------------
; dma_ce.S - the one thing the CE USB HAL needs in assembly: a near-atomic
; read-and-clear of the usbRxZlp register (0xE20150).
;
; Everything else in the driver is done in C with byte MMIO (mmio_ce.h) and the
; DMA engine is driven packet-by-packet from usb_ce.c, so this file is kept
; deliberately tiny (SPEC section B: "you may keep dma_ce.S minimal").
;
; SPEC 5.3 / gotcha 3: usbRxZlp is NOT a write-1-to-clear register. It is a
; plain read/write register whose set bits say "a zero-length OUT packet
; arrived on EPn". It is cleared by writing 0. usbdrvce performs the read and
; the clearing store back-to-back under `di` (drv:4703-4721, `ld c,(hl);
; ld (hl),b`) so no event is lost in the window between them. We mirror that:
; disable interrupts around the read+clear, then restore the caller's previous
; interrupt state (the driver normally runs fully `di`, but this stays correct
; either way). We discard the value: our only bulk OUT endpoint is EP2, and a
; zero-length OUT packet contributes nothing to the host->calc byte stream, so
; it just has to be acknowledged, not decoded.
;
; Assembled by z80-none-elf-as -march=ez80+full after ez80-clang -E, exactly
; like crt0.S. A leading underscore on the label is the C symbol name, i.e.
; C `halce_rxzlp_clear` == asm `_halce_rxzlp_clear`.
;-------------------------------------------------------------------------------

	.assume	adl=1

	.text

	.global	_halce_rxzlp_clear
	.type	_halce_rxzlp_clear, @function
_halce_rxzlp_clear:
	ld	a, i			; P/V flag := IEF2 (were interrupts enabled?)
	di				; close the race window (SPEC 5.3)
	ld	hl, 0x0E20150		; usbRxZlp (SPEC 1.2)
	ld	a, (hl)			; read the pending-ZLP bitmap...
	ld	(hl), 0			; ...and clear it with a plain 0 store (NOT W1C)
	ret	po			; IEF2 was 0: leave interrupts disabled
	ei				; otherwise restore the enabled state
	ret
