# Decomp checks for the MC port (2026-10-07)
Source: FoxdieTeam/mgs_reversing (main exe 100% decompiled; Integral SLPM-86247). PAL addresses from our matcher.

## 1. Hook DG_MakeCameraMatrix itself, not just camera.c's call
- `DG_MakeCameraMatrix(chanl, from, to, screen)` (libdg/frame.c; PAL **0x8001732C**) builds `chanl->eye` from from→to,
  then `eye_inv` = transpose + translated. It has **no roll** (uses the fixed "Lower" up vector).
- camera.c line 963 (PAL call at 0x80031098) is only the *normal* camera. Other code calls it directly and
  would bypass a call-site hook. In the main exe: dog.c, wolf2.c, wolf/unknown.c, evntmous.c, searchli.c, cat_in.c,
  p_lamp.c. In overlays: s11g 11g_demo/hindact, s05a m1e1act.
- **Plan change:** detour the function's `jr ra` (as the US widescreen patch does at its site) and post-multiply
  the head rotation (including roll) and the eye offset into chanl->eye / eye_inv. That covers every camera at once.
  Act only when chanl == &DG_Chanls[1] (PAL 0x800B2A44 = DG_Chanl(0)). Write `screen` (the FOV distance; 320 = default)
  there too.

## 2. Projection / FOV
- `DG_Clip(rect, dist)` → `gte_SetGeomScreen(dist)` = the H register of the PS1's geometry coprocessor (GTE). It is the
  same "clip distance" the GOG mod drives with fov_clip_distance. hfov = 2*atan(160/dist).
- `DG_AdjustOverscan(mat)` squeezes matrix row 1 by 58/64 (vertical). It is the natural place to also squeeze X
  for side-by-side stereo (see 3).

## 3. Framebuffers / stereo options
- 320x240 double buffer side by side in VRAM: disp x = 0 and 320 (`DG_SetDispEnv(0,0,320,240,320)`,
  `DG_InitChanlSystem(320)`). The drawn area is 320x224; buffer = GV_Clock (`which`).
- Chanl 1 (world) draw env offset = (160,112) = screen centre. Chanl 0 = background, chanl 2 = overlay/HUD.
  All are linked into one ordering table: `DG_DrawChanlSystem(which)` = `DrawOTag(&DG_Chanls[0].env1[which].tag)`.
  PAL ~0x80017EC8.
- Packets are double-buffered per `which`. The GPU draws the previous frame while the CPU sorts the next.
  So a GOG-style render-twice needs either a draw-sync between eyes or a second packet set.
- **Candidate A, side by side in one buffer:** sort the world twice per frame. The left eye uses draw-env ofs (80,112),
  clip x 0–159; the right uses ofs (240,112), clip x 160–319. Matrix row 0 is halved in the AdjustOverscan step, and
  each eye is shifted by eye_inv.t.x. One M2 D3D11 capture then holds both eyes. M2's upscaled internal resolution
  helps make up for the halved width. HUD/overlay (chanl 2) stays mono or is drawn once per half.
- **Candidate B, alternate eyes per game frame** (the GOG fallback): trivial, but only 12.5 fps per eye on PAL.
- VRAM x 640–1023 and y 240–511 hold textures (needs a VRAM dump to confirm), so there is no spare third buffer.

## 4. Frame rate
- `DG_FrameRate` = vsyncs per game frame (2 in play; 1 for some movies/loading; 3 for logo and some areas).
  Game logic is per frame, so forcing 1 would double game speed. PAL 25 fps is inherent; the US build gets 30.

## 5. Memory
- Top of RAM: mts stack config at 0x801FFF00. Sound data and stream buffers from 0x801E0000 / 0x801E7800.
  GV memory: pack buffers 2×188 KiB below 0x801E0000, then 428 KiB general heap.
  Zero runs in the PAL snapshot at 0x8016F840 (0x127C0) and 0x801C6820 (0x197E0) are in the heap/pack areas.
  **Not safe** for code.
- Code-cave candidates to verify over a whole play session (the probe can watch them stay zero or unchanged):
  0x80009FB0 (~4 KB, below the exe, in the kernel area), or unused debug strings in main-exe .rdata
  (0x80010000–0x800148B8). The US widescreen patch uses the .rdata route (cave at 0x80011054).

