# v20 TRUE STEREO (milestone 6, GOG render_twice design adapted to the PS1): both eyes drawn in EVERY game frame.
# The PS1 game double-buffers side by side in VRAM (DG_InitChanlSystem(320): buffer 0 at x 0, buffer 1 at x 320) and
# keeps two of everything the renderer makes (OT[2], packs[2], GV memory regions 0/1). True stereo uses both halves at
# once:
#   DG_EndFrame:   `jal DG_SortChanlSystem(GV_Clock)` -> TS_SORT: the normal sort (left eye, the camera routine shifted
#                  the eye -half), then for buffer b = 1-GV_Clock: DG_ClearChanlSystem(b), an eye marker (right) linked
#                  into the overlay OT of b, the eye moved +2*half along its right axis (eye.t; eye_inv.t.x -= 2*half),
#                  GV_Clock = b (prim.c / trans.c index packs by GV_Clock), DG_SortChanlSystem(b), everything restored.
#   DG_StartFrame: `jal DG_DrawChanlSystem(1-which)` -> TS_DRAW: draws that OT and, if a right-eye OT was sorted, the
#                  other one too (each into its own half of VRAM), then DrawSync(0): DG_StartFrame clears that OT next.
#                  `jal PutDispEnv` -> TS_DISP: the display covers both halves (disp.x 0, w 640) while on, w 320 when off.
# The DLL reads the two halves of the 640-wide picture as the two eyes (markers say which half is which).
#
# TSMB = 0x8000B5C0: +0 enable  +4 2*half (s32, game units)  +8 right-eye OT ready  +0xC right sorts  +0x10 second draws
#                    +0x14 PutDispEnv calls (u32 each)
# Code: TS_SORT at TSCODE = 0x8000A150 (spare block after the bar recorder), TS_DRAW + TS_DISP at TSCODE2 = 0x8000B5E0.
#
# usage: asm_ts.py US10 [dis|bin|test|c|addr]
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mipsasm import *
sh_   = lambda rt, off, b: I(0x29, b, rt, off)
lh    = lambda rt, off, b: I(0x21, b, rt, off)
ori   = lambda rt, rs, imm: I(0xD, rs, rt, imm)
xori  = lambda rt, rs, imm: I(0xE, rs, rt, imm)
mult  = lambda a, b: Rt(a, b, 'zero', 0, 0x18)
mflo  = lambda d: Rt('zero', 'zero', d, 0, 0x12)
sra   = lambda d, t, s: Rt('zero', t, d, s, 0x03)
subu  = lambda d, a, b: Rt(a, b, d, 0, 0x23)
or_   = lambda d, a, b: Rt(a, b, d, 0, 0x25)
and_  = lambda d, a, b: Rt(a, b, d, 0, 0x24)
TSMB = 0x8000B5C0; TSCODE = 0x8000A150; TSCODE2 = 0x8000B5E0; SMB = 0x8000A940
# per version: SORT, CLEAR, DRAW, PutDispEnv, GV_Clock, DG_Chanls[1], DG_Chanls[2], hook sites (EndFrame sort, StartFrame draw, StartFrame disp)
VERSIONS = {'US10': dict(DRAWSYNC=0x800922F0, SORT=0x800186E0, CLEAR=0x80018540, DRAW=0x800184EC, PUTDISP=0x80092B20, CLK=0x800AE098,
                         CH1=0x800B4164, CH2=0x800B4350, SITE_SORT=0x80017904, SITE_DRAW=0x80017894, SITE_DISP=0x80017874)}
VER = next((x for x in sys.argv[1:] if x in VERSIONS), 'US10'); V = VERSIONS[VER]

