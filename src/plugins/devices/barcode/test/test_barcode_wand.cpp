#include "test_harness.h"
#include "plugins/devices/barcode/main/barcode_wand.h"
#include <string>
#include <vector>

namespace {

std::string bits(const std::vector<bool>& m) {
    std::string s;
    for (bool b : m) s += b ? '1' : '0';
    return s;
}

// Run lengths of a module string (bars and spaces alternate)
std::vector<int> runs(const std::vector<bool>& m) {
    std::vector<int> r;
    for (size_t i = 0; i < m.size();) {
        size_t j = i;
        while (j < m.size() && m[j] == m[i]) ++j;
        r.push_back((int)(j - i));
        i = j;
    }
    return r;
}

}  // namespace

// Code 39: *A* is start, A, stop; each character 5 bars + 4 spaces, 3 of them wide
TEST_CASE(barcode_code39) {
    const auto m = BarcodeWand::code39("A");
    // * = 010010100, A = 100001001 (n=1, w=3) and a narrow gap between characters
    const std::string star = "100010111011101";    // b1 s3 b1 s1 b3 s1 b3 s1 b1
    const std::string a = "111010100010111";       // b3 s1 b1 s1 b1 s3 b1 s1 b3
    ASSERT_EQ(bits(m), star + "0" + a + "0" + star);
    // Every character in the set encodes with exactly 3 wide of 9 elements (15 + 1 gap modules each)
    const std::string all = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-. $/+%";
    const auto big = BarcodeWand::code39(all);
    ASSERT_EQ(big.size(), (all.size() + 2) * 16 - 1);    // 15 modules a character and a 1-module gap
    const auto r = runs(big);
    ASSERT_EQ(r.size(), (all.size() + 2) * 10 - 1);
    int wide = 0;
    for (int x : r) {
        ASSERT(x == 1 || x == 3 || x == 2 || x == 4);   // narrow, wide, gap merged with a space
        if (x >= 3) ++wide;
    }
    ASSERT(wide > 0);
    ASSERT(BarcodeWand::code39("a") == BarcodeWand::code39("A"));
    ASSERT(BarcodeWand::code39("A*B").empty());
    ASSERT(BarcodeWand::code39("#").empty());
}

// EAN-13 / UPC-A: 95 modules, the check digit computed, guards in place
TEST_CASE(barcode_ean13) {
    const auto m = BarcodeWand::ean13("400638133393");      // check digit 1
    ASSERT_EQ(m.size(), 95u);
    ASSERT(BarcodeWand::ean13("4006381333931") == m);
    ASSERT(BarcodeWand::ean13("4006381333932").empty());
    const std::string s = bits(m);
    ASSERT_EQ(s.substr(0, 3), std::string("101"));
    ASSERT_EQ(s.substr(45, 5), std::string("01010"));
    ASSERT_EQ(s.substr(92, 3), std::string("101"));
    // First digit 4: LGLLGG. Digit 0 (L) = 0001101; digit 0 (G) = 0100111; last digit 1 (R) = 1100110
    ASSERT_EQ(s.substr(3, 7), std::string("0001101"));
    ASSERT_EQ(s.substr(10, 7), std::string("0100111"));
    ASSERT_EQ(s.substr(85, 7), std::string("1100110"));
    ASSERT(BarcodeWand::upca("03600029145") == BarcodeWand::ean13("003600029145"));
    ASSERT(BarcodeWand::parse("upca:03600029145").size() == 95u);
    ASSERT(BarcodeWand::parse("bits:1|0.").size() == 4u);
    ASSERT(BarcodeWand::parse("nope:1").empty());
}

// A swipe: quiet zone, then the modules at MODULE_SECONDS each; data() low over a bar
TEST_CASE(barcode_swipe_timing) {
    const double hz = 1000000.0;
    BarcodeWand w(hz);
    ASSERT(w.data());
    w.swipe(BarcodeWand::fromBits("110"));
    ASSERT(w.scanning());
    const uint64_t mod = (uint64_t)(hz * BarcodeWand::MODULE_SECONDS);
    std::string seen;
    for (int i = 0; i < BarcodeWand::QUIET_MODULES + 3 + BarcodeWand::QUIET_MODULES; ++i) {
        seen += w.data() ? '.' : '|';
        w.tick(mod);
    }
    ASSERT_EQ(seen, std::string(10, '.') + "||." + std::string(10, '.'));
    ASSERT(!w.scanning());
    ASSERT(w.data());
    ASSERT_EQ(w.swipes(), 1);
}
