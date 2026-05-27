# MEGA65 Machine

The MEGA65 is a modern, high-end 8-bit computer that is highly compatible with the Commodore 64 and Commodore 65. It features the powerful 45GS02 CPU, enhanced video (VIC-IV), and a wide range of advanced peripherals.

---

## 1. Specifications

- **CPU**: 45GS02 (running at 40 MHz in Fast mode).
- **Video**: VIC-IV (compatible with VIC-II and VIC-III, supports Full Colour Mode and 80-column text).
- **Audio**: Dual SID (6581/8580 compatible).
- **RAM**: 384 KB Chip RAM, expandable to 128 MB Attic RAM.
- **I/O**:
  - Dual CIA 6526 for legacy C64 compatibility.
  - F018B DMA Controller.
  - Math Acceleration Registers (Hardware Multiply/Divide).
- **Bus**: 28-bit physical address space (256 MB).

---

## 2. Memory Map (MEGA65 Mode)

| Range | Description |
|-------|-------------|
| `$0000 0000 – $000F FFFF` | 1 MB Chip RAM |
| `$0002 0000 – $000D FFFF` | ROM area (MEGA65.ROM) |
| `$0000 D000 – $0000 DFFF` | I/O Area (VIC-IV, SIDs, CIAs, DMA) |
| `$0FF8 0000 – $0FF8 7FFF` | 32 KB Colour RAM |

---

## 3. CPU Core (45GS02)

The MEGA65 uses the 45GS02 CPU, which features:
- **MAP**: Dynamic 28-bit memory mapping.
- **32-bit Ops**: Quad registers and arithmetic.
- **Base Page**: Relocatable zero-page.

See [README-45GS02.md](README-45GS02.md) for details.

---

## 4. Peripherals

### 4.1 VIC-IV Video
The VIC-IV supports traditional C64 modes plus advanced features:
- **H640**: 80-column text mode.
- **FCM**: Full Colour Mode (8x8 glyphs with unique colours per pixel).
- **Palette**: 256 entries from a 4096-colour space.

### 4.2 F018B DMA Controller
The F018B handles high-speed memory transfers within the 28-bit address space:
- **Copy**, **Fill**, **Swap**, and **Mix** operations.
- Fractional step sizes for non-contiguous transfers.
- Overlap-safe copying (automatic backward copy when source and destination overlap).
- Enhanced DMA mode with extended option lists.

### 4.3 Math Accelerator
Hardware registers at `$D760–$D77F` provide fast 32-bit integer multiplication and division.

See [README-MEGA65-MATH.md](README-MEGA65-MATH.md) for detailed register maps and usage.

### 4.4 Hypervisor (HYPPO)
The MEGA65 includes a privileged hypervisor mode implemented in the 45GS02 CPU:
- **HYPPO ROM** (`HICKUP.M65`, 16KB) is loaded at `$8000-$BFFF` in hypervisor mode.
- On reset, the CPU enters hypervisor at `$8100` (rather than reading the standard reset vector).
- **SYSCALL traps** ($D640-$D67F): user-mode writes trigger hypervisor entry at computed trap addresses.
- **Virtualisation control registers** ($D640-$D67F): in hypervisor mode, these provide read/write access to saved CPU state.
- Writing `$D67F` exits hypervisor mode and restores user-mode state.

The machine factory searches for `HICKUP.M65` in `roms/mega65/`, the current directory, and `~/.local/share/xemu-lgb/mega65/`. If not found, the machine falls back to the standard C64 reset vector.

See [README-45GS02.md](README-45GS02.md) for detailed hypervisor register maps.

---

## 5. Development Status in mmsim

The MEGA65 implementation includes the **45GS02 CPU** with hypervisor support, **VIC-IV** video (VIC-II/III/IV modes, FCM, NCM), **F018B DMA**, **MAP MMU**, **Math Accelerator**, dual SID, dual CIA, and **HYPPO hypervisor ROM** loading. Integration testing and further peripheral work is ongoing.