a = Asm(TSCODE)
# ---------------- TS_SORT (a0 = GV_Clock)
a.emit(addiu('sp', 'sp', -48), sw('ra', 44, 'sp'), sw('s0', 40, 'sp'), sw('s1', 36, 'sp'), sw('s2', 32, 'sp'))
a.emit(move('s0', 'a0'), jal(V['SORT']), move('a0', 's0'))                  # left eye: the game's own sort
a.la('s2', TSMB)
a.emit(lw('t0', 0, 's2'), NOP); a.br(4, 't0', 'zero', 'sort_out'); a.emit(NOP)
a.emit(xori('s1', 's0', 1))
a.emit(jal(V['CLEAR']), move('a0', 's1'))                                    # OT b emptied and re-linked
# right-eye marker into the overlay OT of b: eye colour = right, pose colour copied from buffer a's set
a.la('t4', SMB)
a.emit(sll('t8', 's1', 5), sll('t1', 's1', 4), addu('t8', 't8', 't1'), addu('t8', 't8', 't4'), addiu('t8', 't8', 0x20))
a.emit(lw('t9', 0x1C, 't4'), NOP, sw('t9', 4, 't8'))
a.emit(sll('t1', 's0', 5), sll('t2', 's0', 4), addu('t1', 't1', 't2'), addu('t1', 't1', 't4'), addiu('t1', 't1', 0x20))
a.emit(lw('t2', 0x24, 't1'), NOP, sw('t2', 0x24, 't8'))
a.la('t9', V['CH2']); a.emit(sll('t1', 's1', 2), addu('t9', 't9', 't1'), lw('t9', 0, 't9'), NOP)
a.emit(lw('t1', 0, 't9'), lui('t5', 0x00FF), ori('t5', 't5', 0xFFFF))
a.emit(and_('t2', 't1', 't5'), lui('v0', 0x0300), or_('t2', 't2', 'v0'), sw('t2', 0x20, 't8'))
a.emit(addiu('t2', 't8', 0x20), and_('t2', 't2', 't5'), or_('t2', 't2', 'v0'), sw('t2', 0x10, 't8'))
a.emit(addiu('t2', 't8', 0x10), and_('t2', 't2', 't5'), or_('t2', 't2', 'v0'), sw('t2', 0, 't8'))
a.emit(lui('v0', 0xFF00), and_('t1', 't1', 'v0'), and_('t2', 't8', 't5'), or_('t1', 't1', 't2'), sw('t1', 0, 't9'))
# save eye_inv.t.x and eye.t, move the eye to the right eye
a.la('t3', V['CH1'])
a.emit(lw('t0', 0x24, 't3'), lw('t1', 0x44, 't3'), lw('t2', 0x48, 't3'), lw('t4', 0x4C, 't3'))
a.emit(sw('t0', 16, 'sp'), sw('t1', 20, 'sp'), sw('t2', 24, 'sp'), sw('t4', 28, 'sp'))
a.emit(lw('t2', 4, 's2'), NOP)                                               # 2 * half
a.emit(subu('t0', 't0', 't2'), sw('t0', 0x24, 't3'))                         # eye_inv.t.x -= 2h (camera right = +x)
for i in range(3):
    a.emit(lh('t6', 0x30 + 6 * i, 't3'), NOP, mult('t6', 't2'), mflo('t6'), NOP, NOP, sra('t6', 't6', 12))
    a.emit(lw('t7', 0x44 + 4 * i, 't3'), NOP, addu('t7', 't7', 't6'), sw('t7', 0x44 + 4 * i, 't3'))   # eye.t += 2h * column 0
