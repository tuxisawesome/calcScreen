# TI-84 Plus CE: FOTG210 USB device-mode spec for a minimal polled driver (no interrupts)

Scope: everything a polled C driver needs to take the CE's USB controller from TI-OS, enumerate as a
custom full-speed device with EP0, bulk EP1 IN and bulk EP2 OUT, move data by DMA, notice reset,
suspend and unplug, and hand the controller back to TI-OS.

## Sources and how they are cited

| Tag | File |
|---|---|
| `drv:N` | `/tmp/claude-0/ext/toolchain/src/usbdrvce/usbdrvce.asm`, line N (CE toolchain, proven on hardware) |
| `drvh:N` | `/tmp/claude-0/ext/toolchain/src/usbdrvce/usbdrvce.h`, line N |
| `inc:N` | `/tmp/claude-0/ext/toolchain/src/include/ti84pceg.inc`, line N |
| `cemu:usb.c:N` | `/tmp/claude-0/ext/CEmu/core/usb/usb.c` (CEmu HEAD, cloned 2026-10-13) |
| `cemu:fotg210.h` | `/tmp/claude-0/ext/CEmu/core/usb/fotg210.h` (bit names come from Faraday's U-Boot header) |
| `cemu:mem.c:N`, `cemu:interrupt.c` | other CEmu core files |
| `lnx:N`, `lnxh:N` | `/tmp/claude-0/ext/linux_fotg210/fotg210-udc.c` / `fotg210-udc.h` (Linux master) |

Confidence labels used below:
- **[HW]**: usbdrvce does exactly this, so it is proven on hardware.
- **[EMU]**: only CEmu says so.
- **[LNX]**: only the Linux FOTG210 driver says so. It runs on other SoCs, not the CE.
- **[INF]**: my own inference. Not proven.
- **[UNSURE]**: the sources disagree or say nothing.

---

## 0. Corrections to check first

1. **The controller base is `0xE20000`, not `0xE10000`.** `mpUsbRange := 0E20000h`, port `03000h`
   (inc:3078-3079). In CEmu's MMIO decode, `0xE2xxxx` goes to port range 3, which is USB
   (cemu:mem.c:15-16, `asic.c:52`). `0xE1xxxx` goes to port range 2, which is the **SHA-256** unit
   (`asic.c:51`). The USB window is 0x200 bytes (`0xE20000-0xE201FF`, port mirror `0x1FF`,
   `port.c:15`). Every address below is absolute and uses the base `0xE20000`.
2. **Access registers one byte at a time** (`volatile uint8_t *`). The eZ80 bus is 8-bit, and each
   byte write changes only that byte lane of the 32-bit register (CEmu `write8`; usbdrvce writes
   bytes everywhere). Two cases matter:
   - Status registers are write-1-to-clear (W1C). Writing 0 to a bit has no effect, so a byte write
     carrying only the bit you want to clear is safe.
   - `0xE201D0` (EP0 data port) pops data on every byte read. Never read it with multi-byte or
     speculative reads.
3. **The register window can be disabled.** CEmu models it: when `0xE201CB` bit 7 (DMA-control bit
   31) and `PHYTMSR.Unplug` (`0xE20114` bit 0) are both 1, registers `0x044-0x1C7` read 0 and ignore
   writes (cemu:usb.c:1457,1477). The CEmu commit is 9789b2b5, "Fix USB transfers failing after power
   cycle by implementing USB register disable". This implies TI-OS uses this state after a power
   cycle.
   - Write `0x00` to `0xE201CB` **first** during init.
   - That byte only holds DMA length bit 16 (bit 0) and this bit 31, so the write is harmless.
   - usbdrvce never writes this byte explicitly at init. It only clears it as a side effect of its
     first DMA (drv:3318, drv:4058-4063). [EMU, plus INF]

---

## 1. Register map (device mode, absolute addresses)

Bit names come from ti84pceg.inc (TI toolchain names). FOTG210/Linux names are in brackets.
"W1C" means write 1 to clear.

### 1.1 Global and OTG registers

| Addr | Name (inc) | Bits / meaning | Ref |
|---|---|---|---|
| `E20010` | usbCmd (EHCI USBCMD) | Host-controller only. b0 RunStop, b1 HcReset. Used only to quiesce the host side. | inc:3125-3147 |
| `E20014` | usbSts | b12 (`E20015` b4) HcHalted. b4 HostSysErr. Host side. | inc:3149-3174 |
| `E20018` | usbIntEn | Host interrupt enables. Write 0 to quiesce. | inc:3152 |
| `E20080`.. | **usbOtgCsr** (OTGCSR) | See 1.1.1. | inc:3248-3301, cemu:fotg210.h |
| `E20084`.. | usbOtgIsr (OTGISR), W1C | `E20084`: b0 BSrpComplete, b4 ASrpDetect, b5 AVbusErr, **b6 BSessEnd**. `E20085`: b0 (bit8) **RoleChg**, b1 **IdChg**, b2 Overcurr, b3 **BPlugRemoved**, b4 APlugRemoved. | inc:3303-3326; W1C per cemu:usb.c:1581-1584 and drv:4771,4790 |
| `E20088`.. | usbOtgIer | Same layout as OTGISR. 1 = enabled (gates only the IRQ line; status bits latch anyway). | inc:3306; cemu:usb.c:153-161 |
| `E200C0` | usbIsr, W1C | b0 Dev, b1 Otg, b2 Host. Summary of the three sources. | inc:3328-3343 |
| `E200C4` | usbImr | b0/b1/b2 = **mask** Dev/Otg/Host (1 = masked). b3 IntLevel (1 = active high). | inc:3331-3343; cemu:fotg210.h `IMR_*`; lnxh `GMIR_*` |

#### 1.1.1 OTGCSR (`0xE20080`, 32-bit)

| Byte | Bit (abs) | inc name | Meaning |
|---|---|---|---|
| `E20080` | 0 | bUsbBBusReq | B-device bus request (SRP/HNP) |
| | 1 | bUsbBHnp | B-device HNP enable |
| | 2 | bUsbBVbusDisc | B-device VBUS discharge |
| | 4 | bUsbABusReq | A-device bus request (drive VBUS) |
| | 5 | bUsbABusDrop | A-device bus drop (VBUS off) |
| | 6 | bUsbAHnp | |
| | 7 | bUsbASrpEn | A-device SRP detect enable |
| `E20081` | 8 | bUsbASrpMode | |
| | 9 / 10 / 11 | IdFilt / VbusFilt / HdiscFilt | 1 = long noise filter. Per cemu:fotg210.h: ID 3 ms vs 4 ms, VBUS 135 us vs 472 us, HDISC 135 us vs 270 us. |
| `E20082` | 16 | **bUsbBSessEnd** | B-session end (VBUS below the session-end threshold) |
| | 17 | **bUsbBSessVld** | **B-session valid (VBUS present, seen as a B-device)** |
| | 18 | bUsbASessVld | |
| | 19 | bUsbAVbusVld | |
| | **20** | **bUsbRole** | **0 = Host, 1 = Device** (current role, read-only) |
| | **21** | **bUsbId** | **0 = A (mini-A plug), 1 = B** (ID pin) |
| `E20082-83` | 22-23 | bUsbSpd | Host side only: speed of the attached device (0 FS, 1 LS, 2 HS) |

The CEmu reset value is `0x00310E20`: B, Device, long filters, A_BUS_DROP (cemu:usb.c:1291).

### 1.2 Device controller registers

| Addr | inc name | Bits / meaning | Ref |
|---|---|---|---|
| `E20100` | **usbDevCtrl** (DMCR) | b0 RemoteWake (capability). b1 HalfSpd (FPGA test only). **b2 GirqEn** (device global IRQ enable). b3 DevSuspend (go-suspend). **b4 DevReset** (soft reset, self-clearing in CEmu: write mask `0x2AF`). **b5 DevEn** (chip enable). **b6 DevSpd** (read-only: 1 = high speed negotiated). | inc:3345-3363; cemu:usb.c:1607 |
| `E20101` | usbDevCtrl+1 | **b1 (bit 9) ForceFullSpd** | inc:3362 |
| `E20104` | **usbDevAddr** (DAR) | b0-6 USB address. **b7 DevConf** ("SET_CONFIGURATION done", gates non-EP0 traffic). | inc:3365-3371 |
| `E20108` | usbDevTest | **b0 TstClrFifo** (clear all FIFOs). Other bits are test modes. | inc:3373-3389 |
| `E2010C` | usbSofFrNum | b0-10 frame number, b11-13 microframe | inc:3391-3397; drv:1752-1772 |
| `E20114` | **usbPhyTmsr** | **b0 Unplug = soft-detach (removes the D+ pull-up)**. b1-b4 HS test modes. | inc:3405-3417; cemu:fotg210.h `PHYTMSR_UNPLUG` "Enable soft-detachment" |
| `E20120` | **usbCxFifo** (DCFESR) | Write: **b0 CxFifoFin** (`0x01`, "CX data transfer finished": ends the data stage and completes the status stage), b1 TstPktFin, **b2 CxFifoStall** (`0x04`), **b3 CxFifoClr** (`0x08`). Read-only: b4 Full, b5 Empty. | inc:3423-3447 |
| `E20121` | usbCxFifo+1 | Read-only: b0-b3 = **FIFO0..3 empty** (bits 8-11) | inc:3439-3447 |
| `E20123` | **usbCxFifoBytes** | b0-6: byte count in the CX (EP0) FIFO (OUT data) | inc:3449-3453 |
| `E20124` | usbIdle | b0-2: ms of idle before PHY suspend. usbdrvce writes 7 (drv:666-667). | inc:3455-3459 |
| `E20130` | usbGimr | Group mask, 1 = masked. b0 G0 (CX), b1 G1 (FIFO), b2 G2 (device). | inc:3461; cemu:fotg210.h `GIMR_*` |
| `E20134` | usbCxImr (G0 mask) | Same bit layout as CxIsr. 1 = masked. | inc:3474 |
| `E20138` | usbFifoRxImr (G1 mask, low) | Bit 2n = FIFOn OUT, bit 2n+1 = FIFOn SPK. 1 = masked. | inc:3495 |
| `E2013A` | usbFifoTxImr (G1 mask, bits 16-19) | b0-3 = FIFOn IN. 1 = masked. | inc:3520 |
| `E2013C/D` | usbDevImr (G2 mask) | Same layout as DevIsr. 1 = masked. | inc:3537 |
| `E20140` | usbGisr | b0 G0, b1 G1, b2 G2: summary of **unmasked** sources (CEmu computes it, cemu:usb.c:153-161) | inc:3464-3472 |
| `E20144` | **usbCxIsr (G0), W1C** | **b0 Setup**, b1 In (CX FIFO can take more IN data), b2 Out (CX OUT data present), **b3 End** (command end), b4 Err (command fail), b5 Abort (setup overrode a transfer in progress) | inc:3477-3493 |
| `E20148` | **usbFifoRxIsr (G1 low), W1C** | Bit 2n **FIFOn OUT** (packet in FIFO). Bit 2n+1 **FIFOn SPK** (that packet was short). | inc:3498-3518 |
| `E2014A` | **usbFifoTxIsr (G1 bits 16-19), W1C** | b0-3 **FIFOn IN** = "FIFO n empty / ready for IN data". **Level-like**: it reasserts while the FIFO is empty. | inc:3523-3535; drv:3381-3393 |
| `E2014C` | **usbDevIsr (G2 low), W1C** | **b0 Reset** (bus reset), **b1 Suspend**, **b2 Resume**, b3 IsocErr, b4 IsocAbt, **b5 ZlpTx**, **b6 ZlpRx**, **b7 DmaFin** | inc:3540-3566 |
| `E2014D` | usbDevIsr+1, W1C | **b0 DmaErr** (bit 8), b1 Idle (bit 9), **b2 Wakeup** (bit 10, "wake-up by VBUS") | inc:3555-3564 |
| `E20150` | **usbRxZlp** | Bit (n-1) = a zero-length OUT packet arrived on EPn. **Plain read/write: clear it by writing 0** (drv:4684-4721; cemu:usb.c:1696). | inc:3568-3592 |
| `E20154` | **usbTxZlp** | Bit (n-1) = a ZLP was sent on EPn IN. Plain read/write; write 0 to clear (drv:4676-4678). | inc:3571 |
| `E20160+4(n-1)` | **usbInEpN** (n = 1..8) | b0-10 MaxPktSz. **b11 Stall**. **b12 Reset** (reset data toggle; **software must clear it again**, see 4.6). b13-14 TxNum (iso). **b15 SendZlp**. Byte layout: `+0` MPS[7:0]; `+1`: b0-2 MPS[10:8], b3 Stall, b4 Reset, b5-6 TxNum, b7 SendZlp. | inc:3629-3686 |
| `E20180+4(n-1)` | **usbOutEpN** | b0-10 MaxPktSz, b11 Stall, b12 Reset | inc:3653-3683 |
| `E201A0+(n-1)` | **usbEpNMap** (byte per EP, n = 1..8) | b0-3 = FIFO number for IN, b4-7 = FIFO number for OUT | inc:3688-3715 |
| `E201A8+f` | **usbFifoFMap** (f = 0..3) | b0-3 = endpoint number. b4-5 direction: 0 Out, 1 In, 2 Bi. | inc:3717-3735 |
| `E201AC+f` | **usbFifoFCfg** | b0-1 type (1 iso, **2 bulk**, 3 intr). b2-3 blocks-1 (0 = 1 block). b4 block size (0 = 512, 1 = 1024). **b5 FifoEn**. | inc:3737-3764 |
| `E201B0+4f` | **usbFifoFCsr** | **b0-10 byte count of the OUT packet in FIFO f**. b12 FIFO reset. | inc:3766-3781 |
| `E201C0` | **usbDmaFifo** | DMA target select: b0-3 = FIFO0..3, **b4 = CX FIFO**, **0 = none**. Also gates EP0 data-port reads (see 4.1). | inc:3783-3798 |
| `E201C8` | **usbDmaCtrl** | **b0 Start**. **b1 Dir: 0 = FIFO to memory (OUT), 1 = memory to FIFO (IN)**. b2 FIFO-to-FIFO. **b3 Abort**. **b4 ClrFifo** (clear the FIFO on abort). | inc:3800-3813; cemu:fotg210.h `DMACTRL_*` |
| `E201C9-CB` | **usbDmaLen** | DMA byte count, **17 bits** (`0x1FFFF`): `C9` = len[7:0], `CA` = len[15:8], `CB` b0 = len[16]. `CB` b7 = bit 31, the register-disable bit (section 0.3). | inc:3815-3819; cemu `DMACTRL_LEN_MASK 0x1ffff` |
| `E201CC-CE` | **usbDmaAddr** | Memory address for the DMA. Write all 3 bytes. **Only 19 bits are significant (`0x7FFFF`)**, so this is an offset into the RAM window (see 5.4). | inc:3821-3825 |
| `E201D0-D3` | **usbEp0Data** | 8-byte SETUP packet read port. Reads pop only while `usbDmaFifo` = CX (`0x10`). | inc:3827-3831; cemu:usb.c:1446-1466 |

### 1.3 CPU interrupt controller (`0xF00000`)

| Addr | inc name | Meaning | Ref |
|---|---|---|---|
| `F00000` | mpIntStat | **Raw** status (not masked). Bit 13 USB, bit 3 Timer3. | inc:3956-3961; cemu:interrupt.c (index 0 = status) |
| `F00004` | **mpIntMask** | Actually an **enable** register: 1 = delivered to the CPU. **Bit 3 = intTmr3** (byte `F00004` bit 3). **Bit 13 = intUsb** (byte `F00005` bit 5). | inc:3963-3964, 3984-3995 |
| `F00008` | mpIntAck | Write 1 to ack **latched** bits only | inc:3966; cemu:interrupt.c (index 2) |
| `F00014` | mpIntMasked | status & enable | inc:3975 |
| | `intDefaultMask` | `intOn | intTmr3 | intOSTmr | intRtc | intUsb` | inc:3996 |

### 1.4 OS RAM locations involved in the hand-off

| Addr | Name | Meaning | Ref |
|---|---|---|---|
| `D13FD8` | usbArea | 14306 bytes of OS USB-stack RAM | inc:3038 |
| `D14091` | **usbHandleKeys** | "zero this byte while using usbArea to keep GetCSC from affecting other parts of usbArea" | inc:3039 |
| `D177B7` | **usbInited** | "zero this byte while using usbArea and to reset the usb stack when interrupts are re-enabled" | inc:3040 |
| `D0009B` | flags+`1Bh` (apdFlags2), bit 5 | usbdrvce sets it during a session and clears it at cleanup. The meaning is **undocumented** (the include only names bit 6, inc:7004-7005). | drv:603, drv:814, drv:2321 |

### 1.5 How usbdrvce decides it is a device (role detection)

- `currentRole = byte(0xE20082) & 0x30`, i.e. bit 20 Role and bit 21 ID (drv:4772-4773). The enums
  are `ROLE_DEVICE = 0x10` and `ROLE_B = 0x20` (drv:489-497).
- **Device role ⇔ OTGCSR bit 20 is set**: `bit bUsbRole-16` is tested everywhere (drv:1043, 1551,
  1754, 1915, 1970, 1991, 2947, 3605).
- The ID pin (bit 21) only tells A from B. A mini-A plug in the calc gives A/Host. A normal cable to
  a PC gives B/Device.
- At init usbdrvce stores an impossible role value (3, drv:636-637). It then calls the role-change
  handler with `HL = 0xE20085` (drv:756-757). The handler:
  - W1C-clears OTGISR RoleChg|IdChg (`0xE20085` = `0x03`, drv:4771);
  - reads the role;
  - calls `_PowerVbusForRole` (drv:4777).
- `_PowerVbusForRole`, device branch (drv:2460-2472):
  1. `OTGCSR.ASrpEn = 0`.
  2. Stop the host schedules and halt/reset the EHCI host controller (drv:2332-2398).
  3. `OTGCSR.ABusDrop = 1` and `OTGCSR.ABusReq = 0` (VBUS off as A-device).
  4. Tail-jump to the **undocumented OS jump-table routine `0x021C68`** with `HL = 0xE20080`. The
     include lists it only as an unnamed entry (inc:1779), so its function is **[UNSURE]**. It
     probably powers down the VBUS charge pump. The host branch calls the equally unnamed
     `0x021B70` (drv:2445, inc:1717) before `ABusReq = 1`.
- Role changes later arrive as OTGISR RoleChg/IdChg (enabled in OTGIER, drv:689-690).
- **For your driver:** require `(rd8(0xE20082) & 0x10) != 0`. Otherwise the calc is host (A-plug)
  and you should abort or wait. Also poll OTGISR bits 8/9 during operation.

---

## 2. Init and hand-back sequences

### 2.1 Take-over and enumeration (recommended order)

Step references point to usbdrvce lines where applicable. "wr8(a, v)" means one byte write.

**A. Fence off TI-OS**

1. `di` while you change masks.
2. Save the three bytes at `0xF00004-0xF00006` (mpIntMask) for restore.
3. Clear **intTmr3** and **intUsb** in mpIntMask:
   - `wr8(0xF00004, rd8(0xF00004) & ~0x08)`
   - `wr8(0xF00005, rd8(0xF00005) & ~0x20)`

   Why: with timer-3 interrupts enabled, "the OS will try to do USB stuff and corrupt" the driver's
   state (drvh:12-14). usbdrvce refuses to run (`USB_ERROR_SYSTEM`) while `mpIntMask & intTmr3` is set
   (drv:2273-2275). usbdrvce itself only enables intUsb transiently, for `halt` (drv:828-834). A
   polled driver should keep intUsb disabled. You can then `ei` again if you want TI-OS's other
   interrupts (OS timer, ON key) to keep running. Or stay `di` for the whole session, which is
   simplest.
4. `call 0x0004F4` (boot `usb_DisableTimer`). usbdrvce does this first thing (drv:589, inc:2442).
   Exact effect **[UNSURE]**: it is presumably the OS-USB timer. usbdrvce never calls the matching
   `usb_EnableTimer` (`0x0004F8`) at cleanup; it relies on the OS re-init.
5. `wr8(0xD14091 /*usbHandleKeys*/, 0)`, kept at 0 for the whole session. usbdrvce writes 0 (drv:614)
   and even removes that 32-byte block from its allocator so it stays 0 (drv:716-719).
6. `usbInited` (`0xD177B7`): **leave it nonzero during the session; zero it only at hand-back.**
   - usbdrvce sets it to **1** (as part of filling usbArea with `0x01`, drv:601-602, drv:2302-2320).
     It treats "not equal to 1" as "the OS clobbered us" (drv:2279-2281).
   - The include comment says "zero ... to reset the usb stack when interrupts are re-enabled"
     (inc:3040). A zero value is therefore the OS's re-init trigger, which you want at exit, not in
     the middle of the session. **[INF]**
7. `set bit 5 of (0xD0009B)` to mirror usbdrvce (drv:603). Meaning undocumented. Clear it at exit.

**B. Quiesce the host half of the OTG controller** (usbdrvce: drv:693 → drv:2361-2398, and again in
`_PowerVbusForRole` drv:2463)

8. `wr8(0xE201CB, 0x00)`. Clears the register-window disable (section 0.3). **[EMU, INF]**
9. `wr8(0xE20018, 0)` (usbIntEn = 0) and `wr8(0xE20010, 0)` (usbCmd: RunStop = 0).
10. Wait until `rd8(0xE20015) & 0x10` (HcHalted). usbdrvce allows ≤ **2 ms** (drv:2368), then
    continues regardless.
11. Optional, as usbdrvce does: `wr8(0xE20010, 0x02)` (HcReset) and wait for the bit to clear, ≤
    **250 ms** (drv:2376-2382). It is not needed for device operation; skipping it is **[INF]**.

**C. OTG: make sure the calc is a B-device and not driving VBUS**

12. Check that `rd8(0xE20082) & 0x10` (Role = Device) is set; else abort (section 1.5).
13. Clear stale role/ID change bits: `wr8(0xE20085, 0x03)` (drv:4771).
14. Mirror `_PowerVbusForRole` (drv:2461-2472):
    - `OTGCSR.b7 = 0` (byte `E20080` &= `~0x80`);
    - `OTGCSR.b5 = 1, b4 = 0` (byte `E20080` = `(x | 0x20) & ~0x10`);
    - optionally `call 0x021C68` with `HL = 0xE20080`, as usbdrvce does. This is undocumented
      **[UNSURE]**. If TI-OS was already in device role, VBUS is not being driven anyway.
15. OTG interrupt enables as usbdrvce sets them (they matter only for the IRQ line; you will poll
    OTGISR directly):
    - `wr8(0xE20088, 0x31)` (BSrpComplete | ASrpDetect | AVbusErr; BSessEnd is commented out, drv:688);
    - `wr8(0xE20089, 0x1F)` (RoleChg | IdChg | Overcurr | BPlugRemoved | APlugRemoved, drv:690);
    - `wr8(0xE200C4, 0x08)` (usbImr: nothing masked, active-high level, drv:691-692).

**D. Device controller: soft-disconnect, reset, configure** (usbdrvce: drv:651-692, executed in this order)

16. FIFOs disabled: `wr8(0xE201AC..0xE201AF, 0x00)` (drv:651-655).
17. FIFO to EP map, FIFO f serves EP f+1:
    - `wr8(0xE201A8, 0x01)`, `wr8(0xE201A9, 0x02)`, `wr8(0xE201AA, 0x03)`, `wr8(0xE201AB, 0x04)`
      (drv:656-660);
    - direction bits stay 0 until SET_CONFIGURATION.
18. EP to FIFO map, EPn uses FIFO n-1 in both directions:
    - `wr8(0xE201A0, 0x00)`, `wr8(0xE201A1, 0x11)`, `wr8(0xE201A2, 0x22)`, `wr8(0xE201A3, 0x33)`
      (drv:661-665);
    - CEmu resets these to `0xFF`/`0x0F` (cemu:usb.c:1315-1316), so they **must** be written.
19. `wr8(0xE20124, 7)` (idle = 7 ms, drv:666-667).
20. `wr8(0xE20101, 0x02)` (**ForceFullSpd**, drv:668-669).
21. `wr8(0xE20100, 0x35)` = DevReset(`0x10`) | DevEn(`0x20`) | GirqEn(`0x04`) | RemoteWake(`0x01`)
    (drv:670-671). DevReset is a self-clearing soft reset (CEmu does not store bit 4,
    cemu:usb.c:1607-1608).
    - usbdrvce has **no delay** after this.
    - Linux waits 100-200 us after soft reset, "without a datasheet, no idea how long" (lnx:1052-1054).
    - **Recommend** a busy-wait of ≥ 200 us here (≈ 10 000 cycles at 48 MHz), then re-write steps
      16-18 to be safe. **[INF]**
22. `wr8(0xE20114, 0x01)`: **PHYTMSR.Unplug = 1, soft-disconnect** (drv:672-673). From this moment
    the host sees the calc as detached.
23. Interrupt masks as usbdrvce leaves them (drv:674-685):

    | Write | Effect |
    |---|---|
    | `wr8(0xE20130, 0)` | groups unmasked |
    | `wr8(0xE20134, 0)` | all CX sources unmasked |
    | `wr8(0xE20138, 0)` | all FIFO OUT/SPK unmasked |
    | `wr8(0xE2013A, 0x0F)` | **all FIFO IN masked**: they are level and fire whenever a FIFO is empty |
    | `wr8(0xE2013C, 0)` | |
    | `wr8(0xE2013D, 0x02)` | Idle masked |

    For pure polling the masks do not matter, because status bits latch regardless (see 3.2). Keep
    them for the TI-OS-like state.
24. Clear stale status (W1C, **[INF]**; usbdrvce does not do this explicitly):

    | Write | Clears |
    |---|---|
    | `wr8(0xE20144, 0x3F)` | CX |
    | `wr8(0xE20148, 0xFF)` | FIFO OUT/SPK |
    | `wr8(0xE2014A, 0x0F)` | FIFO IN |
    | `wr8(0xE2014C, 0xFF)` | device group, low byte |
    | `wr8(0xE2014D, 0x07)` | device group, high byte |
    | `wr8(0xE20150, 0)` and `wr8(0xE20154, 0)` | ZLP registers |
    | `wr8(0xE20084, 0x71)` and `wr8(0xE20085, 0x1F)` | OTG |

    Also `wr8(0xE201C0, 0)` (no DMA FIFO selected) and `wr8(0xE20104, 0)` (address 0, not configured).
25. **Hold the disconnect** long enough for the host to notice it and forget the old device.
    usbdrvce has no fixed delay: it reconnects on the DevWakeup interrupt (step 26).
    - USB 2.0 hosts debounce attach for 100 ms (T_ATTDB) and poll hub status about every 32 ms or
      less.
    - **Recommend ≥ 100 ms, ideally 250 ms**, e.g. 25× boot `Delay10ms` at `0x0003B4` (inc:2362).
    - These timings are general USB knowledge, not from the sources. **[INF]**
26. **Reconnect**: wait until VBUS is valid, then `wr8(0xE20114, rd8(0xE20114) & ~0x01)` (Unplug = 0).
    - usbdrvce clears Unplug in its **DevWakeup** handler (`0xE2014D` b2, "wake-up by VBUS",
      drv:4738-4744) and then W1C-clears that bit.
    - Polled: clear Unplug when **either** `rd8(0xE2014D) & 0x04` **or** `rd8(0xE20082) & 0x02`
      (B_SESS_VLD).
    - **[UNSURE]**: on hardware it is not known whether DevWakeup fires when VBUS was already present
      before init. CEmu never raises DevWakeup at all (grep shows only RESUME, RESET, SUSPEND, IDLE
      being raised, cemu:usb.c:205,663,691,1325). CEmu signals a cable plug-in with `GISR2_RESUME` plus
      the OTGCSR A_VBUS_VLD / A_SESS_VLD / B_SESS_VLD bits (cemu:usb.c:199-206). Checking B_SESS_VLD
      too covers both.
27. The host now resets the bus. Wait for **DevIsr.Reset** (`0xE2014C` b0) and handle it (section 6.1).
    Then answer EP0 setups (section 4). Enumeration is complete after SET_CONFIGURATION (4.7).

### 2.2 Hand-back to TI-OS

usbdrvce `usb_Cleanup` (drv:779-817) does exactly the following:

1. Stop and reset the host controller (drv:785-786 → drv:2332-2398).
2. Device mask state:
   - `wr8(0xE20130, 0)` (Gimr, drv:788-789);
   - `wr8(0xE2013D, 0)` (unmask DmaErr/Idle/Wakeup, drv:790-791);
   - `wr8(0xE20138, 0xFF)` (mask all FIFO OUT/SPK, drv:798-799);
   - `wr8(0xE2013A, 0x0F)` (mask all FIFO IN, drv:800-801).
3. `_Init(0)`: zero-fill `usbArea` **through** `usbInited` inclusive, i.e. `0xD13FD8..0xD177B7`, 14304
   bytes (drv:813, drv:2302-2320). This makes **usbInited = 0** and **usbHandleKeys = 0**.
4. `res 5,(0xD0009B)` (drv:814).
5. Timer 2 back to count-down (drv:815-816). Only relevant if you used timer 2.

What usbdrvce does **not** do at cleanup:
- touch PHYTMSR, DevCtrl or OTGCSR;
- call `usb_EnableTimer`;
- change mpIntMask (it leaves intUsb enabled, because `usb_WaitForInterrupt` set it, drv:829-831).

A block of device-register restores in cleanup was tried and is commented out (drv:792-812). The OS
re-initializes its USB stack on its own because usbInited == 0 (inc:3040).

**Recommended polled-driver exit sequence:**

1. `di`.
2. Abort DMA: `wr8(0xE201C8, 0x18)` (Abort | ClrFifo, as usbdrvce does on bus reset, drv:4624-4625),
   then `wr8(0xE201C0, 0)`.
3. Optional, to make the host drop your custom device immediately:
   - `wr8(0xE20114, rd8 | 0x01)` (Unplug = 1);
   - delay ≥ 100 ms;
   - **leave Unplug set** and let TI-OS reconnect.

   **[UNSURE]**: usbdrvce never does this. The evidence that TI-OS rewrites PHYTMSR itself is
   indirect: CEmu's comment "TI-OS does not necessarily rewrite PHYTMSR when it was already clear"
   (cemu:usb.c:1588-1592), and usbdrvce's init/wakeup sequence, which is presumably modeled on the
   OS. **The proven path is to not touch PHYTMSR.** If TI-OS fails to reappear on the host after you
   left Unplug set, clear it before hand-back instead.
4. Mask state as usbdrvce leaves it: `0xE20130 = 0`, `0xE2013D = 0`, `0xE20138 = 0xFF`,
   `0xE2013A = 0x0F`.
5. Halt the host controller (2.1 steps 9-10). It should already be halted.
6. Zero `0xD13FD8..0xD177B7` inclusive. At minimum, `usbInited (0xD177B7) = 0` and
   `usbHandleKeys (0xD14091) = 0`. Zeroing the whole area mirrors the proven path.
7. Clear bit 5 of `0xD0009B`.
8. Restore the saved `mpIntMask` bytes. Make sure intUsb (bit 13) ends up enabled, as TI-OS's default
   mask has it (inc:3996). The OS re-init "when interrupts are re-enabled" depends on it.
9. `ei`.

---

## 3. Interrupts versus polling

### 3.1 Keeping TI-OS's ISR out

- Disable **bit 3 (Timer 3)** and **bit 13 (USB)** in `mpIntMask` (`0xF00004`; bytes `F00004` b3 and
  `F00005` b5). See 2.1 step A3 for the citations.
- `mpIntMask` is an enable register: a bit set to 1 delivers the interrupt (cemu:interrupt.c index 1
  `enabled`).
- `mpIntStat` (`0xF00000`) bit 13 shows the raw USB line even when it is disabled
  (cemu:interrupt.c index 0 = `status`). usbdrvce's `usb_HandleEvents` gates on it (drv:872-874) and
  acks it at the end (drv:884-885). Polling the controller registers directly does not need it.
- The USB line is level-triggered from the controller, so `mpIntAck` (latched bits only) does not
  clear it (cemu:interrupt.c).

### 3.2 Polling the controller

- **Status bits latch whether or not they are masked.**
  - Proof on hardware: usbdrvce reads FifoTxIsr and ANDs it with `~FifoTxImr` in software
    (drv:4476-4481).
  - CEmu sets `gisrN |= bits` unconditionally and applies the masks only when computing the summary
    and IRQ (cemu:usb.c:153-181).
  - Linux also reads the raw value and masks it in software (lnx:882-995).
- **Do not rely on `0xE20140` (Gisr) or `0xE200C0` (Isr) for polling.** They summarize only unmasked
  sources, and `Isr.Dev` additionally needs `DevCtrl.GirqEn` (cemu:usb.c:153-161). Read the group
  registers directly.
- **Ack = W1C byte write** of exactly the bits you handled to the same byte. Examples:
  - `wr8(0xE2014C, 0x80)` clears DmaFin (drv:3351).
  - `wr8(0xE2014D, 0x01)` clears DmaErr (drv:3348).
  - `wr8(0xE20144, 0x01)` clears Setup (drv:3894-3895).
- **Never read-modify-write a W1C register.** Writing back a value you read clears every other
  pending bit in it.
- Linux acks with exactly such a read-and-clear-mask-write (lnx:32-38), which is **inconsistent with
  W1C**. CEmu (cemu:usb.c:1581-1695) and usbdrvce both use W1C. Follow them on the CE. **[UNSURE
  about Linux; the CE behavior is HW-proven]**
- After handling a group, usbdrvce also W1C-writes the group bit to Gisr (drv:4470-4471, 4499-4500,
  4516-4517) and the source bit to Isr (drv:4458-4459, 4535). In CEmu those writes have no lasting
  effect because the summaries are recomputed. Optional.

**Polling loop, in usbdrvce's priority order:**

usbdrvce visits groups in the order CX (G0), FIFO (G1), Device (G2) (drv:4449-4461). Within a group:

| Group | Order | Ref |
|---|---|---|
| CX | Setup, In, Out, End, Err, Abort | drv:4463-4473 |
| FIFO | IN0-3 (masked by `~FifoTxImr`), then for each FIFO: SPK, OUT | drv:4475-4501 |
| Device | Reset, Suspend, Resume, IsocErr, IsocAbt, ZlpTx, ZlpRx, DmaFin; then DmaErr, Idle, Wakeup | drv:4503-4519 |
| OTG | BSrp, ASrp, AVbusErr, BSessEnd; then RoleChg, IdChg, Overcurr, BPlugRemoved, APlugRemoved | drv:4521-4537 |

For a polled driver I recommend handling **DevIsr.Reset first**, because a bus reset invalidates
everything else.

```
for (;;) {
  g2l = rd8(E2014C); g2h = rd8(E2014D) & 0x07;
  otgl = rd8(E20084); otgh = rd8(E20085);
  if (g2l & 0x01) handle_bus_reset();                 // 6.1, ack wr8(E2014C,0x01)
  if ((otgh & 0x0B) || (otgl & 0x40) || !(rd8(E20082) & 0x02)) handle_disconnect_or_role(); // 6.3
  if (g2h & 0x04) { clear Unplug; wr8(E2014D,0x04); } // DevWakeup (drv:4738-4744)
  if (g2l & 0x02) { wr8(E2014C,0x02); suspended=1; }  // 6.2
  if (g2l & 0x04) { wr8(E2014C,0x04); suspended=0; }
  g0 = rd8(E20144);
  if (g0 & 0x20) { wr8(E20144,0x20); abort_ep0(); }   // Abort
  if (g0 & 0x01) handle_setup();                      // 4.x (acks Setup itself)
  if (g0 & 0x08) { wr8(E20120,0x01); wr8(E20144,0x08); } // End -> FIN (drv:4570-4576)
  if (g0 & 0x10) { wr8(E20144,0x10); }                // Err (drv:4578-4581)
  if (g0 & 0x06) handle_ep0_data_stage();             // In/Out (drv:4557-4568)
  rx = rd8(E20148); tx = rd8(E2014A) & 0x0F & ~rd8(E2013A);
  if (rx & (0x03 << 2*FIFO_OUT)) bulk_out_packet();   // 5.2
  if (g2l & 0x40) zlp_rx();                           // 5.3
  if (g2l & 0x20) { wr8(E20154,0); wr8(E2014C,0x20);} // ZlpTx (drv:4675-4681)
  if (tx & (1 << FIFO_IN) && have_in_data) bulk_in_packet(); // 5.1
  ... application work ...
}
```

---

## 4. EP0 (control endpoint, CX FIFO)

EP0 MPS is 64 (`bMaxPacketSize0 = 0x40` in usbdrvce's default device descriptor, drv:5222). usbdrvce
loads it from the descriptor at every bus reset (drv:4635-4640). Linux limits EP0 to 64 (lnx:1255).

### 4.1 Reading the 8-byte SETUP packet [HW]

Trigger: `rd8(0xE20144) & 0x01`.

1. `di` if any ISR could touch `0xE201C0`; usbdrvce does (drv:3703-3704).
2. `wr8(0xE201C0, 0x10)`: select the CX FIFO (drv:3705). EP0 data reads only pop while this is set
   (cemu:usb.c:1446-1451).
3. Read the data port **in usbdrvce's exact order** (drv:3700-3716). Each lane is read twice: the
   first read gives byte k, the second gives byte k+4.

   ```
   s[3]=rd8(E201D3); s[7]=rd8(E201D3);
   s[2]=rd8(E201D2); s[6]=rd8(E201D2);
   s[1]=rd8(E201D1); s[5]=rd8(E201D1);
   s[0]=rd8(E201D0); s[4]=rd8(E201D0);
   ```
4. `wr8(0xE201C0, 0)` (drv:3721), then `ei` if you disabled interrupts.
5. `wr8(0xE20120, rd8(0xE20120) | 0x08)`: CX FIFO **Clr** right after the fetch (drv:3722-3723).
   usbdrvce does this before decoding.
6. Decode and act (4.2-4.7). **Always finish with exactly one of the following** (drv:3886-3897):
   - **ACK (no data, or data already sent):** `wr8(0xE20120, 0x01)` (FIN), then `wr8(0xE20144, 0x01)`
     (ack Setup).
   - **STALL (unsupported):** `wr8(0xE20120, 0x04)` (CxFifoStall), then `wr8(0xE20144, 0x01)`
     (drv:3967-3969 → 3891-3895).
   - **Data stage pending (user-scheduled path):** write nothing to `0xE20120` and only ack Setup.
     usbdrvce writes `0x00` in this case (drv:3727, 3733 → 3891-3895).

**[UNSURE] The sources disagree on the EP0 data port.**
- CEmu returns `ep0_data[(k & 4) | lane]` for the k-th read: the first 4 reads come from bytes 0-3,
  the next 4 from bytes 4-7 (cemu:usb.c:1465). Under that model usbdrvce's order yields garbage, and
  the natural order D0, D1, D2, D3, D0, D1, D2, D3 would be correct.
- usbdrvce's order has been deliberate since at least 2019 (toolchain commit 02b500bd, which also read
  each lane twice). This strongly suggests that on hardware every byte read advances the 8-byte
  buffer by one dword (or that each lane has its own pointer).
- Linux does two 32-bit reads (lnx:568-608), which tells us nothing about byte reads.
- **Use usbdrvce's order on hardware.** CEmu cannot enumerate a custom device anyway (section 7,
  gotcha 15). If you need CEmu, autodetect from the first SETUP after reset, which is almost always
  `80 06 00 01 00 00 xx 00`, or make the order a compile-time option.

