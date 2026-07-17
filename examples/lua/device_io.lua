-- Device I/O Access for mmemu Lua Scripts
-- Issue #24 Phase 6.2: Device I/O Access
--
-- High-level Lua bindings for hardware devices (SID, VIC-II, CIA, DMA, etc.)
--
-- Devices are accessed via memory-mapped I/O registers.
-- This module provides convenient functions to read/write device registers.

local device_io = {}
local stdlib = require("stdlib")

-- ============================================================================
-- SID 6581 Sound Chip (Address: $D400-$D41F)
-- ============================================================================

local SID = {}
SID.BASE_ADDR = 0xD400
SID.SIZE = 0x20

-- Register offsets
SID.FREQ_LO_1 = 0x00
SID.FREQ_HI_1 = 0x01
SID.PW_LO_1 = 0x02
SID.PW_HI_1 = 0x03
SID.CTRL_1 = 0x04
SID.ATCK_DCY_1 = 0x05
SID.SUTN_RLS_1 = 0x06
SID.FREQ_LO_2 = 0x07
SID.FREQ_HI_2 = 0x08
SID.PW_LO_2 = 0x09
SID.PW_HI_2 = 0x0A
SID.CTRL_2 = 0x0B
SID.ATCK_DCY_2 = 0x0C
SID.SUTN_RLS_2 = 0x0D
SID.FREQ_LO_3 = 0x0E
SID.FREQ_HI_3 = 0x0F
SID.PW_LO_3 = 0x10
SID.PW_HI_3 = 0x11
SID.CTRL_3 = 0x12
SID.ATCK_DCY_3 = 0x13
SID.SUTN_RLS_3 = 0x14
SID.FC_LO = 0x15
SID.FC_HI = 0x16
SID.RES_FILT = 0x17
SID.MODE_VOL = 0x18
SID.POT_X = 0x19
SID.POT_Y = 0x1A
SID.OSC_3 = 0x1B
SID.ENV_3 = 0x1C

function device_io.SID_set_frequency(backend, channel, freq)
    channel = (channel - 1) * 7  -- Channels 1-3
    local freq_lo = freq & 0xFF
    local freq_hi = (freq >> 8) & 0xFF
    backend:write_byte(SID.BASE_ADDR + SID.FREQ_LO_1 + channel, freq_lo)
    backend:write_byte(SID.BASE_ADDR + SID.FREQ_HI_1 + channel, freq_hi)
end

function device_io.SID_set_pwm(backend, channel, pwm)
    channel = (channel - 1) * 7
    local pw_lo = pwm & 0xFF
    local pw_hi = (pwm >> 8) & 0xFF
    backend:write_byte(SID.BASE_ADDR + SID.PW_LO_1 + channel, pw_lo)
    backend:write_byte(SID.BASE_ADDR + SID.PW_HI_1 + channel, pw_hi)
end

function device_io.SID_set_envelope(backend, channel, attack, decay, sustain, release)
    channel = (channel - 1) * 7
    local atck_dcy = ((attack & 0x0F) << 4) | (decay & 0x0F)
    local sutn_rls = ((sustain & 0x0F) << 4) | (release & 0x0F)
    backend:write_byte(SID.BASE_ADDR + SID.ATCK_DCY_1 + channel, atck_dcy)
    backend:write_byte(SID.BASE_ADDR + SID.SUTN_RLS_1 + channel, sutn_rls)
end

function device_io.SID_set_waveform(backend, channel, waveform)
    -- Waveform: 1=Triangle, 2=Sawtooth, 4=Pulse, 8=Noise
    channel = (channel - 1) * 7
    local ctrl = backend:read_byte(SID.BASE_ADDR + SID.CTRL_1 + channel)
    ctrl = (ctrl & 0x0F) | ((waveform & 0x0F) << 4)
    backend:write_byte(SID.BASE_ADDR + SID.CTRL_1 + channel, ctrl)
end

function device_io.SID_gate(backend, channel, gate)
    channel = (channel - 1) * 7
    local ctrl = backend:read_byte(SID.BASE_ADDR + SID.CTRL_1 + channel)
    if gate then
        ctrl = ctrl | 0x01
    else
        ctrl = ctrl & 0xFE
    end
    backend:write_byte(SID.BASE_ADDR + SID.CTRL_1 + channel, ctrl)
end

function device_io.SID_set_volume(backend, volume)
    local mode_vol = backend:read_byte(SID.BASE_ADDR + SID.MODE_VOL)
    mode_vol = (mode_vol & 0xF0) | (volume & 0x0F)
    backend:write_byte(SID.BASE_ADDR + SID.MODE_VOL, mode_vol)
end

device_io.SID = SID

-- ============================================================================
-- VIC-II Graphics Chip (Address: $D000-$D02E)
-- ============================================================================

local VIC = {}
VIC.BASE_ADDR = 0xD000

