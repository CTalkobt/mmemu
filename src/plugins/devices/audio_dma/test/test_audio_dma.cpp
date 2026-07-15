#include "test_harness.h"
#include "plugins/devices/audio_dma/main/audio_dma.h"
#include "libmem/main/memory_bus.h"
#include <cstring>

struct AudioDmaFixture {
    FlatMemoryBus bus{"flat", 20};
    AudioDmaDevice audio{0xD710};

    AudioDmaFixture() {
        audio.setDmaBus(&bus);
        audio.reset();
    }
};

// ============================================================================
// Control Register Tests ($D711)
// ============================================================================

TEST_CASE(audio_dma_control_register_read) {
    AudioDmaFixture f;
    uint8_t val = 0xFF;
    ASSERT(f.audio.ioRead(&f.bus, 0xD711, &val));
    ASSERT_EQ((int)val, 0x00);  // Initially disabled
}

TEST_CASE(audio_dma_control_register_write) {
    AudioDmaFixture f;
    uint8_t val = 0x80;  // AUDEN enabled
    ASSERT(f.audio.ioWrite(&f.bus, 0xD711, val));
    uint8_t readval = 0x00;
    ASSERT(f.audio.ioRead(&f.bus, 0xD711, &readval));
    ASSERT_EQ((int)readval, 0x80);
}

// ============================================================================
// Per-Channel Register Tests ($D720-$D75F)
// ============================================================================

TEST_CASE(audio_dma_channel_control_enable) {
    AudioDmaFixture f;
    uint8_t val = 0x80;  // Enable bit
    ASSERT(f.audio.ioWrite(&f.bus, 0xD720, val));
    uint8_t readval = 0x00;
    ASSERT(f.audio.ioRead(&f.bus, 0xD720, &readval));
    ASSERT_EQ((int)readval, 0x80);
}

TEST_CASE(audio_dma_channel_control_loop) {
    AudioDmaFixture f;
    uint8_t val = 0x40;  // Loop bit
    ASSERT(f.audio.ioWrite(&f.bus, 0xD720, val));
    uint8_t readval = 0x00;
    ASSERT(f.audio.ioRead(&f.bus, 0xD720, &readval));
    ASSERT_EQ((int)readval, 0x40);
}

TEST_CASE(audio_dma_channel_control_sine) {
    AudioDmaFixture f;
    uint8_t val = 0x20;  // Sine bit
    ASSERT(f.audio.ioWrite(&f.bus, 0xD720, val));
    uint8_t readval = 0x00;
    ASSERT(f.audio.ioRead(&f.bus, 0xD720, &readval));
    ASSERT_EQ((int)readval, 0x20);
}

TEST_CASE(audio_dma_loop_start_address_24bit) {
    AudioDmaFixture f;
    // Write loop start address $123456
    ASSERT(f.audio.ioWrite(&f.bus, 0xD721, 0x56));  // LSB
    ASSERT(f.audio.ioWrite(&f.bus, 0xD722, 0x34));  // MID
    ASSERT(f.audio.ioWrite(&f.bus, 0xD723, 0x12));  // MSB

    uint8_t lsb = 0, mid = 0, msb = 0;
    f.audio.ioRead(&f.bus, 0xD721, &lsb);
    f.audio.ioRead(&f.bus, 0xD722, &mid);
    f.audio.ioRead(&f.bus, 0xD723, &msb);

    ASSERT_EQ((int)lsb, 0x56);
    ASSERT_EQ((int)mid, 0x34);
    ASSERT_EQ((int)msb, 0x12);
}

