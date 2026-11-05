#include "test_harness.h"
#include "plugins/devices/keyboard/main/keyboard_matrix_pet.h"

namespace {
// The bits a selected row reads with `key` held (0 = pressed)
uint8_t rowWith(PetKeyboardMatrix& k, int row, const std::string& key) {
    k.keyDown(key);
    k.writePort((uint8_t)row);
    uint8_t v = k.readPort();
    k.keyUp(key);
    return v;
}
}

// The 8032's business keyboard, from its editor ROM's decode table ($E6D1)
// and VICE's business keymap: not the graphics keyboard's positions
TEST_CASE(pet_business_keyboard_positions) {
    PetKeyboardMatrix k(PetKeyboardMatrix::Layout::BUSINESS);
    ASSERT_EQ((int)rowWith(k, 3, "a"), 0xFE);           // A: row 3, bit 0
    ASSERT_EQ((int)rowWith(k, 6, "b"), 0xFB);           // B: row 6, bit 2
    ASSERT_EQ((int)rowWith(k, 3, "return"), 0xEF);      // RETURN: row 3, bit 4
    ASSERT_EQ((int)rowWith(k, 4, "tab"), 0xFE);
    ASSERT_EQ((int)rowWith(k, 2, "escape"), 0xFE);
    ASSERT_EQ((int)rowWith(k, 1, "1"), 0xFE);           // main row digits
    ASSERT_EQ((int)rowWith(k, 1, "0"), 0xF7);
    ASSERT_EQ((int)rowWith(k, 9, "3"), 0xFD);
    ASSERT_EQ((int)rowWith(k, 8, "space"), 0xFB);
    ASSERT_EQ((int)rowWith(k, 6, "left_shift"), 0xFE);
    ASSERT_EQ((int)rowWith(k, 6, "right_shift"), 0xBF);
    ASSERT_EQ((int)rowWith(k, 0, "a"), 0xFF);           // another row: nothing
    // Characters typed with SHIFT on this keyboard
    ASSERT_EQ(k.shiftedKey("dquote"), std::string("2"));
    ASSERT_EQ(k.shiftedKey("asterisk"), std::string("colon"));
    ASSERT_EQ(k.shiftedKey("equal"), std::string("minus"));
    ASSERT(!k.hasKey("dquote"));

    PetKeyboardMatrix g(PetKeyboardMatrix::Layout::GRAPHICS);   // dedicated keys there
    ASSERT(g.hasKey("dquote"));
    ASSERT_EQ(g.shiftedKey("dquote"), std::string());
    ASSERT_EQ((int)rowWith(g, 4, "a"), 0xFE);           // graphics A: row 4, bit 0
}