## 6. Round 2 (2026-10-07): more addresses, checked against the PAL Dock snapshot
| What | PAL | How confirmed |
|---|---|---|
| **Cutscene/demo flag** = GM_GameStatus bit 31 (`STATE_DEMO`), i.e. byte **0x800AC43B & 0x80** | 0x800AC438 | decomp: camera.c Act runs only `if (GM_GameStatus >= 0)`. The GOG "no control" byte 0x32279F is the top byte of GM_GameStatus (0x32279C). Snapshot value 0 at the Dock. |
| Other GM_GameStatus bits (g_define.h) | | 0x10 BEHIND_CAMERA, 0x40 CUT_IN, 0x4 NVG, 0x8 THERMG, 0x400 TAKING_PHOTO, 0x80000 MENU_DISABLE, 0x4000000 GAME_OVER, 0x40000000 PADDEMO |
| **linkvarbuf** | **0x800B5DF0** | Snapshot: [4]=disk 1, [8..10]=(-5756,1380,-1298) Snake's position, [11]/[12]=256/256 life, [14]/[15]=-1 weapon/item. Matches the DuckStation health cheat (0x800B5E06 = [11]). **The matcher's 0x800B5E0C (4/8 refs) is wrong.** |
| GM_Weapon / GM_Item / ammo | linkvarbuf[14] / [15] / [17+w] (2-byte entries) | linkvar.h |
| **GV_PadData[4]** (16 bytes each: status, press, release, quick, dir, analog, right_dx, right_dy, left_dx, left_dy) | **0x800B15F8** | Snapshot pattern of 4 idle pads: dir=-1, analog=2, sticks 0x80. Confirm with a button press in the probe. |
| GV_PadMask | 0x800AC3E0 | 1/1 refs |
| GV_Time | 0x800AC39C | 8/8 |
| DG_UnDrawFrameCount | 0x800AC3EC | 12/12 |
| DG_HikituriFlagOld (frame-skip flag) | 0x8009E504 | 2/2 |
| GV_ExecActorSystem / GV_NewActor | 0x80014DC8 / 0x80014F2C | fuzzy 0.86 / 0.80 |
| GM_SeSetMode | 0x800334CC | fuzzy 0.80 |
| DG_Clip (gte_SetGeomScreen) | 0x800175E0 | fuzzy 0.80 |
| DG_AdjustOverscan / DG_ScreenObjs / menu_draw_bar / GV_UpdatePadSystem | ~0x80017538 / ~0x8001CF50 / ~0x8003FA84 / ~0x80016724 | interpolated; byte-check before use |
| **Camera actor** (camera.c), act fn | act = **0x80030FA8** | Found by its "camera.c" name pointer. The `jal DG_MakeCameraMatrix` at 0x80031098 is inside it: consistent. |
| **Snake actor** (snake.c / sna_init) | act = **0x8005B628**; actor at 0x8016CAA8 in this snapshot (heap, changes) | Found by its "snake.c" name pointer |

**PS1 actors carry their source filename.** `GV_ACT` = {prev, next, act, die, free, **filename** (+0x14), runtime, count}.
- The GOG technique of finding the Snake or vehicle task by source name ports directly: walk the GV actor lists and
  compare the strings.
- Act-function addresses are an alternative key; they are stable per version.
- PS1 strings are short names like "snake.c", not full paths.

**Frame order** (libdg/dgd.c Act1):
1. `mts_wait_vbl(DG_FrameRate)`
2. **DG_StartFrame** (put the display env for the finished buffer; DrawOTag of the previous frame's OT)
3. GV_UpdatePadSystem
4. Actors run (camera, Snake, weapons; their order follows the actor lists)
5. **DG_EndFrame** (sort the new OT)

This is the frame boundary for the FrameViewRecord mailbox. Our MIPS hook in DG_MakeCameraMatrix runs during step 4.
The frame it shapes is drawn in step 2 of the *next* iteration and shown one vblank pair later, so
tag_delay_frames ≈ 1, the same as GOG. Re-measure on MC.

**DG_HikituriFlag** ("drag" flag): when a frame overruns while STATE_NOSLOW is set, the game skips its vblank wait.
- Render-twice cost shows up here first.
- Log DG_HikituriFlagOld as the "late frame" signal for the governor. It is the PSX equivalent of GOG's lateness test.

**Input injection, clean route:** write `GV_PadData[0]` (status/press/release, analog sticks) right after
GV_UpdatePadSystem, the same way the decomp's own demo playback feeds GV_DemoPadStatus. Alternatively, own the
DirectInput device, since we are dinput8.dll.

## 7. Hook point confirmed by disassembling the PAL snapshot (2026-10-07)
- DG_MakeCameraMatrix at 0x8001732C, confirmed instruction by instruction:
  - Prologue: `addiu sp,-0x50`; s2 = chanl.
  - `sh a3,0x50(s2)`: the `screen` FOV value is stored at chanl+0x50.
  - Epilogue: 0x8001752C–0x80017544, ending `jr ra / nop`.
- **Best hook:** the `jal 0x800940D8` at **0x80017524** (ApplyMatrixLV, the final eye_inv.t computation;
  delay slot `sw v0,0x18(sp)`).
  - Retarget it to `jal cave`. The cave calls 0x800940D8 itself, then runs our code with **s2 = chanl**
    (eye at +0x30, eye_inv at +0x10, screen at +0x50).
  - At that point the matrix is complete, so we post-multiply head rotation and eye offset and overwrite +0x50.
  - The detour is one word, and re-applying it is idempotent.
- Call site 0x80031098 confirmed:
  - a0 = 0x800B2A44 (DG_Chanls[1])
  - a1 = 0x800B88B0 (eye position)
  - a2 = a1+8 (target)
  - a3 = lw 0x800B88CC (zoom)
