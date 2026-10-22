# MC port — PAL (SLES-01370, English EU) address map

Derived 2026-10-05. Method: built the FoxdieTeam decomp's Integral main exe locally (SHA256
`4b8252b6…3a2f`, exact match, via PsyQ + wibo), then matched its functions into a live PAL RAM
snapshot from the Master Collection (664 of 1742 functions matched: 212 exact, 452 fuzzy), and paired
the data references inside aligned functions. Values marked "checked" were also sanity-checked
against the snapshot contents (sensible camera/eye/target values at the Dock).

All addresses are PS1 RAM addresses; at runtime add them (masked with 0x1FFFFF) to the PS1 RAM base
the probe finds each launch.

| What | Integral (decomp) | PAL (MC) | Evidence |
|---|---|---|---|
| gp (start of .sdata) | 0x800AB2E4 | 0x800AC350 | 24/24 gp refs agree |
| GM_GameStatus | 0x800AB3CC | 0x800AC438 | 83/83 refs |
| GV_PauseLevel | 0x800AB928 | 0x800AC980 | 28/28 refs |
| GM_CurrentMap | 0x800AB9B0 | 0x800ACA08 | 17/17 refs |
| GM_PlayerControl (CONTROL*) | 0x800AB9F4 | 0x800ACA4C | 1/1 |
| GM_PlayerPosition (SVECTOR) | 0x800ABA10 | 0x800ACA68 | 5/5, checked |
| GM_PlayerStatus | 0x800ABA50 | 0x800ACAA8 | 16/16 |
| DG_Chanls | 0x800B1800 | 0x800B2858 | 4/4 |
| GM_CameraList[8] | 0x800B7718 | 0x800B8760 | region delta 0x1048 |
| GM_SnakeCamera (rotate2 @+0x28) | 0x800B77B8 | 0x800B8800 | refs + checked |
| GM_Camera (first_person @+0x22) | 0x800B77E8 | 0x800B8830 | refs + checked (zoom 320, track 7500) |
| gUnkCameraStruct2 = final eye/target/zoom | 0x800B7868 | 0x800B88B0 | checked; it is what the view matrix is built from |
| DG_MakeCameraMatrix (fn) | 0x800172D0 | 0x8001732C | call site found |
| **View commit call site** in camera `Act` | — | jal at 0x80031098 | `DG_MakeCameraMatrix(0x800B2A44, &eye, &eye+8, zoom@0x800B88CC)` |
| Stage-name string (CT anchor) | — | 0x800B5DE0 | matches community CT, read `s00a` at Dock |

## PC-port equivalents (for porting the Integral mod's logic)
- PC `GM_SnakeCameraWork` @0x593FA0 ↔ PSX GM_SnakeCamera; PC rotate2 0x593FC8 ↔ PSX +0x28.
- PC "View FROM/TO" (layer 3, 0x593F60/68) ↔ PSX gUnkCameraStruct2.position/target.
- PC view-commit hook 0x53C47 ↔ PSX: just before `jal DG_MakeCameraMatrix` at 0x80031098.

## Notes
- Emulator keeps a second, lagging copy of PS1 RAM; always use the copy that changes.
- Probe v4 logs these values live (read-only) to confirm the map in play.

## Cross-checks from public cheat data (2026-10-07)
- DuckStation chtdb `SLES-01370.cht`: "Play In 1st Person Perspective" writes `800AC4AC = 0x20`.
  That is exactly our mapped **GM_CameraFlagsOrg** (Integral 0x800AB440 + 0x106C). Independent confirmation of the sdata mapping.
- Same file: Infinite Health `800B5E06` = stage block 0x800B5DE0 + 0x26, matching the community CT layout.
- GameHacking US (SLUS-00594): Closeup `800B9F70/78 = 0x240` and 1st Person `800B9F72/7A = 1` are
  GM_Camera.zoom (+0x20) and .first_person (+0x22) for two US disc revisions →
  **US GM_Camera = 0x800B9F50 (rev A) / 0x800B9F58 (rev B)**. Field layout identical to decomp and PAL.
  US pad data (joker) at 0x800B2D58/60.
