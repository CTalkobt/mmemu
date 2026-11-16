#include "test_harness.h"
#include "plugins/devices/tms7000/main/tms7000.h"
#include <initializer_list>
#include <vector>

// The TMS7040 with hand-assembled programs in a stand-in ROM (encodings and
// cycle counts from MAME's tms7000 tables)

namespace {

struct Rom {
    std::vector<uint8_t> b = std::vector<uint8_t>(0x1000, 0x00);
    uint16_t pc = 0xF000;
    void emit(std::initializer_list<int> v) { for (int x : v) b[(size_t)(pc++ - 0xF000)] = (uint8_t)x; }
    void vec(uint16_t at, uint16_t target) { b[at - 0xF000] = (uint8_t)(target >> 8); b[at - 0xF000 + 1] = (uint8_t)target; }
};

struct Mcu {
    Tms7000 cpu;
    uint8_t pins[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    std::vector<std::pair<int, uint8_t>> outs;
    explicit Mcu(Rom& r, uint16_t start = 0xF000) {
        r.vec(0xFFFE, start);
        cpu.setRom(r.b);
        cpu.portIn = [this](int p) { return pins[p]; };
        cpu.portOut = [this](int p, uint8_t v) { outs.push_back({p, v}); };
        cpu.reset();
    }
    void until(uint16_t pc, int limit = 10000) { for (int i = 0; i < limit && cpu.pc() != pc; ++i) cpu.step(); }
};

}  // namespace

// RESET: trap 0 through $FFFE; SP $FF pushes the old PC into A and B (as the chip)
TEST_CASE(tms7000_reset_vector) {
    Rom r;
    r.emit({0x00});
    Mcu m(r, 0xF123);
    ASSERT_EQ((int)m.cpu.pc(), 0xF123);
    ASSERT_EQ((int)m.cpu.sp(), 0x01);
    ASSERT_EQ((int)m.cpu.st(), 0x00);
    // Port C and D's DDRs cleared: their writes at reset reach nothing (0)
    bool sawC = false;
    for (auto& o : m.outs) if (o.first == 2) { sawC = true; ASSERT_EQ((int)o.second, 0); }
    ASSERT(sawC);
}

// The ALU: MOV, ADD, MPY into A:B, DAC (decimal), flags
TEST_CASE(tms7000_alu) {
    Rom r;
    r.emit({0x22, 0x12});           // MOV %>12,A (7)
    r.emit({0x28, 0x34});           // ADD %>34,A (7)
    r.emit({0x52, 0x10});           // MOV %>10,B
    r.emit({0x6C});                 // MPY B,A: A:B = A x B (5 + 39)
    r.emit({0x22, 0x19});           // MOV %>19,A
    r.emit({0x52, 0x28});           // MOV %>28,B
    r.emit({0x6E});                 // DAC B,A: $47
    r.emit({0x22, 0xFF});           // MOV %>FF,A
    r.emit({0xB3});                 // INC A: 0, C and Z
    const uint16_t end = r.pc;
    r.emit({0xE0, 0xFE});           // JMP $
    Mcu m(r);
    ASSERT_EQ(m.cpu.step(), 7);
    ASSERT_EQ(m.cpu.step(), 7);
    ASSERT_EQ((int)m.cpu.reg(0), 0x46);
    ASSERT_EQ((int)(m.cpu.st() & 0xE0), 0x00);
    m.cpu.step();
    ASSERT_EQ(m.cpu.step(), 44);
    ASSERT_EQ((int)m.cpu.reg(0), 0x04);     // $46 x $10 = $0460
    ASSERT_EQ((int)m.cpu.reg(1), 0x60);
    m.cpu.step(); m.cpu.step(); m.cpu.step();
    ASSERT_EQ((int)m.cpu.reg(0), 0x47);
    m.until(end);
    ASSERT_EQ((int)m.cpu.reg(0), 0x00);
    ASSERT_EQ((int)(m.cpu.st() & 0xA0), 0xA0);   // C, Z
}

// Ports: MOVP to B and D (masked by its DDR), from A; the registers, DJNZ, CALL/RETS
TEST_CASE(tms7000_ports_and_flow) {
    Rom r;
    r.emit({0xA2, 0x0F, 0x0B});     // MOVP %>0F,P11 (DDR D: low nibble out)
    r.emit({0xA2, 0x5A, 0x0A});     // MOVP %>5A,P10 (port D)
    r.emit({0xA2, 0x33, 0x06});     // MOVP %>33,P6 (port B)
    r.emit({0x80, 0x04});           // MOVP P4,A (port A)
    r.emit({0x72, 0x05, 0x10});     // MOV %>05,R16
    const uint16_t loop = r.pc;
    r.emit({0xD3, 0x11});           // INC R17
    r.emit({0xDA, 0x10, (int)(uint8_t)(loop - (r.pc + 3))});   // DJNZ R16,loop
    r.emit({0x8E, 0xF0, 0x80});     // CALL @>F080
    const uint16_t end = r.pc;
    r.emit({0xE0, 0xFE});           // JMP $
    r.pc = 0xF080;
    r.emit({0x52, 0x77});           // MOV %>77,B
    r.emit({0x0A});                 // RETS
    Mcu m(r);
    m.outs.clear();
    m.pins[0] = 0xA5;
    m.until(end);
    ASSERT_EQ((int)m.outs.size(), 2);
    ASSERT_EQ(m.outs[0].first, 3);
    ASSERT_EQ((int)m.outs[0].second, 0x0A);  // $5A & DDR $0F
    ASSERT_EQ(m.outs[1].first, 1);
    ASSERT_EQ((int)m.outs[1].second, 0x33);
    ASSERT_EQ((int)m.cpu.reg(0), 0xA5);
    ASSERT_EQ((int)m.cpu.reg(0x11), 5);
    ASSERT_EQ((int)m.cpu.reg(0x10), 0);
    ASSERT_EQ((int)m.cpu.reg(1), 0x77);
    ASSERT_EQ((int)m.cpu.sp(), 0x01);        // back where reset left it
    // Port B reads its latch; port D's input half the pins
    m.pins[3] = 0xC0;
    ASSERT_EQ((int)m.cpu.peek(0x0106), 0x33);
}

// INT3 wakes IDLE through $FFF8; the flag follows the (level) line; RETI
TEST_CASE(tms7000_int3_idle) {
    Rom r;
    r.emit({0xA2, 0x10, 0x00});     // MOVP %>10,P0: INT3 enabled
    r.emit({0x05});                 // EINT
    const uint16_t idle = r.pc;
    r.emit({0x01});                 // IDLE
    r.emit({0xE0, (int)(uint8_t)(idle - (r.pc + 2))});   // JMP idle
    r.pc = 0xF100;
    r.emit({0x80, 0x04});           // MOVP P4,A
    r.emit({0xD3, 0x20});           // INC R32
    r.emit({0x0B});                 // RETI
    r.vec(0xFFF8, 0xF100);
    Mcu m(r);
    for (int i = 0; i < 10; ++i) m.cpu.step();
    ASSERT(m.cpu.idling());
    ASSERT_EQ((int)m.cpu.pc(), idle);
    m.cpu.setInt3(true);
    ASSERT_EQ((int)(m.cpu.ioControl() & 0x20), 0x20);   // flagged
    ASSERT_EQ(m.cpu.step(), 17);                        // taken from IDLE
    ASSERT_EQ((int)m.cpu.pc(), 0xF100);
    ASSERT(!m.cpu.idling());
    m.cpu.setInt3(false);
    m.until(idle);
    ASSERT_EQ((int)m.cpu.reg(0x20), 1);
    ASSERT_EQ((int)(m.cpu.st() & 0x10), 0x10);          // I back on from the stack
    for (int i = 0; i < 10; ++i) m.cpu.step();
    ASSERT_EQ((int)m.cpu.reg(0x20), 1);                 // no more
}

// Timer 1: reload 3, prescale 0: INT2 every 4 x 8 cycles, through $FFFA
TEST_CASE(tms7000_timer1) {
    Rom r;
    r.emit({0xA2, 0x03, 0x02});     // MOVP %>03,P2 (reload)
    r.emit({0xA2, 0x80, 0x03});     // MOVP %>80,P3 (start, internal, prescale 0)
    r.emit({0xA2, 0x04, 0x00});     // MOVP %>04,P0: INT2 enabled
    r.emit({0x05});                 // EINT
    const uint16_t spin = r.pc;
    r.emit({0x00});                 // NOP (5)
    r.emit({0xE0, (int)(uint8_t)(spin - (r.pc + 2))});   // JMP spin (7)
    r.pc = 0xF200;
    r.emit({0xD3, 0x21});           // INC R33
    r.emit({0x0B});                 // RETI
    r.vec(0xFFFA, 0xF200);
    Mcu m(r);
    long cycles = 0;
    for (int i = 0; i < 4; ++i) cycles += m.cpu.step();
    const long start = cycles;
    while (m.cpu.reg(0x21) < 10 && cycles < 100000) cycles += m.cpu.step();
    ASSERT_EQ((int)m.cpu.reg(0x21), 10);
    const long per = (cycles - start) / 10;
    ASSERT(per >= 30 && per <= 34);
}
