// The DMG and CGB PPU, after Mesen2 (Core/Gameboy/GbPpu.cpp), GPL-3.0, Sour and the Mesen contributors.

#include "gb_ppu.h"
#include "gameboy.h"

#include <algorithm>
#include <cstring>

namespace gb {

void Ppu::init() {
    m_scanline = 0; m_cycle = 0; m_idleCycles = 0;
    m_mode = PpuMode::HBlank; m_irqMode = PpuMode::HBlank;
    m_statIrqFlag = false;
    m_ly = 0; m_lyForCompare = 0; m_lyCompare = 0; m_lyCoincidence = false;
    m_bgp = 0; m_obp0 = 0xFF; m_obp1 = 0xFF; m_scx = m_scy = m_wx = m_wy = 0;
    m_control = 0;
    m_lcdEnabled = m_windowTilemap = m_windowEnabled = m_bgTileSelect = false;
    m_bgTilemap = m_largeSprites = m_spritesEnabled = m_bgEnabled = false;
    m_status = 0;
    m_frameCount = 0;
    m_lastFrameTime = 0;
    m_windowCounter = -1;
    m_spriteCount = 0;
    m_tileIndex = 0;
    m_lastPixelBg = true; m_lastBgColor = 0;
    // As Mesen's Init: the renderer and every flag reset (powering the SGB's Game Boy off and on works)
    resetRenderer();
    m_wyEnable = m_wxEnable = false;
    m_lcdDisabled = true;
    m_stopOamBlocked = m_stopVramBlocked = false;
    m_oamReadBlocked = m_oamWriteBlocked = m_vramReadBlocked = m_vramWriteBlocked = false;
    m_isFirstFrame = true;
    m_forceBlankFrame = true;
    m_rendererIdle = false;
    m_hblankPending = false;
    std::memset(m_buf, 0, sizeof(m_buf));
    // The CGB's (the palettes power on cleared here; Mesen's follow its RAM setting)
    m_cgb = m_gb->isCgb();
    m_cgbEnabled = m_cgb;
    m_tileGlitch = m_stopPaletteBlocked = false;
    m_vramBank = 0;
    std::memset(m_bgPal, 0, sizeof(m_bgPal));
    std::memset(m_objPal, 0, sizeof(m_objPal));
    m_bgPalPos = m_objPalPos = 0;
    m_bgPalAutoInc = m_objPalAutoInc = false;
    for (auto& b : m_cbuf) std::fill(b, b + WIDTH * HEIGHT, 0x7FFF);
}

void Ppu::setCpuStopState(bool stopped) {
    if (!m_cgb) {
        if (stopped) m_lcdDisabled = true;
        else if (m_lcdEnabled) m_lcdDisabled = !m_lcdEnabled;
        return;
    }
    // The CGB's LCD keeps going with what it could read when the CPU stopped: in mode 3
    // the picture stays, otherwise OAM, VRAM or the palettes read as nothing (black)
    if (stopped) {
        m_stopOamBlocked = !m_oamReadBlocked;
        m_stopVramBlocked = !m_vramReadBlocked;
        m_stopPaletteBlocked = m_mode <= PpuMode::OamEvaluation;
    } else {
        m_stopOamBlocked = m_stopVramBlocked = m_stopPaletteBlocked = false;
    }
}

void Ppu::setMode(PpuMode m) {
    m_mode = m;
    m_gb->processHdma();                 // the CGB's HDMA: a block on entering HBlank
}

void Ppu::exec(bool single) {
    if (m_lcdDisabled) {
        // The LCD off: no interrupts; a blank frame for each frame's worth of time
        if (m_gb->apuCycles() - m_lastFrameTime > 70224) {
            m_forceBlankFrame = true;
            sendFrame();
            m_forceBlankFrame = true;
            m_lastFrameTime = m_gb->apuCycles();
        }
        return;
    }
    const int n = (single || m_gb->isHighSpeed()) ? 1 : 2;     // double speed: a dot per CPU half-cycle
    for (int i = 0; i < n; ++i) {
        ++m_cycle;
        if (m_idleCycles > 0) --m_idleCycles;
        else execCycle();
        m_tileGlitch = false;              // the fetch glitch lasts the dot of the LCDC write
    }
}

void Ppu::execCycle() {
    const PpuMode oldMode = m_irqMode;
    if (m_scanline < 144) {
        if (m_scanline == 0 && m_isFirstFrame) firstScanlineAfterPowerOn();
        else visibleScanline();
    } else {
        vblankScanline();
    }

    if (m_hblankPending) {
        // The CGB, a dot after the last pixel: the mode 0 interrupt (stat-int), the CGB E's OAM
        // reads (oam-read-cgbE) and, in double speed, mode 0 itself (stat-mode-ds, vram-read)
        m_hblankPending = false;
        startHblank();
    } else if (m_mode == PpuMode::Drawing) {
        drawCycle();
        if (m_drawnPixels == 160) {
            // Mode 0 on the dot the last pixel goes out; on the CGB in single speed VRAM and OAM
            // writes free then too, the rest a dot later (SameBoy)
            if (auto* s = m_gb->sgb()) s->hblank();
            if (!m_cgb) {
                startHblank();
            } else {
                m_hblankPending = true;
                if (!m_gb->isHighSpeed()) {
                    m_oamWriteBlocked = m_vramReadBlocked = m_vramWriteBlocked = false;
                    setMode(PpuMode::HBlank);
                }
            }
        }
    } else if (m_mode == PpuMode::OamEvaluation) {
        spriteEvaluation();
    }

    const bool coincidence = m_lyCompare == m_lyForCompare;
    if (m_irqMode != oldMode || m_lyCoincidence != coincidence) {
        m_lyCoincidence = coincidence;
        updateStatIrq();
    }
}

void Ppu::startHblank() {
    m_irqMode = PpuMode::HBlank;
    m_idleCycles = (uint16_t)(456 - m_cycle - 1);
    m_oamReadBlocked = m_oamWriteBlocked = m_vramReadBlocked = m_vramWriteBlocked = false;
    if (m_mode != PpuMode::HBlank) setMode(PpuMode::HBlank);
}

void Ppu::vblankScanline() {
    switch (m_cycle) {
    case 2:
        if (m_scanline == 144) m_irqMode = PpuMode::OamEvaluation;
        break;
    case 4:
        if (m_cgb && m_scanline == 153) m_lyForCompare = 153;   // the CGB E's: LY 153 two dots longer
        if (m_scanline < 153) {
            m_lyForCompare = m_scanline;
            if (m_scanline == 144) {
                setMode(PpuMode::VBlank);
                m_irqMode = PpuMode::VBlank;
                if (auto* s = m_gb->sgb()) s->vblank();
                m_windowCounter = -1;
                m_gb->requestIrq(IRQ_VBLANK);
                sendFrame();
            }
        }
        break;
    case 6:
        if (m_scanline == 153 && !m_cgb) { m_ly = 0; m_lyForCompare = m_scanline; }
        break;
    case 8:
        if (m_scanline == 153 && !m_cgb) m_lyForCompare = -1;
        break;
    case 12:
        if (m_scanline == 153) m_lyForCompare = 0;
        m_idleCycles = 456 - 12 - 1;
        break;
    case 456:
        m_cycle = 0;
        ++m_scanline;
        m_drawnPixels = 0;
        if (m_scanline == 154) {
            m_scanline = 0;
            m_ly = 0;
            m_lyForCompare = 0;
            m_wyEnable = m_scanline == m_wy && m_windowEnabled;
            if (!m_cgb) setMode(PpuMode::HBlank);     // on the DMG, line 0 starts in mode 0 (not the CGB)
            oamWriteBlockAtLineStart();
        } else {
            m_ly = m_scanline;
            m_lyForCompare = -1;
        }
        break;
    }
}

void Ppu::firstScanlineAfterPowerOn() {
    switch (m_cycle) {
    case 85:
        m_oamWriteBlocked = true;                // 2 dots before mode 3 (SameBoy; oam-write)
        break;
    case 87:
        // The CGB's VRAM stays free 4 dots more in single speed (vram-read)
        m_oamReadBlocked = true;
        m_vramReadBlocked = m_vramWriteBlocked = !m_cgb || m_gb->isHighSpeed();
        m_rendererIdle = true;
        m_wxEnable = false;
        setMode(PpuMode::Drawing);
        m_irqMode = PpuMode::Drawing;
        resetRenderer();
        m_rendererIdle = true;
        break;
    case 91:
        // Mode 3 here ends 2 dots later than on other lines, not 3 (stat-mode)
        m_vramReadBlocked = m_vramWriteBlocked = true;
        m_rendererIdle = false;
        break;
    case 456:
        m_cycle = 0;
        ++m_scanline;
        m_wyEnable |= m_scanline == m_wy && m_windowEnabled;
        m_drawnPixels = 0;
        m_ly = m_scanline;
        oamWriteBlockAtLineStart();
        break;
    }
}

void Ppu::visibleScanline() {
    switch (m_cycle) {
    case 2:
        if (m_scanline > 0) {
            // Lines 1-143: the OAM interrupt a dot early
            m_irqMode = PpuMode::OamEvaluation;
            m_lyForCompare = -1;
        }
        if (m_cgb) m_oamWriteBlocked = true;     // in double speed from here (oam-write)
        break;
    case 3:
        m_oamReadBlocked = true;
        break;
    case 4:
        m_spriteCount = 0;
        m_lyForCompare = m_scanline;
        m_oamWriteBlocked = true;
        setMode(PpuMode::OamEvaluation);
        m_irqMode = PpuMode::OamEvaluation;
        break;
    case 5:
        m_irqMode = PpuMode::NoIrq;
        break;
    case 80:
        // The DMG's OAM writes free and VRAM reads blocked 4 dots before mode 3; the CGB's neither (SameBoy)
        m_oamWriteBlocked = m_cgb;
        m_oamReadBlocked = true;
        m_vramReadBlocked = !m_cgb;
        break;
    case 84:
        m_vramReadBlocked = true;
        m_wxEnable = false;
        setMode(PpuMode::Drawing);
        m_irqMode = PpuMode::Drawing;
        m_oamWriteBlocked = true;
        m_vramWriteBlocked = true;
        m_rendererIdle = true;
        break;
    case 89:
        resetRenderer();
        m_rendererIdle = false;
        break;
    case 456:
        m_cycle = 0;
        ++m_scanline;
        m_ly = m_scanline;
        m_wyEnable |= m_scanline == m_wy && m_windowEnabled;
        m_drawnPixels = 0;
        if (m_scanline == 144) m_lyForCompare = -1;
        oamWriteBlockAtLineStart();
        break;
    }
}

uint8_t Ppu::cgbLy() const {
    // The CGB's LY (AGE's, at either speed): a line's number from its fourth dot, the last
    // one's ANDed with it on the third (lcd-align-ly), LY 153 for 5 dots
    const uint8_t line = m_scanline;
    if (line == 0 || m_cycle >= 3) return (line == 153 && m_cycle >= 8) ? 0 : line;
    const uint8_t prev = (uint8_t)(line - 1);
    return m_cycle == 2 ? (uint8_t)(prev & line) : prev;
}

void Ppu::oamWriteBlockAtLineStart() {
    // The CGB in single speed blocks OAM writes from a visible line's first dot (oam-write)
    if (m_cgb && !m_gb->isHighSpeed() && m_scanline < 144) m_oamWriteBlocked = true;
}

void Ppu::drawCycle() {
    if (m_rendererIdle) return;

    if (!m_wxEnable) {
        m_wxEnable |= m_drawnPixels == (int)m_wx - 7;
        if (m_wxEnable && !m_cgb && m_wx == 166) {
            // The DMG's WX 166: the window's line counted, but no window this line (SameBoy; stat-mode-window-dmgC)
            if (m_windowEnabled && m_wyEnable) ++m_windowCounter;
        } else if (m_fetchWindow != (m_windowEnabled && m_wxEnable && m_wyEnable)) {
            // Background to window: the fetcher and the FIFO restart, fetching from this dot (mode 3
            // 6 dots longer); at WX 0 from the next (7: stat-mode-window), in double speed only with SCX & 7
            m_fetchWindow = true;
            m_fetchColumn = 0;
            ++m_windowCounter;
            m_bgFetcher.step = 0;
            m_bgFifo.reset();
            if (m_wx == 0 && (!m_gb->isHighSpeed() || (m_scx & 7))) return;
        }
        if (!m_cgb && !m_windowEnabled && m_windowCounter > 0 && m_wxEnable && ((int)m_wx & 7) == 7 - ((int)m_scx & 7)) {
            // The DMG's window glitch (Windesync-validate): an extra background pixel
            m_insertGlitchBgPixel = true;
        }
    }

    findNextSprite();
    if (m_fetchSprite >= 0 && m_bgFetcher.step >= 5 && m_bgFifo.size > 0) {
        clockSpriteFetcher();
        findNextSprite();
        return;
    }

    if (m_fetchSprite == -1 && m_bgFifo.size > 0) {
        if (m_drawnPixels >= 0) {
            const FifoEntry entry = m_insertGlitchBgPixel ? FifoEntry{} : m_bgFifo.content[m_bgFifo.position];
            const FifoEntry sprite = m_oamFifo.content[m_oamFifo.position];
            // The object's pixel over colour 0, or with neither its nor the tile's priority bit
            // set; in CGB mode always with LCDC bit 0 clear
            if (sprite.color != 0 && (entry.color == 0 || (!(sprite.attributes & 0x80) && !(entry.attributes & 0x80)) ||
                                      (m_cgbEnabled && !m_bgEnabled))) {
                if (m_cgbEnabled) {
                    writeObjPixel((uint8_t)(sprite.color | ((sprite.attributes & 0x07) << 2)));
                } else {
                    const uint8_t pal = (sprite.attributes & 0x10) ? m_obp1 : m_obp0;
                    writeObjPixel((uint8_t)(((sprite.attributes & 0x10) ? 4 : 0) | ((pal >> (sprite.color * 2)) & 3)));
                }
                m_lastPixelBg = false;
            } else {
                if (m_cgbEnabled) writeBgPixel((uint8_t)(entry.color | ((entry.attributes & 0x07) << 2)));
                else writeBgPixel(m_bgEnabled ? (uint8_t)((m_bgp >> (entry.color * 2)) & 3) : (uint8_t)(m_bgp & 3));
                m_lastPixelBg = true;
                m_lastBgColor = entry.color;
            }
        }
        if (!m_insertGlitchBgPixel) m_bgFifo.pop();
        m_insertGlitchBgPixel = false;
        ++m_drawnPixels;
        if (m_oamFifo.size > 0) m_oamFifo.pop();
    }

    clockTileFetcher();
}

void Ppu::writeBgPixel(uint8_t index) {
    writePixel(index, m_stopPaletteBlocked ? 0 : m_bgPal[index & 0x1F]);
}

void Ppu::writeObjPixel(uint8_t index) {
    writePixel(index, m_stopPaletteBlocked ? 0 : m_objPal[index & 0x1F]);
}

void Ppu::writePixel(uint8_t index, uint16_t color) {
    const int at = m_scanline * WIDTH + m_drawnPixels;
    m_cur[at] = index & 3;
    if (m_cgb) m_ccur[at] = color & 0x7FFF;
    if (auto* s = m_gb->sgb()) s->lcdPixel((uint8_t)m_drawnPixels, index & 3);
}

void Ppu::spriteEvaluation() {
    if (!(m_cycle & 1) || m_spriteCount >= 10) return;
    const uint8_t index = (uint8_t)(((m_cycle - 4) >> 1) * 4);
    if (!m_gb->oamDmaRunning()) {
        // During OAM DMA the last values read are reused (strikethrough)
        m_oamReadBuffer[0] = m_oam[index];
        m_oamReadBuffer[1] = m_oam[index + 1];
    }
    const int y = (int)m_oamReadBuffer[0] - 16;
    if (m_scanline >= y && m_scanline < y + (m_largeSprites ? 16 : 8)) {
        m_spriteY[m_spriteCount] = m_oamReadBuffer[0];
        m_spriteX[m_spriteCount] = m_oamReadBuffer[1];
        m_spriteIndex[m_spriteCount] = index;
        ++m_spriteCount;
    }
}

void Ppu::resetRenderer() {
    m_oamFifo.reset();
    m_oamFetcher.step = 0;
    m_bgFifo.reset();
    m_bgFifo.size = 8;
    m_bgFetcher.step = 0;
    m_drawnPixels = (int16_t)(-8 - (m_scx & 7));
    m_fetchSprite = -1;
    m_fetchWindow = false;
    m_fetchColumn = 0;
    m_insertGlitchBgPixel = false;
}

void Ppu::clockSpriteFetcher() {
    switch (m_oamFetcher.step++) {
    case 1: {
        const uint8_t addr = m_spriteIndex[m_fetchSprite];
        const int y = (int)m_spriteY[m_fetchSprite] - 16;
        if (!m_gb->oamDmaRunning()) {
            m_oamReadBuffer[0] = lcdReadOam((uint8_t)(addr + 2));
            m_oamReadBuffer[1] = lcdReadOam((uint8_t)(addr + 3));
        } else {
            // What OAM DMA is writing at the time
            const uint8_t a = m_gb->dmaLastWriteAddress() & 0xFE;
            m_oamReadBuffer[0] = m_oam[a];
            m_oamReadBuffer[1] = m_oam[a + 1];
        }
        uint8_t tile = m_oamReadBuffer[0];
        const uint8_t attr = m_oamReadBuffer[1];
        const bool vflip = (attr & 0x40) != 0;
        const uint16_t bank = (m_cgbEnabled && (attr & 0x08)) ? 0x2000 : 0;    // the CGB's VRAM bank bit
        const uint8_t row = (uint8_t)(vflip ? (m_largeSprites ? 15 : 7) - (m_scanline - y) : (m_scanline - y));
        if (m_largeSprites) tile &= 0xFE;
        m_oamFetcher.addr = (uint16_t)((tile * 16 + row * 2) | bank);
        m_oamFetcher.attributes = attr;
        break;
    }
    case 3: m_oamFetcher.low = lcdReadVram(m_oamFetcher.addr); break;
    case 5:
        m_oamFetcher.high = lcdReadVram((uint16_t)(m_oamFetcher.addr + 1));
        pushSpriteToFifo();
        break;
    }
}

void Ppu::findNextSprite() {
    if (m_fetchSprite >= 0 || !(m_spritesEnabled || m_cgbEnabled)) return;   // CGB mode: fetched (and dropped) when off
    for (int i = 0; i < m_spriteCount; ++i) {
        // X 0 is fetched at the line's first pixel, whatever SCX discards (11 dots: stat-mode-sprites)
        if ((int)m_spriteX[i] - 8 == m_drawnPixels || (m_spriteX[i] == 0 && m_drawnPixels < -8)) {
            m_fetchSprite = (int16_t)i;
            m_spriteX[i] = 0xFF;          // not again
            m_oamFetcher.step = 0;
            break;
        }
    }
}

void Ppu::clockTileFetcher() {
    switch (m_bgFetcher.step++) {
    case 1: {
        uint16_t tilemap;
        uint8_t yOffset, scrollPos;
        if (m_fetchWindow) {
            tilemap = m_windowTilemap ? 0x1C00 : 0x1800;
            yOffset = (uint8_t)m_windowCounter;
            scrollPos = m_fetchColumn;
        } else {
            tilemap = m_bgTilemap ? 0x1C00 : 0x1800;
            yOffset = (uint8_t)(m_scy + m_scanline);
            scrollPos = (uint8_t)((m_fetchColumn + (m_scx / 8)) & 0x1F);
        }
        const uint16_t tileAddr = (uint16_t)(tilemap + scrollPos + (yOffset >> 3) * 32);
        m_tileIndex = lcdReadVram(tileAddr);
        // CGB mode: the tile's attributes in bank 1 (palette, bank, flips, priority)
        const uint8_t attr = m_cgbEnabled ? m_vram[tileAddr | 0x2000] : 0;
        const bool vflip = (attr & 0x40) != 0;
        const uint16_t bank = (attr & 0x08) ? 0x2000 : 0;
        const uint16_t base = m_bgTileSelect ? 0 : 0x1000;
        const int tileY = vflip ? 7 - (yOffset & 7) : (yOffset & 7);
        m_bgFetcher.addr = (uint16_t)((base + (base ? (int8_t)m_tileIndex * 16 : m_tileIndex * 16) + tileY * 2) | bank);
        m_bgFetcher.attributes = attr & 0xBF;
        break;
    }
    case 3: m_bgFetcher.low = m_tileGlitch ? m_tileIndex : lcdReadVram(m_bgFetcher.addr); break;
    case 5:
        m_bgFetcher.high = m_tileGlitch ? m_tileIndex : lcdReadVram((uint16_t)(m_bgFetcher.addr + 1));
        [[fallthrough]];
    case 6:
    case 7:
        if (m_bgFifo.size == 0) pushTileToFifo();
        else if (m_bgFetcher.step == 8) m_bgFetcher.step = 7;     // wait for the FIFO to empty
        break;
    }
}

void Ppu::pushSpriteToFifo() {
    const uint8_t index = (uint8_t)m_fetchSprite;
    m_fetchSprite = -1;
    m_oamFetcher.step = 0;
    if (!m_spritesEnabled) return;
    uint8_t pos = m_oamFifo.position;
    for (int i = 0; i < 8; ++i) {
        const int shift = (m_oamFetcher.attributes & 0x20) ? i : 7 - i;
        const uint8_t bits = (uint8_t)(((m_oamFetcher.low >> shift) & 1) | (((m_oamFetcher.high >> shift) & 1) << 1));
        // Over a transparent pixel; in CGB mode over a later object's too (OAM order wins, not X)
        if (bits > 0 && (m_oamFifo.content[pos].color == 0 || (m_cgbEnabled && index <= m_oamFifo.content[pos].index))) {
            m_oamFifo.content[pos].color = bits;
            m_oamFifo.content[pos].attributes = m_oamFetcher.attributes;
            m_oamFifo.content[pos].index = index;
        }
        pos = (pos + 1) & 7;
    }
    m_oamFifo.size = 8;
}

void Ppu::pushTileToFifo() {
    for (int i = 0; i < 8; ++i) {
        const int shift = (m_bgFetcher.attributes & 0x20) ? i : 7 - i;     // the CGB's horizontal flip
        m_bgFifo.content[i].color = (uint8_t)(((m_bgFetcher.low >> shift) & 1) | (((m_bgFetcher.high >> shift) & 1) << 1));
        m_bgFifo.content[i].attributes = m_bgFetcher.attributes;
    }
    m_fetchColumn = (m_fetchColumn + 1) & 0x1F;
    m_bgFifo.position = 0;
    m_bgFifo.size = 8;
    m_bgFetcher.step = 0;
}

void Ppu::updateStatIrq() {
    const bool irq = m_lcdEnabled &&
                     ((m_lyCoincidence && (m_status & 0x40)) ||
                      (m_irqMode == PpuMode::HBlank && (m_status & 0x08)) ||
                      (m_irqMode == PpuMode::OamEvaluation && (m_status & 0x20)) ||
                      (m_irqMode == PpuMode::VBlank && (m_status & 0x10)));
    if (irq && !m_statIrqFlag) m_gb->requestIrq(IRQ_STAT);
    m_statIrqFlag = irq;
}

void Ppu::sendFrame() {
    ++m_frameCount;
    // The first frame after the LCD is turned on stays blank (not on the SGB, whose ICD2 gets the lines anyway)
    if (m_forceBlankFrame && !m_gb->isSgb()) {
        std::memset(m_cur, 0, WIDTH * HEIGHT);
        if (m_cgb) for (auto& b : m_cbuf) std::fill(b, b + WIDTH * HEIGHT, 0x7FFF);   // both, white (as Mesen)
    }
    m_forceBlankFrame = false;
    m_isFirstFrame = false;
    std::swap(m_cur, m_front);
    if (m_cgb) std::swap(m_ccur, m_cfront);
}

uint8_t Ppu::read(uint16_t addr) const {
    switch (addr) {
    case 0xFF40: return m_control;
    case 0xFF41: return (uint8_t)(0x80 | (m_status & 0x78) | (m_lyCoincidence ? 0x04 : 0) | ((int)m_mode & 3));
    case 0xFF42: return m_scy;
    case 0xFF43: return m_scx;
    case 0xFF44: return m_cgb ? cgbLy() : m_ly;
    case 0xFF45: return m_lyCompare;
    case 0xFF47: return m_bgp;
    case 0xFF48: return m_obp0;
    case 0xFF49: return m_obp1;
    case 0xFF4A: return m_wy;
    case 0xFF4B: return m_wx;
    }
    return 0xFF;
}

void Ppu::write(uint16_t addr, uint8_t v) {
    switch (addr) {
    case 0xFF40:
        m_control = v;
        if (m_lcdEnabled != ((v & 0x80) != 0)) {
            m_lcdEnabled = (v & 0x80) != 0;
            if (!m_lcdEnabled) {
                // Off: back to the top of the screen
                if (m_mode != PpuMode::VBlank) sendFrame();
                m_cycle = 0; m_scanline = 0; m_ly = 0; m_lyForCompare = 0;
                m_oamReadBlocked = m_oamWriteBlocked = m_vramReadBlocked = m_vramWriteBlocked = false;
                m_irqMode = PpuMode::NoIrq;
                m_hblankPending = false;
                m_wyEnable = false;
                m_lcdDisabled = true;
                m_lastFrameTime = m_gb->apuCycles();
                setMode(PpuMode::HBlank);
            } else {
                m_lcdDisabled = false;
                m_isFirstFrame = true;
                // The CGB blanks the first frame only after the LCD has been off a while (Mesen: Men in Black flickers)
                m_forceBlankFrame = !m_cgb || m_gb->apuCycles() - m_lastFrameTime > 5000;
                m_cycle = 7;
                m_idleCycles = 0;
                m_irqMode = PpuMode::NoIrq;
                resetRenderer();
                m_lyCoincidence = m_lyCompare == m_lyForCompare;
                updateStatIrq();
            }
        }
        m_windowTilemap = (v & 0x40) != 0;
        m_windowEnabled = (v & 0x20) != 0;
        m_bgTileSelect = (v & 0x10) != 0;
        m_bgTilemap = (v & 0x08) != 0;
        m_largeSprites = (v & 0x04) != 0;
        m_spritesEnabled = (v & 0x02) != 0;
        m_bgEnabled = (v & 0x01) != 0;
        m_wyEnable |= m_scanline == m_wy && m_windowEnabled;
        break;
    case 0xFF41:
        if (!m_cgb) {
            // The DMG's STAT write bug: every source on for a moment
            m_status = (uint8_t)(0xF8 | (m_status & 0x07));
            updateStatIrq();
        }
        m_status = v & 0xF8;
        updateStatIrq();
        break;
    case 0xFF42: m_scy = v; break;
    case 0xFF43: m_scx = v; break;
    case 0xFF45:
        m_lyCompare = v;
        if (m_lcdEnabled) {
            m_idleCycles = 0;
            m_lyCoincidence = m_lyCompare == m_lyForCompare;
            updateStatIrq();
        }
        break;
    case 0xFF47:
        if (!m_cgbEnabled && m_mode == PpuMode::Drawing && m_drawnPixels > 0 && m_lastPixelBg) {
            // A change mid-line reaches the pixel just drawn, ORed with the old value on the DMG
            const uint8_t bgp = m_cgb ? v : (uint8_t)(v | m_bgp);
            --m_drawnPixels;
            writeBgPixel((uint8_t)((bgp >> (m_lastBgColor * 2)) & 3));
            ++m_drawnPixels;
        }
        m_bgp = v;
        break;
    case 0xFF48: m_obp0 = v; break;
    case 0xFF49: m_obp1 = v; break;
    case 0xFF4A: m_wy = v; break;
    case 0xFF4B: m_wx = v; break;
    }
}

uint8_t Ppu::readOam(uint8_t addr) const {
    if (addr < 0xA0) return (!m_oamReadBlocked && !m_gb->oamDmaRunning()) ? m_oam[addr] : 0xFF;
    return m_gb->oamDmaRunning() ? 0xFF : 0;
}

void Ppu::writeOam(uint8_t addr, uint8_t v, bool forDma) {
    if (addr >= 0xA0) return;
    if (forDma || (!m_oamWriteBlocked && !m_gb->oamDmaRunning())) m_oam[addr] = v;
}

template <OamCorruption C> void Ppu::oamCorruption(uint16_t addr) {
    if (m_cgb || m_cycle >= 83 || m_mode != PpuMode::OamEvaluation || (addr & 0xFF00) != 0xFE00) return;
    const int row = std::min(19, (m_cycle - 2) >> 2);
    if (row == 0) return;                 // the first two sprites are never hit
    if (C == OamCorruption::ReadIncDec) oamIncDecCorruption(row);
    const int prev = row - 1;
    const uint16_t a = (uint16_t)(m_oam[row * 8] | m_oam[row * 8 + 1] << 8);
    const uint16_t b = (uint16_t)(m_oam[prev * 8] | m_oam[prev * 8 + 1] << 8);
    const uint16_t c = (uint16_t)(m_oam[prev * 8 + 4] | m_oam[prev * 8 + 5] << 8);
    const uint16_t r = C == OamCorruption::Write ? (uint16_t)(((a ^ c) & (b ^ c)) ^ c) : (uint16_t)(b | (a & c));
    m_oam[row * 8] = (uint8_t)r;
    m_oam[row * 8 + 1] = (uint8_t)(r >> 8);
    std::memcpy(m_oam + row * 8 + 2, m_oam + prev * 8 + 2, 6);
}

void Ppu::oamIncDecCorruption(int row) {
    if (row < 4 || row >= 19) return;
    const int prev = row - 1;
    const uint16_t a = (uint16_t)(m_oam[(row - 2) * 8] | m_oam[(row - 2) * 8 + 1] << 8);
    const uint16_t b = (uint16_t)(m_oam[prev * 8] | m_oam[prev * 8 + 1] << 8);
    const uint16_t c = (uint16_t)(m_oam[row * 8] | m_oam[row * 8 + 1] << 8);
    const uint16_t d = (uint16_t)(m_oam[prev * 8 + 4] | m_oam[prev * 8 + 5] << 8);
    const uint16_t r = (uint16_t)((b & (a | c | d)) | (a & c & d));
    m_oam[prev * 8] = (uint8_t)r;
    m_oam[prev * 8 + 1] = (uint8_t)(r >> 8);
    std::memcpy(m_oam + row * 8, m_oam + prev * 8, 8);
    std::memcpy(m_oam + (row - 2) * 8, m_oam + prev * 8, 8);
}

// ---------------------------------------------------------------------------
// The CGB's registers
// ---------------------------------------------------------------------------

uint8_t Ppu::readCgb(uint16_t addr) const {
    if (!m_cgbEnabled) return 0xFF;
    switch (addr) {
    case 0xFF4F: return (uint8_t)(m_vramBank | 0xFE);
    case 0xFF68: return (uint8_t)(m_bgPalPos | (m_bgPalAutoInc ? 0x80 : 0) | 0x40);
    case 0xFF69: return readCgbPalette(m_bgPalPos, m_bgPal);
    case 0xFF6A: return (uint8_t)(m_objPalPos | (m_objPalAutoInc ? 0x80 : 0) | 0x40);
    case 0xFF6B: return readCgbPalette(m_objPalPos, m_objPal);
    }
    return 0xFF;
}

void Ppu::writeCgb(uint16_t addr, uint8_t v) {
    if (!m_cgbEnabled && m_gb->bootRomDisabled()) return;
    switch (addr) {
    case 0xFF4C: m_cgbEnabled = (v & 0x0C) == 0; break;     // KEY0: bits 2-3 set, DMG compatibility
    case 0xFF4F: m_vramBank = v & 0x01; break;
    case 0xFF68:
        m_bgPalPos = v & 0x3F;
        m_bgPalAutoInc = (v & 0x80) != 0;
        break;
    case 0xFF69: writeCgbPalette(m_bgPalPos, m_bgPal, m_bgPalAutoInc, v); break;
    case 0xFF6A:
        m_objPalPos = v & 0x3F;
        m_objPalAutoInc = (v & 0x80) != 0;
        break;
    case 0xFF6B: writeCgbPalette(m_objPalPos, m_objPal, m_objPalAutoInc, v); break;
    }
}

uint8_t Ppu::readCgbPalette(uint8_t pos, const uint16_t* pal) const {
    if (m_mode <= PpuMode::OamEvaluation) return (uint8_t)(pal[pos >> 1] >> ((pos & 1) ? 8 : 0));
    return 0xFF;                           // in mode 3 the PPU has them
}

void Ppu::writeCgbPalette(uint8_t& pos, uint16_t* pal, bool autoInc, uint8_t v) {
    if (m_mode <= PpuMode::OamEvaluation) {
        if (pos & 1) pal[pos >> 1] = (uint16_t)((pal[pos >> 1] & 0xFF) | (v << 8));
        else pal[pos >> 1] = (uint16_t)((pal[pos >> 1] & 0xFF00) | v);
    }
    if (autoInc) pos = (pos + 1) & 0x3F;   // the index moves on even when the write is blocked
}

template void Ppu::oamCorruption<OamCorruption::Read>(uint16_t);
template void Ppu::oamCorruption<OamCorruption::Write>(uint16_t);
template void Ppu::oamCorruption<OamCorruption::ReadIncDec>(uint16_t);

}  // namespace gb
