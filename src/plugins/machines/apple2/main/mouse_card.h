#pragma once

#include "slot_card.h"
#include <cstdint>
#include <string>
#include <vector>

namespace apple2 {

/**
 * Apple's AppleMouse II card, after AppleWin's MouseInterface.cpp: a 6821
 * PIA talking to the card's 68705, which keeps the position, the clamp
 * window and the interrupt state. The 68705's program isn't run; its
 * protocol is (Kyle Kim's Apple in PC).
 *
 * - $C0n0-$C0n3: the 6821 (port A: data to and from the 68705; port B:
 *   handshake, b5 6502 strobe, b4 6502 read strobe, b6/b7 the 68705's
 *   replies, b1-b3 the $Cn00 page of the 2 KB firmware).
 * - $Cn00-$CnFF: one 256-byte page of the firmware (no $C800 ROM).
 * - /IRQ: on movement, a button or the vertical blank, as SETMOUSE's mode
 *   asks; SERVEMOUSE takes it down.
 *
 * ROM: roms/apple2/MouseInterface.rom (2 KB, AppleWin's resource).
 */
class MouseCard : public SlotCard {
public:
    /** Null with `err` set if the ROM is missing. */
    static MouseCard* create(int slot, std::string& err);

    int slot() const override { return m_slot; }
    int deviceIo(int reg, const uint8_t* wr, uint64_t at) override;
    int devicePeek(int reg) override;
    int pageIo(uint8_t off, const uint8_t*, uint64_t) override { return pagePeek(off); }
    int pagePeek(uint8_t off) override { return m_rom[(((unsigned)m_pb << 7) & 0x700u) + off]; }
    bool irq() const override { return m_irq; }
    void vbl() override { event(true); }

    /** The host mouse: a move (in the card's units) and the buttons. */
    void move(int dx, int dy);
    void setButtons(bool b0, bool b1);
    int x() const { return m_ix; }
    int y() const { return m_iy; }
    uint8_t mode() const { return m_mode; }

    // IOHandler
    const char* name() const override { return m_name.c_str(); }
    void reset() override;
    void tick(uint64_t) override {}
    void getDeviceInfo(DeviceInfo& out) const override;

private:
    MouseCard(int slot, std::vector<uint8_t> rom);

    // The 6821 (VICE's mc6821 as AppleWin has it)
    uint8_t piaRead(int rs);
    void piaWrite(int rs, uint8_t v);
    void outA(uint8_t v) { m_pa = v; }
    void outB(uint8_t v);
    // The 68705
    void command();
    void commit();
    void event(bool vbl);
    void clear();
    void clampX();
    void clampY();
    void setClampX(int lo, int hi);
    void setClampY(int lo, int hi);

    std::string m_name;
    int m_slot;
    std::vector<uint8_t> m_rom;
    // 6821 registers and the inputs the 68705 drives
    uint8_t m_pra = 0, m_ddra = 0, m_cra = 0, m_prb = 0, m_ddrb = 0, m_crb = 0;
    uint8_t m_ia = 0, m_ib = 0;
    // 68705 side
    uint8_t m_pa = 0, m_pb = 0x40;
    uint8_t m_buf[8] = {};
    int m_bufPos = 0, m_dataLen = 1;
    uint8_t m_mode = 0, m_state = 0;
    int m_nx = 0, m_ny = 0;                 // the position last read
    int m_ix = 0, m_iy = 0;                 // the position now
    int m_minX = 0, m_maxX = 1023, m_minY = 0, m_maxY = 1023;
    bool m_btn[2] = {}, m_btnRead[2] = {};
    bool m_irq = false;
};

}  // namespace apple2