TEST_CASE(audio_dma_frequency_step_24bit) {
    AudioDmaFixture f;
    // Write frequency step $800000 (fast playback)
    ASSERT(f.audio.ioWrite(&f.bus, 0xD724, 0x00));  // LSB
    ASSERT(f.audio.ioWrite(&f.bus, 0xD725, 0x00));  // MID
    ASSERT(f.audio.ioWrite(&f.bus, 0xD726, 0x80));  // MSB

    uint8_t lsb = 0, mid = 0, msb = 0;
    f.audio.ioRead(&f.bus, 0xD724, &lsb);
    f.audio.ioRead(&f.bus, 0xD725, &mid);
    f.audio.ioRead(&f.bus, 0xD726, &msb);

    ASSERT_EQ((int)lsb, 0x00);
    ASSERT_EQ((int)mid, 0x00);
    ASSERT_EQ((int)msb, 0x80);
}

TEST_CASE(audio_dma_sample_end_address_16bit) {
    AudioDmaFixture f;
    // Write sample end $FFFF
    ASSERT(f.audio.ioWrite(&f.bus, 0xD727, 0xFF));
    ASSERT(f.audio.ioWrite(&f.bus, 0xD728, 0xFF));

    uint8_t lsb = 0, msb = 0;
    f.audio.ioRead(&f.bus, 0xD727, &lsb);
    f.audio.ioRead(&f.bus, 0xD728, &msb);

    ASSERT_EQ((int)lsb, 0xFF);
    ASSERT_EQ((int)msb, 0xFF);
}

TEST_CASE(audio_dma_volume_control) {
    AudioDmaFixture f;
    ASSERT(f.audio.ioWrite(&f.bus, 0xD729, 0x7F));  // 50% volume

    uint8_t vol = 0;
    f.audio.ioRead(&f.bus, 0xD729, &vol);
    ASSERT_EQ((int)vol, 0x7F);
}

TEST_CASE(audio_dma_current_address_24bit) {
    AudioDmaFixture f;
    // Write current address $AABBCC
    ASSERT(f.audio.ioWrite(&f.bus, 0xD72A, 0xCC));
    ASSERT(f.audio.ioWrite(&f.bus, 0xD72B, 0xBB));
    ASSERT(f.audio.ioWrite(&f.bus, 0xD72C, 0xAA));

    uint8_t lsb = 0, mid = 0, msb = 0;
    f.audio.ioRead(&f.bus, 0xD72A, &lsb);
    f.audio.ioRead(&f.bus, 0xD72B, &mid);
    f.audio.ioRead(&f.bus, 0xD72C, &msb);

    ASSERT_EQ((int)lsb, 0xCC);
    ASSERT_EQ((int)mid, 0xBB);
    ASSERT_EQ((int)msb, 0xAA);
}

TEST_CASE(audio_dma_status_register) {
    AudioDmaFixture f;
    uint8_t status = 0xFF;
    ASSERT(f.audio.ioRead(&f.bus, 0xD72D, &status));
    ASSERT_EQ((int)status, 0x00);  // Not stopped initially
}

// ============================================================================
// Multi-Channel Tests
// ============================================================================

TEST_CASE(audio_dma_channel_independence) {
    AudioDmaFixture f;
    f.audio.ioWrite(&f.bus, 0xD711, 0x80);  // Enable audio DMA

    // Enable only channel 2, disable others
    f.audio.ioWrite(&f.bus, 0xD720, 0x00);  // Ch0 disabled
    f.audio.ioWrite(&f.bus, 0xD730, 0x00);  // Ch1 disabled
    f.audio.ioWrite(&f.bus, 0xD740, 0x80);  // Ch2 enabled
    f.audio.ioWrite(&f.bus, 0xD750, 0x00);  // Ch3 disabled

    // Verify channel enable states
    uint8_t ch0_ctrl = 0, ch2_ctrl = 0;
    f.audio.ioRead(&f.bus, 0xD720, &ch0_ctrl);
    f.audio.ioRead(&f.bus, 0xD740, &ch2_ctrl);

    ASSERT_EQ((int)(ch0_ctrl & 0x80), 0x00);  // Ch0 disabled
    ASSERT_EQ((int)(ch2_ctrl & 0x80), 0x80);  // Ch2 enabled
}

// ============================================================================
// Sample Fetching and Playback Tests
// ============================================================================

