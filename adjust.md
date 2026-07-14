# MEGA65 Hardware VHDL vs. mmemu Emulation Discrepancies

This document details the functional and architectural discrepancies between the hardware VHDL implementation in [mega65-core](file:///home/duck/m65/mega65-core/src/vhdl) and the C++ software emulation in [mmemu](file:///home/duck/m65/inpg/mmsim/src).

---

## 1. Hardware Math Accelerator & Pipeline ($D780–$D7DF)

### VHDL Implementation (mega65-core)
The VHDL code in [gs4510.vhdl](file:///home/duck/m65/mega65-core/src/vhdl/gs4510.vhdl#L2766-L2975) defines registers and mapping for a 16-channel Math Function Unit (MFU) pipeline:
* **$D780–$D7BF**: 16 x 32-bit input registers (`MATHIN0` to `MATHINF`).
* **$D7C0–$D7CF**: 16 x config input A/B selectors (`UNIT0INA` to `UNITFINB`).
* **$D7D0–$D7DF**: 16 x config/output registers (`UNIT0OUT` to `UNITFOUT`), supporting barrel-shifters, 32-bit adders, multipliers, and output latching.

However, **this MFU pipeline is not actually functional or implemented in the final hardware**:
1. The hardware pipeline process in [gs4510.vhdl](file:///home/duck/m65/mega65-core/src/vhdl/gs4510.vhdl#L1675) is conditional on `math_unit_enable` being `true`. The generic parameter `math_unit_enable` defaults to `false` (see [gs4510.vhdl:L40](file:///home/duck/m65/mega65-core/src/vhdl/gs4510.vhdl#L40)) and is never enabled in the top-level designs like [machine.vhdl](file:///home/duck/m65/mega65-core/src/vhdl/machine.vhdl#L1116) or [cpu_only.vhdl](file:///home/duck/m65/mega65-core/src/vhdl/cpu_only.vhdl#L269). Thus, the active math logic is disabled/optimized out during synthesis.
2. The registers `$D780–$D7DF` function purely as standard, non-functional read/write scratch RAM in the VHDL (as the write/read decoding exists, but no math computations occur).
3. Furthermore, even if `math_unit_enable` were set to `true`, the 32-bit divider module in [divider32.vhdl](file:///home/duck/m65/mega65-core/src/vhdl/divider32.vhdl) is a copy-paste duplicate of `multiply32.vhdl` and performs multiplication (`a * b`) rather than division.

### mmemu Implementation
In [mega65_math.cpp](file:///home/duck/m65/inpg/mmsim/src/plugins/devices/mega65_math/main/mega65_math.cpp#L74-L109), the emulator implements the basic math registers (`$D760–$D77F` for multiplier/divider) and treats the MFU pipeline registers `$D780–$D7DF` as dummy read/write RAM. 

Because the hardware's MFU pipeline is disabled/non-functional, there is **no functional discrepancy** between the emulator and real hardware regarding these registers; both act as simple read/write memory blocks without processing any pipeline calculations.

---

## 2. MMU Address Mapping (MAP) Carry/Overflow

### VHDL Implementation
In [gs4510.vhdl](file:///home/duck/m65/mega65-core/src/vhdl/gs4510.vhdl#L9210), physical addresses are computed using a 12-bit addition slice:
```vhdl
temp_address(19 downto 8) := reg_offset_low + to_integer(short_address(15 downto 8));
```
Any carry overflow beyond bit 19 wraps modulo 4096 (12-bit wrapping) and does **not** propagate into the separate 8-bit megabyte segment slice:
```vhdl
temp_address(27 downto 20) := reg_mb_low;
```

### mmemu Implementation
In [map_mmu.cpp](file:///home/duck/m65/inpg/mmsim/src/plugins/devices/map_mmu/main/map_mmu.cpp#L21-L33), address translation is computed as a flat 32-bit addition:
```cpp
return (megabyte + (offset << 8) + (vaddr & 0x1FFF)) & m_physBus->config().addrMask;
```
If the offset sum overflows bit 19, the carry propagates directly into the megabyte base (bits 20–27). This causes a translation difference under offset overflow conditions.

---

## 3. MAP Instruction Interrupt Inhibit & EOM ($EA)

### VHDL Implementation
In [gs4510.vhdl](file:///home/duck/m65/mega65-core/src/vhdl/gs4510.vhdl#L3834), executing the `MAP` instruction ($5C) asserts `map_interrupt_inhibit <= '1'`, which defers all interrupts. Interrupts are only re-enabled when the `EOM` (End of Map, opcode `$EA`) instruction is executed:
```vhdl
when x"EA" => map_interrupt_inhibit <= '0'; -- EOM
```

### mmemu Implementation
In [cpu45gs02.cpp](file:///home/duck/m65/inpg/mmsim/src/plugins/45gs02/main/cpu45gs02.cpp#L1274), the emulator CPU does not implement the `map_interrupt_inhibit` flag. Opcode `$EA` is treated as a naked `NOP` ([cpu45gs02.cpp:L460](file:///home/duck/m65/inpg/mmsim/src/plugins/45gs02/main/cpu45gs02.cpp#L460)) and does not clear any interrupt inhibit state. As a result, interrupts can incorrectly fire mid-mapping setup in the emulator.

---

## 4. Hypervisor Exit State Reconstruction (MAP Corruption)

### VHDL Implementation
On hardware, MAP offsets are backed by single physical registers (`reg_offset_low`/`reg_offset_high`). When exiting hypervisor mode, these physical offset values are restored directly.

### mmemu Implementation
The emulator's [MapState](file:///home/duck/m65/inpg/mmsim/src/include/imap_controller.h#L7) splits offsets into an array of 8 block offsets (`offsets[8]`). However, on exiting hypervisor mode in [cpu45gs02.cpp](file:///home/duck/m65/inpg/mmsim/src/plugins/45gs02/main/cpu45gs02.cpp#L106-L116), the emulator only restores `offsets[0]` and `offsets[4]`:
```cpp
ms.offsets[0] = m_hyperState.mapLo0 | ((uint16_t)m_hyperState.mapLo1 << 8);
ms.offsets[4] = m_hyperState.mapHi0 | ((uint16_t)m_hyperState.mapHi1 << 8);
```
The remaining blocks (`1, 2, 3, 5, 6, 7`) are left as `0` (corrupted). Any user-mode program using these non-restored blocks will experience corrupted address mapping after returning from a hypervisor trap.

---

## 5. Virtualization Registers ($D640–$D67F)

### Y Register ($D642)
* **VHDL**: [gs4510.vhdl:L3076](file:///home/duck/m65/mega65-core/src/vhdl/gs4510.vhdl#L3076) exposes the saved Y register at offset `0x02` (`$D642`).
* **Emulator**: [hypervisor_regs.cpp:L40](file:///home/duck/m65/inpg/mmsim/src/plugins/devices/mega65_hypervisor/main/hypervisor_regs.cpp#L40) leaves `$D642` unwired (reads return `0`, writes are ignored), making the saved Y register inaccessible/non-writable to the hypervisor.

### MAP Register Layout Mismatch ($D64A–$D64F)
* **VHDL**: Maps enables and offsets in a packed layout. `$D64A` returns enables and the high nibble of the lower offset; `$D64B` returns the low byte. `$D64E` and `$D64F` return the megabyte selection values (`reg_mb_low`/`reg_mb_high`).
* **Emulator**: [hypervisor_regs.cpp:L48-L53](file:///home/duck/m65/inpg/mmsim/src/plugins/devices/mega65_hypervisor/main/hypervisor_regs.cpp#L48-L53) maps `$D64A–$D64D` as low/high bytes of block 0/4 offsets, and `$D64E–$D64F` as enable masks. The megabyte selection registers are completely unmapped.

### DMAgic & SD State ($D653–$D659)
* **VHDL**: [gs4510.vhdl:L3097-L3105](file:///home/duck/m65/mega65-core/src/vhdl/gs4510.vhdl#L3097-L3105) exposes saved DMA list addresses, bank megabytes, and SD card virtualization flags.
* **Emulator**: Falls through to return `0` on read and ignores writes, completely leaving these states unvirtualized.

---

## 6. VIC-IV Bitplane Bank Select ($D07C)

### VHDL Implementation
In [viciv.vhdl](file:///home/duck/m65/mega65-core/src/vhdl/viciv.vhdl#L5035), bits 0–2 of `$D07C` choose the 128KB bank from which bitplanes are fetched (`sprite_pointer_address(19 downto 17)`).

### mmemu Implementation
In [vic4.cpp](file:///home/duck/m65/inpg/mmsim/src/plugins/devices/vic4/main/vic4.cpp#L613-L618), the base address calculation during bitplane rendering ignores the value of `$D07C` entirely and assumes the physical address starts from bank 0 (limiting fetches to the first 128KB).

---

## 7. 2KB Color RAM View ($D030.0 / CRAM2K)

### VHDL Implementation
Bit 0 of `$D030` (`CRAM2K`) maps the second KB of color RAM into `$DC00-$DFFF` instead of leaving it mapped to C64 I/O chips.

### mmemu Implementation
In [mega65_io_stub.cpp](file:///home/duck/m65/inpg/mmsim/src/plugins/devices/mega65_io/main/mega65_io_stub.cpp#L57), the CRAM 2K logic checks a callback query:
```cpp
bool cram2k = m_cram2kQuery ? m_cram2kQuery() : false;
```
However, the machine factory in [machine_mega65.cpp](file:///home/duck/m65/inpg/mmsim/src/plugins/machines/mega65/main/machine_mega65.cpp) never calls `setCram2kQuery()`, leaving `m_cram2kQuery` uninitialized (`nullptr`). The 2KB Color RAM view is thus permanently disabled.

---

## 8. DMAgic Spiral, Floppy & SID Modes

### VHDL Implementation
The VHDL DMAgic controller inside [gs4510.vhdl](file:///home/duck/m65/mega65-core/src/vhdl/gs4510.vhdl#L6080) supports:
* **Spiral Mode**: Option `x"53"` draws the "Shallan Spiral" by updating destination address steps by `+1`, `+40`, `-1`, `-40` depending on the spiral phase.
* **Floppy Mode**: Options `x"0D"–x"0F"` transfer raw flux data to/from the floppy drive.
* **SID Mode**: Option `x"10"` enables SID mode transfers.

### mmemu Implementation
In [f018b_dma.cpp](file:///home/duck/m65/inpg/mmsim/src/plugins/devices/f018b_dma/main/f018b_dma.cpp), spiral mode, floppy mode, and SID transfer mode options are completely unhandled and unimplemented.

---

## 9. Fake RTC/NVRAM Registry Space Intrusion ($D710–$D77F)

### VHDL Implementation
Real hardware maps:
* **$D710**: CPU speed and slow interrupt control (`CPU:SLIEN`).
* **$D711–$D73F**: Audio DMA control registers.
* The RTC and NVRAM are accessed over the I2C bus via board-specific I2C controller registers, and are not direct memory-mapped.

### mmemu Implementation
In [mega65_rtc.h](file:///home/duck/m65/inpg/mmsim/src/plugins/devices/mega65_rtc/main/mega65_rtc.h#L33), the emulator maps the RTC to `$D710–$D71F` and a battery-backed NVRAM to `$D740–$D77F`. This maps over and collides with the real hardware's mapping of CPU interrupt controls and Audio DMA.

---

## 10. System Cycle Counters ($D7F2–$D7F9)

### VHDL Implementation
Registers `$D7F2–$D7F5` and `$D7F6–$D7F9` map cycle counters that measure the total number of PHI cycles and proceed cycles per video frame, respectively ([gs4510.vhdl:L3029-L3036](file:///home/duck/m65/mega65-core/src/vhdl/gs4510.vhdl#L3029-L3036)).

### mmemu Implementation
The emulator's math register device does not handle registers `$D7F2–$D7F9`. Accessing these registers in the emulator yields unhandled/default register shadow bytes rather than active cycle counts.
