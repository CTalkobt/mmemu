#pragma once

// Hand-assembled 6809 stand-ins for Color BASIC (the real ROMs are
// copyright and not in the repository): just enough to set the hardware up
// the way BASIC does and exercise it.

#include "plugins/machines/coco/main/machine_coco.h"
#include "plugins/machines/coco/main/coco_system.h"
#include "libcore/main/machine_desc.h"
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace cocotest {

/** A tiny assembler: bytes at an origin, relative branches by target. */
struct Asm {
    std::vector<uint8_t> b;
    uint16_t org, pc;
    explicit Asm(uint16_t origin, size_t size = 0x2000, uint8_t fill = 0x12) : b(size, fill), org(origin), pc(origin) {}
    void emit(std::initializer_list<int> v) { for (int x : v) b[(size_t)(pc++ - org)] = (uint8_t)x; }
    void branch(int op, uint16_t target) { emit({op, (target - (pc + 2)) & 0xFF}); }
    void ext(int op, uint16_t addr) { emit({op, addr >> 8, addr & 0xFF}); }
    void word(uint16_t at, uint16_t v) { b[(size_t)(at - org)] = (uint8_t)(v >> 8); b[(size_t)(at - org + 1)] = (uint8_t)v; }
    void text(const std::string& s) { for (char c : s) emit({(uint8_t)c}); emit({0}); }
};

constexpr uint16_t COUNTER = 0x0700;      // FS interrupts counted
constexpr uint16_t CART_MARK = 0x0701;    // the cartridge ran
constexpr uint16_t MESSAGE_AT = 0x0400;

/**
 * Color BASIC's stand-in at $A000: the PIAs set up as BASIC does (PIA0 port
 * A in, B out; PIA1 PA1-7, PB1 and PB3-7 out), the SAM's display at $0400 and
 * 64K mode, the screen cleared to green, "HELLO WORLD" at its top, then
 * FS interrupts counted at $0700 and the cartridge FIRQ enabled (its
 * handler jumps to $C000), waiting in a loop.
 */
inline std::vector<uint8_t> colorBasic() {
    Asm a(0xA000);
    a.emit({0x10, 0xCE, 0x7F, 0x00});           // LDS #$7F00
    // PIA0: DDRA $00, DDRB $FF; CA2/CB2 outputs low
    a.ext(0x7F, 0xFF01); a.ext(0x7F, 0xFF00);   // CLR CRA; CLR DDRA
    a.emit({0x86, 0x34}); a.ext(0xB7, 0xFF01);  // CRA = $34
    a.ext(0x7F, 0xFF03);
    a.emit({0x86, 0xFF}); a.ext(0xB7, 0xFF02);  // DDRB = $FF
    a.emit({0x86, 0x34}); a.ext(0xB7, 0xFF03);  // CRB = $34
    // PIA1: DDRA $FE, DDRB $F8
    a.ext(0x7F, 0xFF21);
    a.emit({0x86, 0xFE}); a.ext(0xB7, 0xFF20);
    a.emit({0x86, 0x34}); a.ext(0xB7, 0xFF21);
    a.ext(0x7F, 0xFF23);
    a.emit({0x86, 0xFA}); a.ext(0xB7, 0xFF22);  // PB0 (RS-232 in) and PB2 (RAM size) in
    a.emit({0x86, 0x34}); a.ext(0xB7, 0xFF23);
    a.ext(0x7F, 0xFF22);                        // VDG: alphanumerics
    a.emit({0x86, 0x02}); a.ext(0xB7, 0xFF20);  // RS-232 out marking
    // SAM: display at $0400 (F1), 64K (M1)
    a.ext(0xB7, 0xFFC9);
    a.ext(0xB7, 0xFFDD);
    // Clear the screen to spaces (green)
    a.emit({0x8E, 0x04, 0x00});                 // LDX #$0400
    a.emit({0x86, 0x60});                       // LDA #$60
    const uint16_t clr = a.pc;
    a.emit({0xA7, 0x80});                       // STA ,X+
    a.emit({0x8C, 0x06, 0x00});                 // CMPX #$0600
    a.branch(0x26, clr);                        // BNE
    // The message
    a.emit({0x8E, 0x04, 0x00});                 // LDX #$0400
    const uint16_t msgRef = a.pc;
    a.emit({0x10, 0x8E, 0x00, 0x00});           // LDY #message (patched)
    const uint16_t ploop = a.pc;
    a.emit({0xA6, 0xA0});                       // LDA ,Y+
    const uint16_t beq = a.pc;
    a.emit({0x27, 0x00});                       // BEQ done (patched)
    a.emit({0xA7, 0x80});                       // STA ,X+
    a.branch(0x20, ploop);                      // BRA
    const uint16_t done = a.pc;
    a.b[(size_t)(beq + 1 - a.org)] = (uint8_t)(done - (beq + 2));
    a.ext(0x7F, COUNTER);                       // CLR counter
    a.ext(0x7F, CART_MARK);
    a.emit({0x86, 0x35}); a.ext(0xB7, 0xFF03);  // PIA0 CRB: FS (CB1) falling edge, IRQ on
    a.emit({0x86, 0x37}); a.ext(0xB7, 0xFF23);  // PIA1 CRB: CART (CB1) rising edge, FIRQ on
    a.emit({0x1C, 0xAF});                       // ANDCC #$AF: IRQ and FIRQ on
    const uint16_t spin = a.pc;
    a.branch(0x20, spin);                       // BRA *
    // IRQ: clear PIA0's flag, count
    const uint16_t irq = a.pc;
    a.ext(0xB6, 0xFF02);                        // LDA $FF02
    a.ext(0x7C, COUNTER);                       // INC counter
    a.emit({0x3B});                             // RTI
    // FIRQ: the cartridge
    const uint16_t firq = a.pc;
    a.ext(0xB6, 0xFF22);                        // LDA $FF22 (clears the flag)
    a.ext(0x7E, 0xC000);                        // JMP $C000
    const uint16_t msg = a.pc;
    a.text("HELLO WORLD");
    a.word((uint16_t)(msgRef + 2), msg);
    a.word(0xBFF6, firq);
    a.word(0xBFF8, irq);
    a.word(0xBFFC, irq);
    a.word(0xBFFE, 0xA000);
    return a.b;
}

/** A ROM Pak's stand-in: marks $0701 with `mark` and waits. */
inline std::vector<uint8_t> romPak(uint8_t mark, size_t size = 0x2000) {
    Asm a(0xC000, size, 0x00);
    a.emit({0x86, mark});                       // LDA #mark
    a.ext(0xB7, CART_MARK);                     // STA $0701
    const uint16_t spin = a.pc;
    a.branch(0x20, spin);
    return a.b;
}

inline MachineDescriptor* make(coco::Model model = coco::Model::CoCo2, int ramKb = 64) {
    coco::Roms roms;
    roms.colorBasic = colorBasic();
    std::string err;
    return coco::create(model, &err, ramKb, &roms);
}

inline uint64_t run(MachineDescriptor* m, uint64_t cycles) {
    uint64_t t = 0;
    while (t < cycles) t += (uint64_t)m->schedulerStep(*m);
    return t;
}

}  // namespace cocotest
