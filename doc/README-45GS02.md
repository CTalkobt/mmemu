# mmemu-plugin-45gs02: MOS 45GS02 Implementation (MEGA65)

The 45GS02 is the CPU core of the MEGA65. It extends the 8-bit MOS 6502/65C02 architecture with 65CE02 and 4510-compatible enhancements, along with MEGA65-specific 32-bit "Quad" operations and a 28-bit MMU.

---

## 1. Architecture Overview

### 1.1 Registers

| Register | Name | Width | Description |
|----------|------|-------|-------------|
| **A**    | Accumulator | 8-bit | Primary 8-bit accumulator. |
| **X**    | Index X | 8-bit | Index register. |
| **Y**    | Index Y | 8-bit | Index register. |
| **Z**    | Index Z | 8-bit | Third index register (65CE02). |
| **B**    | Base Page | 8-bit | Replaces Zero Page; points to any 256-byte page. |
| **SP**   | Stack Pointer | 16-bit| Can address any page; wraps at 8-bit if E-flag=1. |
| **PC**   | Program Counter | 16-bit| Address of next instruction. |
| **P**    | Status Flags | 8-bit | NV-BDIZC flags. Bit 3 (E) is the 6502 Emulation flag. |
| **Q**    | Quad | 32-bit| Logical combination of A (LSB), X, Y, Z (MSB). |

### 1.2 Status Flags (P Register)

Same as 6502, but bit 5 is used as the **E (Emulation)** flag in some variations. In this implementation, when **E** is set, the stack pointer behaves like a standard 6502 (fixed at page 1).

---

## 2. Instruction Set Discussion

The 45GS02 supports the full 6502/65C02 sets, plus many extensions.

### 2.1 Standard 6502/65C02 Base
- All standard operations: `LDA`, `STA`, `ADC`, `SBC`, `AND`, `ORA`, `EOR`, `CMP`, `CPX`, `CPY`, `BIT`, `INC`, `DEC`, `ASL`, `LSR`, `ROL`, `ROR`, `JMP`, `JSR`, `RTS`, `RTI`, and all branches.
- **65C02 Extensions**: `STZ` (Store Zero), `BRA` (Branch Always), `PHX/PLX`, `PHY/PLY`, `TSB`/`TRB` (Test and Set/Reset Bits).

### 2.2 65CE02 / 4510 Enhancements
- **New Registers**: `LDZ`, `STZ`, `INZ`, `DEZ`, `PHZ`, `PLZ`, `TSY`, `TYS`.
- **Advanced Addressing**:
  - `(zp),Z`: Indirect indexed via Z.
  - `(d,S),Y`: Stack-relative indirect indexed via Y.
- **16-bit Word Ops**:
  - `INW`: Increment 16-bit word in memory (Zp).
  - `DEW`: Decrement 16-bit word in memory (Zp).
  - `ASW`: Arithmetic Shift Left 16-bit word (Absolute).
  - `ROW`: Rotate Left 16-bit word (Absolute).
- **Long Branches**: 16-bit relative offsets for all conditions (e.g., `LBRA`, `LBEQ`, `LBNE`).

### 2.3 MEGA65 Specific Extensions (45GS02)

#### 32-bit "Quad" Operations
Prefixing an instruction with `$42 $42` (actually double prefix) or using the `LDQ`/`STQ` mnemonics allows treating A, X, Y, and Z as a single 32-bit register.
- **Quad Load/Store**: `LDQ`, `STQ`.
- **Quad Arithmetic**: `ADCQ`, `SBCQ`, `CMPQ`.
- **Quad Logical**: `ANDQ`, `ORAQ`, `EORQ`, `BITQ`.
- **Quad Shifts**: `ASLQ`, `LSRQ`, `ROLQ`, `RORQ`, `ASRQ`.
- **Quad Misc**: `NEGQ`, `INQ`, `DEQ`.

#### Memory Management
- **MAP**: Enables the 28-bit MMU (4 x 16KB slots).
- **EOM**: Disables the MMU (returns to flat 16-bit 64KB mode).

#### Bit Manipulation (RMBx / SMBx / BBRx / BBSx)
- **RMB0-7**: Reset Memory Bit (Zp).
- **SMB0-7**: Set Memory Bit (Zp).
- **BBR0-7**: Branch if Bit Reset (Zp, Rel).
- **BBS0-7**: Branch if Bit Set (Zp, Rel).

