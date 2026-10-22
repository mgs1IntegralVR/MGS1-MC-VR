# MIPS routine v13 (v20 TRUE STEREO): = v12 + the stereo block takes the LEFT eye (-half, code 1) whatever the draw buffer
# while TSMB (0x8000B5C0) enable != 0 (the second, right-eye sort is done by asm_ts.py at DG_EndFrame).
# MIPS routine v12 (v15 DIORAMA): = v11 + in mode 1 eye.t += the int16 x,y,z at DIO = 0x8000B580 (0 = no change)
# (v11: MIPS routine v11 (= v10, but the FrameRunDemo check ignores the address segment: cutscene code runs from the KUSEG
# mirror, so ra there is 0x0007D8B4, not 0x8007D8B4)
# v10: (= v9, but s3 and GV_Time are recorded only when the call came from FrameRunDemo's hook site: in a
# cutscene the camera actor calls the routine a second time each frame and overwrote v9's record; ra still always stored)
# v9: (= v8 + the stereo block records s3, ra and GV_Time at 0x8000A794/98/9C on every world-eye call:
# called from FrameRunDemo (ra = its hook site + 8), s3 is the MGSDEMOACT, so the DLL can find the cutscene cast)
# v8: (= v7 + a third marker TILE whose colour carries the low 6 bits of the head-pose seq used for the
# frame: 2 bits per channel at levels 0/85/170/255, so the DLL knows the exact head pose of every picture)
# v7: (= v6 with the eye chosen by GV_Clock, the frame's draw buffer, and an eye marker drawn into the frame)
# The stereo block (0x8000A9C0) moves eye.t by +/-half along the camera right (GV_Clock 1 = right eye) and links two
# 3x2-pixel TILEs at screen (0,0) and (3,0) into the overlay channel's OT for this buffer: an eye colour (blue mono, red
# left, green right) and a magenta validator. The DLL reads the eye from the captured pixels, so the eye of every picture
# is known exactly, whatever the emulator's present timing.
# v5: (walking eye anchored to Snake's head joint): head-look (mode 1) and Snake's-eyes walking camera (mode 2).
#
# Hooked into the world camera's jal TransposeMatrix (DG_MakeCameraMatrix and FrameRunDemo). Acts only when
# a0 == DG_Chanls[1].eye. Tail-jumps to TransposeMatrix so eye_inv (and later eye_inv.t from eye.t) follow.
#
# Mailbox at 0x8000A400 (host <-> MIPS):
#   +0x00 magic 'MCV4'   +0x04 mode (0 off, 1 eye = eye * M, 2 eye = M with t = SnakeCamera.position + offset)
#   +0x08 active slot    +0x0C call counter   +0x10 applied counter   +0x14 last eye ptr
#   +0x18 FOV screen override (u16, 0 = keep)   +0x1C seq of the last matrix applied
#   +0x20 slot 0: M (9 x int16, x4096) at +0x20, seq (u32) at +0x34
#   +0x38 slot 1: M at +0x38, seq at +0x4C
#   +0x50 ring[8] (u32): ring[applied & 7] = seq used by that applied call
#   +0x70 eye offset x,y,z (int16)  +0x76 PadOrg value (u16)  +0x78 PadOrg enable (u32)   (mode 2 only)
#   +0x7C Snake's body DG_OBJS pointer (u32, 0 = none). Mode 2 takes eye x,z from objs[6].world.t (+0x284/+0x28C,
#         the head joint) and y from GM_SnakeCamera.position.y (the game's own eye height: crawl, vents).
#         Pointers outside 0x80000000-0x801FFFFF are ignored (falls back to GM_SnakeCamera.position).
# Routine at 0x8000A480.
#
# usage: asm_cave11.py PAL|US10 [dis|bin]
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
mult=lambda a,b: Rt(a,b,'zero',0,0x18)
mflo=lambda d: Rt('zero','zero',d,0,0x12)
addu=lambda d,a,b: Rt(a,b,d,0,0x21)
sra=lambda d,t,s: Rt('zero',t,d,s,0x03)
sll=lambda d,t,s: Rt('zero',t,d,s,0x00)
NOP=0
sltu=lambda d,a,b: Rt(a,b,d,0,0x2B)
subu=lambda d,a,b: Rt(a,b,d,0,0x23)
or_=lambda d,a,b: Rt(a,b,d,0,0x25)
xori=lambda rt,rs,imm: I(0xE,rs,rt,imm)
srl=lambda d,t,s: Rt('zero',t,d,s,0x02)
and_=lambda d,a,b: Rt(a,b,d,0,0x24)
ori=lambda rt,rs,imm: I(0xD,rs,rt,imm)
MBOX=0x8000A400; CODE=0x8000A480; SMB=0x8000A940; SCODE=0x8000A9D0
# per version: TransposeMatrix, DG_Chanls[1].eye, GM_SnakeCamera (position SVECTOR at +0), PadOrg
VERSIONS={'PAL': (0x8001ED38, 0x800B2A44+0x30, 0x800B8800, 0x800AC3E4),
          'US10':(0x8001F688, 0x800B4164+0x30, 0x800B9F20, 0x800ADAA8)}
