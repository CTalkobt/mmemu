#!/usr/bin/env python3
"""
45GS02 opcode-by-opcode comparison: mmsim (rawMega65) vs xemu-xmega65.

Generates test programs that, for every opcode (plus the $42 $42 quad and $EA
32-bit-pointer prefixed forms), load a known machine state, execute exactly one
instruction, then capture A/X/Y/Z/B/SP/P plus the scratch memory regions the
instruction could touch.  Each program is run on both emulators and the
captured records are compared.

The program bytes are produced by a tiny built-in assembler (not ca45) so that
the harness code is exactly what is written here, with no optimizer rewriting.

Usage: opcode_compare.py [--workdir DIR] [--filter HEX,...] [--keep]
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

XMEGA65 = "/usr/local/bin/xemu-xmega65"
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MMSIM_CLI = os.path.join(REPO, "bin", "mmemu-cli")

# 65CE02/4510 opcode table (names + addressing modes), from xemu's generated
# cpu65ce02_disasm_tables.c.  Mode indices:
#  0 impl, 1 #nn, 2 #nnnn, 3 nn, 4 nn,rr, 5 nn,X, 6 nn,Y, 7 nnnn, 8 nnnn,X,
#  9 nnnn,Y, 10 rr, 11 rrrr, 12 (nn),Y, 13 (nn),Z, 14 (nn,SP),Y, 15 (nn,X),
# 16 (nnnn), 17 (nnnn,X), 18 A
NAMES = ("BRK ORA CLE SEE TSB ORA ASL RMB0 PHP ORA ASL TSY TSB ORA ASL BBR0 "
         "BPL ORA ORA BPL TRB ORA ASL RMB1 CLC ORA INC INZ TRB ORA ASL BBR1 "
         "JSR AND JSR JSR BIT AND ROL RMB2 PLP AND ROL TYS BIT AND ROL BBR2 "
         "BMI AND AND BMI BIT AND ROL RMB3 SEC AND DEC DEZ BIT AND ROL BBR3 "
         "RTI EOR NEG ASR ASR EOR LSR RMB4 PHA EOR LSR TAZ JMP EOR LSR BBR4 "
         "BVC EOR EOR BVC ASR EOR LSR RMB5 CLI EOR PHY TAB MAP EOR LSR BBR5 "
         "RTS ADC RTS BSR STZ ADC ROR RMB6 PLA ADC ROR TZA JMP ADC ROR BBR6 "
         "BVS ADC ADC BVS STZ ADC ROR RMB7 SEI ADC PLY TBA JMP ADC ROR BBR7 "
         "BRA STA STA BRA STY STA STX SMB0 DEY BIT TXA STY STY STA STX BBS0 "
         "BCC STA STA BCC STY STA STX SMB1 TYA STA TXS STX STZ STA STZ BBS1 "
         "LDY LDA LDX LDZ LDY LDA LDX SMB2 TAY LDA TAX LDZ LDY LDA LDX BBS2 "
         "BCS LDA LDA BCS LDY LDA LDX SMB3 CLV LDA TSX LDZ LDY LDA LDX BBS3 "
         "CPY CMP CPZ DEW CPY CMP DEC SMB4 INY CMP DEX ASW CPY CMP DEC BBS4 "
         "BNE CMP CMP BNE CPZ CMP DEC SMB5 CLD CMP PHX PHZ CPZ CMP DEC BBS5 "
         "CPX SBC LDA INW CPX SBC INC SMB6 INX SBC EOM ROW CPX SBC INC BBS6 "
         "BEQ SBC SBC BEQ PHW SBC INC SMB7 SED SBC PLX PLZ PHW SBC INC BBS7").split()
MODES = [0,15,0,0,3,3,3,3,0,1,18,0,7,7,7,4,10,12,13,11,3,5,5,3,0,9,0,0,7,8,8,4,
         7,15,16,17,3,3,3,3,0,1,18,0,7,7,7,4,10,12,13,11,5,5,5,3,0,9,0,0,8,8,8,4,
         0,15,0,0,3,3,3,3,0,1,18,0,7,7,7,4,10,12,13,11,5,5,5,3,0,9,0,0,0,8,8,4,
         0,15,1,11,3,3,3,3,0,1,18,0,16,7,7,4,10,12,13,11,5,5,5,3,0,9,0,0,17,8,8,4,
         10,15,14,11,3,3,3,3,0,1,0,8,7,7,7,4,10,12,13,11,5,5,6,3,0,9,0,9,7,8,8,4,
         1,15,1,1,3,3,3,3,0,1,0,7,7,7,7,4,10,12,13,11,5,5,6,3,0,9,0,8,8,8,9,4,
         1,15,1,3,3,3,3,3,0,1,0,7,7,7,7,4,10,12,13,11,3,5,5,3,0,9,0,0,7,8,8,4,
         1,15,14,3,3,3,3,3,0,1,0,7,7,7,7,4,10,12,13,11,2,5,5,3,0,9,0,0,7,8,8,4]
MODE_FMT = ["", "#$nn", "#$nnnn", "$nn", "$nn,$rr", "$nn,X", "$nn,Y", "$nnnn",
            "$nnnn,X", "$nnnn,Y", "$rr", "$rrrr", "($nn),Y", "($nn),Z",
            "($nn,SP),Y", "($nn,X)", "($nnnn)", "($nnnn,X)", "A"]

# Documented quad (Q register) forms: NEG NEG prefix + base opcode
QUAD_OPS = {
    "ORQ": [0x05, 0x0D, 0x12], "ANDQ": [0x25, 0x2D, 0x32], "EORQ": [0x45, 0x4D, 0x52],
    "ADCQ": [0x65, 0x6D, 0x72], "STQ": [0x85, 0x8D, 0x92], "LDQ": [0xA5, 0xAD, 0xB2],
    "CMPQ": [0xC5, 0xCD, 0xD2], "SBCQ": [0xE5, 0xED, 0xF2], "BITQ": [0x24, 0x2C],
    "ASLQ": [0x06, 0x0E, 0x0A, 0x16, 0x1E], "LSRQ": [0x46, 0x4E, 0x4A, 0x56, 0x5E],
    "ROLQ": [0x26, 0x2E, 0x2A, 0x36, 0x3E], "RORQ": [0x66, 0x6E, 0x6A, 0x76, 0x7E],
    "ASRQ": [0x44, 0x54, 0x43], "INQ": [0xE6, 0xEE, 0x1A, 0xF6, 0xFE],
    "DEQ": [0xC6, 0xCE, 0x3A, 0xD6, 0xDE],
}
FLAT_OPS = [0x12, 0x32, 0x52, 0x72, 0x92, 0xB2, 0xD2, 0xF2]   # EA prefix: [nn],Z

# Memory layout
ENTRY = 0x2020
RESULTS = 0x8000
REC_SIZE = 48
MAX_RECS = (0xCFF0 - RESULTS) // REC_SIZE
DONE_MARK = 0xCFF0
CAP_REGS = 0x0C00       # capture staging: A X Y Z B SPL SPH P
CONTVEC = 0x0C10        # BRK continuation vector
RESPTR = 0xF0           # zp pointer to next result record
SCRATCH = 0x0E00        # $0E00-$0E3F abs scratch
ZP_PTR = 0x80           # $80: 32-bit ptr -> $0E10, $84: ptr -> $0E18
ZP_DATA = 0x88          # $88-$8F zp data
ABS_DATA = 0x0E10
STACK_TMPL = 0x01E0     # $01E0-$01F7 filled from template

ZP_TEMPLATES = [
    [0x10, 0x0E, 0, 0, 0x18, 0x0E, 0, 0, 0xFF, 0x7F, 0x80, 0x00, 0x55, 0xAA, 0x01, 0xFE] +
    [(0x11 * i + 3) & 0xFF for i in range(16)],
    [0x10, 0x0E, 0, 0, 0x18, 0x0E, 0, 0, 0x00, 0xFF, 0x7F, 0x80, 0xAA, 0x55, 0xFE, 0x01] +
    [(0xEE - 0x11 * i) & 0xFF for i in range(16)],
]
ABS_TEMPLATES = [
    [(0x37 * i + 1) & 0xFF for i in range(16)] +
    [0xFF, 0xFF, 0xFF, 0x7F, 0x80, 0x00, 0x00, 0x00, 0x55, 0xAA, 0x01, 0xFE, 0x33, 0xCC, 0x99, 0x66] +
    [(0x5B * i + 7) & 0xFF for i in range(32)],
    [(0xC9 * i + 2) & 0xFF for i in range(16)] +
    [0x00, 0x00, 0x00, 0x80, 0x7F, 0xFF, 0xFF, 0xFF, 0xAA, 0x55, 0xFE, 0x01, 0xCC, 0x33, 0x66, 0x99] +
    [(0xA5 * i + 9) & 0xFF for i in range(32)],
]
# $01E0-$01F7; $01F1/$01F2 is the (d,SP),Y pointer (S=$F0, d=1) -> $0E10
STACK_TEMPLATE = [(0x3B * i + 0x41) & 0xFF for i in range(0x11)] + [0x10, 0x0E] + \
                 [(0x29 * i + 0x13) & 0xFF for i in range(5)]

# (template, A, X, Y, Z, P, imm)
BASE_VECTORS = [
    (0, 0x45, 0x04, 0x03, 0x02, 0x24, 0x3C),
    (1, 0xC3, 0x04, 0x03, 0x02, 0xE7, 0x81),
    (0, 0x99, 0x04, 0x03, 0x02, 0x2D, 0x27),   # decimal mode + carry
    (1, 0x80, 0x04, 0x03, 0x02, 0x65, 0xFF),
]
EXTRA_VECTORS = [
    (0, 0xFF, 0xFF, 0x80, 0x00, 0xAE, 0x00),
    (1, 0x7F, 0x00, 0x7F, 0xFF, 0x27, 0x80),
]
TYS_Y = [0x03, 0x05, 0x06, 0x07, 0x03, 0x05]   # keep relocated stack page clear of code/results


class Asm:
    """Minimal two-pass byte emitter with labels."""

    def __init__(self, org):
        self.org = org
        self.items = []
        self.labels = {}
        self.uniq = 0

    def label(self, name):
        self.items.append(("label", name))

    def newlabel(self, base):
        self.uniq += 1
        return f"{base}_{self.uniq}"

    def b(self, *vals):
        self.items.append(("bytes", vals))

    def lo(self, x): return lambda L: (L(x) if isinstance(x, str) else x) & 0xFF
    def hi(self, x): return lambda L: ((L(x) if isinstance(x, str) else x) >> 8) & 0xFF
    def w(self, x):
        self.b(self.lo(x), self.hi(x))

    def op_abs(self, opc, x):
        self.b(opc, self.lo(x), self.hi(x))

    def assemble(self):
        for pass_no in (1, 2):
            pc = self.org
            out = bytearray()
            def L(name):
                if pass_no == 1:
                    return self.labels.get(name, 0)
                return self.labels[name]
            for kind, val in self.items:
                if kind == "label":
                    self.labels[val] = pc
                else:
                    for v in val:
                        out.append((v(L) if callable(v) else v) & 0xFF)
                        pc += 1
        return bytes(out)

    @property
    def pc_estimate(self):
        return self.org + sum(len(v) for k, v in self.items if k == "bytes")


# Harness opcodes
LDA_I, LDX_I, LDY_I, LDZ_I = 0xA9, 0xA2, 0xA0, 0xA3
STA_A, STX_A, STY_A, STZ_A, LDA_A = 0x8D, 0x8E, 0x8C, 0x9C, 0xAD
SEI, CLD, SEE, TYS, TSY, TAB, TBA, TXS, TSX = 0x78, 0xD8, 0x03, 0x2B, 0x0B, 0x5B, 0x7B, 0x9A, 0xBA
PHA, PLA, PHP, PLP, RTS, RTI, CLC, INY, INX, DEX = 0x48, 0x68, 0x08, 0x28, 0x60, 0x40, 0x18, 0xC8, 0xE8, 0xCA
JSR, JMP, JMPI, BNE, BPL, BCC = 0x20, 0x4C, 0x6C, 0xD0, 0x10, 0x90
STA_IY, LDA_ZX, STA_ZX, LDA_AX, STA_AX, LDA_Z, STA_Z = 0x91, 0xB5, 0x95, 0xBD, 0x9D, 0xA5, 0x85
ADC_I, INC_Z, CPY_I, CPX_I, TAX, TAY, TAZ, MAP, EOM = 0x69, 0xE6, 0xC0, 0xE0, 0xAA, 0xA8, 0x4B, 0x5C, 0xEA


def rel8(a, target_label, from_label):
    a.b(lambda L: (L(target_label) - L(from_label)) & 0xFF)


def emit_runtime(a):
    """Init code, capture routine, setup routines, IRQ/NMI handlers."""
    a.label("start")
    a.b(SEI)
    a.b(LDA_I, 0x47); a.op_abs(STA_A, 0xD02F)
    a.b(LDA_I, 0x53); a.op_abs(STA_A, 0xD02F)
    a.b(LDA_I, 0x00, TAX, TAY, TAZ, MAP, EOM)
    a.b(LDA_I, 0x00); a.op_abs(STA_A, 0xD030)
    a.b(LDA_I, 0x35, STA_Z, 0x01)
    a.b(LDA_I, 0x7F); a.op_abs(STA_A, 0xDC0D); a.op_abs(STA_A, 0xDD0D)
    a.b(LDA_I, 0x00); a.op_abs(STA_A, 0xD01A)
    a.b(LDA_I, 0xFF); a.op_abs(STA_A, 0xD019)
    a.op_abs(LDA_A, 0xDC0D); a.op_abs(LDA_A, 0xDD0D)
    for vec, lbl in ((0xFFFE, "irq"), (0xFFFA, "nmi")):
        a.b(LDA_I, a.lo(lbl)); a.op_abs(STA_A, vec)
        a.b(LDA_I, a.hi(lbl)); a.op_abs(STA_A, vec + 1)
    a.b(LDA_I, 0x00, TAB)
    a.b(LDA_I, RESULTS & 0xFF, STA_Z, RESPTR)
    a.b(LDA_I, RESULTS >> 8, STA_Z, RESPTR + 1)
    a.op_abs(JMP, "tests")

    # capture: entered via JSR immediately after the instruction under test
    a.label("capture")
    a.b(PHP)
    a.op_abs(STA_A, CAP_REGS + 0)
    a.op_abs(STX_A, CAP_REGS + 1)
    a.op_abs(STY_A, CAP_REGS + 2)
    a.op_abs(STZ_A, CAP_REGS + 3)
    a.b(PLA); a.op_abs(STA_A, CAP_REGS + 7)
    a.b(TBA); a.op_abs(STA_A, CAP_REGS + 4)
    a.b(TSX); a.op_abs(STX_A, CAP_REGS + 5)
    a.b(TSY); a.op_abs(STY_A, CAP_REGS + 6)
    a.b(LDA_I, 0x00, TAB, CLD)
    a.b(LDY_I, 0x00)
    for src, n, idx_op in ((CAP_REGS, 8, LDA_AX), (ZP_PTR, 16, LDA_ZX),
                           (ABS_DATA, 16, LDA_AX), (0x01EC, 8, LDA_AX)):
        a.b(LDX_I, 0x00)
        lp = a.newlabel("cp")
        a.label(lp)
        if idx_op == LDA_ZX:
            a.b(LDA_ZX, src)
        else:
            a.op_abs(LDA_AX, src)
        a.b(STA_IY, RESPTR, INY, INX, CPX_I, n, BNE)
        after = a.newlabel("cpa")
        a.b(lambda L, lp=lp, after=after: (L(lp) - L(after)) & 0xFF)
        a.label(after)
    a.b(CLC, LDA_Z, RESPTR, ADC_I, REC_SIZE, STA_Z, RESPTR, BCC, 0x02, INC_Z, RESPTR + 1)
    a.b(RTS)

    # setupN: copy templates into scratch areas (called with SP=$FF)
    for t in (0, 1):
        a.label(f"setup{t}")
        for dst, n, tmpl, zp in ((ZP_PTR, 32, f"tzp{t}", True),
                                 (SCRATCH, 64, f"tabs{t}", False),
                                 (STACK_TMPL, len(STACK_TEMPLATE), "tstk", False)):
            a.b(LDX_I, n - 1)
            lp = a.newlabel("st")
            a.label(lp)
            a.op_abs(LDA_AX, tmpl)
            if zp:
                a.b(STA_ZX, dst)
            else:
                a.op_abs(STA_AX, dst)
            a.b(DEX, BPL)
            after = a.newlabel("sta")
            a.b(lambda L, lp=lp, after=after: (L(lp) - L(after)) & 0xFF)
            a.label(after)
        a.b(RTS)

    a.label("irq")
    a.op_abs(JSR, "capture")
    a.op_abs(JMPI, CONTVEC)
    a.label("nmi")
    a.b(RTI)

    for t in (0, 1):
        a.label(f"tzp{t}"); a.b(*ZP_TEMPLATES[t])
        a.label(f"tabs{t}"); a.b(*ABS_TEMPLATES[t])
    a.label("tstk"); a.b(*STACK_TEMPLATE)


def emit_prologue(a, vec, pre=None):
    tmpl, A, X, Y, Z, P, _ = vec
    a.b(SEI, SEE, LDY_I, 0x01, TYS, LDX_I, 0xFF, TXS)
    a.op_abs(JSR, f"setup{tmpl}")
    a.b(LDX_I, 0xF0, TXS)
    if pre:
        pre()
    a.b(LDX_I, X, LDY_I, Y, LDZ_I, Z, LDA_I, P, PHA, LDA_I, A, PLP)


def operand_bytes(mode, imm):
    """Operand bytes for a non-control-flow addressing mode."""
    return {
        0: [], 18: [], 1: [imm], 2: [0x34, 0x12],
        3: [ZP_DATA], 5: [ZP_DATA], 6: [ZP_DATA],
        7: [ABS_DATA & 0xFF, ABS_DATA >> 8], 8: [ABS_DATA & 0xFF, ABS_DATA >> 8],
        9: [ABS_DATA & 0xFF, ABS_DATA >> 8],
        12: [ZP_PTR], 13: [ZP_PTR], 14: [0x01], 15: [ZP_PTR],
    }[mode]


def emit_test(a, t):
    """Emit one test. t = dict(kind, opc, prefix, vec)."""
    kind, opc, prefix, vec = t["kind"], t["opc"], t["prefix"], t["vec"]
    mode = MODES[opc]
    imm = vec[6]

    if kind == "brk":
        nxt = a.newlabel("brknext")
        def pre():
            a.b(LDA_I, a.lo(nxt)); a.op_abs(STA_A, CONTVEC)
            a.b(LDA_I, a.hi(nxt)); a.op_abs(STA_A, CONTVEC + 1)
        emit_prologue(a, vec, pre)
        a.b(0x00, 0x00)
        a.label(nxt)
        return

    if kind in ("rts", "rtn", "rti"):
        tgt = a.newlabel("rtgt")
        def pre():
            if kind == "rti":
                a.b(LDA_I, a.hi(tgt), PHA, LDA_I, a.lo(tgt), PHA, LDA_I, 0xA5, PHA)
            else:
                a.b(LDA_I, lambda L: ((L(tgt) - 1) >> 8) & 0xFF, PHA,
                    LDA_I, lambda L: (L(tgt) - 1) & 0xFF, PHA)
        emit_prologue(a, vec, pre)
        a.b(*({"rts": [0x60], "rtn": [0x62, 0x02], "rti": [0x40]}[kind]))
        a.b(LDY_I, 0xEE)
        a.label(tgt)
        a.op_abs(JSR, "capture")
        return

    emit_prologue(a, vec)

    if kind == "branch":
        # Not taken -> LDY #$EE; INY.  Taken with offset base op+2 (8-bit std /
        # 65CE02 16-bit) -> INY only; base op+3 -> skip both.
        if mode == 4:
            a.b(opc, ZP_DATA, 0x03)
        elif mode == 10:
            a.b(opc, 0x03)
        else:
            a.b(opc, 0x03, 0x00)
        a.b(LDY_I, 0xEE, INY)
        a.op_abs(JSR, "capture")
        return

    if kind == "jump":
        tgt = a.newlabel("jtgt")
        if mode == 7:
            a.b(opc, a.lo(tgt), a.hi(tgt), LDY_I, 0xEE)
            a.label(tgt)
            a.op_abs(JSR, "capture")
            return
        vec_l, nxt = a.newlabel("jvec"), a.newlabel("jnext")
        a.b(opc, a.lo(vec_l), a.hi(vec_l), LDY_I, 0xEE)
        a.label(tgt)
        a.op_abs(JSR, "capture")
        a.op_abs(JMP, nxt)
        a.label(vec_l)
        if mode == 17:  # (abs,X) with X=4
            a.b(0xDE, 0xAD, 0xBE, 0xEF)
        a.w(tgt)
        a.label(nxt)
        return

    a.b(*prefix, opc, *operand_bytes(mode, imm))
    if opc == MAP and not prefix:
        a.b(EOM)
    a.op_abs(JSR, "capture")


def build_tests():
    tests = []

    def add(kind, opc, name, vectors, prefix=()):
        for i, v in enumerate(vectors):
            tests.append(dict(kind=kind, opc=opc, prefix=tuple(prefix), vec=v,
                              name=name, vidx=i))

    for opc in range(256):
        name, mode = NAMES[opc], MODES[opc]
        disp = f"{name} {MODE_FMT[mode]}".strip()
        if opc == 0x00:
            add("brk", opc, disp, BASE_VECTORS)
        elif opc == 0x40:
            add("rti", opc, disp, BASE_VECTORS)
        elif opc == 0x60:
            add("rts", opc, disp, BASE_VECTORS)
        elif opc == 0x62:
            add("rtn", opc, "RTS #$02", BASE_VECTORS)
        elif opc == MAP:
            add("plain", opc, "MAP (A=X=Y=Z=0) ; EOM", [(0, 0, 0, 0, 0, 0x24, 0)])
        elif mode in (4, 10, 11):
            add("branch", opc, disp, BASE_VECTORS)
        elif opc in (0x20, 0x22, 0x23, 0x4C, 0x6C, 0x7C):
            add("jump", opc, disp, BASE_VECTORS)
        elif opc == TYS:
            add("plain", opc, disp, [v[:2] + (v[2], TYS_Y[i]) + v[4:]
                                     for i, v in enumerate(BASE_VECTORS + EXTRA_VECTORS)])
        elif mode in (0, 18):
            add("plain", opc, disp, BASE_VECTORS + EXTRA_VECTORS)
        else:
            add("plain", opc, disp, BASE_VECTORS)

    for qname, opcs in QUAD_OPS.items():
        for opc in opcs:
            mode = MODES[opc]
            fmt = {3: "$nn", 7: "$nnnn", 13: "($nn)", 18: "Q", 5: "$nn,X", 8: "$nnnn,X", 0: "Q"}[mode]
            vecs = BASE_VECTORS if mode in (5, 8, 13) else BASE_VECTORS + EXTRA_VECTORS
            add("plain", opc, f"{qname} {fmt}", vecs, prefix=(0x42, 0x42))
            if mode == 13:
                add("plain", opc, f"{qname} [$nn]", BASE_VECTORS, prefix=(0x42, 0x42, 0xEA))

    # No quad immediates in gs4510.vhdl: NEG NEG + immediate runs as NEG, NEG, 8-bit op
    for opc in (0x09, 0x29, 0x49, 0x69, 0xA9, 0xC9, 0xE9):
        add("plain", opc, f"NEG NEG ; {NAMES[opc]} #$nn", BASE_VECTORS + EXTRA_VECTORS,
            prefix=(0x42, 0x42))

    for opc in FLAT_OPS:
        add("plain", opc, f"{NAMES[opc]} [$nn],Z", BASE_VECTORS, prefix=(0xEA,))
    return tests


def build_program(tests):
    a = Asm(0x2001)
    # BASIC stub: 10 SYS 8224
    a.w("bnext"); a.w(10); a.b(0x9E, *b"8224", 0)
    a.label("bnext"); a.w(0)
    while a.pc_estimate < ENTRY:
        a.b(0)
    emit_runtime(a)
    a.label("tests")
    for t in tests:
        emit_test(a, t)
    a.b(LDA_I, 0x00, TAB)
    for i, v in enumerate(b"DONE"):
        a.b(LDA_I, v); a.op_abs(STA_A, DONE_MARK + i)
    a.b(LDA_I, len(tests) & 0xFF); a.op_abs(STA_A, DONE_MARK + 4)
    a.b(LDA_I, len(tests) >> 8); a.op_abs(STA_A, DONE_MARK + 5)
    a.b(LDA_I, 0x42); a.op_abs(STA_A, 0xD6CF)
    a.label("hang")
    a.op_abs(JMP, "hang")
    code = a.assemble()
    end = 0x2001 + len(code)
    if end > RESULTS:
        raise RuntimeError(f"program too large: ends at ${end:04X}")
    return bytes([0x01, 0x20]) + code, a.labels


def run_mmsim(prg, workdir, tag):
    dump = os.path.join(workdir, f"mmsim_{tag}.bin")
    if os.path.exists(dump):
        os.remove(dump)
    script = (f"create rawMega65\nbrktrap off\nload {prg}\nrun ${ENTRY:04X}\n"
              f"save {dump} ${RESULTS:04X} {DONE_MARK + 16 - RESULTS}\nquit\n")
    r = subprocess.run([MMSIM_CLI], input=script.encode(), cwd=REPO, timeout=120,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    with open(os.path.join(workdir, f"mmsim_{tag}.log"), "wb") as f:
        f.write(r.stdout)
    if not os.path.exists(dump):
        return None
    with open(dump, "rb") as f:
        return f.read()


def run_xemu(prg, workdir, tag, timeout=90):
    dump = os.path.join(workdir, f"xemu_{tag}.bin")
    if os.path.exists(dump):
        os.remove(dump)
    env = os.environ.copy()
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    env["XEMU_NO_DIALOGS"] = "1"
    cmd = [XMEGA65, "-headless", "-sleepless", "-testing", "-fastboot", "-besure",
           "-prg", prg, "-dumpmem", dump]
    with open(os.path.join(workdir, f"xemu_{tag}.log"), "wb") as log:
        try:
            subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
        except subprocess.TimeoutExpired:
            return None
    if not os.path.exists(dump):
        return None
    with open(dump, "rb") as f:
        data = f.read()
    return data[RESULTS:DONE_MARK + 16]


REG_NAMES = ["A", "X", "Y", "Z", "B", "SPL", "SPH", "P"]

# Forms where xemu deviates from gs4510.vhdl (the reference); mmsim follows the VHDL.
# Keyed by (prefix, opcode).
KNOWN_XEMU_DEVIATIONS = {
    ((), 0x02): "CLE: VHDL pushes P with E=1 always (mcStoreP); xemu pushes the real E, "
                "visible via the harness's PHP capture",
}


def normalize(rec):
    """Mask the B bit in bytes produced by the harness's own PHP (captured P and
    the stack byte PHP wrote at SPH:SPL).  The pushed B bit is checked where it
    matters: in the PHP/BRK tests' own pushes."""
    if rec is None:
        return None
    r = bytearray(rec)
    r[7] &= 0xEF
    addr = (r[6] << 8) | r[5]
    if 0x01EC <= addr <= 0x01F3:
        r[40 + addr - 0x01EC] &= 0xEF
    return bytes(r)