### 4.2 Sending IN data (descriptors, GET_STATUS, ...) [HW]

usbdrvce's default handler sends the whole response with **one DMA into the CX FIFO**, even when it
is longer than 64 bytes. Its config descriptors are 35 bytes; srldrvce's CDC config is 67 bytes, so
DMAs over 64 bytes have run on hardware. Sequence (drv:4041-4074):

1. `wr8(0xE201C0, 0x10)` (DMA target = CX).
2. DMA address: `wr8(0xE201CC, A0); wr8(0xE201CD, A1); wr8(0xE201CE, A2)`, where A is the buffer
   address. **The buffer must be in RAM** (5.4).
3. DmaCtrl direction: set b1 (Mem→FIFO) for device-to-host. usbdrvce does a read-modify-write on bit
   1 only (drv:4046-4053). Simplest: `wr8(0xE201C8, 0x02)` (or `0x12`, keeping ClrFifo as the bulk
   path does).
4. Length `L = min(wLength, descriptor_length)`: `wr8(0xE201C9, L&0xFF); wr8(0xE201CA, L>>8);
   wr8(0xE201CB, 0)` (drv:4054-4063).
5. Start: `wr8(0xE201C8, rd8(0xE201C8) | 0x01)` (drv:4064).
6. Poll until `rd8(0xE2014C) & 0x80` (DmaFin), then `wr8(0xE2014C, 0x80)` (drv:4065-4070).
   - **usbdrvce has no timeout and does not check DmaErr or bus reset here.**
   - Add both, as Linux does (lnx:304-335): on `0xE2014C` b0 (Reset) or `0xE2014D` b0 (DmaErr), write
     `0xE201C8 |= 0x08` (Abort), then CX Clr.
