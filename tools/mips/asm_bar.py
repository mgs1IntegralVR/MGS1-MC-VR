# v13 LIFE-bar recorder (boss / Meryl / other LIFE bars on the wrist, as the GOG mod's menu_draw_bar hook).
# Every LIFE bar the game draws goes through menu_draw_bar(prim, x, y, rest, now, max, conf) (decomp menu/life.c; US 1.0 at
# 0x800411AC, 5 callers, all in the menu life code). Those 5 `jal menu_draw_bar` words are pointed at REC instead. REC uses
# only t registers and no stack, records the call by its MENU_BAR_CONF pointer, then tail-jumps to menu_draw_bar with every
# register and the stack exactly as the caller left them.
#
# Table at BAR = 0x80009FB0: +0x00 call counter (u32); 8 entries from +0x10, 0x1C each:
#   +0x00 conf  +0x04 rest  +0x08 now  +0x0C max  +0x10 counter at the last call  +0x14 x  +0x18 y
# A conf already in the table updates its entry; a new one takes the first empty entry, else entry (counter & 7).
#
# usage: asm_bar.py US10 [dis|bin|test|c]
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mipsasm import *
BAR = 0x80009FB0; NENT = 8; ESZ = 0x1C; REC = 0x8000A0C0
VERSIONS = {'US10': 0x800411AC}
VER = next((x for x in sys.argv[1:] if x in VERSIONS), 'US10'); DRAWBAR = VERSIONS[VER]
a = Asm(REC)
a.la('t0', BAR)
a.emit(lw('t1', 0, 't0'), NOP, addiu('t1', 't1', 1), sw('t1', 0, 't0'))       # counter
a.emit(lw('t2', 24, 'sp'))                                                       # conf (7th argument)
a.emit(addiu('t3', 't0', 0x10), addiu('t4', 'zero', NENT))
a.label('find')
a.emit(lw('t5', 0, 't3'), NOP)
a.br(4, 't5', 't2', 'put'); a.emit(NOP)
a.br(4, 't5', 'zero', 'put'); a.emit(NOP)
a.emit(addiu('t4', 't4', -1))
a.br(7, 't4', 'zero', 'find'); a.emit(addiu('t3', 't3', ESZ))                   # bgtz (delay slot: next entry)
a.emit(andi('t5', 't1', 7), sll('t6', 't5', 3), sll('t5', 't5', 2))            # full: entry (counter & 7): *0x1C = *8*4 - *4
a.emit(Rt('t6', 't5', 't6', 0, 0x23))                                            # subu t6, t6, t5  -> idx*28
a.emit(addu('t3', 't0', 't6'), addiu('t3', 't3', 0x10))
a.label('put')
a.emit(sw('t2', 0x00, 't3'), sw('a3', 0x04, 't3'), lw('t5', 16, 'sp'), lw('t6', 20, 'sp'), sw('t1', 0x10, 't3'))
a.emit(sw('t5', 0x08, 't3'), sw('t6', 0x0C, 't3'), sw('a1', 0x14, 't3'), sw('a2', 0x18, 't3'))
a.emit(j(DRAWBAR), NOP)
words = a.done(); END = a.end()
assert END <= 0x8000A400, hex(END)

if 'dis' in sys.argv[1:]:
    print('\n'.join(dis(words, REC))); print('%d words, ends %08X' % (len(words), END))
elif 'bin' in sys.argv[1:]:
    sys.stdout.buffer.write(a.code())
elif 'test' in sys.argv[1:]:
    from unicorn.mips_const import *
    def call(u, x, y, rest, now, mx, conf):
        SP = 0x801FFF00
        u.mem_write(P(SP + 16), struct.pack('<3I', now, mx, conf))
        u.reg_write(UC_MIPS_REG_SP, SP); u.reg_write(UC_MIPS_REG_RA, 0x80060000)
        for r, v in ((UC_MIPS_REG_A0, 0x800F0000), (UC_MIPS_REG_A1, x), (UC_MIPS_REG_A2, y), (UC_MIPS_REG_A3, rest),
                     (UC_MIPS_REG_S0, 0x5150), (UC_MIPS_REG_V0, 0x77)):
            u.reg_write(r, v)
        u.emu_start(REC, DRAWBAR, count=200)
        regs = [u.reg_read(r) for r in (UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_A3, UC_MIPS_REG_SP, UC_MIPS_REG_RA, UC_MIPS_REG_S0)]
        assert regs == [0x800F0000, x, y, rest, SP, 0x80060000, 0x5150], regs        # arrives at menu_draw_bar untouched
        assert struct.unpack('<3I', u.mem_read(P(SP + 16), 12)) == (now, mx, conf)
    def table(u): return [struct.unpack('<7I', u.mem_read(P(BAR + 0x10 + k * ESZ), ESZ)) for k in range(NENT)]
    u = emu(); u.mem_write(P(REC), a.code())
    call(u, 16, 16, 200, 256, 256, 0x80090010)     # Snake LIFE
    call(u, 16, 28, 900, 800, 1024, 0x80180000)    # a boss
    call(u, 16, 16, 200, 250, 256, 0x80090010)     # Snake LIFE again: same entry
    t = table(u)
    assert t[0] == (0x80090010, 200, 250, 256, 3, 16, 16) and t[1] == (0x80180000, 900, 800, 1024, 2, 16, 28), t[:2]
    print('two bars recorded by conf: %s | %s' % (['%X' % v for v in t[0]], ['%X' % v for v in t[1]]))
    for k in range(10): call(u, 1, 2, 3, 4, 5, 0x80100000 + k * 0x40)   # overflow: wraps by counter & 7, no crash
    t = table(u); assert all(e[0] for e in t)
    print('10 more confs: table full, all 8 entries valid; registers and stack untouched every call')
    print('ok, %d words, ends %08X' % (len(words), END))
else:
    print(c_array('BARCAVE_' + VER, words))
