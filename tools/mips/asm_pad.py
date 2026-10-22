# MIPS pad routine: wraps the frame loop's `jal GV_UpdatePadSystem` (libdg/dgd.c), lets the game read its real pads,
# then merges the VR controllers (written by the DLL) into GV_PadData[0] (pad 1, configured) and [2] (pad 1, raw).
#
# Pad mailbox at 0x8000A780 (host <-> MIPS):
#   +0x00 enable (u32)
#   +0x04 VR buttons (u16, PS1 bits as libgv.h: Triangle 0x10, Circle 0x20, Cross 0x40, Square 0x80, L2 1, R2 2,
#         L1 4, R1 8, Select 0x100, L3 0x200, R3 0x400, Start 0x800, Up 0x1000, Right 0x2000, Down 0x4000, Left 0x8000)
#   +0x06 previous VR buttons (u16, MIPS-owned: press/release edges are per game frame)
#   +0x08 stick angle (u16, 4096 = 360, same convention as GV_VecDir2: up = 2048, right = 1024)
#   +0x0A stick active (u16)   +0x0C left_dx (u8)   +0x0D left_dy (u8)   +0x10 call counter (u32)
# GV_PAD: status +0, press +2, release +4, quick +6, dir +8, analog +0xA, right_dx +0xC, right_dy +0xD, left_dx +0xE,
# left_dy +0xF; 0x10 bytes per pad.  dir = angle + PadOrg (as libgv/pad.c), so walk mode's PadOrg steers it too.
# Skipped while GM_GameStatus has PADMASK | PADRELEASE | PADDEMO (0x58000000) or DG_UnDrawFrameCount > 0, exactly the
# cases where GV_UpdatePadSystem itself withholds input. Cutscenes (STATE_DEMO alone) still get buttons, for skipping.
#
# usage: asm_pad.py PAL|US10 [dis|bin]
import struct, sys
R = {n:i for i,n in enumerate("zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra".split())}
def I(op,rs,rt,imm): return (op<<26)|(R[rs]<<21)|(R[rt]<<16)|(imm&0xFFFF)
def Rt(rs,rt,rd,sh,fn): return (R[rs]<<21)|(R[rt]<<16)|(R[rd]<<11)|(sh<<6)|fn
lui=lambda rt,imm: I(0xF,'zero',rt,imm)
addiu=lambda rt,rs,imm: I(0x9,rs,rt,imm)
andi=lambda rt,rs,imm: I(0xC,rs,rt,imm)
lw=lambda rt,off,b: I(0x23,b,rt,off)
sw=lambda rt,off,b: I(0x2B,b,rt,off)
lh=lambda rt,off,b: I(0x21,b,rt,off)
lhu=lambda rt,off,b: I(0x25,b,rt,off)
lbu=lambda rt,off,b: I(0x24,b,rt,off)
sh_=lambda rt,off,b: I(0x29,b,rt,off)
sb=lambda rt,off,b: I(0x28,b,rt,off)
and_=lambda d,a,b: Rt(a,b,d,0,0x24)
or_=lambda d,a,b: Rt(a,b,d,0,0x25)
nor=lambda d,a,b: Rt(a,b,d,0,0x27)
addu=lambda d,a,b: Rt(a,b,d,0,0x21)
jr=lambda rs: Rt(rs,'zero','zero',0,0x08)
NOP=0
PMB=0x8000A780; CODE=0x8000A7A0
# per version: GV_UpdatePadSystem, GV_PadData, PadOrg, GM_GameStatus, DG_UnDrawFrameCount
VERSIONS={'PAL': (0x80016728, 0x800B1618, 0x800AC3E4, 0x800AC438, 0x800AC3EC),
          'US10':(0x80016E7C, 0x800B2D38, 0x800ADAA8, 0x800ADB3C, 0x800ADAB0)}
VER=next((a for a in sys.argv[1:] if a in VERSIONS),'PAL')
UPDATE,PADDATA,PADORG,GSTATUS,UNDRAW=VERSIONS[VER]
def hi(a): return ((a+0x8000)>>16)&0xFFFF
def lo(a): return a&0xFFFF
c=[]; labels={}; fix=[]
def emit(*ws): c.extend(ws)
def br(op,rs,rt,lab): fix.append((len(c),op,rs,rt,lab)); c.append(0)
def label(n): labels[n]=len(c)