7. `wr8(0xE201C0, 0)` (drv:4071-4072).
8. FIN plus ack Setup: `wr8(0xE20120, 0x01); wr8(0xE20144, 0x01)` (drv:4073 → 3886-3895).

Alternative: usbdrvce's user-scheduled control path does **one packet per DMA**. The first packet
goes immediately. Each later packet goes when `CxIsr.In` (`0xE20144` b1) fires, ack `0x06`. FIN is
written after the last packet (drv:1871-1895, 4557-4568, 3380-3382, 3401-3404). **CEmu never raises
CX IN or CX OUT** (no `GISR0_CXIN` or `GISR0_CXOUT` is raised anywhere in cemu:usb.c), so prefer the
single-DMA method.

**[UNSURE] Zero-length packet on EP0.** If the response length is an exact multiple of 64 **and**
less than wLength, USB requires a terminating ZLP. usbdrvce's default handler does not generate one
(it only sets FIN), and its EP0 endpoint is created with manual termination (flags = 0, drv:2893-2895).
**Keep every descriptor response length off multiples of 64**, e.g. pad the configuration descriptor
with a harmless class/vendor descriptor.

### 4.3 Status stage and zero-length responses [HW]

For requests with no data stage (SET_ADDRESS, SET_CONFIGURATION, CLEAR/SET_FEATURE), write FIN plus
ack Setup immediately (drv:3884-3895). For IN data stages, FIN goes after the DMA (4.2). In both cases
the controller then handles the host's status packet. For a user-scheduled zero-length data stage,
usbdrvce just writes FIN (drv:1896-1898).