a.la('t4', V['CLK']); a.emit(sw('s1', 0, 't4'))                              # GV_Clock = b while sorting b
a.emit(jal(V['SORT']), move('a0', 's1'))                                    # right eye into buffer b
a.la('t4', V['CLK']); a.emit(sw('s0', 0, 't4'))
a.la('t3', V['CH1'])
a.emit(lw('t0', 16, 'sp'), lw('t1', 20, 'sp'), lw('t2', 24, 'sp'), lw('t4', 28, 'sp'))
a.emit(sw('t0', 0x24, 't3'), sw('t1', 0x44, 't3'), sw('t2', 0x48, 't3'), sw('t4', 0x4C, 't3'))
a.emit(addiu('t0', 'zero', 1), sw('t0', 8, 's2'))                            # right-eye OT ready
a.emit(lw('t0', 0xC, 's2'), NOP, addiu('t0', 't0', 1), sw('t0', 0xC, 's2'))
a.label('sort_out')
a.emit(lw('ra', 44, 'sp'), lw('s0', 40, 'sp'), lw('s1', 36, 'sp'), lw('s2', 32, 'sp'), addiu('sp', 'sp', 48), jr('ra'), NOP)
words1 = a.done(); END1 = a.end(); assert END1 <= 0x8000A400, hex(END1)
code1 = a.code()
a = Asm(TSCODE2)
TS_DRAW = a.end()
# ---------------- TS_DRAW (a0 = 1 - which, set in the caller's delay slot)
a.emit(addiu('sp', 'sp', -32), sw('ra', 28, 'sp'), sw('s0', 24, 'sp'))
a.emit(move('s0', 'a0'), jal(V['DRAW']), NOP)
a.la('t0', TSMB)
a.emit(lw('t1', 0, 't0'), lw('t2', 8, 't0'), NOP)
a.br(4, 't1', 'zero', 'draw_out'); a.emit(NOP)
a.br(4, 't2', 'zero', 'draw_out'); a.emit(NOP)
a.emit(sw('zero', 8, 't0'), lw('t1', 0x10, 't0'), NOP, addiu('t1', 't1', 1), sw('t1', 0x10, 't0'))
a.emit(jal(V['DRAW']), xori('a0', 's0', 1))                                  # the right-eye OT
a.emit(jal(V['DRAWSYNC']), move('a0', 'zero'))                               # DrawSync(0): drawn before DG_StartFrame clears it
a.label('draw_out')
a.emit(lw('ra', 28, 'sp'), lw('s0', 24, 'sp'), addiu('sp', 'sp', 32), jr('ra'), NOP)
TS_DISP = a.end()
# ---------------- TS_DISP (a0 = &g_dispenv, disp.x already stored by the caller's delay slot)
a.la('t0', TSMB)
a.emit(lw('t1', 0, 't0'), NOP)
a.br(4, 't1', 'zero', 'disp_norm'); a.emit(addiu('t2', 'zero', 320))
a.emit(sh_('zero', 0, 'a0'), addiu('t2', 'zero', 640))
a.label('disp_norm')
a.emit(sh_('t2', 4, 'a0'))
a.emit(lw('t1', 0x14, 't0'), NOP, addiu('t1', 't1', 1), sw('t1', 0x14, 't0'))
a.emit(j(V['PUTDISP']), NOP)
words = a.done(); END = a.end()
assert END <= 0x8000B870, hex(END)
HOOKS = [(V['SITE_SORT'], jal(V['SORT']), jal(TSCODE)), (V['SITE_DRAW'], jal(V['DRAW']), jal(TS_DRAW)), (V['SITE_DISP'], jal(V['PUTDISP']), jal(TS_DISP))]

if 'dis' in sys.argv[1:]:
    print('\n'.join(dis(words1, TSCODE))); print('\n'.join(dis(words, TSCODE2))); print('sort %d words ends %08X; draw+disp %d words ends %08X; TS_DRAW %08X TS_DISP %08X' % (len(words1), END1, len(words), END, TS_DRAW, TS_DISP))
    for s_, o, n in HOOKS: print('hook %08X: %08X -> %08X' % (s_, o, n))
elif 'bin' in sys.argv[1:]:
    sys.stdout.buffer.write(code1)
