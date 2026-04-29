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

### 4.2 Math Accelerator
Hardware registers at `$D760–$D77F` provide fast 32-bit integer multiplication and division.

See [README-MEGA65-MATH.md](README-MEGA65-MATH.md) for detailed register maps and usage.

---

## 5. Development Status in mmsim

The MEGA65 implementation is currently focused on the **45GS02 CPU core** and the **rawMega65** test machine. Full chipset emulation (VIC-IV, DMA) is planned for Phase 20-21.
