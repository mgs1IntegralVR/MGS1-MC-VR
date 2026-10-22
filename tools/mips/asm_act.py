# MIPS act-dispatch wrapper: replaces GV_ExecActorSystem's `jalr <act>` (US 0x80015138 jalr $s0; PAL 0x80014E14 jalr $v0)
# with `jal 0x8000AC80`. The delay slot (a0 = actor) is the game's own and runs first.
#
# Act mailbox at 0x8000AC60 (host <-> MIPS):
#   +0x00 box act function (u32, 0 = none): while it runs, GM_Camera.first_person = 1, so box.c shows its own
#         inside-the-box first-person view (it needs first_person AND Snake's body hidden, as in the game's first person)
#   +0x04 weapon act function (u32, 0 = none): while it runs, Snake's body is shown, so the weapon (gun, grenade,
#         laser sight) behaves as in third person even though the body is hidden from the camera
#   +0x08 Snake's body DG_OBJS (u32)   +0x0C scratch (saved first_person / was-hidden)
#   +0x10 calls (u32)  +0x14 box runs (u32)  +0x18 weapon runs (u32)
# usage: asm_act.py PAL|US10 [dis|bin|test]
import struct, sys
R = {n:i for i,n in enumerate("zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra".split())}
def I(op,rs,rt,imm): return (op<<26)|(R[rs]<<21)|(R[rt]<<16)|(imm&0xFFFF)
def Rt(rs,rt,rd,sh,fn): return (R[rs]<<21)|(R[rt]<<16)|(R[rd]<<11)|(sh<<6)|fn
lui=lambda rt,imm: I(0xF,'zero',rt,imm)
addiu=lambda rt,rs,imm: I(0x9,rs,rt,imm)
andi=lambda rt,rs,imm: I(0xC,rs,rt,imm)
ori=lambda rt,rs,imm: I(0xD,rs,rt,imm)
lw=lambda rt,off,b: I(0x23,b,rt,off)
sw=lambda rt,off,b: I(0x2B,b,rt,off)
lh=lambda rt,off,b: I(0x21,b,rt,off)
lhu=lambda rt,off,b: I(0x25,b,rt,off)
sh_=lambda rt,off,b: I(0x29,b,rt,off)
and_=lambda d,a,b: Rt(a,b,d,0,0x24)
jr=lambda rs: Rt(rs,'zero','zero',0,0x08)
jalr=lambda rs: Rt(rs,'zero','ra',0,0x09)
NOP=0
AMB=0x8000AC60; CODE=0x8000AC80
VERSIONS={'PAL': ('v0', 0x800B8830), 'US10': ('s0', 0x800B9F50)}   # act register, GM_Camera
VER=next((a for a in sys.argv[1:] if a in VERSIONS),'US10')
REG,GMCAM=VERSIONS[VER]
FP=GMCAM+0x22
def hi(a): return ((a+0x8000)>>16)&0xFFFF
def lo(a): return a&0xFFFF
c=[]; labels={}; fix=[]
def emit(*ws): c.extend(ws)
def br(op,rs,rt,lab): fix.append((len(c),op,rs,rt,lab)); c.append(0)
def label(n): labels[n]=len(c)
def amb(): emit(lui('t0',hi(AMB)), addiu('t0','t0',lo(AMB)))

