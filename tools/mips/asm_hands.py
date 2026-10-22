# v13 FLOATING HANDS (GOG design, motion_aim.cpp "Snake's OWN hands, drawn by the game at your controllers").
# DG_ScreenObjs (US 0x8001D88C) builds every part's world matrix (DG_ApplyRots) and then `jal DG_ScreenModels` (US site
# 0x8001D998 -> 0x8001CEC4, a0 = objs, a1 = n_models in the delay slot) turns them into screen matrices. That jal is pointed
# at HANDS. For Snake's body only (objs == HMB+4, enabled): every part's world is saved, the right / left hand parts get the
# controller matrices, every other part collapses to a point inside its hand (zero rotation draws nothing), DG_ScreenModels
# runs, and the saved worlds are put back -- so the eye (head joint 6) and everything else that reads them see the
# animation as before. Any other object goes straight to DG_ScreenModels.
#
# HMB = 0x800075E0 (spare block, empty on every stage watched):
#   +0x00 enable (u32)  +0x04 body DG_OBJS (u32)  +0x08 right hand part (u16)  +0x0A left hand part (u16)
#   +0x0C alive counter (u32, every DG_ScreenModels call)  +0x10 applied counter (u32, Snake's body redrawn as hands)
#   +0x20 right hand MATRIX  +0x40 left hand MATRIX  +0x60 side[32] (0 collapse into the right hand, 1 into the left, 2 keep)
#   +0x80 (unused since v14; v13 saved obj->world here)
# Code at HCODE = 0x80007A60.
#
# usage: asm_hands.py US10 [dis|bin|test|c]
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mipsasm import *
HMB = 0x800075E0; HCODE = 0x80007A60; MAXP = 32
VERSIONS = {'US10': 0x8001CEC4}
VER = next((x for x in sys.argv[1:] if x in VERSIONS), 'US10'); SCREENMODELS = VERSIONS[VER]
a = Asm(HCODE)
a.emit(addiu('sp', 'sp', -40), sw('ra', 36, 'sp'), sw('s0', 32, 'sp'), sw('s1', 28, 'sp'), sw('s2', 24, 'sp'),
       sw('s3', 20, 'sp'), sw('s4', 16, 'sp'))
a.emit(move('s0', 'a0'), move('s1', 'a1'))
a.la('s2', HMB)
a.emit(lw('t0', 0x0C, 's2'), NOP, addiu('t0', 't0', 1), sw('t0', 0x0C, 's2'))         # alive
a.emit(lw('t0', 0x00, 's2'), NOP); a.br(4, 't0', 'zero', 'plain'); a.emit(NOP)          # off
a.emit(lw('t0', 0x04, 's2'), NOP); a.br(5, 't0', 's0', 'plain'); a.emit(NOP)            # not Snake's body
a.br(6, 's1', 'zero', 'plain'); a.emit(NOP)                                             # blez n
a.emit(slti('t0', 's1', 25)); a.br(4, 't0', 'zero', 'plain'); a.emit(NOP)              # n > 24 (TEMP holds 24)
a.emit(lw('t0', 0x10, 's2'), NOP, addiu('t0', 't0', 1), sw('t0', 0x10, 's2'))         # applied
# ---- v14: DG_ScreenModels builds each part's screen matrix from the scratchpad copy TEMP[i] (0x1F800040 + i*0x20, written by
# DG_ApplyRots just before) -- never from obj->world (v13 overwrote obj->world and so changed nothing on screen). So the hands
# go into TEMP; obj->world (the animation, which the eye reads from joint 6) is never touched and needs no restore.
a.emit(move('s3', 'zero'), lui('s4', 0x1F80), addiu('s4', 's4', 0x40))
a.label('l1')
a.emit(lhu('t0', 0x08, 's2'), NOP); a.br(4, 't0', 's3', 'useR'); a.emit(NOP)
a.emit(lhu('t0', 0x0A, 's2'), NOP); a.br(4, 't0', 's3', 'useL'); a.emit(NOP)
a.emit(addu('t1', 's2', 's3'), lbu('t1', 0x60, 't1'), addiu('t2', 'zero', 2), NOP)
a.br(4, 't1', 't2', 'n1'); a.emit(NOP)                                                  # keep
a.emit(addiu('t8', 's2', 0x20))                                                         # collapse into the right hand...
a.br(4, 't1', 'zero', 'col'); a.emit(NOP)
a.emit(addiu('t8', 's2', 0x40))                                                         # ...or the left
a.label('col')
a.emit(sw('zero', 0x00, 's4'), sw('zero', 0x04, 's4'), sw('zero', 0x08, 's4'), sw('zero', 0x0C, 's4'), sw('zero', 0x10, 's4'))
a.emit(lw('t0', 0x14, 't8'), lw('t1', 0x18, 't8'), lw('t2', 0x1C, 't8'), sw('t0', 0x14, 's4'), sw('t1', 0x18, 's4'), sw('t2', 0x1C, 's4'))
a.b('n1'); a.emit(NOP)
for nm, off in (('useR', 0x20), ('useL', 0x40)):
    a.label(nm)
    a.emit(*[lw(r, off + 4 * k, 's2') for k, r in enumerate(('t0', 't1', 't2', 't3', 't4', 't5', 't6', 't7'))])
    a.emit(*[sw(r, 4 * k, 's4') for k, r in enumerate(('t0', 't1', 't2', 't3', 't4', 't5', 't6', 't7'))])
    if nm == 'useR': a.b('n1'); a.emit(NOP)
