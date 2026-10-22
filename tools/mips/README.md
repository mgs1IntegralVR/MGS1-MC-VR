# MIPS routines

Small routines the DLL writes into unused PS1 RAM (the US 1.0 / PAL addresses are in each script's header).
Each script prints a C array (default), a disassembly (`dis`), raw words (`bin`) or runs a unicorn test (`test`, where present).

| Script | Routine | Where |
|---|---|---|
| asm_cave12.py | camera: head-look (mode 1, + offset), Snake's eyes / diorama (mode 2), stereo eye shift + eye marker | 0x8000A480, stereo block 0x8000A9D0 |
| asm_pad.py | VR controllers merged into the game pad, then the call routine | 0x8000A7A0 |
| asm_call2.py | calls game functions for the DLL (10 slots), in the game's own frame | 0x8000B100 |
| asm_bar.py | records every LIFE bar the game draws (boss bars on the wrist) | 0x8000A0C0 |
| asm_hands.py | floating hands: Snake's hand parts drawn at the controllers | 0x80007A60 |
| asm_cave13.py | camera v13 (current): asm_cave12 plus the true-stereo eye lock | 0x8000A480 |
| asm_ts.py | true stereo (test): second sort/draw for the right eye, side-by-side display | 0x8000A150, 0x8000B5E0 |
| asm_act.py | act-dispatch wrapper (box / first-person tricks) | 0x8000AC80 |

Requirements: `pip install capstone unicorn`. Run with `python3 -I <script> US10 test`.
`asm_cave12.py PAL` / `US10` prints `CAVE11_*` / `SCAVE11_*` arrays; paste them over the arrays of the same name in `src/mgs1mc_vr.cpp`.