# per version: GV_Clock, GV_Time, DG_Chanls[2] (overlay channel: ot[2] at +0)
EXTRA={'PAL': (0x800AC978, 0x800AC39C, 0x800B2C30, 0x8007BF48), 'US10': (0x800AE098, 0x800ADA48, 0x800B4350, 0x8007D8B4)}
VER=next((a for a in sys.argv[1:] if a in VERSIONS),'PAL')
TARGET,WORLD_EYE,SNAKECAM,PADORG=VERSIONS[VER]
CLK,GVT,CH2,H2RET=EXTRA[VER]
def lo(a): return a&0xFFFF
def hi(a): return ((a+0x8000)>>16)&0xFFFF
c=[]; labels={}; fix=[]
def emit(*ws): c.extend(ws)
def br(op,rs,rt,lab): fix.append((len(c),op,rs,rt,lab)); c.append(0)
def jmp(lab): fix.append((len(c),'j',None,None,lab)); c.append(0)
def label(n): labels[n]=len(c)

emit(lui('t0',hi(MBOX)), addiu('t0','t0',MBOX&0xFFFF))
emit(lw('t1',0x0C,'t0'), NOP, addiu('t1','t1',1), sw('t1',0x0C,'t0'))      # calls++
emit(sw('a0',0x14,'t0'))
emit(lw('t1',0x04,'t0'))                                                 # mode
emit(lui('t2',hi(WORLD_EYE)), addiu('t2','t2',WORLD_EYE&0xFFFF))
br(4,'t1','zero','done'); emit(NOP)                                      # mode 0 -> done
br(5,'a0','t2','done'); emit(NOP)                                        # not the world eye -> done
emit(lw('t2',0x08,'t0'), NOP, addiu('t3','t0',0x20))                     # t3 = slot 0
br(4,'t2','zero','use'); emit(NOP)
emit(addiu('t3','t0',0x38))                                              # t3 = slot 1
label('use')
emit(addiu('t2','zero',2))
br(4,'t1','t2','override'); emit(NOP)                                    # mode 2 -> override
# ---- mode 1: eye = eye * M
emit(addu('t4','a0','zero'), addiu('t5','zero',3))
label('row')
emit(lh('t6',0,'t4'), lh('t7',2,'t4'), lh('t8',4,'t4'), NOP)
for j in range(3):
    emit(lh('v0',2*j,'t3'), lh('v1',6+2*j,'t3'), lh('a2',12+2*j,'t3'))
    emit(mult('t6','v0'), mflo('t9'), NOP, NOP)
    emit(mult('t7','v1'), mflo('a3'), addu('t9','t9','a3'), NOP)
    emit(mult('t8','a2'), mflo('a3'), addu('t9','t9','a3'), sra('t9','t9',12))
    emit(sh_('t9',2*j,'t4'))
emit(addiu('t4','t4',6), addiu('t5','t5',-1))
br(5,'t5','zero','row'); emit(NOP)
DIO=0x8000B580
emit(lui('t4',hi(DIO)), addiu('t4','t4',DIO&0xFFFF))
for k in range(3):
    emit(lh('t5',2*k,'t4'), lw('t6',0x14+4*k,'a0'), NOP, addu('t6','t6','t5'), sw('t6',0x14+4*k,'a0'))
jmp('common'); emit(NOP)
# ---- mode 2: eye rotation = M; eye.t = SnakeCamera.position + offset; PadOrg
label('override')
for k in range(9):
    emit(lh('t5',2*k,'t3'), NOP, sh_('t5',2*k,'a0'))
emit(lui('t4',hi(SNAKECAM)), addiu('t4','t4',SNAKECAM&0xFFFF))
emit(lw('t7',0x7C,'t0'), NOP)
fix.append((len(c),1,'t7','at','nojoint')); c.append(0); emit(NOP)       # bgez t7 -> not KSEG0 -> fallback (rt=1 = BGEZ)
emit(lui('t6',0x8020), sltu('t6','t7','t6'))
br(4,'t6','zero','nojoint'); emit(NOP)                                   # >= 0x80200000 -> fallback
emit(lw('t5',0x284,'t7'), lh('t6',0x70,'t0'), NOP, addu('t5','t5','t6'), sw('t5',0x14,'a0'))   # x: head joint
emit(lh('t5',2,'t4'), lh('t6',0x72,'t0'), NOP, addu('t5','t5','t6'), sw('t5',0x18,'a0'))       # y: game eye height
emit(lw('t5',0x28C,'t7'), lh('t6',0x74,'t0'), NOP, addu('t5','t5','t6'), sw('t5',0x1C,'a0'))   # z: head joint
jmp('posdone'); emit(NOP)
label('nojoint')
for k in range(3):
    emit(lh('t5',2*k,'t4'), lh('t6',0x70+2*k,'t0'), NOP, addu('t5','t5','t6'), sw('t5',0x14+4*k,'a0'))