elif 'test' in sys.argv[1:]:
    from unicorn.mips_const import *
    # stubs log (fn id, a0, GV_Clock, eye_inv.t.x, eye.t x,y,z) to LOG
    LOG = 0x80150000
    def stub(addr, fid):
        s = Asm(addr)
        s.la('t0', LOG); s.emit(lw('t1', 0, 't0'), NOP, sll('t2', 't1', 5), addu('t2', 't2', 't0'), addiu('t1', 't1', 1), sw('t1', 0, 't0'))
        s.emit(addiu('t3', 'zero', fid), sw('t3', 4, 't2'), sw('a0', 8, 't2'))
        s.la('t4', V['CLK']); s.emit(lw('t4', 0, 't4'), NOP, sw('t4', 12, 't2'))
        s.la('t4', V['CH1']); s.emit(lw('t5', 0x24, 't4'), lw('t6', 0x44, 't4'), lw('t7', 0x48, 't4'), lw('t8', 0x4C, 't4'))
        s.emit(sw('t5', 16, 't2'), sw('t6', 20, 't2'), sw('t7', 24, 't2'), sw('t8', 28, 't2'))
        s.emit(jr('ra'), NOP); s.done(); return s
    def run(enable, clk, entry, a0, ready=0):
        u = emu(); u.mem_write(P(TSCODE), code1); u.mem_write(P(TSCODE2), a.code())
        for fid, ad in ((1, V['SORT']), (2, V['CLEAR']), (3, V['DRAW']), (4, V['PUTDISP']), (5, V['DRAWSYNC'])):   # real stubs far away, a jump at each address
            far = 0x80170000 + fid * 0x200; u.mem_write(P(far), stub(far, fid).code()); u.mem_write(P(ad), struct.pack('<II', j(far), NOP))
        u.mem_write(P(TSMB), struct.pack('<6I', enable, 64, ready, 0, 0, 0))
        u.mem_write(P(V['CLK']), struct.pack('<I', clk))
        # eye: 45 deg yaw (col0 = (2896, 0, -2896)), t = (1000, 2000, 3000); eye_inv.t.x = 555
        u.mem_write(P(V['CH1'] + 0x10), struct.pack('<9hh3i', 2896, 0, -2896, 0, 4096, 0, 2896, 0, 2896, 0, 555, 0, 0))
        u.mem_write(P(V['CH1'] + 0x30), struct.pack('<9hh3i', 2896, 0, 2896, 0, 4096, 0, -2896, 0, 2896, 0, 1000, 2000, 3000))
        OT = [0x80120000, 0x80121000]
        u.mem_write(P(V['CH2']), struct.pack('<II', *OT))
        for o in OT: u.mem_write(P(o), struct.pack('<I', 0x00FFFFFF))
        sm = bytearray(0x8C); struct.pack_into('<3I', sm, 0x14, 0x60FF0000, 0x600000FF, 0x6000FF00)
        struct.pack_into('<I', sm, 0x20 + 0x30 * clk + 0x24, 0x60AA55FF); u.mem_write(P(SMB), bytes(sm))
        DISP = 0x800B2D78; u.mem_write(P(DISP), struct.pack('<4h', 320, 0, 320, 240))
        RET = 0x80060000
        u.reg_write(UC_MIPS_REG_SP, 0x801FFF00); u.reg_write(UC_MIPS_REG_RA, RET); u.reg_write(UC_MIPS_REG_A0, DISP if entry == TS_DISP else a0)
        for r in (UC_MIPS_REG_S0, UC_MIPS_REG_S1, UC_MIPS_REG_S2): u.reg_write(r, 0x5000 + r)
        stop = V['PUTDISP'] + 0x7C if entry == TS_DISP else RET
        u.emu_start(entry, RET, count=20000)
        assert u.reg_read(UC_MIPS_REG_SP) == 0x801FFF00
        for r in (UC_MIPS_REG_S0, UC_MIPS_REG_S1, UC_MIPS_REG_S2): assert u.reg_read(r) == 0x5000 + r
        n = struct.unpack('<I', u.mem_read(P(LOG), 4))[0]
        calls = [struct.unpack('<8i', u.mem_read(P(LOG) + 32 * k + 4, 32))[:7] for k in range(n)]
        mb = struct.unpack('<6I', u.mem_read(P(TSMB), 24))
        after = (struct.unpack('<i', u.mem_read(P(V['CH1'] + 0x24), 4))[0], struct.unpack('<3i', u.mem_read(P(V['CH1'] + 0x44), 12)), struct.unpack('<I', u.mem_read(P(V['CLK']), 4))[0])
        return calls, mb, after, u, OT
    calls, mb, after, u, OT = run(0, 0, TSCODE, 0)
    assert [c[0] for c in calls] == [1] and calls[0][1] == 0 and mb[2] == 0, (calls, mb)
    print('sort, TS off: one sort (left), nothing else')
    for clk in (0, 1):
        calls, mb, after, u, OT = run(1, clk, TSCODE, clk)
        b = 1 - clk
        assert [c[0] for c in calls] == [1, 2, 1], calls
        assert calls[0][1:4] == (clk, clk, 555), calls[0]
        assert calls[1][1] == b, calls[1]
        assert calls[2][1] == b and calls[2][2] == b, calls[2]                     # GV_Clock = b during the right sort
        assert calls[2][3] == 555 - 64, calls[2]                                   # eye_inv.t.x -= 2h
        assert tuple(calls[2][4:7]) == (1000 + (2896 * 64 >> 12), 2000, 3000 + (-2896 * 64 >> 12)), calls[2]   # eye.t += 2h * col0
        assert after == (555, (1000, 2000, 3000), clk), after                      # all restored
        assert mb[2] == 1 and mb[3] == 1, mb
        pk = SMB + 0x20 + 0x30 * b
        eyecol = struct.unpack('<I', u.mem_read(P(pk + 4), 4))[0]; seqcol = struct.unpack('<I', u.mem_read(P(pk + 0x24), 4))[0]
        ot0 = struct.unpack('<I', u.mem_read(P(OT[b]), 4))[0]
        tags = [struct.unpack('<I', u.mem_read(P(pk + o), 4))[0] for o in (0, 0x10, 0x20)]
        assert eyecol == 0x6000FF00 and seqcol == 0x60AA55FF, (hex(eyecol), hex(seqcol))
        assert (ot0 & 0xFFFFFF) == (pk & 0xFFFFFF) and tags[0] == 0x03000000 | ((pk + 0x10) & 0xFFFFFF) and tags[1] == 0x03000000 | ((pk + 0x20) & 0xFFFFFF) \
               and tags[2] == 0x03000000 | 0xFFFFFF, [hex(x) for x in [ot0] + tags]
        print('sort, TS on, GV_Clock %d: left sort, clear %d, right marker linked into overlay OT %d, right sort with GV_Clock %d and the eye 2h to the right, all restored' % (clk, b, b, b))
    calls, mb, after, u, OT = run(1, 0, TS_DRAW, 1, ready=1)
    assert [c[:2] for c in calls] == [(3, 1), (3, 0), (5, 0)] and mb[2] == 0 and mb[4] == 1, (calls, mb)
    calls, mb, after, u, OT = run(1, 0, TS_DRAW, 1, ready=0); assert [c[:2] for c in calls] == [(3, 1)]
    calls, mb, after, u, OT = run(0, 0, TS_DRAW, 1, ready=1); assert [c[:2] for c in calls] == [(3, 1)]
    print('draw: both OTs when on and a right-eye OT is ready (flag cleared, then DrawSync(0)), else only the game\'s')
    for en, want in ((1, (0, 640)), (0, (320, 320))):
        calls, mb, after, u, OT = run(en, 0, TS_DISP, 0)
        dx, dw = struct.unpack('<hxxh', u.mem_read(P(0x800B2D78), 6))
        assert [c[0] for c in calls] == [4] and (dx, dw) == want, (calls, dx, dw)
    print('disp: 640 wide from x 0 when on, 320 when off; then PutDispEnv')
    print('ok, sort %d words ends %08X, draw+disp %d words ends %08X' % (len(words1), END1, len(words), END))
elif 'addr' in sys.argv[1:]:
    print('TS_DRAW %08X TS_DISP %08X' % (TS_DRAW, TS_DISP))
else:
    print(c_array('TSCAVE1_' + VER, words1))
    print(c_array('TSCAVE2_' + VER, words))
    print('static const uint32_t TS_HOOKS_%s[3][3] = { %s };' % (VER, ', '.join('{0x%08X, 0x%08X, 0x%08X}' % h for h in HOOKS)))