- chtdb `patches/SLUS-00594.cht` widescreen: replaces `jr ra` at **0x8001F6E0** with `j 0x80011054`, a code cave
  (in .rdata strings) that scales a SVECTOR arg (a1) X and Z by 3/4, then returns. A projection/aspect hook — relevant
  to VR FOV. Function identity not yet pinned (needs a US snapshot). Uses DuckStation, so it is not proof that live code
  patches execute under M2's emulator.

## Autosplitter (bstnsolari/livesplit-mgs1-autosplitter, 2026-10-07)
- MC PS1 RAM rule: the committed region whose RegionSize == 0x211000; RAM starts at region base + 0x40.
- Version id: ASCII at RAM+0x10000 (0x50 bytes) contains the product code, e.g. "MGS2.EXE..MGS1.EXE..BESLES-01370".
  Integral VR = header starts "SLPM_862.49"; US 1.0 vs 1.1: "KETCHUP" at RAM+0xADC98 means 1.0.
- Per-version tables for SLPM-86111/86247/86249, SLUS-00594 (1.0/1.1), SLUS-00957, SLES-01370/02136/01734/01507:
  Location, Progress, Frames, ControllerInput, NoControl, InMenu, boss HP, etc. (Location PAL = 0x800AC430).
- OPEN: probe v3 saw two copies (alloc+0xA060 changing, alloc+0xA040 = region+0x40 lagging?). Probe v5 logs the picked
  copy vs the region-rule copy side by side (COPIES lines) to settle which one the game writes and which a mod must use.
- Over-the-shoulder CE table: forces GM_Camera.first_person (US 0x800B9F72) + zoom, then uses DuckStation's own GTE
  free-camera (emulator-side) for the offset. FPV look angles = AOB "?? ?? C2 01 C2 01 01 00 00 00 00 00 00 00 00 00 00 00 E0 FF"
  +0x18 (up/down) / +0x1A (left/right). Letterbox off: US 0x800AF47C (also on GameHacking).

## RetroAchievements code notes (566 notes, US-centric; full text in retroachievements_mgs1_code_notes.txt)
VR-relevant, all PS1 RAM offsets (add 0x80000000):
- **US camera block confirms our struct map exactly** (1.0 / 1.1): GM_SnakeCamera ("camera tracker", freezing stops
  the camera following Snake) 0x0B9F20 / 0x0B9F28; GM_Camera.track ("3E08 = 1st person") 0x0B9F6C / 0x0B9F74;
  GM_Camera.first_person 0x0B9F72 / 0x0B9F7A; zoom-flag bits at 0x0B9F70 (bit4 wall zoom, bit5 heliport cam);
  "action cam vs wall" 0x0B9F68 (1.0). => US 1.0 = rev A (GM_Camera 0x800B9F50), US 1.1 = rev B (0x800B9F58).
- Player position X/Y/Z: 0x0AE188 (1.0) / 0x0AE190 (1.1) = GM_PlayerPosition.
- HUD/cutscene: 0x0ADB3F (1.0) / 0x0ADB47 (1.1): 00/10 gameplay, 80 cutscene. PAL & JP via secondary pointer at
  0x0075D0 +0x12EF (US +0x1347).
- Codec open / paused / item menu bits: 0x0AE0A8 (1.1) (01 codec, 02 paused, 04 weapon/item menu).
- Codec mode ID: 0x0BFCD8 (1.0) / 0x0BFCE0 (1.1).
- Game mode 2 (16-bit, all regions except JP): 0x117084 — 0000 campaign, 2a2f demo theater, 4d47 main menu, a8a1 VR mission...
- Game mode: 0x0B4B0F / 0x0B4B17 (10 = main campaign, 02 = VR missions).
- Input buttons: 0x0AE0D0/D1 (1.1).
- Region pointer at 0x00EA50: value 800AEB68 US1.0, 800AEB70 US1.1, 800AD448 Europe, 800ACCD8 Japan, etc.
- Camera X/Y 32-bit: 0x0B4190 / 0x0B4194 (1.1) — unidentified; candidate view-channel eye translation.
- Debug menu: write 0x85E4 to 0x116F00 (Previous Operations -> debug menu) or 0x116F84 (VR Missions -> debug menu), US.
