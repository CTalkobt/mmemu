// The SP0256 Narrator speech processor, after MAME's src/devices/sound/sp0256.cpp
// (Joseph Zbiciak, Tim Lindner; BSD-3-Clause; from jzIntv).

#include "sp0256.h"

#include <cstring>

namespace {

constexpr int PER_PAUSE = 64;      // the equivalent period of a pause
constexpr int PER_NOISE = 64;      // ... and of noise

// A data-format control word: len 4 bits, left shift 4, parameter 4,
// delta (sign-extended add), field (replace the MSBs), clear B5/F5, clear all
constexpr uint16_t CR(int l, int s, int p, int d, int f, int c5, int ca) {
    return (uint16_t)(((l & 15) << 0) | ((s & 15) << 4) | ((p & 15) << 8) | ((d & 1) << 12) | ((f & 1) << 13) |
                      ((c5 & 1) << 14) | ((ca & 1) << 15));
}
constexpr uint16_t CR_DELTA = CR(0, 0, 0, 1, 0, 0, 0);
constexpr uint16_t CR_FIELD = CR(0, 0, 0, 0, 1, 0, 0);
constexpr uint16_t CR_CLR5 = CR(0, 0, 0, 0, 0, 1, 0);
constexpr uint16_t CR_CLRA = CR(0, 0, 0, 0, 0, 0, 1);

enum { AM = 0, PR, B0, F0, B1, F1, B2, F2, B3, F3, B4, F4, B5, F5, IA, IP };

// The microsequencer's data blocks (MAME's sp0256_datafmt)
const uint16_t DATAFMT[] = {
    /*   0 */ CR( 0,  0,  0,  0,  0,  0,  1),  /*  Clear all   */
    /*   1 */ CR( 8,  0,  AM, 0,  0,  0,  1),  /*  Amplitude   */
    /*   2 */ CR( 8,  0,  PR, 0,  0,  0,  0),  /*  Period      */
    /*   3 */ CR( 8,  0,  B0, 0,  0,  0,  0),  /*  B0          */
    /*   4 */ CR( 8,  0,  F0, 0,  0,  0,  0),  /*  F0          */
    /*   5 */ CR( 8,  0,  B1, 0,  0,  0,  0),  /*  B1          */
    /*   6 */ CR( 8,  0,  F1, 0,  0,  0,  0),  /*  F1          */
    /*   7 */ CR( 8,  0,  B2, 0,  0,  0,  0),  /*  B2          */
    /*   8 */ CR( 8,  0,  F2, 0,  0,  0,  0),  /*  F2          */
    /*   9 */ CR( 8,  0,  B3, 0,  0,  0,  0),  /*  B3          */
    /*  10 */ CR( 8,  0,  F3, 0,  0,  0,  0),  /*  F3          */
    /*  11 */ CR( 8,  0,  B4, 0,  0,  0,  0),  /*  B4          */
    /*  12 */ CR( 8,  0,  F4, 0,  0,  0,  0),  /*  F4          */
    /*  13 */ CR( 8,  0,  B5, 0,  0,  0,  0),  /*  B5          */
    /*  14 */ CR( 8,  0,  F5, 0,  0,  0,  0),  /*  F5          */
    /*  15 */ CR( 8,  0,  IA, 0,  0,  0,  0),  /*  Amp Interp  */
    /*  16 */ CR( 8,  0,  IP, 0,  0,  0,  0),  /*  Pit Interp  */
    /*  17 */ CR( 6,  2,  AM, 0,  0,  0,  1),  /*  Amplitude   */
    /*  18 */ CR( 8,  0,  PR, 0,  0,  0,  0),  /*  Period      */
    /*  19 */ CR( 4,  3,  B3, 0,  0,  0,  0),  /*  B3 (S=0)    */
    /*  20 */ CR( 6,  2,  F3, 0,  0,  0,  0),  /*  F3          */
    /*  21 */ CR( 7,  1,  B4, 0,  0,  0,  0),  /*  B4          */
    /*  22 */ CR( 6,  2,  F4, 0,  0,  0,  0),  /*  F4          */
    /*  23 */ CR( 8,  0,  B5, 0,  0,  0,  0),  /*  B5          */
    /*  24 */ CR( 8,  0,  F5, 0,  0,  0,  0),  /*  F5          */
    /*  25 */ CR( 6,  2,  AM, 0,  0,  0,  1),  /*  Amplitude   */
    /*  26 */ CR( 8,  0,  PR, 0,  0,  0,  0),  /*  Period      */
    /*  27 */ CR( 6,  1,  B3, 0,  0,  0,  0),  /*  B3 (S=0)    */
    /*  28 */ CR( 7,  1,  F3, 0,  0,  0,  0),  /*  F3          */
    /*  29 */ CR( 8,  0,  B4, 0,  0,  0,  0),  /*  B4          */
    /*  30 */ CR( 8,  0,  F4, 0,  0,  0,  0),  /*  F4          */
    /*  31 */ CR( 8,  0,  B5, 0,  0,  0,  0),  /*  B5          */
    /*  32 */ CR( 8,  0,  F5, 0,  0,  0,  0),  /*  F5          */
    /*  33 */ CR( 0,  0,  0,  0,  0,  1,  0),  /*  Clear 5     */
    /*  34 */ CR( 6,  2,  AM, 0,  0,  0,  0),  /*  Amplitude   */
    /*  35 */ CR( 6,  2,  F3, 0,  1,  0,  0),  /*  F3 (5 MSBs) */
    /*  36 */ CR( 6,  2,  F4, 0,  1,  0,  0),  /*  F4 (5 MSBs) */
    /*  37 */ CR( 8,  0,  F5, 0,  1,  0,  0),  /*  F5 (5 MSBs) */
    /*  38 */ CR( 0,  0,  0,  0,  0,  1,  0),  /*  Clear 5     */
    /*  39 */ CR( 6,  2,  AM, 0,  0,  0,  0),  /*  Amplitude   */
    /*  40 */ CR( 7,  1,  F3, 0,  1,  0,  0),  /*  F3 (6 MSBs) */
    /*  41 */ CR( 8,  0,  F4, 0,  1,  0,  0),  /*  F4 (6 MSBs) */
    /*  42 */ CR( 8,  0,  F5, 0,  1,  0,  0),  /*  F5 (6 MSBs) */
    /*  43 */ 0,  /* unused */
    /*  44 */ 0,  /* unused */
    /*  45 */ CR( 4,  2,  AM, 1,  0,  0,  0),  /*  Amplitude   */
    /*  46 */ CR( 5,  0,  PR, 1,  0,  0,  0),  /*  Period      */
    /*  47 */ CR( 3,  4,  B0, 1,  0,  0,  0),  /*  B0 4 MSBs   */
    /*  48 */ CR( 3,  3,  F0, 1,  0,  0,  0),  /*  F0 5 MSBs   */
    /*  49 */ CR( 3,  4,  B1, 1,  0,  0,  0),  /*  B1 4 MSBs   */
    /*  50 */ CR( 3,  3,  F1, 1,  0,  0,  0),  /*  F1 5 MSBs   */
    /*  51 */ CR( 3,  4,  B2, 1,  0,  0,  0),  /*  B2 4 MSBs   */
    /*  52 */ CR( 3,  3,  F2, 1,  0,  0,  0),  /*  F2 5 MSBs   */
    /*  53 */ CR( 3,  3,  B3, 1,  0,  0,  0),  /*  B3 5 MSBs   */
    /*  54 */ CR( 4,  2,  F3, 1,  0,  0,  0),  /*  F3 6 MSBs   */
    /*  55 */ CR( 4,  1,  B4, 1,  0,  0,  0),  /*  B4 7 MSBs   */
    /*  56 */ CR( 4,  2,  F4, 1,  0,  0,  0),  /*  F4 6 MSBs   */
    /*  57 */ CR( 5,  0,  B5, 1,  0,  0,  0),  /*  B5 8 MSBs   */
    /*  58 */ CR( 5,  0,  F5, 1,  0,  0,  0),  /*  F5 8 MSBs   */
    /*  59 */ CR( 4,  2,  AM, 1,  0,  0,  0),  /*  Amplitude   */
    /*  60 */ CR( 5,  0,  PR, 1,  0,  0,  0),  /*  Period      */
    /*  61 */ CR( 4,  1,  B0, 1,  0,  0,  0),  /*  B0 7 MSBs   */
    /*  62 */ CR( 4,  2,  F0, 1,  0,  0,  0),  /*  F0 6 MSBs   */
    /*  63 */ CR( 4,  1,  B1, 1,  0,  0,  0),  /*  B1 7 MSBs   */
    /*  64 */ CR( 4,  2,  F1, 1,  0,  0,  0),  /*  F1 6 MSBs   */
    /*  65 */ CR( 4,  1,  B2, 1,  0,  0,  0),  /*  B2 7 MSBs   */
    /*  66 */ CR( 4,  2,  F2, 1,  0,  0,  0),  /*  F2 6 MSBs   */
    /*  67 */ CR( 4,  1,  B3, 1,  0,  0,  0),  /*  B3 7 MSBs   */
    /*  68 */ CR( 5,  1,  F3, 1,  0,  0,  0),  /*  F3 7 MSBs   */
    /*  69 */ CR( 5,  0,  B4, 1,  0,  0,  0),  /*  B4 8 MSBs   */
    /*  70 */ CR( 5,  0,  F4, 1,  0,  0,  0),  /*  F4 8 MSBs   */
    /*  71 */ CR( 5,  0,  B5, 1,  0,  0,  0),  /*  B5 8 MSBs   */
    /*  72 */ CR( 5,  0,  F5, 1,  0,  0,  0),  /*  F5 8 MSBs   */
    /*  73 */ CR( 0,  0,  0,  0,  0,  1,  0),  /*  Clear 5     */
    /*  74 */ CR( 6,  2,  AM, 0,  0,  0,  0),  /*  Amplitude   */
    /*  75 */ CR( 5,  3,  F0, 0,  1,  0,  0),  /*  F0 (5 MSBs) */
    /*  76 */ CR( 5,  3,  F1, 0,  1,  0,  0),  /*  F1 (5 MSBs) */
    /*  77 */ CR( 5,  3,  F2, 0,  1,  0,  0),  /*  F2 (5 MSBs) */
    /*  78 */ CR( 0,  0,  0,  0,  0,  1,  0),  /*  Clear 5     */
    /*  79 */ CR( 6,  2,  AM, 0,  0,  0,  0),  /*  Amplitude   */
    /*  80 */ CR( 6,  2,  F0, 0,  1,  0,  0),  /*  F0 (6 MSBs) */
    /*  81 */ CR( 6,  2,  F1, 0,  1,  0,  0),  /*  F1 (6 MSBs) */
    /*  82 */ CR( 6,  2,  F2, 0,  1,  0,  0),  /*  F2 (6 MSBs) */
    /*  83 */ CR( 6,  2,  AM, 0,  0,  0,  1),  /*  Amplitude   */
    /*  84 */ CR( 8,  0,  PR, 0,  0,  0,  0),  /*  Period      */
    /*  85 */ CR( 3,  4,  B0, 0,  0,  0,  0),  /*  B0 (S=0)    */
    /*  86 */ CR( 5,  3,  F0, 0,  0,  0,  0),  /*  F0          */
    /*  87 */ CR( 3,  4,  B1, 0,  0,  0,  0),  /*  B1 (S=0)    */
    /*  88 */ CR( 5,  3,  F1, 0,  0,  0,  0),  /*  F1          */
    /*  89 */ CR( 3,  4,  B2, 0,  0,  0,  0),  /*  B2 (S=0)    */
    /*  90 */ CR( 5,  3,  F2, 0,  0,  0,  0),  /*  F2          */
    /*  91 */ CR( 4,  3,  B3, 0,  0,  0,  0),  /*  B3 (S=0)    */
    /*  92 */ CR( 6,  2,  F3, 0,  0,  0,  0),  /*  F3          */
    /*  93 */ CR( 7,  1,  B4, 0,  0,  0,  0),  /*  B4          */
    /*  94 */ CR( 6,  2,  F4, 0,  0,  0,  0),  /*  F4          */
    /*  95 */ CR( 5,  0,  IA, 0,  0,  0,  0),  /*  Ampl. Intr. */
    /*  96 */ CR( 5,  0,  IP, 0,  0,  0,  0),  /*  Per. Intr.  */
    /*  97 */ CR( 6,  2,  AM, 0,  0,  0,  1),  /*  Amplitude   */
    /*  98 */ CR( 8,  0,  PR, 0,  0,  0,  0),  /*  Period      */
    /*  99 */ CR( 6,  1,  B0, 0,  0,  0,  0),  /*  B0 (S=0)    */
    /* 100 */ CR( 6,  2,  F0, 0,  0,  0,  0),  /*  F0          */
    /* 101 */ CR( 6,  1,  B1, 0,  0,  0,  0),  /*  B1 (S=0)    */
    /* 102 */ CR( 6,  2,  F1, 0,  0,  0,  0),  /*  F1          */
    /* 103 */ CR( 6,  1,  B2, 0,  0,  0,  0),  /*  B2 (S=0)    */
    /* 104 */ CR( 6,  2,  F2, 0,  0,  0,  0),  /*  F2          */
    /* 105 */ CR( 6,  1,  B3, 0,  0,  0,  0),  /*  B3 (S=0)    */
    /* 106 */ CR( 7,  1,  F3, 0,  0,  0,  0),  /*  F3          */
    /* 107 */ CR( 8,  0,  B4, 0,  0,  0,  0),  /*  B4          */
    /* 108 */ CR( 8,  0,  F4, 0,  0,  0,  0),  /*  F4          */
    /* 109 */ CR( 5,  0,  IA, 0,  0,  0,  0),  /*  Ampl. Intr. */
    /* 110 */ CR( 5,  0,  IP, 0,  0,  0,  0),  /*  Per. Intr.  */
    /* 111 */ CR( 4,  2,  AM, 1,  0,  0,  0),  /*  Amplitude   */
    /* 112 */ CR( 5,  0,  PR, 1,  0,  0,  0),  /*  Period      */
    /* 113 */ CR( 3,  3,  B3, 1,  0,  0,  0),  /*  B3 5 MSBs   */
    /* 114 */ CR( 4,  2,  F3, 1,  0,  0,  0),  /*  F3 6 MSBs   */
    /* 115 */ CR( 4,  1,  B4, 1,  0,  0,  0),  /*  B4 7 MSBs   */
    /* 116 */ CR( 4,  2,  F4, 1,  0,  0,  0),  /*  F4 6 MSBs   */
    /* 117 */ CR( 5,  0,  B5, 1,  0,  0,  0),  /*  B5 8 MSBs   */
    /* 118 */ CR( 5,  0,  F5, 1,  0,  0,  0),  /*  F5 8 MSBs   */
    /* 119 */ CR( 4,  2,  AM, 1,  0,  0,  0),  /*  Amplitude   */
    /* 120 */ CR( 5,  0,  PR, 1,  0,  0,  0),  /*  Period      */
    /* 121 */ CR( 4,  1,  B3, 1,  0,  0,  0),  /*  B3 7 MSBs   */
    /* 122 */ CR( 5,  1,  F3, 1,  0,  0,  0),  /*  F3 7 MSBs   */
    /* 123 */ CR( 5,  0,  B4, 1,  0,  0,  0),  /*  B4 8 MSBs   */
    /* 124 */ CR( 5,  0,  F4, 1,  0,  0,  0),  /*  F4 8 MSBs   */
    /* 125 */ CR( 5,  0,  B5, 1,  0,  0,  0),  /*  B5 8 MSBs   */
    /* 126 */ CR( 5,  0,  F5, 1,  0,  0,  0),  /*  F5 8 MSBs   */
    /* 127 */ CR( 6,  2,  AM, 0,  0,  0,  0),  /*  Amplitude   */
    /* 128 */ CR( 8,  0,  PR, 0,  0,  0,  0),  /*  Period      */
    /* 129 */ CR( 6,  2,  AM, 0,  0,  0,  1),  /*  Amplitude   */
    /* 130 */ CR( 8,  0,  PR, 0,  0,  0,  0),  /*  Period      */
    /* 131 */ CR( 3,  4,  B0, 0,  0,  0,  0),  /*  B0 (S=0)    */
    /* 132 */ CR( 5,  3,  F0, 0,  0,  0,  0),  /*  F0          */
    /* 133 */ CR( 3,  4,  B1, 0,  0,  0,  0),  /*  B1 (S=0)    */
    /* 134 */ CR( 5,  3,  F1, 0,  0,  0,  0),  /*  F1          */
    /* 135 */ CR( 3,  4,  B2, 0,  0,  0,  0),  /*  B2 (S=0)    */
    /* 136 */ CR( 5,  3,  F2, 0,  0,  0,  0),  /*  F2          */
    /* 137 */ CR( 4,  3,  B3, 0,  0,  0,  0),  /*  B3 (S=0)    */
    /* 138 */ CR( 6,  2,  F3, 0,  0,  0,  0),  /*  F3          */
    /* 139 */ CR( 7,  1,  B4, 0,  0,  0,  0),  /*  B4          */
    /* 140 */ CR( 6,  2,  F4, 0,  0,  0,  0),  /*  F4          */
    /* 141 */ CR( 8,  0,  B5, 0,  0,  0,  0),  /*  B5          */
    /* 142 */ CR( 8,  0,  F5, 0,  0,  0,  0),  /*  F5          */
    /* 143 */ CR( 5,  0,  IA, 0,  0,  0,  0),  /*  Ampl. Intr. */
    /* 144 */ CR( 5,  0,  IP, 0,  0,  0,  0),  /*  Per. Intr.  */
    /* 145 */ CR( 6,  2,  AM, 0,  0,  0,  1),  /*  Amplitude   */
    /* 146 */ CR( 8,  0,  PR, 0,  0,  0,  0),  /*  Period      */
    /* 147 */ CR( 6,  1,  B0, 0,  0,  0,  0),  /*  B0 (S=0)    */
    /* 148 */ CR( 6,  2,  F0, 0,  0,  0,  0),  /*  F0          */
    /* 149 */ CR( 6,  1,  B1, 0,  0,  0,  0),  /*  B1 (S=0)    */
    /* 150 */ CR( 6,  2,  F1, 0,  0,  0,  0),  /*  F1          */
    /* 151 */ CR( 6,  1,  B2, 0,  0,  0,  0),  /*  B2 (S=0)    */
    /* 152 */ CR( 6,  2,  F2, 0,  0,  0,  0),  /*  F2          */
    /* 153 */ CR( 6,  1,  B3, 0,  0,  0,  0),  /*  B3 (S=0)    */
    /* 154 */ CR( 7,  1,  F3, 0,  0,  0,  0),  /*  F3          */
    /* 155 */ CR( 8,  0,  B4, 0,  0,  0,  0),  /*  B4          */
    /* 156 */ CR( 8,  0,  F4, 0,  0,  0,  0),  /*  F4          */
    /* 157 */ CR( 8,  0,  B5, 0,  0,  0,  0),  /*  B5          */
    /* 158 */ CR( 8,  0,  F5, 0,  0,  0,  0),  /*  F5          */
    /* 159 */ CR( 5,  0,  IA, 0,  0,  0,  0),  /*  Ampl. Intr. */
    /* 160 */ CR( 5,  0,  IP, 0,  0,  0,  0),  /*  Per. Intr.  */
    /* 161 */ CR( 0,  0,  0,  0,  0,  1,  0),  /*  Clear 5     */
    /* 162 */ CR( 6,  2,  AM, 0,  0,  0,  0),  /*  Amplitude   */
    /* 163 */ CR( 8,  0,  PR, 0,  0,  0,  0),  /*  Period      */
    /* 164 */ CR( 5,  3,  F0, 0,  1,  0,  0),  /*  F0 (5 MSBs) */
    /* 165 */ CR( 5,  3,  F1, 0,  1,  0,  0),  /*  F1 (5 MSBs) */
    /* 166 */ CR( 5,  3,  F2, 0,  1,  0,  0),  /*  F2 (5 MSBs) */
    /* 167 */ CR( 5,  0,  IA, 0,  0,  0,  0),  /*  Ampl. Intr. */
    /* 168 */ CR( 5,  0,  IP, 0,  0,  0,  0),  /*  Per. Intr.  */
    /* 169 */ CR( 0,  0,  0,  0,  0,  1,  0),  /*  Clear 5     */
    /* 170 */ CR( 6,  2,  AM, 0,  0,  0,  0),  /*  Amplitude   */
    /* 171 */ CR( 8,  0,  PR, 0,  0,  0,  0),  /*  Period      */
    /* 172 */ CR( 6,  2,  F0, 0,  1,  0,  0),  /*  F0 (6 MSBs) */
    /* 173 */ CR( 6,  2,  F1, 0,  1,  0,  0),  /*  F1 (6 MSBs) */
    /* 174 */ CR( 6,  2,  F2, 0,  1,  0,  0),  /*  F2 (6 MSBs) */
    /* 175 */ CR( 5,  0,  IA, 0,  0,  0,  0),  /*  Ampl. Intr. */
    /* 176 */ CR( 5,  0,  IP, 0,  0,  0,  0),  /*  Per. Intr.  */
};

const int16_t DF_IDX[16 * 8] =
{
    /*  OPCODE 0000 */      -1, -1,     -1, -1,     -1, -1,     -1, -1,
    /*  OPCODE 1000 */      -1, -1,     -1, -1,     -1, -1,     -1, -1,
    /*  OPCODE 0100 */      17, 22,     17, 24,     25, 30,     25, 32,
    /*  OPCODE 1100 */      83, 94,     129,142,    97, 108,    145,158,
    /*  OPCODE 0010 */      83, 96,     129,144,    97, 110,    145,160,
    /*  OPCODE 1010 */      73, 77,     74, 77,     78, 82,     79, 82,
    /*  OPCODE 0110 */      33, 36,     34, 37,     38, 41,     39, 42,
    /*  OPCODE 1110 */      127,128,    127,128,    127,128,    127,128,
    /*  OPCODE 0001 */      1,  14,     1,  16,     1,  14,     1,  16,
    /*  OPCODE 1001 */      45, 56,     45, 58,     59, 70,     59, 72,
    /*  OPCODE 0101 */      161,166,    162,166,    169,174,    170,174,
    /*  OPCODE 1101 */      111,116,    111,118,    119,124,    119,126,
    /*  OPCODE 0011 */      161,168,    162,168,    169,176,    170,176,
    /*  OPCODE 1011 */      -1, -1,     -1, -1,     -1, -1,     -1, -1,
    /*  OPCODE 0111 */      -1, -1,     -1, -1,     -1, -1,     -1, -1,
    /*  OPCODE 1111 */      0,  0,      0,  0,      0,  0,      0,  0
};

// The SP0250 data sheet's coefficient quantisation
const int16_t QTBL[128] = {
    0,   9,   17,  25,  33,  41,  49,  57,  65,  73,  81,  89,  97,  105, 113, 121,
    129, 137, 145, 153, 161, 169, 177, 185, 193, 201, 209, 217, 225, 233, 241, 249,
    257, 265, 273, 281, 289, 297, 301, 305, 309, 313, 317, 321, 325, 329, 333, 337,
    341, 345, 349, 353, 357, 361, 365, 369, 373, 377, 381, 385, 389, 393, 397, 401,
    405, 409, 413, 417, 421, 425, 427, 429, 431, 433, 435, 437, 439, 441, 443, 445,
    447, 449, 451, 453, 455, 457, 459, 461, 463, 465, 467, 469, 471, 473, 475, 477,
    479, 481, 482, 483, 484, 485, 486, 487, 488, 489, 490, 491, 492, 493, 494, 495,
    496, 497, 498, 499, 500, 501, 502, 503, 504, 505, 506, 507, 508, 509, 510, 511,
};

uint32_t bitrev32(uint32_t v) {
    v = ((v & 0xFFFF0000u) >> 16) | ((v & 0x0000FFFFu) << 16);
    v = ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8);
    v = ((v & 0xF0F0F0F0u) >> 4) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v & 0xCCCCCCCCu) >> 2) | ((v & 0x33333333u) << 2);
    v = ((v & 0xAAAAAAAAu) >> 1) | ((v & 0x55555555u) << 1);
    return v;
}