`CxIsr.End` (b3): usbdrvce responds by writing FIN and acking End (drv:4570-4576). Do the same.

### 4.4 STALL

Write CxFifo bit 2: `wr8(0xE20120, 0x04)`, then ack Setup (drv:3967-3969). usbdrvce stalls every
request it doesn't support. Examples:
- **GET_DESCRIPTOR(DEVICE_QUALIFIER = 6)** is stalled. This is correct for a full-speed-only device
  (the descriptor dispatch only knows types 1, 2 and 3, drv:3970-4040).
- GET/SET_INTERFACE (drv:3962-3969).
- SYNCH_FRAME.

### 4.5 SET_ADDRESS timing [HW]

usbdrvce writes the new address **immediately on receiving the SETUP**: `wr8(0xE20104, wValue & 0x7F)`.
It actually writes 3 bytes: address, 0, 0 (drv:3870-3885). Then it writes FIN and acks Setup. There is
**no delay** and no wait for the status stage. Linux does the same: `set_dev_addr` then `set_cxdone`
(lnx:640-648).
- The controller evidently defers applying the address until the status stage completes. That is
  inferred from both drivers, not documented.
- usbdrvce rejects `wValue > 127`, a nonzero wIndex or wLength, or a wrong bmRequestType with STALL
  (drv:3873-3883).
