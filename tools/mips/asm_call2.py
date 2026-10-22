# v13 multi-slot "call a game function for the DLL" routine. Replaces v11's single-slot asm_call.py (0x8000ACB0): the pad
# routine's exit now jals CODE2 = 0x8000B100. Every game frame it runs, IN ORDER, each of the 10 slots whose request number
# changed -- so a line test and the getters that read its scratchpad results (HZD_GetOnlinePoint / ...Atr) run back to back
# in the same frame. Same words for PAL and US (no game addresses inside).
#
# Slots at SLOTS = 0x8000B300, 10 x 0x28 (ends 0x8000B490):
#   +0x00 request seq (u32, DLL writes LAST)  +0x04 consumed seq (MIPS, set before the call: a call never runs twice)
#   +0x08 fn (0 = nothing)  +0x0C a0  +0x10 a1  +0x14 a2  +0x18 a3  +0x1C 5th argument (passed at 16(sp))
#   +0x20 v0 (MIPS)  +0x24 result seq (MIPS: = request seq once v0 is stored)
# Alive counter at ALIVE = 0x8000B2FC (++ every frame).
#
# usage: asm_call2.py [dis|bin|test|c]
import struct, sys
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mipsasm import *
CODE2 = 0x8000B100; SLOTS = 0x8000B300; NSLOT = 10; SLOTSZ = 0x28; ALIVE = SLOTS - 4
a = Asm(CODE2)
a.emit(addiu('sp', 'sp', -40), sw('ra', 36, 'sp'), sw('s0', 32, 'sp'), sw('s1', 28, 'sp'))
a.la('t0', ALIVE); a.emit(lw('t1', 0, 't0'), NOP, addiu('t1', 't1', 1), sw('t1', 0, 't0'))
a.la('s0', SLOTS); a.emit(addiu('s1', 'zero', NSLOT))
a.label('loop')
a.emit(lw('t1', 0x00, 's0'), lw('t2', 0x04, 's0'), NOP)
a.br(4, 't1', 't2', 'next'); a.emit(NOP)                       # nothing new in this slot
a.emit(sw('t1', 0x04, 's0'))                                     # consumed
a.emit(lw('t9', 0x08, 's0'), NOP)
a.br(4, 't9', 'zero', 'next'); a.emit(NOP)
a.emit(lw('t3', 0x1C, 's0'), NOP, sw('t3', 16, 'sp'))           # 5th argument
a.emit(lw('a0', 0x0C, 's0'), lw('a1', 0x10, 's0'), lw('a2', 0x14, 's0'), lw('a3', 0x18, 's0'))
a.emit(jalr('t9'), NOP)
a.emit(sw('v0', 0x20, 's0'), lw('t1', 0x04, 's0'), NOP, sw('t1', 0x24, 's0'))
a.label('next')
a.emit(addiu('s1', 's1', -1), addiu('s0', 's0', SLOTSZ))
a.br(7, 's1', 'zero', 'loop'); a.emit(NOP)                       # bgtz
a.emit(lw('ra', 36, 'sp'), lw('s0', 32, 'sp'), lw('s1', 28, 'sp'), addiu('sp', 'sp', 40), jr('ra'), NOP)
words = a.done(); END = a.end()
assert END <= ALIVE, hex(END)

if 'dis' in sys.argv[1:]:
    print('\n'.join(dis(words, CODE2))); print('%d words, ends %08X; slots %08X-%08X' % (len(words), END, SLOTS, SLOTS + NSLOT * SLOTSZ))
elif 'bin' in sys.argv[1:]:
    sys.stdout.buffer.write(a.code())
elif 'test' in sys.argv[1:]:
    from unicorn.mips_const import UC_MIPS_REG_SP, UC_MIPS_REG_RA, UC_MIPS_REG_S0, UC_MIPS_REG_S1
    FN = 0x80050000   # v0 = a0 + 2*a1 + 3*a2 + 4*a3 + 5*arg5 + 1000*(calls so far, from a counter at 0x80050100)
    fn = [0x8FA80010, 0x00851021, 0x00A21021, 0x00461021, 0x00461021, 0x00461021,
          0x00471021, 0x00471021, 0x00471021, 0x00471021, 0x00481021, 0x00481021, 0x00481021, 0x00481021, 0x00481021,
          0x03E00008, 0]
    def run(slots, frames=1):
        u = emu(); u.mem_write(P(CODE2), a.code()); u.mem_write(P(FN), b''.join(struct.pack('<I', w) for w in fn))
        for k, s in enumerate(slots):
            u.mem_write(P(SLOTS + k * SLOTSZ), struct.pack('<8I', *s) + b'\0' * 8)
        RET = 0x80060000; u.mem_write(P(RET), b'\0' * 8)
        for f in range(frames):
            u.reg_write(UC_MIPS_REG_SP, 0x801FFF00); u.reg_write(UC_MIPS_REG_RA, RET)
            u.reg_write(UC_MIPS_REG_S0, 0x1111); u.reg_write(UC_MIPS_REG_S1, 0x2222)
            u.emu_start(CODE2, RET, count=2000)
            assert u.reg_read(UC_MIPS_REG_SP) == 0x801FFF00 and u.reg_read(UC_MIPS_REG_S0) == 0x1111 and u.reg_read(UC_MIPS_REG_S1) == 0x2222
        out = [struct.unpack('<10I', u.mem_read(P(SLOTS + k * SLOTSZ), SLOTSZ)) for k in range(NSLOT)]
        return out, struct.unpack('<I', u.mem_read(P(ALIVE), 4))[0]
    o, al = run([(1, 0, FN, 1, 2, 3, 4, 6), (0, 0, FN, 9, 9, 9, 9, 9), (3, 2, FN, 2, 0, 0, 0, 0), (5, 5, FN, 1, 1, 1, 1, 1)] + [(0,) * 8] * 6)
    assert o[0][8] == 60 and o[0][9] == 1 and o[0][1] == 1, o[0]
    assert o[1][8] == 0 and o[1][9] == 0, o[1]          # req == consumed -> skipped
    assert o[2][8] == 2 and o[2][9] == 3, o[2]
    assert o[3][8] == 0, o[3]
    print('frame 1: slot0 v0 %d (expect 60), slot1 skipped, slot2 v0 %d (expect 2), slot3 skipped, alive %d' % (o[0][8], o[2][8], al))
    o, al = run([(1, 0, FN, 1, 2, 3, 4, 6)] + [(0,) * 8] * 9, frames=3)
    assert o[0][8] == 60 and al == 3
    print('3 frames: the call ran once, alive %d' % al)
    o, al = run([(k + 1, 0, FN, k, 0, 0, 0, 0) for k in range(10)])
    assert [x[8] for x in o] == list(range(10)), [x[8] for x in o]
    print('10 slots in one frame: v0 %s' % [x[8] for x in o])
    print('ok, %d words, ends %08X' % (len(words), END))
else:
    print(c_array('CALLCAVE2', words))