amb()
emit(lw('t1',0x10,'t0'), NOP, addiu('t1','t1',1), sw('t1',0x10,'t0'))
emit(lw('t1',0x00,'t0'), NOP)
br(4,REG,'t1','box'); emit(NOP)
emit(lw('t1',0x04,'t0'), NOP)
br(4,REG,'t1','weap'); emit(NOP)
emit(jr(REG), NOP)                                                   # anything else: straight to its act
# ---- box: first_person = 1 for the act
label('box')
emit(addiu('sp','sp',-24), sw('ra',20,'sp'))
emit(lw('t4',0x14,'t0'), NOP, addiu('t4','t4',1), sw('t4',0x14,'t0'))
emit(lui('t2',hi(FP)), lh('t3',lo(FP),'t2'), NOP, sw('t3',0x0C,'t0'))
emit(addiu('t3','zero',1), sh_('t3',lo(FP),'t2'))
emit(jalr(REG), NOP)
amb()
emit(lw('t3',0x0C,'t0'), lui('t2',hi(FP)), sh_('t3',lo(FP),'t2'))
emit(lw('ra',20,'sp'), addiu('sp','sp',24), jr('ra'), NOP)
# ---- weapon: body shown for the act, hidden again after (only if it was hidden)
label('weap')
emit(addiu('sp','sp',-24), sw('ra',20,'sp'))
emit(lw('t4',0x18,'t0'), NOP, addiu('t4','t4',1), sw('t4',0x18,'t0'))
emit(sw('zero',0x0C,'t0'))
emit(lw('t2',0x08,'t0'), NOP)
br(4,'t2','zero','wcall'); emit(NOP)
emit(lw('t3',0x28,'t2'), NOP, andi('t4','t3',0x80), sw('t4',0x0C,'t0'))
emit(addiu('t5','zero',-129), and_('t3','t3','t5'), sw('t3',0x28,'t2'))
label('wcall')
emit(jalr(REG), NOP)
amb()
emit(lw('t4',0x0C,'t0'), NOP)
br(4,'t4','zero','wout'); emit(NOP)
emit(lw('t2',0x08,'t0'), NOP)
br(4,'t2','zero','wout'); emit(NOP)
emit(lw('t3',0x28,'t2'), NOP, ori('t3','t3',0x80), sw('t3',0x28,'t2'))
label('wout')
emit(lw('ra',20,'sp'), addiu('sp','sp',24), jr('ra'), NOP)

for idx,op,rs,rt,lab in fix:
    off=labels[lab]-(idx+1); c[idx]=I(op,rs,rt,off)