label('posdone')
emit(lw('t5',0x78,'t0'), NOP)
br(4,'t5','zero','common'); emit(NOP)
emit(lhu('t5',0x76,'t0'), lui('t4',hi(PADORG)), NOP, sw('t5',PADORG&0xFFFF,'t4'))
label('common')
# FOV override: chanl->screen (+0x50) = eye ptr + 0x20
emit(lhu('t5',0x18,'t0'), NOP)
br(4,'t5','zero','nofov'); emit(NOP)
emit(sh_('t5',0x20,'a0'))
label('nofov')
emit((2<<26)|((SCODE>>2)&0x3FFFFFF), NOP)                                # j stereo block (it ends in j TransposeMatrix)
label('done')
emit((2<<26)|((TARGET>>2)&0x3FFFFFF), NOP)                               # j TransposeMatrix
for i,op,rs,rt,lab in fix:
    if op=='j': c[i]=(2<<26)|(((CODE+4*labels[lab])>>2)&0x3FFFFFF)
    else: c[i]=I(op,rs,rt,labels[lab]-i-1)
main=c
# ---- stereo block at SCODE. In: t0 = mailbox, t3 = matrix slot, a0 = world eye (rotation and t final); a1 untouched.
# Stereo mailbox SMB: +0 half IPD (s16, 0 = mono)  +8/+C/+10 mono/left/right counters  +14/+18/+1C TILE colour words
#   (mono, left, right)  +20 packets for buffer 0 (eye +20, magenta +30, seq +40)  +50 packets for buffer 1 (+50/+60/+70)
#   +80/+84 GV_Time stamp of the last link into buffer 0/1 (never link twice in one frame: that would loop the OT)
#   +88 colour levels {0, 85, 170, 255}
c=[]; labels={}; fix=[]
PMB=0x8000A780
emit(lui('t1',hi(PMB)), addiu('t1','t1',lo(PMB)), sw('ra',0x18,'t1'))
emit(lui('t2',hi(H2RET & 0x1FFFFFFF)), addiu('t2','t2',lo(H2RET & 0x1FFFFFFF)), sll('t5','ra',3), srl('t5','t5',3))
br(5,'t5','t2','norec'); emit(NOP)                                       # not FrameRunDemo -> keep the last record
emit(sw('s3',0x14,'t1'), lui('t2',hi(GVT)), lw('t2',lo(GVT),'t2'), NOP, sw('t2',0x1C,'t1'))
label('norec')
emit(lui('t4',hi(SMB)), addiu('t4','t4',lo(SMB)))
emit(lui('t6',hi(CLK)), lw('t6',lo(CLK),'t6'), lh('t5',0,'t4'), andi('t6','t6',1))   # t6 = draw buffer of this frame
TSMB=0x8000B5C0
sltiu=lambda rt,rs,imm: I(0xB,rs,rt,imm)
emit(lui('t2',hi(TSMB)), lw('t2',lo(TSMB),'t2'), NOP, sltiu('t2','t2',1), and_('t2','t2','t6'))   # t2 = eye side: TS on -> 0 (left)
br(4,'t5','zero','mono'); emit(addu('t7','zero','zero'))                  # (delay) eye code 0
br(5,'t2','zero','pos'); emit(NOP)                                       # side 1 = right eye: +half
emit(subu('t5','zero','t5'))                                             # side 0 = left eye: -half
label('pos')
for i in range(3):
    emit(lh('t8',6*i,'a0'), NOP, mult('t8','t5'), mflo('t8'), NOP, NOP, sra('t8','t8',12))
    emit(lw('t9',0x14+4*i,'a0'), NOP, addu('t9','t9','t8'), sw('t9',0x14+4*i,'a0'))