#### Miscellaneous
- **NEG**: Two's complement negation of Accumulator.
- **RTN**: Return from subroutine and add immediate to SP (Cleanup stack).
- **BSR**: Branch to Subroutine (8-bit or 16-bit relative).
- **PHW**: Push 16-bit Word (Immediate or Absolute) onto stack.

---

## 3. Implementation Specifics

### 3.1 32-bit Indirect Addressing
Prefixing an indirect instruction (like `LDA ($zp),Z`) with `$EA` enables 32-bit pointers. This implementation treats them as **28-bit physical addresses**, bypassing the MAP MMU and addressing the full 256MB MEGA65 space directly.
Syntax in disassembly: `LDA [$zp],Z`.

### 3.2 MMU (MAP)
When `MAP` is active, the 64KB virtual space is split into eight 8KB blocks:
- Block 0: `$0000–$1FFF`  (Lower half)
- Block 1: `$2000–$3FFF`
- Block 2: `$4000–$5FFF`
- Block 3: `$6000–$7FFF`
- Block 4: `$8000–$9FFF`  (Upper half)
- Block 5: `$A000–$BFFF`
- Block 6: `$C000–$DFFF`
- Block 7: `$E000–$FFFF`

Register encoding: `A`/`X` control the lower half, `Y`/`Z` control the upper half.
- Lower: offset = `((X & 0x0F) << 8) | A`, enables = `X[7:4]` (blocks 0–3)
- Upper: offset = `((Z & 0x0F) << 8) | Y`, enables = `Z[7:4]` (blocks 4–7)

Physical Address = `vaddr + (offset << 8)` for enabled blocks.

### 3.3 Hypervisor Mode

The 45GS02 supports a privileged **hypervisor mode** used by the MEGA65's HYPPO firmware. When hypervisor mode is active:

- **$8000-$BFFF** is mapped to the 16KB HYPPO ROM (`HICKUP.M65`). Writes to this range are ignored.
- Stack is relocated to **$BE00** (SP = $BEFF), base page to **$BF00** (B = $BF).
- Interrupts are disabled (I flag set).
- MAP state is cleared (flat memory).

#### Entry

- **Reset**: If HYPPO ROM is loaded, reset enters hypervisor at **$8100** instead of reading the standard reset vector.
- **SYSCALL**: In user mode, writing to **$D640-$D67F** triggers a SYSCALL trap. The trap entry point is `$8000 + offset * 4` (e.g., write to $D640 = trap at $8000, $D641 = $8004, etc.).

On entry, the full CPU state (A, X, Y, Z, B, SP, P, PC) and MAP state are saved into virtualisation control registers.

#### Exit

Writing to **$D67F** (ENTEREXIT register) while in hypervisor mode restores the saved CPU and MAP state and returns to user mode.

#### Virtualisation Control Registers ($D640-$D67F)

In hypervisor mode, these registers provide read/write access to the saved user-mode state:

| Offset | Register | Description |
|--------|----------|-------------|
| $00 | REGA | Saved Accumulator |
| $01 | REGX | Saved X register |
| $03 | REGZ | Saved Z register |
| $04 | REGB | Saved Base Page |
| $05 | SPL | Saved Stack Pointer (low) |
| $06 | SPH | Saved Stack Pointer (high) |
| $07 | PFLAGS | Saved Status Flags |
| $08-$09 | PCL/PCH | Saved Program Counter |
| $0A-$0D | MAP0-3 | Saved MAP offsets (lo/hi for lower/upper halves) |
| $0E-$0F | MAPMB | Saved MAP megabyte enables |
| $10-$11 | PORT00/01 | Saved CPU port registers |
| $12 | VICMODE | Saved VIC mode |
| $3F | ENTEREXIT | Write exits hypervisor mode |

In user mode, reads return $FF.

---

## 4. Usage in mmsim

The plugin is registered as `"45gs02"`.

### Example Machine Configuration
```json
{
  "cpu": {
    "type": "45gs02",
    "dataBus": "system",
    "codeBus": "system"
  }
}
```

### Debugging
The 45GS02 implementation includes a "Hypervisor Trap" at `$D6CF`. Writing `$42` to this address will immediately halt the simulation, which is useful for finishing automated test runs.
