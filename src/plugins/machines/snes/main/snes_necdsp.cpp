// The NEC uPD7725 (DSP-1..4) after Mesen2's NecDsp.cpp, GPL-3.0, Sour and
// the Mesen contributors.

#include "snes_necdsp.h"

namespace snes {

void NecDsp::setModel(bool upd96050, double hz) {
    m_96050 = upd96050;
    m_hz = hz;
    m_progMask = upd96050 ? 0x3FFF : 0x7FF;
    m_dataMask = upd96050 ? 0x7FF : 0x3FF;
    m_ramMask = upd96050 ? 0x7FF : 0xFF;
    m_stackMask = upd96050 ? 7 : 3;
    m_ram.assign(m_ramMask + 1u, 0);
    m_prog.clear();
    m_data.clear();
}

bool NecDsp::load(const std::vector<uint8_t>& fw, std::string& err) {
    const uint32_t prog = programSize(), data = dataSize();
    if (fw.size() != prog + data) {
        err = "DSP firmware must be " + std::to_string(prog + data) + " bytes (got " + std::to_string(fw.size()) + ")";
        return false;
    }
    m_prog.resize(prog / 3);
    for (size_t i = 0; i < m_prog.size(); ++i)
        m_prog[i] = (uint32_t)fw[i * 3] | (uint32_t)fw[i * 3 + 1] << 8 | (uint32_t)fw[i * 3 + 2] << 16;
    m_data.resize(data / 2);
    for (size_t i = 0; i < m_data.size(); ++i)
        m_data[i] = (uint16_t)(fw[prog + i * 2] | fw[prog + i * 2 + 1] << 8);
    reset();
    return true;
}

void NecDsp::reset() {
    m_s = State{};
    m_cycles = 0;
    m_rqmWait = false;
}

void NecDsp::runTo(uint64_t master, uint64_t masterHz) {
    const uint64_t target = (uint64_t)((double)master * (m_hz / (double)masterHz));
    if (m_prog.empty()) { m_cycles = target; return; }
    if (m_rqmWait) { m_cycles = target; return; }
    while (m_cycles < target && !m_rqmWait) {
        exec(m_prog[m_s.pc++ & m_progMask]);
        ++m_cycles;
    }
    if (m_rqmWait) m_cycles = target;
}

void NecDsp::step(int n) {
    while (n-- > 0 && !m_prog.empty()) { exec(m_prog[m_s.pc++ & m_progMask]); ++m_cycles; }
}

void NecDsp::exec(uint32_t op) {
    switch (op & 0xC00000) {
    case 0x000000:                                  // OP
    case 0x400000: {                                // RT: OP, then return
        const uint8_t fn = (op >> 16) & 0x0F;
        const uint16_t src = source((op >> 4) & 0x0F);
        if (fn) alu(op, fn, src);
        const uint8_t dest = op & 0x0F;
        load(dest, src);
        if (dest != 0x04) updateDp(op);
        if (((op >> 8) & 1) && dest != 0x05) --m_s.rp;
        if (op & 0x400000) { m_s.sp = (m_s.sp - 1) & m_stackMask; m_s.pc = m_stack[m_s.sp]; }
        break;
    }
    case 0x800000: jump(op); break;
    default: load(op & 0x0F, (uint16_t)(op >> 6)); break;   // LD immediate
    }
    // The multiplier works every cycle
    const int32_t r = (int16_t)m_s.k * (int16_t)m_s.l;
    m_s.m = (uint16_t)(r >> 15);
    m_s.n = (uint16_t)(r << 1);
}

void NecDsp::alu(uint32_t op, uint8_t fn, uint16_t src) {
    const bool useB = (op >> 15) & 1;
    Flags f = useB ? m_s.fb : m_s.fa;
    const uint16_t acc = useB ? m_s.b : m_s.a;
    const uint16_t otherCarry = (useB ? m_s.fa.c : m_s.fb.c) ? 1 : 0;
    uint16_t p = 0;
    switch ((op >> 20) & 3) {
    case 0: p = ram(m_s.dp); break;
    case 1: p = src; break;
    case 2: p = m_s.m; break;
    default: p = m_s.n; break;
    }
    uint16_t r = 0;
    switch (fn) {
    case 0x1: r = acc | p; break;
    case 0x2: r = acc & p; break;
    case 0x3: r = acc ^ p; break;
    case 0x4: r = (uint16_t)(acc - p); break;
    case 0x5: r = (uint16_t)(acc + p); break;
    case 0x6: r = (uint16_t)(acc - p - otherCarry); break;
    case 0x7: r = (uint16_t)(acc + p + otherCarry); break;
    case 0x8: r = (uint16_t)(acc - 1); p = 1; break;
    case 0x9: r = (uint16_t)(acc + 1); p = 1; break;
    case 0xA: r = (uint16_t)~acc; break;
    case 0xB: r = (uint16_t)((acc >> 1) | (acc & 0x8000)); break;
    case 0xC: r = (uint16_t)((acc << 1) | otherCarry); break;
    case 0xD: r = (uint16_t)((acc << 2) | 3); break;
    case 0xE: r = (uint16_t)((acc << 4) | 0xF); break;
    case 0xF: r = (uint16_t)((acc << 8) | (acc >> 8)); break;
    default: break;
    }
    f.z = r == 0;
    f.s0 = (r & 0x8000) != 0;
    if (!f.ov1) f.s1 = f.s0;
    switch (fn) {
    case 0x4: case 0x5: case 0x6: case 0x7: case 0x8: case 0x9: {
        const uint16_t ov = (uint16_t)((acc ^ r) & (p ^ ((fn & 1) ? r : acc)));
        f.ov0 = (ov & 0x8000) != 0;
        if (f.ov0 && f.ov1) f.ov1 = f.s0 == f.s1;
        else f.ov1 = f.ov1 || f.ov0;
        f.c = (((acc ^ p ^ r ^ ov) & 0x8000) != 0);
        break;
    }
    case 0xB: f.c = acc & 1; f.ov0 = f.ov1 = false; break;
    case 0xC: f.c = (acc >> 15) & 1; f.ov0 = f.ov1 = false; break;
    default: f.c = false; f.ov0 = f.ov1 = false; break;
    }
    if (useB) { m_s.b = r; m_s.fb = f; } else { m_s.a = r; m_s.fa = f; }
}

void NecDsp::updateDp(uint32_t op) {
    uint16_t dp = m_s.dp;
    switch ((op >> 13) & 3) {
    case 1: dp = (uint16_t)((dp & 0xF0) | ((dp + 1) & 0x0F)); break;
    case 2: dp = (uint16_t)((dp & 0xF0) | ((dp - 1) & 0x0F)); break;
    case 3: dp &= 0xF0; break;
    default: break;
    }
    m_s.dp = (uint16_t)(dp ^ (((op >> 9) & 0x0F) << 4));
}

void NecDsp::jump(uint32_t op) {
    const uint16_t target = (uint16_t)((m_s.pc & 0x2000) | ((op & 3) << 11) | ((op >> 2) & 0x7FF));
    const uint16_t type = (op >> 13) & 0x1FF;
    bool cond = false;
    const State& s = m_s;
    switch (type) {
    case 0x00: m_s.pc = s.so; break;
    case 0x80: cond = !s.fa.c; break;   case 0x82: cond = s.fa.c; break;
    case 0x84: cond = !s.fb.c; break;   case 0x86: cond = s.fb.c; break;
    case 0x88: cond = !s.fa.z; break;   case 0x8A: cond = s.fa.z; break;
    case 0x8C: cond = !s.fb.z; break;   case 0x8E: cond = s.fb.z; break;
    case 0x90: cond = !s.fa.ov0; break; case 0x92: cond = s.fa.ov0; break;
    case 0x94: cond = !s.fb.ov0; break; case 0x96: cond = s.fb.ov0; break;
    case 0x98: cond = !s.fa.ov1; break; case 0x9A: cond = s.fa.ov1; break;
    case 0x9C: cond = !s.fb.ov1; break; case 0x9E: cond = s.fb.ov1; break;
    case 0xA0: cond = !s.fa.s0; break;  case 0xA2: cond = s.fa.s0; break;
    case 0xA4: cond = !s.fb.s0; break;  case 0xA6: cond = s.fb.s0; break;
    case 0xA8: cond = !s.fa.s1; break;  case 0xAA: cond = s.fa.s1; break;
    case 0xAC: cond = !s.fb.s1; break;  case 0xAE: cond = s.fb.s1; break;
    case 0xB0: cond = !(s.dp & 0x0F); break;
    case 0xB1: cond = (s.dp & 0x0F) != 0; break;
    case 0xB2: cond = (s.dp & 0x0F) == 0x0F; break;
    case 0xB3: cond = (s.dp & 0x0F) != 0x0F; break;
    case 0xB4: cond = !(s.sr & 0x0100); break;  case 0xB6: cond = (s.sr & 0x0100) != 0; break;
    case 0xB8: cond = !(s.sr & 0x0200); break;  case 0xBA: cond = (s.sr & 0x0200) != 0; break;
    case 0xBC: cond = !(s.sr & SR_RQM); break;  case 0xBE: cond = (s.sr & SR_RQM) != 0; break;
    case 0x100: m_s.pc = target & ~0x2000; break;
    case 0x101: m_s.pc = target | 0x2000; break;
    case 0x140: m_stack[m_s.sp] = m_s.pc; m_s.sp = (m_s.sp + 1) & m_stackMask; m_s.pc = target & ~0x2000; break;
    case 0x141: m_stack[m_s.sp] = m_s.pc; m_s.sp = (m_s.sp + 1) & m_stackMask; m_s.pc = target | 0x2000; break;
    default: break;
    }
    if (cond) {
        // A jump to itself on RQM: the firmware waits for the SNES
        if ((uint16_t)(m_s.pc - 1) == target && (type == 0xBC || type == 0xBE)) m_rqmWait = true;
        m_s.pc = target;
    }
}

void NecDsp::load(uint8_t dest, uint16_t v) {
    switch (dest) {
    case 0x1: m_s.a = v; break;
    case 0x2: m_s.b = v; break;
    case 0x3: m_s.tr = v; break;
    case 0x4: m_s.dp = v; break;
    case 0x5: m_s.rp = v; break;
    case 0x6: m_s.dr = v; m_s.sr |= SR_RQM; break;
    case 0x7: m_s.sr = (uint16_t)((m_s.sr & 0x907C) | (v & ~0x907C)); break;
    case 0x8: case 0x9: m_s.so = v; break;
    case 0xA: m_s.k = v; break;
    case 0xB: m_s.k = v; m_s.l = m_data[m_s.rp & m_dataMask]; break;
    case 0xC: m_s.l = v; m_s.k = ram((uint16_t)(m_s.dp | 0x40)); break;
    case 0xD: m_s.l = v; break;
    case 0xE: m_s.trb = v; break;
    case 0xF: m_ram[m_s.dp & m_ramMask] = v; break;
    default: break;                                  // 0: none
    }
}

uint16_t NecDsp::source(uint8_t src) {
    switch (src) {
    case 0x0: return m_s.trb;
    case 0x1: return m_s.a;
    case 0x2: return m_s.b;
    case 0x3: return m_s.tr;
    case 0x4: return m_s.dp;
    case 0x5: return m_s.rp;
    case 0x6: return m_data[m_s.rp & m_dataMask];
    case 0x7: return (uint16_t)(0x8000 - (m_s.fa.s1 ? 1 : 0));
    case 0x8: m_s.sr |= SR_RQM; return m_s.dr;
    case 0x9: return m_s.dr;
    case 0xA: return m_s.sr;
    case 0xB: case 0xC: return m_s.si;
    case 0xD: return m_s.k;
    case 0xE: return m_s.l;
    default: return ram(m_s.dp);
    }
}

uint8_t NecDsp::read(bool sr) {
    if (sr) return (uint8_t)(m_s.sr >> 8);
    m_rqmWait = false;
    if (m_s.sr & SR_DRC) {                           // 8-bit transfers
        m_s.sr &= ~SR_RQM;
        return (uint8_t)m_s.dr;
    }
    if (m_s.sr & SR_DRS) {                           // the second byte of 16
        m_s.sr &= ~(SR_RQM | SR_DRS);
        return (uint8_t)(m_s.dr >> 8);
    }
    m_s.sr |= SR_DRS;
    return (uint8_t)m_s.dr;
}

void NecDsp::write(bool sr, uint8_t v) {
    if (sr) return;                                  // SR is read-only
    m_rqmWait = false;
    if (m_s.sr & SR_DRC) {
        m_s.sr &= ~SR_RQM;
        m_s.dr = (uint16_t)((m_s.dr & 0xFF00) | v);
    } else if (m_s.sr & SR_DRS) {
        m_s.sr &= ~(SR_RQM | SR_DRS);
        m_s.dr = (uint16_t)((m_s.dr & 0x00FF) | v << 8);
    } else {
        m_s.sr |= SR_DRS;
        m_s.dr = (uint16_t)((m_s.dr & 0xFF00) | v);
    }
}

}  // namespace snes