int16_t limit(int s) { return (int16_t)(s > 8191 ? 8191 : s < -8192 ? -8192 : s); }

}  // namespace

// ---------------------------------------------------------------------------
// The filter
// ---------------------------------------------------------------------------
int Sp0256::Lpc12::update(int16_t& out) {
    // An impulse every period, or noise
    bool doInt = false;
    int samp = 0;
    if (per) {
        if (cnt <= 0) {
            cnt += (int)per;
            samp = amp;
            rpt--;
            doInt = interp != 0;
            for (auto& s : z) s[0] = s[1] = 0;
        } else {
            cnt--;
        }
    } else {
        if (--cnt <= 0) {
            doInt = interp != 0;
            cnt = PER_NOISE;
            rpt--;
            for (auto& s : z) s[0] = s[1] = 0;
        }
        const bool bit = rng & 1;
        rng = (rng >> 1) ^ (bit ? 0x4001 : 0);
        samp = bit ? amp : -amp;
    }
    // (MAME keeps the sample in 16 bits, wrapping)
    samp = (int16_t)(uint16_t)samp;
    if (doInt) {
        r[0] = (uint8_t)(r[0] + r[14]);
        r[1] = (uint8_t)(r[1] + r[15]);
        amp = (r[0] & 0x1F) << ((r[0] & 0xE0) >> 5);
        per = r[1];
    }
    if (rpt <= 0) return 0;
    // Six 2nd-order stages
    for (int j = 0; j < 6; ++j) {
        samp = (int16_t)(uint16_t)(samp + (((int)bCoef[j] * (int)z[j][1]) >> 9));
        samp = (int16_t)(uint16_t)(samp + (((int)fCoef[j] * (int)z[j][0]) >> 8));
        z[j][1] = z[j][0];
        z[j][0] = (int16_t)samp;
    }
    out = (int16_t)(limit(samp) << 2);
    return 1;
}