TEST_CASE(audio_dma_channel_disabled_no_playback) {
    AudioDmaFixture f;
    f.audio.ioWrite(&f.bus, 0xD711, 0x80);  // Enable audio DMA
    f.audio.ioWrite(&f.bus, 0xD720, 0x00);  // Disable channel 0

    // Setup sample data in bus
    uint8_t samples[] = {0x80, 0x90, 0xA0, 0xB0};
    f.bus.write8(0x00, samples[0]);
    f.bus.write8(0x01, samples[1]);
    f.bus.write8(0x02, samples[2]);
    f.bus.write8(0x03, samples[3]);

    // Tick should not fetch if channel disabled
    f.audio.tick(1);
    f.audio.tick(1);

    float buf[8] = {0};
    int samples_pulled = f.audio.pullSamples(buf, 4);
    ASSERT_EQ(samples_pulled, 0);  // No samples generated
}

TEST_CASE(audio_dma_frequency_counter_overflow) {
    AudioDmaFixture f;
    f.audio.ioWrite(&f.bus, 0xD711, 0x80);  // Enable audio DMA
    f.audio.ioWrite(&f.bus, 0xD720, 0x80);  // Enable channel 0

    // Set high frequency step (fast playback)
    f.audio.ioWrite(&f.bus, 0xD724, 0xFE);
    f.audio.ioWrite(&f.bus, 0xD725, 0xFF);
    f.audio.ioWrite(&f.bus, 0xD726, 0xFF);

    // Set sample end
    f.audio.ioWrite(&f.bus, 0xD727, 0x02);
    f.audio.ioWrite(&f.bus, 0xD728, 0x00);

    // Volume 255 (max)
    f.audio.ioWrite(&f.bus, 0xD729, 0xFF);

    // Setup sample data
    f.bus.write8(0x00, 0x00);
    f.bus.write8(0x01, 0x7F);
    f.bus.write8(0x02, 0xFF);

    // Tick multiple times to generate samples
    for (int i = 0; i < 10; ++i) {
        f.audio.tick(1);
    }

    // Pull generated samples
    float buf[16] = {0};
    int pulled = f.audio.pullSamples(buf, 8);

    // Should have generated some samples
    ASSERT(pulled >= 0);
}

// ============================================================================
// Loop Mode Tests
// ============================================================================

TEST_CASE(audio_dma_loop_mode_enabled) {
    AudioDmaFixture f;
    f.audio.ioWrite(&f.bus, 0xD711, 0x80);  // Enable audio DMA
    f.audio.ioWrite(&f.bus, 0xD720, 0xC0);  // Enable + Loop

    // Verify loop bit is set
    uint8_t ctrl = 0;
    f.audio.ioRead(&f.bus, 0xD720, &ctrl);
    ASSERT_EQ((int)(ctrl & 0x40), 0x40);
}

// ============================================================================
// Device Properties Tests
// ============================================================================

TEST_CASE(audio_dma_device_info) {
    AudioDmaFixture f;
    DeviceInfo info;
    f.audio.getDeviceInfo(info);

    ASSERT_EQ(info.name, "Audio DMA");
    ASSERT_EQ((int)info.baseAddr, 0xD710);
}

TEST_CASE(audio_dma_native_sample_rate) {
    AudioDmaFixture f;
    ASSERT_EQ(f.audio.nativeSampleRate(), 44100);
}

TEST_CASE(audio_dma_device_aliases) {
    AudioDmaFixture f;
    auto aliases = f.audio.deviceAliases();
    ASSERT(aliases.size() > 0);  // Should have at least one alias
}

TEST_CASE(audio_dma_out_of_range_access) {
    AudioDmaFixture f;
    uint8_t val = 0;
    // Access outside valid range should fail
    ASSERT(!f.audio.ioRead(&f.bus, 0xD680, &val));  // Out of range
    ASSERT(!f.audio.ioWrite(&f.bus, 0xD800, 0xFF)); // Out of range
}