a.label('n1')
a.emit(addiu('s3', 's3', 1), addiu('s4', 's4', 0x20), slt('t0', 's3', 's1'))
a.br(5, 't0', 'zero', 'l1'); a.emit(NOP)
a.label('plain')
a.emit(move('a0', 's0'), jal(SCREENMODELS), move('a1', 's1'))
a.label('out')
a.emit(lw('ra', 36, 'sp'), lw('s0', 32, 'sp'), lw('s1', 28, 'sp'), lw('s2', 24, 'sp'), lw('s3', 20, 'sp'), lw('s4', 16, 'sp'),
       addiu('sp', 'sp', 40), jr('ra'), NOP)
words = a.done(); END = a.end()
assert END <= 0x80008530, hex(END)

if 'dis' in sys.argv[1:]:
    print('\n'.join(dis(words, HCODE))); print('%d words, ends %08X' % (len(words), END))
elif 'bin' in sys.argv[1:]:
    sys.stdout.buffer.write(a.code())
elif 'test' in sys.argv[1:]:
    from unicorn.mips_const import *
    # fake DG_ScreenModels, like the real one: reads TEMP[i] at 0x1F800040 + i*0x20 (n of them), copies them to SNAP
    SNAP = 0x80150000; TEMPA = 0x1F800040
    fk = Asm(SCREENMODELS)
    fk.la('t0', SNAP); fk.emit(lui('t1', 0x1F80), addiu('t1', 't1', 0x40))
    fk.label('l')
    for k in range(8): fk.emit(lw('t2', 4 * k, 't1'), NOP, sw('t2', 4 * k, 't0'))
    fk.emit(addiu('t0', 't0', 0x20), addiu('t1', 't1', 0x20), addiu('a1', 'a1', -1)); fk.br(7, 'a1', 'zero', 'l'); fk.emit(NOP)
    fk.emit(jr('ra'), NOP); fk.done()
    OBJS = 0x80176AF0; N = 16; RET = 0x80060000
    def world(i): return struct.pack('<9hh3i', *[(i * 9 + k) % 3000 for k in range(9)], 0, 1000 + i, 2000 + i, 3000 + i)
    def temp(i): return struct.pack('<9hh3i', *[(i * 7 + k) % 2000 for k in range(9)], 0, 5000 + i, 6000 + i, 7000 + i)
    MR = struct.pack('<9hh3i', 4096, 0, 0, 0, 4096, 0, 0, 0, 4096, 0, 111, 222, 333)
    ML = struct.pack('<9hh3i', 0, 4096, 0, 4096, 0, 0, 0, 0, 4096, 0, 444, 555, 666)
    def run(enable, objs_reg, target, n=N):
        u = emu(); u.mem_map(0x1F800000, 0x1000)
        u.mem_write(P(HCODE), a.code()); u.mem_write(P(SCREENMODELS), fk.code())
        for i in range(N): u.mem_write(P(OBJS + 0x48 + i * 0x5C), world(i)); u.mem_write(TEMPA + i * 0x20, temp(i))
        side = bytes([1 if i in (7, 8) else (2 if i == 15 else 0) for i in range(32)])
        u.mem_write(P(HMB), struct.pack('<IIHHII', enable, target, 4, 9, 0, 0) + b'\0' * 12 + MR + ML + side)
        u.reg_write(UC_MIPS_REG_SP, 0x801FFF00); u.reg_write(UC_MIPS_REG_RA, RET)
        u.reg_write(UC_MIPS_REG_A0, objs_reg); u.reg_write(UC_MIPS_REG_A1, n)
        for r in (UC_MIPS_REG_S0, UC_MIPS_REG_S1, UC_MIPS_REG_S2, UC_MIPS_REG_S3, UC_MIPS_REG_S4): u.reg_write(r, 0xABC0 + r)
        u.emu_start(HCODE, RET, count=20000)
        assert u.reg_read(UC_MIPS_REG_SP) == 0x801FFF00
        for r in (UC_MIPS_REG_S0, UC_MIPS_REG_S1, UC_MIPS_REG_S2, UC_MIPS_REG_S3, UC_MIPS_REG_S4): assert u.reg_read(r) == 0xABC0 + r
        seen = [bytes(u.mem_read(P(SNAP + i * 0x20), 0x20)) for i in range(N)]
        worlds = [bytes(u.mem_read(P(OBJS + 0x48 + i * 0x5C), 0x20)) for i in range(N)]
        cnt = struct.unpack('<II', u.mem_read(P(HMB + 0x0C), 8))
        return seen, worlds, cnt
    seen, worlds, cnt = run(1, OBJS, OBJS)
    zr = b'\0' * 0x14
    assert seen[4] == MR and seen[9] == ML, 'hands'
    assert seen[0][:0x14] == zr and seen[0][0x14:] == MR[0x14:], 'part 0 collapses into the right hand'
    assert seen[8][:0x14] == zr and seen[8][0x14:] == ML[0x14:], 'part 8 collapses into the left hand'
    assert seen[15] == temp(15), 'part 15 kept as animated'
    assert all(worlds[i] == world(i) for i in range(N)), 'obj->world untouched (the eye reads joint 6)'
    assert cnt == (1, 1), cnt
    print('Snake body: the renderer sees the hands at the controllers, other parts collapsed into their hand, part 15 as animated; obj->world untouched; alive/applied %s' % (cnt,))
    seen, worlds, cnt = run(1, 0x80180000, OBJS); assert cnt == (1, 0) and seen[4] == temp(4)
    seen, worlds, cnt = run(0, OBJS, OBJS); assert cnt == (1, 0) and seen[4] == temp(4)
    seen, worlds, cnt = run(1, OBJS, OBJS, n=25); assert cnt == (1, 0)
    print('other objects, disabled, and more than 24 parts: drawn untouched')
    print('ok, %d words, ends %08X' % (len(words), END))
else:
    print(c_array('HANDCAVE_' + VER, words))