emit(addiu('sp','sp',-24), sw('ra',20,'sp'))
emit(0x0C000000|((UPDATE>>2)&0x3FFFFFF), NOP)                          # jal GV_UpdatePadSystem
emit(lui('t0',hi(PMB)), addiu('t0','t0',lo(PMB)))
emit(lw('t1',0x10,'t0'), NOP, addiu('t1','t1',1), sw('t1',0x10,'t0'))  # calls++
emit(lw('t1',0x00,'t0'), NOP)
br(4,'t1','zero','out'); emit(NOP)                                     # disabled
emit(lui('t1',hi(GSTATUS)), lw('t1',lo(GSTATUS),'t1'), lui('t2',0x5800), NOP, and_('t1','t1','t2'))
br(5,'t1','zero','out'); emit(NOP)                                     # PADMASK|PADRELEASE|PADDEMO -> leave alone
emit(lui('t1',hi(UNDRAW)), lw('t1',lo(UNDRAW),'t1'), NOP)
fix.append((len(c),7,'t1','zero','out')); c.append(0); emit(NOP)       # bgtz UnDrawFrameCount -> leave alone
emit(lhu('t2',4,'t0'), lhu('t3',6,'t0'), NOP)                          # t2 = VR buttons, t3 = previous
emit(nor('t4','t3','zero'), and_('t4','t4','t2'))                      # press   = now & ~prev
emit(nor('t5','t2','zero'), and_('t5','t5','t3'))                      # release = prev & ~now
emit(sh_('t2',6,'t0'))                                                  # prev = now
emit(lhu('t6',0x0A,'t0'), lhu('t7',0x08,'t0'), lui('t8',hi(PADORG)), lw('t8',lo(PADORG),'t8'), NOP)
emit(addu('t7','t7','t8'), andi('t7','t7',0xFFF))                       # dir = angle + PadOrg
emit(lbu('a2',0x0C,'t0'), lbu('a3',0x0D,'t0'))                          # stick bytes
emit(lui('t9',hi(PADDATA)), addiu('t9','t9',lo(PADDATA)))
for k,base in enumerate((0x00,0x20)):                                   # GV_PadData[0] and [2]
    emit(lhu('v0',base+0,'t9'), NOP, or_('v0','v0','t2'), sh_('v0',base+0,'t9'))
    emit(lhu('v0',base+2,'t9'), NOP, or_('v0','v0','t4'), sh_('v0',base+2,'t9'))
    emit(lhu('v0',base+4,'t9'), NOP, or_('v0','v0','t5'), sh_('v0',base+4,'t9'))
    br(4,'t6','zero','nostick%d'%k); emit(NOP)
    emit(sh_('t7',base+8,'t9'))                                         # dir
    emit(lh('v0',base+0x0A,'t9'), NOP)
    br(5,'v0','zero','hasan%d'%k); emit(NOP)
    emit(addiu('v0','zero',1), sh_('v0',base+0x0A,'t9'))                # analog = 1 if the real pad was digital
    label('hasan%d'%k)
    emit(sb('a2',base+0x0E,'t9'), sb('a3',base+0x0F,'t9'))              # left_dx, left_dy
    label('nostick%d'%k)
label('out')
emit(0x0C000000|((0x8000B100>>2)&0x3FFFFFF), NOP)                     # v13: jal the call routine (asm_call2.py) every frame
emit(lw('ra',20,'sp'), NOP, addiu('sp','sp',24), jr('ra'), NOP)
for i,op,rs,rt,lab in fix: c[i]=I(op,rs,rt,labels[lab]-i-1)
code=b''.join(struct.pack('<I',w) for w in c)
hook=0x0C000000|((CODE>>2)&0x3FFFFFF)
if 'dis' in sys.argv[1:]:
    from capstone import Cs,CS_ARCH_MIPS,CS_MODE_MIPS32,CS_MODE_LITTLE_ENDIAN
    md=Cs(CS_ARCH_MIPS,CS_MODE_MIPS32+CS_MODE_LITTLE_ENDIAN)
    for x in md.disasm(code,CODE): print("%08x: %s %s"%(x.address,x.mnemonic,x.op_str))
    print("hook word %08X, %d words, %d bytes, ends %08X"%(hook,len(c),len(code),CODE+len(code)))
elif 'bin' in sys.argv[1:]:
    sys.stdout.buffer.write(code)
else:
    print("static const uint32_t PADCAVE_%s[%d] = {"%(VER,len(c)))
    for k in range(0,len(c),6): print("    "+", ".join("0x%08X"%w for w in c[k:k+6])+",")
    print("};")
