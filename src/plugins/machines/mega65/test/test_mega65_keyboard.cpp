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

    // 'A' is port A line 1, port B bit 2 (standard C64/C65 matrix, #138)
    kbd.pressKeyByName("A", true);
    colPort->writePort(0xFD); // Drive port A line 1 low
    ASSERT_EQ(rowPort->readPort(), 0xFB); // Bit 2 low
    colPort->writePort(0xFB); // Line 2 would be R: not pressed
    ASSERT_EQ(rowPort->readPort(), 0xFF);
    colPort->writePort(0xFD);

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

    // RETURN: port A line 0, port B bit 1
    kbd.pressKeyByName("RETURN", true);

    // Drive port A line 0 low via CIA1 $DC00
    cia.ioWrite(nullptr, 0xDC00, 0xFE);

    // Read the key bits via CIA1 $DC01
    uint8_t rowVal = 0;
    ASSERT(cia.ioRead(nullptr, 0xDC01, &rowVal));
    ASSERT_EQ(rowVal, 0xFD); // Bit 1 low
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

// Every named key reads back at its standard C64 matrix position through the
// ports (the MEGA65 matrix was transposed before #138).
TEST_CASE(mega65_keyboard_matches_c64_matrix) {
    // C64 matrix: [port A line][port B bit]
    static const char* kMatrix[8][8] = {
        {"DEL", "RETURN", "LEFT", "F7", "F1", "F3", "F5", "UP"},
        {"3", "W", "A", "4", "Z", "S", "E", "LSHIFT"},
        {"5", "R", "D", "6", "C", "F", "T", "X"},
        {"7", "Y", "G", "8", "B", "H", "U", "V"},
        {"9", "I", "J", "0", "M", "K", "O", "N"},
        {"+", "P", "L", "-", ".", ":", "@", ","},
        {"POUND", "*", ";", "HOME", "RSHIFT", "=", "^", "/"},
        {"1", "ESC", "CTRL", "2", "SPACE", "COMMODORE", "Q", "RUNSTOP"},
    };
    KbdMega65 kbd;
    IPortDevice* colPort = kbd.getPort(0);
    IPortDevice* rowPort = kbd.getPort(1);
    for (int line = 0; line < 8; line++) {
        for (int bit = 0; bit < 8; bit++) {
            ASSERT(kbd.pressKeyByName(kMatrix[line][bit], true));
            for (int sel = 0; sel < 8; sel++) {
                colPort->writePort((uint8_t)~(1 << sel));
                uint8_t expect = (sel == line) ? (uint8_t)~(1 << bit) : 0xFF;
                ASSERT_EQ((int)rowPort->readPort(), (int)expect);
            }
            kbd.pressKeyByName(kMatrix[line][bit], false);
        }
    }
}