void Sp0256::Lpc12::regdec() {
    amp = (r[0] & 0x1F) << ((r[0] & 0xE0) >> 5);
    cnt = 0;
    per = r[1];
    auto iq = [](uint8_t x) -> int16_t { return (x & 0x80) ? QTBL[0x7F & (uint8_t)-x] : (int16_t)-QTBL[x]; };
    for (int i = 0; i < 6; ++i) {
        bCoef[i] = iq(r[2 + 2 * i]);
        fCoef[i] = iq(r[3 + 2 * i]);
    }
    interp = r[14] || r[15];
}

// ---------------------------------------------------------------------------
// The chip
// ---------------------------------------------------------------------------
Sp0256::Sp0256() : m_rom(0x10000, 0) {}

void Sp0256::setRom(const std::vector<uint8_t>& rom) {
    std::fill(m_rom.begin(), m_rom.end(), 0);
    for (size_t i = 0; i < rom.size() && 0x1000 + i < m_rom.size(); ++i) m_rom[0x1000 + i] = rom[i];
    m_hasRom = !rom.empty();
}

void Sp0256::reset() {
    m_filt = Lpc12();
    m_filt.rpt = -1;
    m_filt.rng = 1;
    m_halted = true;
    m_ald = 0;
    m_pc = 0;
    m_stack = 0;
    m_mode = 0;
    m_page = 0x1000 << 3;
    m_silent = true;
    m_out = 0;
    setLrq(true);
    m_sby = false;                  // (MAME: the line low, then raised: SBY's callback fires)
    setSby(true);
}

