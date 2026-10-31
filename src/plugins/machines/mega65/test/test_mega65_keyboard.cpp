#include "test_harness.h"
#include <vector>
#include <string>
#include "plugins/devices/keyboard/main/keyboard_matrix_mega65.h"
#include "plugins/devices/cia6526/main/cia6526.h"
#include "libdevices/main/shared_signal_line.h"

TEST_CASE(mega65_keyboard_matrix_basic) {
    KbdMega65 kbd;
    IPortDevice* colPort = kbd.getPort(0);
    IPortDevice* rowPort = kbd.getPort(1);

    // Initial state: no keys pressed, row reads $FF regardless of column
    colPort->writePort(0xFE); // Select column 0
    ASSERT_EQ(rowPort->readPort(), 0xFF);

    // Press 'DEL' (Row 0, Col 0)
    kbd.keyDown(0, 0);
    
    colPort->writePort(0xFE); // Select column 0
    ASSERT_EQ(rowPort->readPort(), 0xFE); // Bit 0 low

    colPort->writePort(0xFD); // Select column 1
    ASSERT_EQ(rowPort->readPort(), 0xFF); // Still high for other columns

    // Release 'DEL'
    kbd.keyUp(0, 0);
    colPort->writePort(0xFE);
    ASSERT_EQ(rowPort->readPort(), 0xFF);
}

TEST_CASE(mega65_keyboard_name_mapping) {
    KbdMega65 kbd;
    IPortDevice* colPort = kbd.getPort(0);
    IPortDevice* rowPort = kbd.getPort(1);

    // 'A' is Row 1, Col 2
    kbd.pressKeyByName("A", true);
    colPort->writePort(0xFB); // Select column 2
    ASSERT_EQ(rowPort->readPort(), 0xFD); // Bit 1 low

    kbd.pressKeyByName("A", false);
    ASSERT_EQ(rowPort->readPort(), 0xFF);
}

TEST_CASE(mega65_keyboard_restore_signal) {
    KbdMega65 kbd;
    SharedSignalLine sigRestore("restore");
    kbd.setRestoreLine(&sigRestore);

    ASSERT(sigRestore.get() == false);
    kbd.pressKeyByName("RESTORE", true);
}

TEST_CASE(mega65_keyboard_cia_integration) {
    KbdMega65 kbd;
    CIA6526 cia("CIA1", 0xDC00);
    
    cia.setPortADevice(kbd.getPort(0));
    cia.setPortBDevice(kbd.getPort(1));

    // Configure CIA1 Port A as output, Port B as input
    cia.ioWrite(nullptr, 0xDC02, 0xFF); // DDR A = Output
    cia.ioWrite(nullptr, 0xDC03, 0x00); // DDR B = Input

    // Press 'RETURN' (Row 1, Col 0 - wait, my mapping says RETURN is row 0 col 1)
    // RETURN: Row 0, Col 1
    kbd.pressKeyByName("RETURN", true);

    // Select Column 1 via CIA1 Port A
    cia.ioWrite(nullptr, 0xDC00, 0xFD);

    // Read Row sense via CIA1 Port B
    uint8_t rowVal = 0;
    ASSERT(cia.ioRead(nullptr, 0xDC01, &rowVal));
    ASSERT_EQ(rowVal, 0xFE); // Row 0 bit low
}

TEST_CASE(mega65_keyboard_type_text_uses_key_sink) {
    KbdMega65 kbd;
    std::vector<std::string> events;
    kbd.setKeySink([&](const std::string& name, bool down) {
        events.push_back(name + (down ? "+" : "-"));
        return true;
    });

    kbd.enqueueText("Pa\"\\n");
    for (int i = 0; i < 1000000; i++) kbd.tick(1);

    // Uppercase = unshifted key, lowercase = shifted key, '"' = LSHIFT+2, \n = RETURN
    std::vector<std::string> expected = {
        "P+", "P-",
        "LSHIFT+", "A+", "A-", "LSHIFT-",
        "LSHIFT+", "2+", "2-", "LSHIFT-",
        "RETURN+", "RETURN-",
    };
    ASSERT(events == expected);
}