emit(addiu('t7','t2',1))                                                 # eye code 1 left / 2 right
label('mono')
emit(sll('t8','t7',2), addu('t8','t8','t4'), lw('t9',8,'t8'), NOP, addiu('t9','t9',1), sw('t9',8,'t8'))   # per-eye counter
emit(lw('t9',0x14,'t8'), NOP)                                            # t9 = TILE colour word for this eye
emit(sll('t7','t7',30))
emit(lw('t1',0x10,'t0'), lw('t5',0x14,'t3'), andi('t2','t1',7), sll('t2','t2',2), addu('t2','t2','t0'))
emit(or_('t5','t5','t7'))
emit(sw('t5',0x50,'t2'), sw('t5',0x1C,'t0'), addiu('t1','t1',1), sw('t1',0x10,'t0'))
# ---- seq colour word into v1: R = level[seq&3], G = level[(seq>>2)&3], B = level[(seq>>4)&3], code 0x60
emit(addiu('a3','t4',0x88))
emit(andi('a2','t5',3), addu('a2','a2','a3'), lbu('v1',0,'a2'))
emit(srl('a2','t5',2), andi('a2','a2',3), addu('a2','a2','a3'), lbu('a2',0,'a2'), NOP, sll('a2','a2',8), or_('v1','v1','a2'))
emit(srl('a2','t5',4), andi('a2','a2',3), addu('a2','a2','a3'), lbu('a2',0,'a2'), NOP, sll('a2','a2',16), or_('v1','v1','a2'))
emit(lui('a2',0x6000), or_('v1','v1','a2'))
# ---- eye marker into the overlay OT of this buffer, once per frame
emit(lui('t1',hi(GVT)), lw('t1',lo(GVT),'t1'), sll('t8','t6',2), addu('t8','t8','t4'), lw('t2',0x80,'t8'), NOP)
br(4,'t1','t2','skipm'); emit(NOP)
emit(sw('t1',0x80,'t8'))                                                 # stamp
emit(sll('t8','t6',5), sll('t1','t6',4), addu('t8','t8','t1'), addu('t8','t8','t4'), addiu('t8','t8',0x20))   # eye TILE: +0x20+W*0x30
emit(sw('t9',4,'t8'), sw('v1',0x24,'t8'))                                # eye colour, seq colour
emit(lui('t9',hi(CH2)), addiu('t9','t9',lo(CH2)), sll('t1','t6',2), addu('t9','t9','t1'), lw('t9',0,'t9'), NOP)   # ot
emit(lw('t1',0,'t9'), lui('t5',0x00FF), ori('t5','t5',0xFFFF))         # t1 = ot[0], t5 = 24-bit mask
emit(and_('t2','t1','t5'), lui('v0',0x0300), or_('t2','t2','v0'), sw('t2',0x20,'t8'))          # seq.tag -> old chain
emit(addiu('t2','t8',0x20), and_('t2','t2','t5'), or_('t2','t2','v0'), sw('t2',0x10,'t8'))     # magenta.tag -> seq
emit(addiu('t2','t8',0x10), and_('t2','t2','t5'), or_('t2','t2','v0'), sw('t2',0,'t8'))        # eye.tag -> magenta
emit(lui('v0',0xFF00), and_('t1','t1','v0'), and_('t2','t8','t5'), or_('t1','t1','t2'), sw('t1',0,'t9'))   # ot[0] -> eye
label('skipm')
emit((2<<26)|((TARGET>>2)&0x3FFFFFF), NOP)                               # j TransposeMatrix
for i,op,rs,rt,lab in fix: c[i]=I(op,rs,rt,labels[lab]-i-1)
stereo=c
c=main
code=b''.join(struct.pack('<I',w) for w in c)
scode=b''.join(struct.pack('<I',w) for w in stereo)
hook=0x0C000000|((CODE>>2)&0x3FFFFFF)
if 'dis' in sys.argv[1:]:
    from capstone import Cs,CS_ARCH_MIPS,CS_MODE_MIPS32,CS_MODE_LITTLE_ENDIAN
    md=Cs(CS_ARCH_MIPS,CS_MODE_MIPS32+CS_MODE_LITTLE_ENDIAN)
    for x in md.disasm(code,CODE): print("%08x: %s %s"%(x.address,x.mnemonic,x.op_str))
    print("hook word %08X, %d words, %d bytes, ends %08X"%(hook,len(c),len(code),CODE+len(code)))
    for x in md.disasm(scode,SCODE): print("%08x: %s %s"%(x.address,x.mnemonic,x.op_str))
    print("stereo block %d words, ends %08X"%(len(stereo),SCODE+len(scode)))
elif 'bin' in sys.argv[1:]:
    sys.stdout.buffer.write(code)
elif 'sbin' in sys.argv[1:]:
    sys.stdout.buffer.write(scode)
else:
    print("static const uint32_t CAVE11_%s[%d] = {"%(VER,len(c)))
    for k in range(0,len(c),6): print("    "+", ".join("0x%08X"%w for w in c[k:k+6])+",")
    print("};")
    print("static const uint32_t SCAVE11_%s[%d] = {"%(VER,len(stereo)))
    for k in range(0,len(stereo),6): print("    "+", ".join("0x%08X"%w for w in stereo[k:k+6])+",")
    print("};")