def describe_diff(t, ra, rb):
    """Return list of human-readable field diffs between two records."""
    out = []
    for i in range(8):
        if ra[i] != rb[i]:
            if i == 7:
                out.append(f"P mm={ra[i]:02X}({flags(ra[i])}) xemu={rb[i]:02X}({flags(rb[i])})")
            else:
                out.append(f"{REG_NAMES[i]} mm={ra[i]:02X} xemu={rb[i]:02X}")
    regions = ((8, 16, ZP_PTR), (24, 16, ABS_DATA), (40, 8, 0x01EC))
    for off, n, base in regions:
        for j in range(n):
            if ra[off + j] != rb[off + j]:
                out.append(f"${base + j:04X} mm={ra[off + j]:02X} xemu={rb[off + j]:02X}")
    return out


def flags(p):
    return "".join(c if p & (0x80 >> i) else "." for i, c in enumerate("NVEBDIZC"))


def fmt_bytes(t):
    return " ".join(f"{b:02X}" for b in (*t["prefix"], t["opc"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--workdir", default=None)
    ap.add_argument("--filter", default=None, help="comma-separated base opcodes (hex) to test")
    ap.add_argument("--batch", type=int, default=380)
    ap.add_argument("--report", default=None, help="write full report to this file")
    args = ap.parse_args()

    workdir = args.workdir or tempfile.mkdtemp(prefix="opcmp_")
    os.makedirs(workdir, exist_ok=True)
    tests = build_tests()
    if args.filter:
        want = {int(x, 16) for x in args.filter.split(",")}
        tests = [t for t in tests if t["opc"] in want]

    batches = [tests[i:i + args.batch] for i in range(0, len(tests), args.batch)]
    assert all(len(b) <= MAX_RECS for b in batches)
    print(f"{len(tests)} test vectors in {len(batches)} batch(es); workdir {workdir}")

    results = []   # (test, mm_rec, xemu_rec)
    for bi, batch in enumerate(batches):
        prg_bytes, labels = build_program(batch)
        prg = os.path.join(workdir, f"batch{bi}.prg")
        with open(prg, "wb") as f:
            f.write(prg_bytes)
        t0 = time.time()
        mm = run_mmsim(prg, workdir, bi)
        t1 = time.time()
        xe = run_xemu(prg, workdir, bi)
        t2 = time.time()
        def done(d):
            if d is None:
                return "no dump"
            m = d[DONE_MARK - RESULTS:DONE_MARK - RESULTS + 6]
            return "ok" if m[:4] == b"DONE" and m[4] | (m[5] << 8) == len(batch) else "INCOMPLETE"
        print(f"batch {bi}: {len(batch)} tests, end=${0x1FFF + len(prg_bytes):04X}  "
              f"mmsim {done(mm)} ({t1 - t0:.1f}s)  xemu {done(xe)} ({t2 - t1:.1f}s)")
        for i, t in enumerate(batch):
            off = i * REC_SIZE
            ra = normalize(mm[off:off + REC_SIZE]) if mm else None
            rb = normalize(xe[off:off + REC_SIZE]) if xe else None
            results.append((t, ra, rb))

    # Group by instruction form
    groups = {}
    for t, ra, rb in results:
        key = (t["prefix"], t["opc"])
        groups.setdefault(key, []).append((t, ra, rb))

    lines = []
    n_pass = n_fail = n_known = 0
    for key in sorted(groups, key=lambda k: (len(k[0]), k[0], k[1])):
        entries = groups[key]
        t0 = entries[0][0]
        fails = []
        for t, ra, rb in entries:
            if ra is None or rb is None:
                fails.append((t, ["missing record"], ra, rb))
            elif ra != rb:
                fails.append((t, describe_diff(t, ra, rb), ra, rb))
        label = f"{fmt_bytes(t0):9s} {t0['name']}"
        if fails and key in KNOWN_XEMU_DEVIATIONS:
            n_known += 1
            lines.append(f"xemu {label}  (known xemu deviation: {KNOWN_XEMU_DEVIATIONS[key]})")
        elif fails:
            n_fail += 1
            lines.append(f"FAIL {label}  ({len(fails)}/{len(entries)} vectors)")
            for t, d, ra, rb in fails:
                v = t["vec"]
                lines.append(f"       v{t['vidx']} in: A={v[1]:02X} X={v[2]:02X} Y={v[3]:02X} "
                             f"Z={v[4]:02X} P={v[5]:02X} tmpl={v[0]} imm={v[6]:02X}")
                lines.append("          " + "; ".join(d))
        else:
            n_pass += 1
            lines.append(f"ok   {label}")

    summary = (f"\n{n_pass} instruction forms match, {n_fail} differ, "
               f"{n_known} known xemu deviations (of {n_pass + n_fail + n_known})")
    report = "\n".join(lines) + summary + "\n"
    if args.report:
        with open(args.report, "w") as f:
            f.write(report)
    print("\n".join(l for l in lines if not l.startswith("ok")))
    print(summary)
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