words=c
if __name__=='__main__':
    mode=next((a for a in sys.argv[1:] if a in ('dis','bin','test','inc')),'dis')
    if mode=='dis':
        import capstone
        md=capstone.Cs(capstone.CS_ARCH_MIPS,capstone.CS_MODE_MIPS32+capstone.CS_MODE_LITTLE_ENDIAN)
        for i in md.disasm(b''.join(struct.pack('<I',w) for w in words),CODE): print(f'{i.address:08X} {i.mnemonic} {i.op_str}')
        print(len(words),'words, ends',hex(CODE+4*len(words)))
    elif mode=='inc':
        name='ACTCAVE_'+VER
        print(f'static const uint32_t {name}[{len(words)}] = {{')
        for k in range(0,len(words),6): print('    '+', '.join(f'0x{w:08X}' for w in words[k:k+6])+',')
        print('};')
    elif mode=='test':
        from unicorn import Uc, UC_ARCH_MIPS, UC_MODE_MIPS32, UC_MODE_LITTLE_ENDIAN
        from unicorn.mips_const import UC_MIPS_REG_SP, UC_MIPS_REG_RA, UC_MIPS_REG_A0, UC_MIPS_REG_S0, UC_MIPS_REG_V0, UC_MIPS_REG_PC
        regid={'s0':UC_MIPS_REG_S0,'v0':UC_MIPS_REG_V0}[REG]
        BASE=0x80000000
        def run(actfn, boxfn, weapfn, body, flag0, fp0):
            mu=Uc(UC_ARCH_MIPS,UC_MODE_MIPS32+UC_MODE_LITTLE_ENDIAN)
            mu.mem_map(0,0x200000)
            mu.mem_write((CODE)&0x1FFFFFFF,b''.join(struct.pack('<I',w) for w in words))
            mu.mem_write((AMB)&0x1FFFFFFF,struct.pack('<7I',boxfn,weapfn,body,0,0,0,0))
            if body: mu.mem_write((body+0x28)&0x1FFFFFFF,struct.pack('<I',flag0))
            mu.mem_write((FP)&0x1FFFFFFF,struct.pack('<h',fp0))
            # act stub: records first_person and body flag into 0x80100000, sets v0 garbage, returns
            STUB=actfn
            stub=[lui('t6',0x8010), lui('t7',hi(FP)), lh('t7',lo(FP),'t7'), NOP, sw('t7',0,'t6'),
                  addiu('t7','zero',0), ]
            if body: stub+= [lui('t8',hi(body+0x28)), lw('t8',lo(body+0x28),'t8'), NOP, sw('t8',4,'t6')]
            stub+= [lw('t9',0,'a0'), NOP, sw('t9',8,'t6'), addiu('v0','zero',-1), jr('ra'), NOP]
            mu.mem_write((STUB)&0x1FFFFFFF,b''.join(struct.pack('<I',w) for w in stub))
            RET=0x80015140
            mu.mem_write((RET)&0x1FFFFFFF,struct.pack('<I',0))
            mu.mem_write((0x80120000)&0x1FFFFFFF,struct.pack('<I',0xA5A5))
            mu.reg_write(UC_MIPS_REG_SP,0x801FFF00); mu.reg_write(UC_MIPS_REG_RA,RET); mu.reg_write(UC_MIPS_REG_A0,0x80120000)
            mu.reg_write(regid,actfn)
            mu.emu_start(CODE,RET,count=500)
            rec=struct.unpack('<3I',mu.mem_read((0x80100000)&0x1FFFFFFF,12))
            fp=struct.unpack('<h',mu.mem_read((FP)&0x1FFFFFFF,2))[0]
            fl=struct.unpack('<I',mu.mem_read((body+0x28)&0x1FFFFFFF,4))[0] if body else None
            cnt=struct.unpack('<7I',mu.mem_read((AMB)&0x1FFFFFFF,28))
            sp=mu.reg_read(UC_MIPS_REG_SP)
            return rec, fp, fl, cnt, sp
        ok=True
        def chk(name,cond):
            global ok
            print(('PASS ' if cond else 'FAIL ')+name); ok&=bool(cond)
        BODY=0x80150000; A=0x80060000; BOX=0x80070000; W=0x80080000
        rec,fp,fl,cnt,sp=run(A,BOX,W,BODY,0x80|0x1,0)
        chk('plain act: fp untouched, body still hidden, a0 passed, sp balanced', rec[0]==0 and rec[1]==0x81 and rec[2]==0xA5A5 and fp==0 and fl==0x81 and sp==0x801FFF00 and cnt[4]==1)
        rec,fp,fl,cnt,sp=run(BOX,BOX,W,BODY,0x81,0)
        chk('box act: sees first_person 1 and hidden body; first_person restored to 0', rec[0]==1 and rec[1]==0x81 and rec[2]==0xA5A5 and fp==0 and fl==0x81 and sp==0x801FFF00 and cnt[5]==1)
        rec,fp,fl,cnt,sp=run(BOX,BOX,W,BODY,0x81,1)
        chk('box act in real first person: restores 1', rec[0]==1 and fp==1)
        rec,fp,fl,cnt,sp=run(W,BOX,W,BODY,0x81,0)
        chk('weapon act: sees body shown; hidden again after', rec[1]==0x01 and fl==0x81 and rec[2]==0xA5A5 and sp==0x801FFF00 and cnt[6]==1)
        rec,fp,fl,cnt,sp=run(W,BOX,W,BODY,0x01,0)
        chk('weapon act with body visible: stays visible', rec[1]==0x01 and fl==0x01)
        rec,fp,fl,cnt,sp=run(W,BOX,W,0,0,0)
        chk('weapon act with no body pointer: just runs', rec[2]==0xA5A5 and sp==0x801FFF00)
        rec,fp,fl,cnt,sp=run(A,0,0,BODY,0x81,0)
        chk('all disabled: plain', rec[2]==0xA5A5 and fl==0x81)
        print('ALL PASS' if ok else 'SOME FAILED')
