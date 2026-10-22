# Tiny MIPS I assembler helpers shared by the v13 routine generators (asm_call2.py, asm_bar.py, asm_hands.py).
import struct
R = {n: i for i, n in enumerate("zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra".split())}
def I(op, rs, rt, imm): return (op << 26) | (R[rs] << 21) | (R[rt] << 16) | (imm & 0xFFFF)
def Rt(rs, rt, rd, sh, fn): return (R[rs] << 21) | (R[rt] << 16) | (R[rd] << 11) | (sh << 6) | fn
lui   = lambda rt, imm: I(0xF, 'zero', rt, imm)
addiu = lambda rt, rs, imm: I(0x9, rs, rt, imm)
slti  = lambda rt, rs, imm: I(0xA, rs, rt, imm)
andi  = lambda rt, rs, imm: I(0xC, rs, rt, imm)
lw    = lambda rt, off, b: I(0x23, b, rt, off)
sw    = lambda rt, off, b: I(0x2B, b, rt, off)
lhu   = lambda rt, off, b: I(0x25, b, rt, off)
lbu   = lambda rt, off, b: I(0x24, b, rt, off)
addu  = lambda d, a, b: Rt(a, b, d, 0, 0x21)
slt   = lambda d, a, b: Rt(a, b, d, 0, 0x2A)
sll   = lambda d, t, sh: Rt('zero', t, d, sh, 0x00)
move  = lambda d, s: addu(d, s, 'zero')
jr    = lambda rs: Rt(rs, 'zero', 'zero', 0, 0x08)
jalr  = lambda rs: Rt(rs, 'zero', 'ra', 0, 0x09)
jal   = lambda a: 0x0C000000 | ((a >> 2) & 0x3FFFFFF)
j     = lambda a: 0x08000000 | ((a >> 2) & 0x3FFFFFF)
NOP = 0
def hi(a): return ((a + 0x8000) >> 16) & 0xFFFF
def lo(a): return a & 0xFFFF

class Asm:
    def __init__(self, base): self.base = base; self.c = []; self.labels = {}; self.fix = []
    def emit(self, *ws): self.c.extend(ws)
    def label(self, n): assert n not in self.labels, n; self.labels[n] = len(self.c)
    def br(self, op, rs, rt, lab): self.fix.append((len(self.c), op, rs, rt, lab)); self.c.append(0)   # beq 4 bne 5 blez 6 bgtz 7
    def b(self, lab): self.br(4, 'zero', 'zero', lab)
    def la(self, r, a): self.emit(lui(r, hi(a)), addiu(r, r, lo(a)))
    def done(self):
        for i, op, rs, rt, lab in self.fix: self.c[i] = I(op, rs, rt, self.labels[lab] - i - 1)
        return self.c
    def code(self): return b''.join(struct.pack('<I', w) for w in self.c)
    def end(self): return self.base + 4 * len(self.c)

def dis(words, base):
    from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 + CS_MODE_LITTLE_ENDIAN)
    code = b''.join(struct.pack('<I', w) for w in words)
    return ["%08x: %s %s" % (x.address, x.mnemonic, x.op_str) for x in md.disasm(code, base)]

def c_array(name, words):
    out = ["static const uint32_t %s[%d] = {" % (name, len(words))]
    for k in range(0, len(words), 6): out.append("    " + ", ".join("0x%08X" % w for w in words[k:k + 6]) + ",")
    out.append("};")
    return "\n".join(out)

def emu(ram_bytes=None):
    from unicorn import Uc, UC_ARCH_MIPS, UC_MODE_MIPS32, UC_MODE_LITTLE_ENDIAN
    u = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 + UC_MODE_LITTLE_ENDIAN); u.mem_map(0, 0x200000)
    if ram_bytes: u.mem_write(0, ram_bytes)
    return u
P = lambda a: a & 0x1FFFFFFF