void Sp0256::setLrq(bool v) {
    if (m_lrq == v) return;
    m_lrq = v;
    if (onLrq) onLrq(v);
}

void Sp0256::setSby(bool v) {
    if (m_sby == v) return;
    m_sby = v;
    if (onSby) onSby(v);
}

void Sp0256::ald(uint8_t data) {
    if (!m_lrq) return;             // busy: dropped
    m_ald = (uint32_t)data << 4;    // x 2 bytes, in bits
    setLrq(false);
    setSby(false);
}

uint32_t Sp0256::getb(int len) {
    const uint32_t d0 = m_rom[(m_pc >> 3) & 0xFFFF];
    const uint32_t d1 = m_rom[((m_pc + 8) >> 3) & 0xFFFF];
    const uint32_t data = ((d1 << 8) | d0) >> (m_pc & 7);
    m_pc += (uint32_t)len;
    return data & ((1u << len) - 1);
}

// Run the microsequencer until the filter has a repeat count (or it halts)
void Sp0256::micro() {
    while (m_filt.rpt <= 0) {
        if (m_halted && !m_lrq) {
            // A command in the ALD buffer: start it
            m_pc = m_ald | (0x1000 << 3);
            m_halted = false;
            m_ald = 0;
            std::memset(m_filt.r, 0, sizeof m_filt.r);
            setLrq(true);
        }
        if (m_halted) {
            m_filt.rpt = 1;
            m_ald = 0;
            std::memset(m_filt.r, 0, sizeof m_filt.r);
            setSby(true);
            return;
        }
        const uint8_t immed4 = (uint8_t)getb(4);
        const uint8_t opcode = (uint8_t)getb(4);
        int repeat = 0;
        bool ctrlXfer = false;
        switch (opcode) {
            case 0x0:                               // RTS / SETPAGE
                if (immed4) {
                    m_page = bitrev32(immed4) >> 13;
                } else {
                    const uint32_t target = m_stack;
                    m_stack = 0;
                    if (!target) { m_halted = true; m_pc = 0; }
                    else m_pc = target;
                    ctrlXfer = true;
                }
                break;
            case 0xE:                               // JMP
            case 0xD: {                             // JSR
                const uint32_t target = m_page | (bitrev32(immed4) >> 17) | (bitrev32(getb(8)) >> 21);
                ctrlXfer = true;
                if (opcode == 0xD) m_stack = (m_pc + 7) & ~7u;
                m_pc = target;
                break;
            }
            case 0x1:                               // SETMODE
                m_mode = ((immed4 & 8) >> 2) | (immed4 & 4) | ((immed4 & 3) << 4);
                break;
            default:                                // the data blocks, PAUSE
                repeat = immed4 | (m_mode & 0x30);
                break;
        }
        if (opcode != 1) m_mode &= 0xF;
        if (ctrlXfer) continue;                     // (no FIFO: never the SPB640's address)
        if (!repeat) continue;

        m_filt.rpt = repeat + 1;
        int i = (opcode << 3) | (m_mode & 6);
        const int idx0 = DF_IDX[i++];
        const int idx1 = DF_IDX[i];
        if (idx0 < 0 || idx1 < idx0) continue;      // (no such block: as MAME's assert, skipped)
        for (i = idx0; i <= idx1; ++i) {
            const uint16_t cr = DATAFMT[i];
            const int len = cr & 15, shf = (cr >> 4) & 15, prm = (cr >> 8) & 15;
            if (cr & CR_CLRA) {
                std::memset(m_filt.r, 0, sizeof m_filt.r);
                m_silent = true;
            }
            if (cr & CR_CLR5) m_filt.r[B5] = m_filt.r[F5] = 0;
            if (!len) continue;
            int8_t value = (int8_t)getb(len);
            if ((cr & CR_DELTA) && (value & (1 << (len - 1)))) value = (int8_t)(value | ~((1 << len) - 1));
            if (shf) value = (int8_t)(value << shf);
            m_silent = false;
            if (cr & CR_FIELD) {
                m_filt.r[prm] &= (uint8_t)~(~0u << shf);
                m_filt.r[prm] |= (uint8_t)value;
                continue;
            }
            if (cr & CR_DELTA) { m_filt.r[prm] = (uint8_t)(m_filt.r[prm] + value); continue; }
            m_filt.r[prm] = (uint8_t)value;
        }
        if (opcode == 0xF) {                        // PAUSE: silence for its period
            m_silent = true;
            m_filt.r[1] = PER_PAUSE;
        }
        m_filt.regdec();
        break;
    }
}