- Writing byte `0xE20104` also clears DevConf (b7). That is fine: SET_ADDRESS happens only before
  configuration.

### 4.6 GET_STATUS, CLEAR_FEATURE and SET_FEATURE (endpoint halt) [HW]

- **GET_STATUS** returns 2 bytes:
  - device: `deviceStatus`, which is 1 (self-powered) after reset (drv:4622-4623);
  - interface: 0;
  - endpoint: Stall bit 11 of InEp/OutEp (drv:3736-3783).
- **Endpoint register byte address** (drv:4093-4104): IN EPn → `0xE20161 + 4(n-1)`, OUT EPn →
  `0xE20181 + 4(n-1)`. This is byte 1 of the register: b3 = Stall, b4 = Reset.
- **SET_FEATURE(ENDPOINT_HALT):** byte1 = `(byte1 & ~0x10) | 0x08` (drv:3855-3858). Linux first waits
  for the IN FIFO to be empty (`0xE20121` bit (n-1)) before stalling an IN endpoint (lnx:455-474).
- **CLEAR_FEATURE(ENDPOINT_HALT):** usbdrvce writes byte1 = `(byte1 | 0x10) & ~0x08` (set Reset, clear
  Stall, drv:3809-3812) and **leaves Reset set**.
  - Linux sets Reset, then clears it, and comments "Controller wouldn't clear this bit"
    (lnx:185-206; used on clear-halt at lnx:490-518).
  - usbdrvce's own endpoint setup also writes Reset and then overwrites it with 0 (drv:3057-3061).
  - **Recommend set-then-clear:** byte1 |= `0x10`, then byte1 &= `~0x18`. **[UNSURE]** whether leaving
    Reset set (usbdrvce's CLEAR_FEATURE path) is harmful.

### 4.7 SET_CONFIGURATION and endpoint configuration [HW]

usbdrvce (drv:3916-3961, `_ParseInterfaceDescriptors` drv:3201-3258, `_CreateEndpoint` drv:2816-3070):

1. Validate: `bmRequestType = 0x00`, wIndex = 0, wLength = 0, and wValue is 0 or matches a
   `bConfigurationValue`. Otherwise STALL (drv:3917-3941).
2. If wValue ≠ 0:
   1. **Clear all FIFOs**: `0xE20108 |= 0x01`, then `&= ~0x01` (DevTest.TstClrFifo pulse, drv:3207-3209).
   2. For each endpoint of alternate setting 0 (drv:3014-3070). EP number n must be 1..4; FIFO f = n-1.
      For **EP1 IN** (f = 0) and **EP2 OUT** (f = 1):

      | Step | EP1 IN, FIFO0 | EP2 OUT, FIFO1 | Ref |
      |---|---|---|---|
      | FIFO map: keep EP bits, set direction | `wr8(E201A8, (rd8 & ~0x30) | 0x10)` → `0x11` | `wr8(E201A9, rd8 & ~0x30)` → `0x02` | drv:3034-3041 |
      | FIFO config: bulk (`2`) + enable (`0x20`), 1 block × 512 B | `wr8(E201AC, 0x22)` | `wr8(E201AD, 0x22)` | drv:3043-3047 |
      | EP register: toggle reset, then MPS | `wr8(E20161, 0x10); wr8(E20161, 0x00); wr8(E20160, 0x40)` | `wr8(E20185, 0x10); wr8(E20185, 0x00); wr8(E20184, 0x40)` | drv:3048-3063 |
      | DMA FIFO mask, kept in software | `0x01` | `0x02` | drv:3064-3070 |

      EP map (`0xE201A0`: EP1 → FIFO0, `0xE201A1`: EP2 → FIFO1) was set at init (2.1 step 18). Re-write
      it here if you want to be independent of init.
3. `wr8(0xE20104, rd8(0xE20104) | 0x80)` (**DevConf = 1**). For wValue = 0, clear b7 instead
   (drv:3952-3958: rla/rrca puts the carry into b7).
4. FIN plus ack Setup (drv:3959 → 3886-3895).

Limits of this scheme: four FIFOs, so EP numbers 1-4 only. One FIFO per EP number, so EP1 IN and EP1
OUT cannot both be used. That is why TI's own descriptors use EP1 IN (`0x81`) and EP2 OUT (`0x02`),
both bulk MPS 64 (drv:5223-5225).

Untested option: FIFO cfg bits 2-3 (blocks = 2 or 3) for double buffering. A multi-block FIFO very
likely consumes the following FIFO's memory, so map EP2 OUT to FIFO2 if you try it. **[UNSURE]**

---

## 5. Bulk IN and OUT through the device DMA engine

There is one shared DMA engine (`0xE201C0`/`C8`/`C9-CB`/`CC-CE`). Program it for one transfer at a
time and set `0xE201C0` back to 0 afterwards. usbdrvce does that "before restoring interrupts"
(drv:3404, 3407), and with `di` around the whole DMA (drv:3281-3287, 3405-3412).

Generic DMA recipe (usbdrvce `_ExecuteDma`, drv:3270-3458):

| Step | Write | Ref |
|---|---|---|
| select FIFO | `wr8(E201C0, 1 << f)` | drv:3283-3287 |
| length | `wr8(E201C9, L & 0xFF); wr8(E201CA, L >> 8); wr8(E201CB, (L >> 16) & 1)` | drv:3318 |
| address | `wr8(E201CC, a0); wr8(E201CD, a1); wr8(E201CE, a2)` | drv:3330-3331 |
| direction | `wr8(E201C8, dir)` with dir = `0x12` for IN (Mem→FIFO + ClrFifo) or `0x10` for OUT (FIFO→Mem + ClrFifo) | drv:3332-3333 |
| start | `wr8(E201C8, dir | 0x01)` | drv:3334-3335 |
| wait | `while (!(rd8(E2014C) & 0x80) && !(rd8(E2014D) & 0x01));` | drv:3336-3343 |
| on error | `wr8(E2014D, 0x01)`; the transfer has failed (usbdrvce sets halted\|bufErr) | drv:3344-3349 |
| finish | `wr8(E2014C, 0x80)` | drv:3350-3351 |
| deselect | `wr8(E201C0, 0)` | drv:3405-3407 |

usbdrvce always sets ClrFifo (bit 4): the transfer's `cerr` bits are always 3 (drv:2022) and get
rotated into DmaCtrl bit 4 (drv:3302-3306).

**DmaErr** means "the memory bus could not be accessed in a timely enough fashion". usbdrvce.h says
this "probably means that non-default cpu speed or lcd parameters are in effect" (drvh:176-181).

### 5.1 Bulk IN (EP1 IN, FIFO0) [HW]

- usbdrvce moves **at most one max-size packet per DMA**: `L = min(MPS, remaining)` (drv:3288-3297).
- It starts a DMA only when **FifoTxIsr bit f** (`0xE2014A` b0 for FIFO0) is set, meaning "FIFO empty,
  ready" (drv:4605-4612).
- It W1C-clears that bit after the DMA: `wr8(0xE2014A, 0x01)` (drv:4609-4610).
- The bit is level-like and comes back once the packet has gone out.
- When nothing is queued, usbdrvce masks it in FifoTxImr (drv:3383-3393) and unmasks it when a
  transfer is queued (drv:1997-2003). For polling, simply test the bit only when you have data.
- An equivalent readiness test is `rd8(0xE20121) & (1 << f)` (FIFO f empty). That test is used by
  Linux (lnx:461-466). **[LNX]**
- A DMA shorter than MPS is sent as a **short packet**, which ends the transfer on the host side
  (drv:3352-3370).

**Sending a ZLP** (transfer length is a multiple of MPS, or you want an explicit terminator):

1. When the FIFO is empty (FifoTxIsr bit set), set **InEpN.SendZlp**: `wr8(0xE20161 + 4(n-1),
   rd8(...) | 0x80)` (drv:3430-3446, `bUsbInEpSendZlp` = 15). usbdrvce does this instead of a 0-length
   DMA.
2. When it has gone out, DevIsr.ZlpTx (`0xE2014C` b5) fires: `wr8(0xE20154, 0); wr8(0xE2014C, 0x20)`
   (drv:4675-4681).

**[UNSURE]** whether SendZlp clears itself. Read it back before setting it again.

**Multi-packet DMA (Linux-style):**
- Linux bulk IN programs **one DMA of the whole request length** and lets the controller packetize it
  (lnx:345-349, 265-297). CEmu also accepts any length ≤ `0x5000` in one DMA (cemu:usb.c:1725-1790).
- This would be far faster for screen streaming, but **usbdrvce never does it on the CE**.
  **[UNSURE on hardware]** Try it with fallback to per-packet DMA.
- Field width: 17 bits, max 131071 (inc:3819). Linux masks to 16 bits (lnxh:201). CEmu raises DmaErr
  above **20480 bytes** (`usb.buffer[0x5000]`, cemu:usb.h:22, cemu:usb.c:1729-1732).
- A full VRAM frame (153600 B) therefore needs ≥ 2 DMAs on hardware, and ≥ 8 if you want it to work
  under CEmu.

### 5.2 Bulk OUT (EP2 OUT, FIFO1) [HW]

1. Trigger: FifoRxIsr **OUT** bit `2f` (`0xE20148` b2 for FIFO1). **SPK** bit `2f+1` (b3) is set too
   when the packet is short (cemu:usb.c:763-771). usbdrvce's SPK handler only acks it (drv:4599-4603);
   it detects short packets by comparing the count with MPS (drv:3352-3370).
2. **Byte count:** `n = rd8(0xE201B4) | ((rd8(0xE201B5) & 0x07) << 8)` (FIFO1 Csr bits 0-10,
   drv:4590-4591, 3299-3301). For FIFO f the register is `0xE201B0 + 4f`.
3. If `n > room` (room = min(MPS, buffer space left)), that is **babble**. usbdrvce still DMAs the `n`
   bytes, but to `vRamEnd = 0xD65800` (unmapped, so the data is discarded) and fails the transfer
   (drv:3301-3313, 3421-3429). Linux clamps the length to the room left (lnx:351-354).
4. DMA FIFO→memory with `L = n` (generic recipe, dir `0x10`).
5. W1C-clear: `wr8(0xE20148, 0x04 | 0x08)` (OUT + SPK of FIFO1). usbdrvce acks OUT after the DMA
   (drv:4594-4596) and SPK separately. CEmu clears RX status at DMA start anyway (cemu:usb.c:1763-1766).
6. A transfer is complete when `n < MPS` (short packet) or the buffer is full (drv:3352-3370;
   drvh:142-144).

**Bulk OUT ZLP:** see 5.3.

### 5.3 Zero-length OUT packets [HW]

A ZLP does **not** appear as FIFO OUT. It sets `usbRxZlp` bit (n-1) and DevIsr.ZlpRx (`0xE2014C` b6)
(cemu:usb.c:772-778).

1. Read `0xE20150`, then write 0 to it. This must be as close to atomic as possible: usbdrvce runs
   `ld c,(hl); ld (hl),b` from the LCD palette register area under `di` to minimize the window
   (drv:4703-4721).
2. Treat each set bit as "EPn transfer ends with a short (0-byte) packet" (drv:4686-4698, 3430-3434).
3. `wr8(0xE2014C, 0x40)` (drv:4699-4700).

### 5.4 Where DMA can read and write; alignment

- **DMA reaches RAM only.**
  - DmaAddr has 19 significant bits (`bmUsbDmaAddr = 0x7FFFF`, inc:3825).
  - CEmu masks the address with `0x7FFFF`, uses RAM (`0xD00000 + offset`, `SIZE_RAM 0x65800`), and
    treats offsets ≥ `0x65800` as unmapped: reads return junk, writes are dropped (cemu:mem.c:132-180,
    `mem.h:12`).
  - So `0xD00000-0xD657FF` (all user RAM plus **VRAM `0xD40000-0xD657FF`**) works. **Flash (archived
    data, ROM) cannot be DMA-read**, so copy descriptors to RAM first. Code and data of a normal asm or
    C program run from RAM at `0xD1A881+`, so their consts are fine.
  - usbdrvce deliberately DMAs babble data to `0xD65800` as a bit bucket (drv:3428), which shows that
    writes there are harmless.
- **Reading VRAM while the LCD scans it out** is fine at default LCD settings. Non-default LCD or CPU
  settings can starve the USB DMA and cause DmaErr (drvh:176-181). **[HW per usbdrvce docs]**
- **Alignment:** none is required. usbdrvce passes arbitrary user buffer pointers and descriptor
  pointers (drv:3326-3331, 4044-4045). CEmu copies byte-granular. Linux also has no alignment rule.
  **[HW, inferred from usage]**
- **Max length per DMA:** 17-bit field (131071). In practice: one packet ≤ MPS (usbdrvce FIFO path),
  ≤ 65535 for EP0 (wLength), and ≤ 20480 under CEmu.

---

## 6. Bus reset, suspend, disconnect

### 6.1 Bus reset: DevIsr b0 (`0xE2014C` & `0x01`) [HW]

usbdrvce `_HandleDevResetInt` (drv:4615-4643):

1. Tear down the software state of the old session (drv:4616).
2. `wr8(0xE20104, 0)`: address 0 and DevConf = 0 (drv:4618-4620). Clear the selected configuration;
   `deviceStatus = 1` (self-powered) (drv:4621-4623).
3. `wr8(0xE201C8, 0x18)`: DMA Abort | ClrFifo (drv:4624-4625).
4. `0xE20120 |= 0x08`: CX FIFO Clr (drv:4626-4627).
5. Speed = `rd8(0xE20100) & 0x40` (0 = full speed, drv:4629-4631).
6. EP0 MPS = `bMaxPacketSize0` (drv:4635-4640).
7. `wr8(0xE2014C, 0x01)` (drv:4641-4642).

Also recommended: mark EP1/EP2 unconfigured. The FIFO config is left as is until the next
SET_CONFIGURATION, which pulses ClrFifo. Abort any in-progress DMA wait loop when Reset appears, as
Linux does (lnx:304-335).

### 6.2 Suspend and resume [HW]

- Suspend: `0xE2014C` b1. usbdrvce only acks it (`wr8(0xE2014C, 0x02)`) and raises an event
  (drv:4645-4653).
- Resume: b2, ack `0x04` (drv:4655-4663).
- The PHY drops into suspend by itself after the idle time (`usbIdle` = 7 ms, drv:666-667).
  DevCtrl.GoSuspend (b3) is never used.
- Note: CEmu also raises **Resume** when a cable is plugged in as a B-device (cemu:usb.c:199-206).
  Don't treat a spurious Resume as an error.

### 6.3 Disconnect (VBUS lost) and role change [HW + EMU]

- usbdrvce enables and handles OTGISR **BPlugRemoved** (`0xE20085` b3) and **APlugRemoved** (b4). It
  acks both with `wr8(0xE20085, 0x18)` and tears the session down (drv:4788-4791, 4799-4810).
  **BSessEnd** (`0xE20084` b6) is deliberately not enabled (drv:688).
- CEmu raises only **BSessEnd** on a B-side unplug (cemu:usb.c:208-215) and clears OTGCSR
  A_VBUS_VLD / A_SESS_VLD / B_SESS_VLD while setting B_SESS_END.
- **Polled recommendation:** treat any of the following as "disconnected". Then reset your state, keep
  the endpoint config, and wait for the next bus reset.
  - `0xE20085 & 0x18`;
  - `0xE20084 & 0x40`;
  - `!(rd8(0xE20082) & 0x02)` (B_SESS_VLD low).
  W1C-clear the bits you saw.
- usbdrvce does not set Unplug on disconnect. A new attach produces a new bus reset (6.1).
- **RoleChg / IdChg** (`0xE20085` b0/b1, ack `0x03`): re-read OTGCSR bit 20. If it now reads host
  (someone plugged in a mini-A), stop device operation (drv:4766-4781).

---

## 7. Timing and gotchas

**Timing**

| Item | Value | Source |
|---|---|---|
| CPU clock and cycles | 48 MHz. usbdrvce's delay loops and `usb_MsToCycles` assume 48 000 cycles/ms (drv:2368, 2378, 5129-5160). Hardware wait states make instruction cycle counts approximate. For real ms delays use boot `Delay10ms` (`0x0003B4`, inc:2362) or a GPT. usbdrvce runs timer 2 up at the CPU clock (drv:604-612). | drv, inc |
| Host halt wait | ≤ 2 ms | drv:2368 |
| Host reset wait | ≤ 250 ms | drv:2378 |
| Schedule-disable sync | ≤ 20 ms | drv:2344 (also 2198, 2218) |
| Device soft reset → use | none in usbdrvce; 100-200 us in Linux | drv:671; lnx:1052-1054 |
| SET_ADDRESS | no delay, write immediately | drv:3884 |
| Soft-disconnect hold | ≥ 100 ms recommended | **[INF]** USB attach debounce |
| PHY idle → suspend | 7 ms | drv:666-667 |
| OTG filters | ID 4 ms, VBUS 472 us (long filters, CEmu reset value) | cemu:usb.c:1291, cemu:fotg210.h |

**Gotchas**

1. The base address is `0xE20000` (section 0).
2. Register-disable bit `0xE201CB` b7 combined with Unplug (section 0). Clear it before configuring.
3. All ISR registers are W1C. Write single bits and never read-modify-write them (3.2).
   `0xE20150` and `0xE20154` (RxZlp/TxZlp) are the exception: plain read/write, cleared by writing 0
   (drv:4676-4678, 4703-4721).
4. EP0 data port: select `0xE201C0 = 0x10` first. Read in usbdrvce's order; CEmu's model differs
   (4.1). Any stray byte read of `0xE201D0-D3` pops data.
5. One shared DMA engine. Always deselect (`0xE201C0 = 0`) after use (drv:3404-3407). Don't start a
   CX DMA while a FIFO DMA is in flight.
6. DMA only reaches RAM (19-bit offset into `0xD00000`). Descriptors must be in RAM (5.4).
7. DMA wait loops in usbdrvce have no timeout (drv:4067-4069) and no bus-reset check. Add both
   (Linux lnx:304-335: on Reset or DmaErr, Abort, then FIFO reset via FifoCsr b12 or CX Clr).
8. FIFO IN status is level-like. Only act on it when you have data (drv:3383-3393, 4476-4481).
9. EPn ↔ FIFO n-1 for both directions. Max EP number 4 (4.7).
10. The EP toggle Reset bit (b12) must be cleared by software after setting it (lnx:185-206;
    drv:3057-3061). usbdrvce's CLEAR_FEATURE path leaves it set (drv:3809-3812). **[UNSURE]**
11. Linux waits for the IN FIFO to empty before stalling an IN endpoint (lnx:455-474).
12. usbdrvce clears the CX FIFO right after reading SETUP (drv:3722-3723).
13. Reconnect: usbdrvce clears Unplug only in the DevWakeup handler (drv:4738-4744). CEmu never raises
    DevWakeup, so check B_SESS_VLD too (2.1 step 26).
14. ZLP after an EP0 IN response is not generated. Keep response lengths off multiples of 64 (4.2).
15. **CEmu cannot enumerate a custom device.** Its built-in "dusb" host sends only SET_ADDRESS(1) and
    SET_CONFIGURATION(1) and then speaks TI's DUSB protocol (`dusb.c:1125-1137, 845-850`). Its
    "physical" backend passes through real devices with the calc as host. Test enumeration on real
    hardware against a Linux host (`dmesg`, `lsusb -v`, `usbmon`).
16. The undocumented OS routine `0x021C68` and flags+`1Bh` bit 5 are part of usbdrvce's device-role
    entry and session. Mirror them or skip them knowingly (1.5, 2.1).
17. A GET_DESCRIPTOR(DEVICE_QUALIFIER) request must be STALLed by a full-speed-only device. usbdrvce
    does this (4.4).
18. eZ80 C compilers may emit multi-byte accesses for `uint16_t` / `uint24_t` / `uint32_t` volatile
    lvalues. Use `uint8_t` accesses for status and data-port registers.

---

## 8. Speed: full speed only

- usbdrvce sets **DevCtrl.ForceFullSpd** (bit 9, `0xE20101 = 0x02`) at init (drv:668-669).
- It reads DevCtrl b6 (HS status) at every reset to set the device speed (drv:4629-4631). With bit 9
  set this reads 0 (FS).
- CEmu never sets DevCtrl.HS in device mode; it only reads it for SOF timing (cemu:usb.c:1388).
  The `FS_FORCED` check at cemu:usb.c:1530 is on the host-port path. So the calc is always FS in
  CEmu device mode.
- TI's default device descriptor is bcdUSB 2.00, EP0 MPS 64, bulk MPS 64 (drv:5222-5225). That is the
  full-speed bulk maximum.
- **Design for 12 Mbit/s full speed:** EP0 MPS 64, bulk MPS 64, no device-qualifier descriptor (stall
  it). The theoretical bulk ceiling is about 1.2 MB/s. The real ceiling here is the per-packet DMA plus
  polling overhead, which favors multi-packet DMA if it works (5.1).

---

## 9. Disagreements and unknowns (summary)

| Topic | usbdrvce (HW) | CEmu | Linux | Recommendation |
|---|---|---|---|---|
| EP0 data port byte order | lane k read twice → bytes k, k+4 (drv:3706-3716) | k-th read → byte `(k&4)\|lane` (usb.c:1465) | two 32-bit reads | usbdrvce order on HW |
| Interrupt ack | W1C single-bit writes | W1C | read, clear bit, write (lnx:32-38) | W1C |
| Toggle Reset bit | set, then overwritten 0 at config; left set on CLEAR_FEATURE | stored as written | set then clear, "controller won't clear" | set then clear |
| Delay after DevCtrl soft reset | none | n/a | 100-200 us | ≥ 200 us |
| Max DMA length | ≤ MPS per FIFO DMA; CX up to wLength | ≤ 0x5000 else DmaErr | 16-bit, whole request | per-packet proven; try larger |
| Reconnect trigger | DevWakeup int clears Unplug | never raises Wakeup | n/a (clears Unplug at probe, lnx:1268) | clear when Wakeup **or** B_SESS_VLD |
| Unplug detection | OTG Bplug/Aplug removed | BSessEnd only | n/a | poll all + B_SESS_VLD |
| Register-window disable (`0x1CB` b7 & Unplug) | not explicitly handled | modeled (2023 fix) | n/a | write `0xE201CB = 0` early |
| `0x021C68`, `0x021B70`, flags+1Bh b5, `usb_DisableTimer` | used | n/a | n/a | semantics unknown; mirror usbdrvce |
| EP0 ZLP on multiple-of-64 response | not generated | n/a | n/a | avoid such lengths |
| Hand-back PHY state | untouched | TI-OS rewrites PHYTMSR (comment) | n/a | proven: untouched |