-- Register offsets
VIC.SPR0_X = 0x00
VIC.SPR0_Y = 0x01
VIC.SPR1_X = 0x02
VIC.SPR1_Y = 0x03
VIC.SPR_MSB_X = 0x10
VIC.SPR_ENABLE = 0x15
VIC.BORDER_COLOR = 0x20
VIC.BG_COLOR = 0x21
VIC.SPR_COLOR_0 = 0x27
VIC.CONTROL = 0x11

function device_io.VIC_set_sprite_pos(backend, sprite, x, y)
    backend:write_byte(VIC.BASE_ADDR + VIC.SPR0_X + (sprite * 2), x & 0xFF)
    backend:write_byte(VIC.BASE_ADDR + VIC.SPR0_Y + (sprite * 2), y & 0xFF)

    -- Set MSB of X if needed
    if x > 255 then
        local msb = backend:read_byte(VIC.BASE_ADDR + VIC.SPR_MSB_X)
        msb = msb | (1 << sprite)
        backend:write_byte(VIC.BASE_ADDR + VIC.SPR_MSB_X, msb)
    end
end

function device_io.VIC_set_sprite_color(backend, sprite, color)
    backend:write_byte(VIC.BASE_ADDR + VIC.SPR_COLOR_0 + sprite, color)
end

function device_io.VIC_enable_sprite(backend, sprite, enable)
    local enable_reg = backend:read_byte(VIC.BASE_ADDR + VIC.SPR_ENABLE)
    if enable then
        enable_reg = enable_reg | (1 << sprite)
    else
        enable_reg = enable_reg & ~(1 << sprite)
    end
    backend:write_byte(VIC.BASE_ADDR + VIC.SPR_ENABLE, enable_reg)
end

function device_io.VIC_set_border_color(backend, color)
    backend:write_byte(VIC.BASE_ADDR + VIC.BORDER_COLOR, color)
end

function device_io.VIC_set_bg_color(backend, color)
    backend:write_byte(VIC.BASE_ADDR + VIC.BG_COLOR, color)
end

device_io.VIC = VIC

-- ============================================================================
-- CIA 6526 Timer/IO Chip (Address: $DC00-$DD0F)
-- ============================================================================

local CIA = {}
CIA.CIA1_BASE = 0xDC00
CIA.CIA2_BASE = 0xDD00

-- Register offsets (per CIA)
CIA.PRA = 0x00
CIA.PRB = 0x01
CIA.DDRA = 0x02
CIA.DDRB = 0x03
CIA.TALO = 0x04
CIA.TAHI = 0x05
CIA.TBLO = 0x06
CIA.TBHI = 0x07
CIA.TOD_10TH = 0x08
CIA.TOD_SEC = 0x09
CIA.TOD_MIN = 0x0A
CIA.TOD_HR = 0x0B
CIA.SDR = 0x0C
CIA.ICR = 0x0D
CIA.CRA = 0x0E
CIA.CRB = 0x0F

function device_io.CIA_set_timer_a(backend, cia_num, value, start)
    local base = cia_num == 1 and CIA.CIA1_BASE or CIA.CIA2_BASE
    backend:write_byte(base + CIA.TALO, value & 0xFF)
    backend:write_byte(base + CIA.TAHI, (value >> 8) & 0xFF)

    if start then
        local cra = backend:read_byte(base + CIA.CRA)
        backend:write_byte(base + CIA.CRA, cra | 0x01)
    end
end

function device_io.CIA_set_port_a(backend, cia_num, value, ddr)
    local base = cia_num == 1 and CIA.CIA1_BASE or CIA.CIA2_BASE
    backend:write_byte(base + CIA.DDRA, ddr or 0xFF)
    backend:write_byte(base + CIA.PRA, value)
end

function device_io.CIA_read_port_a(backend, cia_num)
    local base = cia_num == 1 and CIA.CIA1_BASE or CIA.CIA2_BASE
    return backend:read_byte(base + CIA.PRA)
end

device_io.CIA = CIA

-- ============================================================================
-- F018B DMA Controller (Address: $D640-$D65F)
-- ============================================================================

local DMA = {}
DMA.BASE_ADDR = 0xD640

-- DMA registers
DMA.ADDR = 0x00
DMA.ADDR_HI = 0x01
DMA.COUNT = 0x02
DMA.COUNT_HI = 0x03
DMA.STATUS = 0x04
DMA.MODE = 0x05
DMA.COMMAND = 0x06
DMA.OPTIONS = 0x07

function device_io.DMA_copy(backend, src, dst, count)
    -- Setup copy operation
    backend:write_byte(DMA.BASE_ADDR + DMA.ADDR, src & 0xFF)
    backend:write_byte(DMA.BASE_ADDR + DMA.ADDR_HI, (src >> 8) & 0xFF)
    backend:write_byte(DMA.BASE_ADDR + DMA.COUNT, count & 0xFF)
    backend:write_byte(DMA.BASE_ADDR + DMA.COUNT_HI, (count >> 8) & 0xFF)
    backend:write_byte(DMA.BASE_ADDR + DMA.MODE, 0x00)  -- Copy mode
    backend:write_byte(DMA.BASE_ADDR + DMA.COMMAND, 0x01)  -- Start