int16_t Sp0256::sample() {
    // As MAME's stream update, a sample at a time
    for (int guard = 0; guard < 64; ++guard) {
        if (m_filt.rpt <= 0) micro();
        if (m_silent && m_filt.rpt <= 0) { m_out = 0; return m_out; }
        int16_t s = 0;
        if (m_filt.update(s)) { m_out = s; return m_out; }
    }
    m_out = 0;
    return m_out;
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------
std::vector<uint8_t> Sp0256::saveState() const {
    std::vector<uint8_t> v;
    auto put = [&v](uint32_t x, int n) { for (int i = 0; i < n; ++i) v.push_back((uint8_t)(x >> (8 * i))); };
    put((uint32_t)m_filt.rpt, 4); put((uint32_t)m_filt.cnt, 4); put(m_filt.per, 4); put(m_filt.rng, 4);
    put((uint32_t)m_filt.amp, 4); put((uint32_t)m_filt.interp, 1);
    for (int i = 0; i < 6; ++i) {
        put((uint16_t)m_filt.fCoef[i], 2); put((uint16_t)m_filt.bCoef[i], 2);
        put((uint16_t)m_filt.z[i][0], 2); put((uint16_t)m_filt.z[i][1], 2);
    }
    for (uint8_t r : m_filt.r) put(r, 1);
    put((m_silent ? 1u : 0u) | (m_lrq ? 2u : 0u) | (m_sby ? 4u : 0u) | (m_halted ? 8u : 0u), 1);
    put(m_ald, 4); put(m_pc, 4); put(m_stack, 4); put(m_page, 4); put((uint32_t)m_mode, 1);
    put((uint16_t)m_out, 2);
    return v;
}

void Sp0256::loadState(const std::vector<uint8_t>& s) {
    if (s.size() < 21 + 48 + 16 + 20) return;
    size_t p = 0;
    auto get = [&s, &p](int n) { uint32_t x = 0; for (int i = 0; i < n; ++i) x |= (uint32_t)s[p++] << (8 * i); return x; };
    m_filt.rpt = (int)get(4); m_filt.cnt = (int)get(4); m_filt.per = get(4); m_filt.rng = get(4);
    m_filt.amp = (int)get(4); m_filt.interp = (int)get(1);
    for (int i = 0; i < 6; ++i) {
        m_filt.fCoef[i] = (int16_t)get(2); m_filt.bCoef[i] = (int16_t)get(2);
        m_filt.z[i][0] = (int16_t)get(2); m_filt.z[i][1] = (int16_t)get(2);
    }
    for (auto& r : m_filt.r) r = (uint8_t)get(1);
    const uint32_t f = get(1);
    m_silent = f & 1; m_lrq = f & 2; m_sby = f & 4; m_halted = f & 8;
    m_ald = get(4); m_pc = get(4); m_stack = get(4); m_page = get(4); m_mode = (int)get(1);
    m_out = (int16_t)get(2);
}