end

function device_io.DMA_fill(backend, dst, count, value)
    -- Setup fill operation
    backend:write_byte(DMA.BASE_ADDR + DMA.ADDR, value)
    backend:write_byte(DMA.BASE_ADDR + DMA.ADDR_HI, value)
    backend:write_byte(DMA.BASE_ADDR + DMA.COUNT, count & 0xFF)
    backend:write_byte(DMA.BASE_ADDR + DMA.COUNT_HI, (count >> 8) & 0xFF)
    backend:write_byte(DMA.BASE_ADDR + DMA.MODE, 0x01)  -- Fill mode
    backend:write_byte(DMA.BASE_ADDR + DMA.COMMAND, 0x01)  -- Start
end

device_io.DMA = DMA

-- ============================================================================
-- Audio DMA (Address: $D720-$D75F)
-- ============================================================================

local AUDIO_DMA = {}
AUDIO_DMA.BASE_ADDR = 0xD720

-- Per-channel registers (4 channels)
AUDIO_DMA.CH_FREQ_LO = 0x00
AUDIO_DMA.CH_FREQ_MID = 0x01
AUDIO_DMA.CH_FREQ_HI = 0x02
AUDIO_DMA.CH_SAMPLE_ADDR = 0x03
AUDIO_DMA.CH_VOLUME = 0x04
AUDIO_DMA.CH_MODE = 0x05

function device_io.AUDIO_DMA_set_frequency(backend, channel, freq)
    local base = AUDIO_DMA.BASE_ADDR + (channel * 8)
    backend:write_byte(base + AUDIO_DMA.CH_FREQ_LO, freq & 0xFF)
    backend:write_byte(base + AUDIO_DMA.CH_FREQ_MID, (freq >> 8) & 0xFF)
    backend:write_byte(base + AUDIO_DMA.CH_FREQ_HI, (freq >> 16) & 0xFF)
end

function device_io.AUDIO_DMA_set_volume(backend, channel, volume)
    local base = AUDIO_DMA.BASE_ADDR + (channel * 8)
    backend:write_byte(base + AUDIO_DMA.CH_VOLUME, volume & 0xFF)
end

function device_io.AUDIO_DMA_enable_loop(backend, channel, enable)
    local base = AUDIO_DMA.BASE_ADDR + (channel * 8)
    local mode = backend:read_byte(base + AUDIO_DMA.CH_MODE)
    if enable then
        mode = mode | 0x01
    else
        mode = mode & 0xFE
    end
    backend:write_byte(base + AUDIO_DMA.CH_MODE, mode)
end

device_io.AUDIO_DMA = AUDIO_DMA

-- ============================================================================
-- Color Palette Access (Address: $D800-$DBFF, C64 Color RAM)
-- ============================================================================

function device_io.set_palette_color(backend, index, color)
    backend:write_byte(0xD800 + (index & 0x3FF), color & 0x0F)
end

function device_io.get_palette_color(backend, index)
    return backend:read_byte(0xD800 + (index & 0x3FF)) & 0x0F
end

function device_io.fill_palette(backend, color)
    for i = 0, 1023 do
        backend:write_byte(0xD800 + i, color & 0x0F)
    end
end

-- ============================================================================
-- Keyboard Matrix (via CIA1)
-- ============================================================================

function device_io.read_keyboard_row(backend, row)
    -- Write row select to CIA1 PRA (Port A)
    device_io.CIA_set_port_a(backend, 1, 0xFF - (1 << row), 0x00)
    -- Read column state from CIA1 PRB (Port B)
    return backend:read_byte(CIA.CIA1_BASE + CIA.PRB)
end

function device_io.is_key_pressed(backend, row, col)
    local port_b = device_io.read_keyboard_row(backend, row)
    return (port_b & (1 << col)) == 0
end

-- ============================================================================
-- Utility Functions
-- ============================================================================

--- Check if device is accessible (read/write test)
function device_io.probe_device(backend, base_addr)
    local ok, err = pcall(function()
        local test_val = 0xA5
        backend:write_byte(base_addr, test_val)
        local read_val = backend:read_byte(base_addr)
        return read_val == test_val or read_val == 0xFF or read_val == 0x00
    end)
    return ok
end

--- List available devices
function device_io.enumerate_devices(backend)
    local devices = {
        {name = "SID", addr = 0xD400, accessible = device_io.probe_device(backend, 0xD400)},
        {name = "VIC-II", addr = 0xD000, accessible = device_io.probe_device(backend, 0xD000)},
        {name = "CIA1", addr = 0xDC00, accessible = device_io.probe_device(backend, 0xDC00)},
        {name = "CIA2", addr = 0xDD00, accessible = device_io.probe_device(backend, 0xDD00)},
        {name = "DMA", addr = 0xD640, accessible = device_io.probe_device(backend, 0xD640)},
    }
    return devices
end

-- ============================================================================
-- Version & Export
-- ============================================================================

device_io.VERSION = "1.0.0"

return device_io
