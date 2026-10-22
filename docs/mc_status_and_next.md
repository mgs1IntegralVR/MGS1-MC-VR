# MC port: status and next steps (read this first; details in mc_port_findings_2026-10-07.md, mc_decomp_findings_2026-10-08.md and MGS1_MC_VR_port_report_for_GOG.md)

## Done (all on US 1.0, Quest 2 via Virtual Desktop; PAL tables kept in step)
- **Phase 1:** live MIPS hook in DG_MakeCameraMatrix proven (US + PAL).
- **Phase 2:** head tracking in the game's first person (owner-confirmed).
- **Phase 3 v2:** cutscene head-look; first-person look; world lock.
- **Walk v1-v3:** first-person walking (Snake's eyes; eye on the head joint); projection layer instead of quads. Scripted in-engine scenes (e.g. the escape) also stay in Snake's eyes, because the player actor exists.
- **ctrl v1 / v2:** Quest controllers in game and in the MC menus. No-wait picture hand-off. VD wired: "works really great".
- **stereo v3 (10-12; "This feels perfect now!")**: alternate-eye stereo with an exact eye and an exact head pose for every picture. Milestone 2 DONE.
- **body v1:** body follows head plus snap turning. **panel v1/v2:** in-headset settings panel. Milestone 4 DONE.
- **cutscene v3 (10-12 17:22; owner 17:30: "Yep that did it!")**: true (DMO) cutscenes through Snake's eyes. Milestone 3 DONE.
- **hud v1 / v2 (10-12/13):** wrist panels and the floating screen.
  - hud v1 put four separate panels on the left wrist; the owner rejected it ("mimic what I had going in the Integral VR project").
  - hud v2 uses the Integral layout: left wrist = item, LIFE, radar; right wrist = weapon and ammo. Grip-pose action spaces, two quads from one swapchain (left half / right half), shown when the wrist is inside a 28° gaze cone.
  - Owner 10-13: "the options on the wrist look better now." Wrist panels accepted.
  - Floating screen: GV_PauseLevel bits 1 (codec / movie / game over), 2 (pause), 4 (item/weapon menu) put the picture on a flat screen anchored where the user looks; the last tagged pose is held (g_keep_pose) when a type stays in VR. Each type is an option ([screen] codec / pause / menus). Not yet explicitly confirmed by the owner.
  - **Current build:** `mc_hud_v2\`, dinput8.dll 1,011,435 bytes.
- **Wireless check (10-13, hud v2 log):** game steady at 30 fps and PC GPU about 15-30%, but headset frames fell to 10-20 fps, xrEndFrame stalled 40-200 ms and the network send rate dropped to 0-20 Mbit/s. The PERF verdict said the PC had headroom while the headset waited. So the slowdown was the wireless link, not the mod.
- **motion v1 (10-13, built, awaiting test):** gun in hand. The DLL finds the socom.c / famas.c actor by its source name (GV_ACT+0x14), checks the layout (objs->root == Snake's hand joint = *(root_obj)+0x48+unit*0x5C), then writes a MATRIX at 0x8000AC40 each head pose and points objs->root (and the SOCOM laser / glow prims, +0x58 / +0x10C) at it. DG copies root into world at render, and the weapon fires from objs->world, so bullets leave the held gun's muzzle.
  - Gun axes (decomp + GOG): -Y = barrel, +Z = top; columns c1 = -forward, c2 = up, c0 = c1 x c2.
  - LOCAL -> game world: G = Base(heading) * S * Ry(-ref), the same chain as the eye. Position = game eye centre (head joint x/z, eye height y, plus offsets) + G * (grip - head) * units_per_m.
  - Orientation from the right AIM pose, position from the right GRIP, both located gun_predict_ms (40) ahead.
  - [motion] gun_in_hand, gun_predict_ms, gun_rotation_deg, gun_offset_mm, max_reach_mm. Also a panel item. Log line MOTION every 3 s.
  - Build `mc_motion_v1\`, dinput8.dll 1,024,927 bytes (md5 9d70e193...).
  - Known v1 limits: Snake's own arm stays where the animation puts it; the laser length is still measured from the hand joint.
- **Wireless (OPEN, parked by the owner 10-13):** the GOG mod runs smoothly wireless on the same Wi-Fi; the MC mod does not. Virtual Desktop drops to 45 Hz, xrEndFrame blocks 60-200 ms, and the stream falls under 30 Mbit/s within 1-2 minutes, in any mode (not only first person). Wired is smooth. Ruled out: the wrist panels (never shown in the mot2 session). Prime suspect: the PDH GPU-engine and WLAN queries every 2 s (the GOG mod has none). From mot3 on, `[motion] perf_counters` defaults to 0 (untested wireless). Next suspects: alternate-eye stereo, the double-wide 2880x1080 swapchain (GOG uses one swapchain per eye at 50% of the recommended size), and frame pacing (GOG frame_pacing=2).
- **motion v5 (10-13, built, awaiting test):** adds melee as in the GOG mod. Punch = either hand thrown forward faster than 2 m/s and more than 18 cm out -> Circle 70 ms. Chokehold (GM_CurrentWeaponId < 0, at stage name + 0x2C, US 0x800B752C) = hands together in front and still for 350 ms -> Square held; a jerk -> Square again. Rumble on punch, grab and gun trigger. Bits go through g_melee_bits into the pad on the next poll. Build `mc_motion_v5\` 1,037,434 bytes (md5 9e7bc18b...).
- **motion v5 tested 10-13 (wired):** punches and the chokehold work ("seems to work!"). The gun-in-hand layout check passed on FAMAS (actor found by name, hand joint 80176CA8, the root redirected). GM_CurrentWeaponId at 0x800B752C confirmed (FAMAS 1, chaff 8, none -1).
- **motion v6 (10-13, built, awaiting test):** fixes from the owner's notes.
  - Body clipping: Snake's body is hidden from the camera while walking in first person ([walk] hide_body; one byte of DG_OBJS.flag, re-set by the writer).
  - **Act-dispatch wrapper** (asm_act.py, 77 words at 0x8000AC80, mailbox 0x8000AC60; site US 0x80015138 `jalr $s0` 0x0200F809 -> jal 0x8000AC80; PAL 0x80014E14 `jalr $v0`, unverified). While box.c acts, GM_Camera.first_person = 1, which gives its own inside-the-box view. While the equipped weapon acts, the body is shown, so the gun, grenade and laser still draw. Emulated: 7/7 cases on US and PAL.
  - The grenade / stun / chaff model is in hand too (grenade.c: objs +0x28, root_obj +0x24, unit +0x4C). Throws still leave from Snake's hand joint.
  - Grips now open the item / weapon lists (L2 / R2; X / Y = L1 / R1 lean; a one-time ini migration, [controls] layout=2). While a grip holds a menu open, the view stays in VR and that wrist shows the game's own selection list (crops item_selection / weapon_selection, 1.6x wide).
  - Build `mc_motion_v6\` 1,044,182 bytes (md5 2cf1ad2a...).
- **motion v7/v8 (10-13):** the act wrapper never ran: 0 calls, though the site word reads back as our jal. The emulator does not execute patches inside GV_ExecActorSystem, so that approach is dropped. v8: the floating screen moves in front of you when it is more than 70 deg away for 1.5 s (the "black screen" was the screen anchored behind the user).
- **motion v9 (10-13, built, awaiting test):**
  - Body hidden from the CAMERA only: DG_OBJS.group_id (+0x2C) = 0x8000. DG_BoundChanl draws only if group_id == 0 or (group_id & DG_CurrentGroupID). The flag stays visible, so weapons, grenade, laser and box behave as in third person. Joints are still posed (DG_ScreenChanl). GM_ActObject re-groups only on map change.
  - Box model: box.c objs (+0x20) root -> a zero-size matrix at 0x8000AC60 while walking (orig = parent objs + 0x48).
  - Wrist HUD from game memory: linkvar = stage name + 0x10 (US 0x800B7510; vit [11], max [12], weapon [14], item [15], ammo [17+w], max [27+w], items [37+i]). Lists come from menuman.c MenuWork: state +0x2A (1 weapons, 2 items); Menu_Inventory item +0x1DC / weapon +0x1F0, alloc +0xC -> {count, selected, PANEL[] at +0x20 stride 8: id, num}. Text panels are drawn with GDI.
  - Radar cut in PS1 pixels (232,13,76,58), host = gx0 + x*fw/320, H*8/240 + y*H/240. Checked on frame_000.bmp: the whole radar with its frame.
  - Build `mc_motion_v9\` 1,051,818 bytes (md5 1e5dba51...).
- **Spare RAM:** 0x8000AC60 = zero-size matrix (v9; was the act mailbox). 0x8000AC80-0x8000ADB4 is unused since v9 = act mailbox + wrapper (watch starts 0x8000ADC0). 0x8000AC40-0x8000AC60 now holds the gun matrix (the watch starts at 0x8000AC80). 0x8000FAA0-0x80010000 is used at boot (NOT safe). 0x8000A940-0x8000B070 stayed empty through s00a; now used to 0x8000AC08.

- **motion v9 tested 10-13 (owner):** wrist HUDs not yet 1:1 with GOG; no lean; throwables not present. The log showed stun (7) and chaff (8) equipped but grenade.c never matched.
- **motion v10 (10-13, built, awaiting test):** template chain `patch_mot10.py` (parts A/B) → `patch_mot10c.py` (part C) → `mgs1mc_vr_mot10c.template.cpp` → fill.py → `mgs1mc_vr_mot10.cpp`.
  - **Throwables:** actor names were read 8 bytes long, so "grenade.c" (9) and "menuman.c" never matched. Now 15. Grenade / stun / chaff in hand like the guns. The throw starts at the hand: while it is in hand, grenade.c Work +0x24 (root_obj) points at a stand-in OBJECT at 0x8000ADC0 whose objs[unit].world = the hand matrix copy at 0x8000B040, and whose flag mirrors Snake's. Decomp: grenade.c reads root_obj only for objs->flag and objs[unit].world.t; the throw direction is still Snake's facing. Log: THROW.
  - **Lean (6DoF):** head movement since the last recentre (F6 / entering first person) goes through G = Base(heading)·S·Ry(-ref), times units_per_m (1000, as GOG position_scale), clamped to [walk] lean_max_cm (100), and is added to the mailbox eye offset. The gun's eye centre includes it, so the gun stays in the hand. Walk mode and cutscene POV. Log: LEAN.
  - **Wrist panels = GOG RenderPanels / BuildWristHudLayers:** 512x320 RGBA canvas, pixel font with outline, translucent black (alpha 150), LIFE bar max/8 x 125%, O2 when < 1024 (GM_O2 US 0x800AE1AC, trusted after it reads 1024), radar plus game item / weapon boxes cut from the picture on the GPU (alpha 235), mounts per wrist (0 top of hand, 1 back, 2 inside; open list → list_mount 2), gaze cone with 8° hysteresis and a facing test, width x1.3 for an open list. Quick-select on the grips (hold, stick up/down, release equips by writing linkvar 14/15; quick tap toggles the last). The game's own boxes, radar and LIFE are painted out of the main view (shader b1 masks, fill from the surroundings). Logs: HUD:, WRIST:.
  - Not ported yet: boss / other LIFE bars (GOG hooks menu_draw_bar; MC needs a MIPS hook), A-to-use an item from the list, the subtitle strip.
  - Build `mc_motion_v10\` 1,247,113 bytes (md5 09282ea0...). This toolchain builds v9 to 1,218,795 bytes, so the size change is the toolchain.
- **Spare RAM (v10):** 0x8000ADC0 stand-in OBJECT; its DG_OBJS sits at 0x8000B040 - 0x48 - unit*0x5C; 0x8000B040-0x8000B060 hand matrix copy; the watch is now 0x8000B060-0x8000B070.

- **motion v10 tested 10-13 (owner):** lean works. Throwables appear but stay in one spot. Radar missing on the left wrist. Wants the first-person overlays (box, gas mask, etc.) while in Snake's eyes.
- **motion v11 (10-13, built, awaiting test):** `patch_mot11.py` on mot10c; asm_pad.py now ends with `jal 0x8000ACB0` (98 words, ends 0x8000A928).
  - **Grenade froze:** after the throw-point redirect, grenade.c +0x24 = 0x8000ADC0, which fails k0()'s >= 0x10000 check. The scan logged "does not match the expected layout", dropped the actor, and the writer stopped (and never restored). Now a raw compare, and the restore is unconditional whenever the redirect is no longer wanted.
  - **Call routine** (asm_call.py, 38 words at 0x8000ACB0, mailbox 0x8000AC80: req +0, consumed +4, fn +8, a0-a3 +0xC.., 5th arg +0x1C -> 16(sp), v0 +0x20, alive +0x24, result seq +0x28, box flag +0x2C). Runs every frame from the pad routine; the DLL asks the game to call its own functions. Unicorn: 3/3 synthetic.
  - **First-person overlays** (the game's own actors need GM_Camera.first_person plus the invisible body flag; setting first_person globally would change the camera, sound and AI). US 1.0, found by their filename strings in RAM: NewSight 0x80074304 (current sight id 0x800A1D4C), NewGasmaskSight 0x800658F4, NewGoggleSight 0x8007AE84, GV_DestroyOtherActor 0x8001543C, GV_NewActor 0x800152CC. Emulated on two US snapshots: NewSight(cb_box 0xE2A9, flag, IT_Scope) -> sight.c actor; NewGasmaskSight -> gmsight.c. NewGoggleSight waits on the GPU (BIOS) in unicorn, so it is unverified. Box = NewSight with our flag at 0x8000ACAC (0 lets it die); mask and goggles are removed with GV_DestroyOtherActor; checked alive in the actor list / sight id. [walk] fp_overlays=1. Not for the Stinger or PSG1 (as the game). PAL: off. Log: FP:.
  - **Radar on the wrist:** cause unknown. F10 now also writes mgs1mc_vr_probe\wrist_NNN.bmp and logs the radar's cut rect, uv and blit rect. The wrist draw re-zeroes the b1 mask constants itself.
  - Build `mc_motion_v11\` 1,255,568 bytes (md5 05742fee...).

- **motion v11 tested 10-13:** the grenade is thrown from the hand (THROW: "thrown from your hand"). The F10 dump showed the game picture itself had no radar (game status 0): draw_radar skips it when GM_CONFIG_RADAR_OFF (0x800 in GM_Configuration = linkvar[2], US 0x800B7514; set by the new-game menu for Hard / Extreme) or PLAYER_INTRUDE (duct), unless GM_AlertMode (US 0x800AE178) is 1-3. The wrist pipeline is fine (boxes cut correctly). Overlays not tested yet (no gas mask / box / goggles used).
- **motion v12 (10-13):** the wrist radar follows the game's rule (HudData.radar_on); log "HUD: the game's radar is ...". Build `mc_motion_v12\` 1,257,372 bytes (md5 85601dfc...).

- **motion v12 tested 10-13 (owner):** "the box, goggles and mask now work as intended." First-person overlays through the call routine are confirmed on US 1.0.

- **motion v13 (10-14, built, awaiting test):** the owner's list (10-14: "action the rest"; the optional radar was dropped). `patch_mot13.py` on mot12. New generators: `mipsasm.py` (shared), `asm_call2.py`, `asm_bar.py`, `asm_hands.py`; asm_pad.py now jals 0x8000B100.
  - **Call routine v2** (asm_call2.py, code 0x8000B100, 10 slots of 0x28 at 0x8000B300, alive 0x8000B2FC). Runs changed slots in order every frame, so a line test and the getters that read its scratchpad results run in the same frame. Unicorn: 5 args, skip, run-once, 10 in order, s0/s1/sp kept. Slots: 0 overlays (box flag moved to 0x8000B500), 1-3 / 4-6 HZD right / left, 7 item use, 8 knock sound. Data: item list 0x8000B510, line ends 0x8000B540, hit points 0x8000B560.
  - **Boss / other LIFE bars:** asm_bar.py recorder at 0x8000A0C0 (table 0x80009FB0, 8 entries by MENU_BAR_CONF). The 5 `jal menu_draw_bar` sites (US 0x800411AC; sites 0x80041814/54/B8, 0x8004190C, 0x80041960) jal it, and it tail-jumps on with registers and stack untouched (unicorn). DLL: bars recorded in the last 0.5 s, not "LIFE" / "O2" (tagged conf = bit 31 cleared), drawn under O2 as in GOG; their screen area joins the LIFE mask. Log: BARS:.
  - **Item use from the wrist:** A with the item list open -> item_use_check(list, 0x20) (US 0x8003E5AC). Emulated on US RAM: a ration, count -1, LIFE 100 -> 256 (the max). The list stays open; releasing the grip changes nothing (GOG). Log: ITEM:.
  - **PSG1 / Stinger:** sight = GM_Camera.first_person (+0x22) == 1 and weapon 9 or 4. Aim by head: CONTROL rot / turn vx = -pitch*[scope] pitch_sign, vy = view heading. PSG1: MB_FOV 0 (the game's zoom), stereo held mono, the picture head-locked in a round lens of [scope] display_deg (shader opts.w). Log: SCOPE: / AIM:.
  - **Floating hands:** asm_hands.py at 0x80007A60 (HMB 0x800075E0) on DG_ScreenObjs' `jal DG_ScreenModels` (US 0x8001D998 -> 0x8001CEC4; DG_ScreenObjs 0x8001D88C matches the decomp). For Snake's body: worlds saved, parts 4 / 9 = the hand matrices (hand_gmat, the gun's chain, left aim pose added), others collapsed into their hand ([hands] left_arm_parts 7,8 -> left), DG_ScreenModels, worlds restored (the eye keeps joint 6). Unicorn: all cases. The body is shown (group restored) only while the routine is live; if it never reaches the body within 1.5 s the hands turn off. The box keeps its own condition (fp_walk).
  - **Wall touch / knock:** per hand per frame, eye -> hand: HZD_OnlineHazardCheck(hzd, from, to, 0xF, 4) 0x80029974, GetOnlinePoint 0x80029DB0, GetOnlineHazardAtr 0x80029D48 (scratchpad 0x1F800054-68: must be called in the same frame). hzd = (*(GM_PlayerControl)+0x2C)->+8 (US map record 0x800BA078, hzd 0x801575A0 in the dock). The hand stops wall_margin short of the hit (the gun with it). A touch faster than knock_speed_cm_s: sound = byte 0x800B9E48[((atr>>8)&3)*3] (GM_GetNoiseSound(atr, 0)) and NewAfterSe(sound, 6) 0x80062624 (plays it and raises noise 100/16, as Snake's own knock), if ((atr>>8)&7) < 4; plus rumble. Unicorn cannot run the HZD code (GTE mtc2). Log: HANDS:, KNOCK:.
  - Other US addresses found: GM_GetMap 0x80033360, GM_CurrentMap 0x800AE128, GM_SeSet 0x80034728, GM_SeSet2 0x80034838, GM_Configuration = linkvar[2], GM_AlertMode 0x800AE178, rifle.c ctor 0x8006A498, aam.c ctor 0x800696BC.
  - Build `mc_motion_v13\` 1,277,025 bytes (md5 30bb31da...).

- **motion v13 tested 10-14 (owner):** knocking works (KNOCK lines: sound 154, atr 0100). Item use works (a ration at full LIFE was refused by the game: count 2 -> 2). The hands did not appear, although the routine ran (1422 body redraws). The PSG1 / Stinger did not fire where aimed (head or stick). The boss bars were not tested (no boss).
- **motion v14 (10-14, built, awaiting test):**
  - **Hands:** DG_ScreenModels (US 0x8001CEC4) builds the screen matrices from the scratchpad TEMP[i] (0x1F800040 + i*0x20, written by DG_ApplyRots), never from obj->world. v13 wrote obj->world, so nothing changed. asm_hands.py now writes TEMP (117 words, n <= 24); obj->world is untouched, so there is no save/restore. Unicorn test updated (it reads the scratchpad like the real one).
  - **Sights:** game first person (first_person = 1) with a PSG1 / Stinger switched the routine to mode 1 (head-look added on top of the game's aim), so v13's head aim (a mode 2 writer block) never ran. Now `fp == 1 && weapon 9/4 && walk && aim_by_head` -> mode 2 ("MODE 2 (sight)"). An AIM log every 2 s shows head yaw/pitch, what was written, and what the game holds.
  - Build `mc_motion_v14\` 1,277,586 bytes (md5 6d712c10...).

- **motion v14 tested 10-17 (owner):** "sniper works and floating hands appear" -- PSG1 head aim (mode 2 sight) and the hands (scratchpad TEMP) are confirmed. Stinger and boss bars not yet reported.

- **motion v15 (10-17, built, awaiting test): "the rest"** (patch_mot15.py on mot14; includes panel15*.inc, writer15.inc, setpiece15.inc, wheel15_xr.inc, wheel_mc.inc; fill15.py)
  - **Options menu:** the settings panel is paged: VIEW / MOVEMENT / DIORAMA / HANDS & AIM / WRIST / SCENES / CONTROLS. Row 0 turns the page (stick left/right or A). New kinds: text choice (button remap, [controls] a..menu, right_stick_click also "recentre"), numbered choice (selector), normal/inverted (scope pitch_sign). Every change writes the ini and logs `PANEL: label -> value ([sec] key)`.
  - **Diorama** ([diorama] enabled=0 default, scale 20 (5..60), table_distance_cm 60, table_drop_cm 50, follow_radius 1500, follow_speed 30, cut_units 4000, reorient_on_area_change 1, snake_eye_height 1500, turn_step_deg 30): DLL mode 3 = routine mode 2 with a stand-in body DIO_FAKE = 0x8000B31C, whose "head joint" x/z (0x8000B5A0 / 0x8000B5A8) the writer fills with absolute world coordinates; y = GM_SnakeCamera.y + off.y. Eye = table centre + Base(table) * S * (recentred head offset + (0, drop, distance)) * units_per_m * scale; rotation Base(table) * head. Right stick: L/R turns the table in steps, U/D zooms. Stereo half and the movement hold are multiplied by the scale. Unicorn: stand-in body places the eye at absolute x/z. Log: MODE 3, DIORAMA: (every 5 s).
  - **Camera routine v12** (asm_cave12.py, 186 words, ends 0x8000A768): mode 1 adds int16 x,y,z at DIO 0x8000B580 to eye.t. Unicorn differential test vs v11 (US + PAL, 40 trials each): identical except that. Used for lean in head-look views ([walk] third_person_lean=1): head offset in the game camera's axes (E * M^T), off in cutscenes. Zeroed at install.
  - **Set pieces** ([setpiece] snake_eyes=1): actor by source name jeep_sne.c, snake18.c, rope.c, torture.c / sne_03c.c (only while sna_init is gone); pkjp_sne.c / sne17a.c listed but PC-only (never match on PS1). Snake's body = the largest DG_OBJS pointer in the work (+0x20..+0x800) with 8-40 parts and joint 6 within 50-2500 units of the root. Mode 2 through the cutscene path (joint eye height, PadOrg off, cut eye_forward); heading follows atan2(m02, m22) of the body root every 100 ms. Log: SETPIECE:, MODE 2 (set piece: ...).
  - **Hologram wheel** ([wrist_hud] selector=1, wheel_size_cm 30, wheel_height_cm 12, wheel_stereo_deg 4): GOG wrist_wheel.cpp unchanged; GOG game_models.cpp parsers fed from the PS1 RAM: GV cache table found by shape (US 0x800AF668: 128 x {id, buf}, id % 128 = slot), KMDs 'k' and item.rpk 'r' resident, relocated pointers turned back into offsets. Offline test on the US snapshot: 12 / 12 models, 35 / 35 icons, both wheels drawn. Models are untextured (hologram cyan): textures are in PS1 VRAM. The stick points at a slot while the grip is held (SlotForStick); release equips; A uses an item. Quad layer(s) above the wrist facing the head, per eye when the model is drawn in stereo. Log: WHEEL:.
  - Build `mc_motion_v15\` 1,350,509 bytes (md5 0fde8a55...), root dinput8.dll md5 verified.
  - **True stereo (both eyes per frame, B4b): not in v15.** See the design note below; it needs its own probe rounds.

- **motion v15 tested 10-17 (owner):** could not find the options panel (expected it on the left stick click, as in GOG); the hologram was not the GOG one (GOG's default is the 3D carousel, selector=2; v15 had ported the 2D wheel). Diorama untested (needs the panel). Log: wheel loaded 12/12 models, 35/35 icons from the GV cache.
- **motion v16 (10-17, built, awaiting test):** (patch_mot16.py + car16_xr.inc)
  - VR settings like GOG: hold L3 ~0.9 s (buzz) opens; L3 tap closes; L3 tap otherwise = the game's L3 mapping (sent on release). Left menu hold 0.7 s still works. Panel: triggers = page, Y = default.
  - GOG 3D carousel ported (DrawCarouselTile / DrawCarouselLabel into a 1152x640 atlas, GOG's tile cache, per-entry quads on a 26-degree arc, cover-flow turn, stereo front tile, name plate). Stick flick on the holding hand steps (260 ms repeat); a sideways flick of the other hand (> 0.7 m/s, re-armed under 0.35) steps 1-3. selector=2 written once (selector_v16). Plate has no game description text (GOG reads it from the PC exe).
  - 2D wheel placed with GOG WheelPoseFor (unit-tested: +Z faces the head, X level).
  - Pause while choosing: GV_PauseLevel |= 4 while a list is open (from pl == 0 only), cleared on close, hidden from the DLL's own pause logic. Log: SELECT:.
  - Not ported yet: GOG tap-to-open gauntlet (tap a wrist with the other hand), fingertip / pointing on the 2D wheel.
  - Build `mc_motion_v16\` superseded by `mc_motion_v17\` = v16 + no list on the wrist panel while the wheel / carousel floats (GOG FloatOn); 1,378,462 bytes, md5 2494f5bc..., root md5 verified.

- **motion v16/v17 tested 10-17 (owner):** "very nearly 1:1 to GOG".
- **motion v17 code, build `mc_motion_v18\` (10-17, awaiting test; stripped 563,712 bytes, md5 4de13ab5..., root md5 verified):** (patch_mot17.py + sub17_xr.inc)
  - R3 (GOG): double-press cycles VR -> DIORAMA -> FLAT SCREEN (routine mode 0, the floating screen) -> VR (g_view_mode; the ini's diorama switch sets it too); hold 0.7 s or a single tap (after 350 ms) = recentre.
  - L3 + R3 together: DirectInput keys E (0x12) + M (0x32) for 300 ms = back to the Master Collection menu (also during gameplay).
  - Descriptions: the game's own item / weapon texts from the PS1 RAM (US 0x800A0B30 items 24+2 frozen, 0x800A0D1C weapons 10; found by shape: pointers to strings starting B0 14). Codes: D0 15 / 80 7C new line, 80 80 "(O)", 80 81 "(X)", 80 00 "[]" (81 xx skipped). Offline: 34 / 34 decoded.
  - Cutscene subtitles (GOG): in the cutscene POV the strip ([subtitles] strip 0,0.7667,1,0.1667 of the 4:3 frame) goes onto a 1024x128 head-locked panel 20 deg below gaze; shader mode opts.y=3 keys the text (alpha from brightness, dark halo) so the panel is invisible without a caption; the strip is masked out of the main view. Shader checked with dxc (ps_6_0).
  - GitHub: source tree (src/mgs1mc_vr.cpp single file, OpenXR headers, tools/mips, docs, build.sh / build.bat, MIT) and release zip (dinput8.dll, openxr_loader.dll 1.1.40 x64, README.txt, licences). Copies in the game folder's github_release\.

- **v18 tested 10-18 (owner):** L3+R3 opens the Master Collection menu. Problems: cutscene shimmer (Sniper Wolf's death: the scene overlaid several times); a thin "slit" of the cutscene next to the subtitle panel; doubling while choosing on the wrist; the Master Collection menus' sides warped.
- **motion v18 code, build `mc_motion_v19\` (10-18, awaiting test; 564,224 bytes, md5 05fdf6eb..., root md5 verified):** (patch_mot18.py)
  - Cutscenes are mono by default ([cutscene] stereo=0): alternate-eye stereo shows each eye a different moment of a camera the game itself moves, which head-pose tags cannot correct.
  - The subtitle strip in the main view is filled black (it had used the HUD in-paint, which smears the scene's bottom rows into the bar); the panel fades its top and bottom rows.
  - Held mono while the wrist selection pauses the game (the cause of the doubling is unconfirmed; this is a likely fix).
  - Master Collection menus (PS1 game not running): the whole 16:9 picture, no 4:3 crop.
  - GitHub zips refreshed with this build.

- **v19 tested 10-18 (owner):** the mono meant for cutscenes was only for Snake's-eyes (first-person) cutscenes. Doubling goes back and forth when looking at the floating hands or around, and on the item / weapon carousel.
- **motion v19 code, build `mc_motion_v20\` (10-18, awaiting test; 565,248 bytes, md5 0b2a72b7..., root md5 verified):** (patch_mot19.py)
  - Mono only in the cutscene POV (g_cut_pov); director-camera cutscenes keep stereo.
  - Hands-moving hold: either controller faster than [stereo] hold_hand_cm_s (25) while the hands are drawn (mode 2) -> mono for hold_ms. The hands are in the game picture and move between the two eyes' frames (an alternate-eye limit; only true stereo removes it). STEREO log: "hands moving".
  - Carousel / wheel per-eye model turn: default 0 (wheel_stereo_deg, moved once).
  - GitHub zips refreshed.

- **v20 tested 10-18 (owner):** fine; "move on to the next milestones".
- **motion v20 code, build `mc_motion_v21\` (10-18, awaiting test; 569,856 bytes, md5 b7ddc50c..., root md5 verified): TRUE STEREO probe (milestone 6)** (patch_mot20.py, asm_ts.py, asm_cave13.py, fill20.py)
  - Design: the game double-buffers side by side in VRAM (DG_InitChanlSystem(320)) and keeps two of every renderer resource (OT[2], packs[2], GV memory 0/1). DG_EndFrame's sort -> TS_SORT (0x8000A150, 139 words): game sort (left eye, routine v13's stereo block forces -half and code 1), then DG_ClearChanlSystem(b = 1-GV_Clock), right-eye marker linked into CH2 ot[b], eye.t += 2h * column 0 and eye_inv.t.x -= 2h, GV_Clock = b (prim.c / trans.c index packs by GV_Clock), DG_SortChanlSystem(b), all restored. DG_StartFrame's DrawChanlSystem -> TS_DRAW (0x8000B5E0): both OTs, then DrawSync(0) (StartFrame clears OT b next). StartFrame's PutDispEnv -> TS_DISP: disp.x 0, w 640 (320 when off). TSMB 0x8000B5C0.
  - US addresses: DG_StartFrame 0x800177DC (PutDispEnv jal 0x80017874, DrawChanlSystem jal 0x80017894), DG_EndFrame 0x800178F4 (sort jal 0x80017904), DG_SortChanlSystem 0x800186E0, DG_ClearChanlSystem 0x80018540, DG_DrawChanlSystem 0x800184EC, PutDispEnv 0x80092B20, DrawSync 0x800922F0, g_dispenv 0x800B2D78 (disp 320,0,320,240; screen 0,8,256,224), gClipHeights 0x800AE0D8 {0, 320}. DG_CHANL: eye_inv +0x10, eye +0x30, screen +0x50.
  - Unicorn: asm_ts.py test (TS off / on for both buffers: call order, GV_Clock during the right sort, eye shift, restore, marker linking; draw both + DrawSync; display 640 / 320); routine v13 vs v12 (TS off identical; TS on always the left eye).
  - DLL: [stereo] true_stereo=0 default + panel "True stereo (test)"; on only in VR views, not paused / sight / Snake's-eyes cutscene. Capture reads a half-width marker at the start of each half; a picture with left + right markers = side by side: each eye samples its own half (same pose); HUD masks, wrist radar / boxes and the caption strip map into the HUD (left-eye) half. Log: TRUE STEREO lines (right-eye sorts, second draws, display calls, side-by-side pictures).
  - Unknowns the test answers: the Master Collection emulator showing a 640-wide display; the frame rate with two sorts per frame; DrawOTag timing (tearing); actor-made screen prims (effects) one frame old in the right eye; HUD / overlay only in the left eye.

- **v21 tested 10-21 (owner):** the Master Collection menu (L3 + R3) no longer opened in game (log: combo fired 9 times, sent through SendInput only: the game was not reading DirectInput then). The left trigger "changed Snake's camera": it was Triangle = the game's first-person view (old default layout). True stereo not yet tried (true_stereo=0).
- **motion v21 code, build `mc_motion_v22\` (10-21, awaiting test; 571,904 bytes, md5 39b4471d..., root md5 verified):** (patch_mot21.py)
  - GOG button layout by default (moved once, [controls] layout=3, only from the old defaults): A Circle (action), B Triangle (first person), left trigger Cross (crawl), right trigger Square (weapon); X / Y L1 / R1; grips lists. In the Master Collection menus A / B = Enter / Escape whatever they map to.
  - E + M held 500 ms through DirectInput AND SendInput; the game window brought to the front first. Log: "MENUS: E + M held".
  - GitHub zips refreshed (README controls table updated).

- **v22 tested 10-21 (owner):** Stinger head aim and the REX boss LIFE bar CONFIRMED. L3 + R3 opens the Master Collection menu (a few seconds to load). Floating hands gone (log: hands switched off for the session when the MC menu paused the game: wall-clock 1.5 s check). Missing: the GOG codec ear gesture. Asked: where the settings menu is (hold L3). Reported: a blurry "gloss" in some Snake's-eyes cutscenes (cutscene not yet named).
- **motion v22 code, build `mc_motion_v23\` (10-21, awaiting test; 572,928 bytes, md5 fa5fefa0..., root md5 verified):** (patch_mot22.py) codec ear gesture (left grip within codec_ear_cm 17 of the left ear -> Select, the grip not a list; [controls] codec_ear_gesture); hands liveness in draw-routine calls (300 without Snake's body) and retried after 10 s. GitHub zips refreshed.

- **Blur (10-21):** the gloss is in the Snake's-eyes cutscenes with Liquid before REX: the game's afterimage effect (okajima/blur.c, blurpure.c: every frame the previous frame, offset, semi-transparent over the new one). Here the previous frame has another head pose / eye. **motion v23, build `mc_motion_v24\` (574,976 bytes, md5 54da6fa9..., root md5 verified):** in VR views (mode 1-3) those actors' act -> NOPFN 0x8000B6A0 (jr ra), given back in the flat view or with [cutscene] blur_effect=1 (panel: Scenes). Log: BLUR:. GitHub zips refreshed.

## Next
- **Milestones left (10-21):** 1. true stereo test (v21+, VR Settings -> View -> True stereo (test)); 2. (done 10-21: Stinger and boss bars confirmed); 3. PAL pass (true stereo, descriptions and the model cache were only checked on US 1.0); 4. SteamVR / Quest Link test; 5. GOG extras not ported: tap a wrist to open its list, fingertip / pointing on the 2D wheel, alert ring, elevator VR; 6. GitHub v1.0 release.
- Confirm the floating screen for codec calls and pause.
- Still to confirm: Stinger head aim, boss bars on the wrist (needs a boss). v15: options pages + remap, diorama, set pieces (jeep / rope / torture / REX), hologram wheel, third-person lean. Earlier note on wrist parity: boss bars (MIPS hook on menu_draw_bar), A to use an item from the wrist list, the subtitle strip.
- **Motion controls** (owner priority): hands, gun in hand, motion aim, throws, melee, wall knock (milestone 8).
- Then true stereo (6) and vehicles/set pieces (7).

## Cutscene design (milestone 3, B6)
- **Camera routine v11** (asm_cave11.py, magic 'MCVB'). The stereo block is at 0x8000A9D0 (142 words, ends 0x8000AC08).
  - It always stores ra at PMB+0x18 (0x8000A798).
  - It stores s3 (MGSDEMOACT) at PMB+0x14 and GV_Time at +0x1C only when (ra & 0x1FFFFFFF) == FrameRunDemo's hook2 return (US 0x0007D8B4, PAL 0x0007BF48).
- **Two traps found:**
  - **v1:** the camera actor calls the routine a second time each cutscene frame and overwrote the record.
  - **v2:** cutscene code runs from the **KUSEG mirror**: ra = 0x0007D8B4 (not 0x8007D8B4), and pointers may be 0x00xxxxxx. Every pointer is now normalised to KSEG0 in the DLL (K()), and comparisons ignore the segment.
- **Cast lookup:** MGSDEMOACT+0x30 DMO_DEF (n_models at +0x10, DMO_MDL[] at +0x18, stride 0x14, filename hash at +0xC); +0x34 DEMO_MODEL[] (stride 0x1A4; DG_OBJS at +0x7C).
  - Snake = filename 992D ("snake"; the tuxedo save also used 992D) or 34C2 ("sne_nude"), set in [cutscene] snake_models.
  - Visible = !(objs.flag & 0x80).
- **POV:** routine mode 2 on the demo Snake's objs (joint 6).
  - y: off.y = joint y − GM_SnakeCamera.y + eye_up.
  - PadOrg is off.
  - Facing = atan2(m02, m22) of the joint-6 matrix. Re-faced on a new cutscene, Snake back on set, or a joint jump over 1500 units.
  - Off set: director camera plus head-look.
- **Log sample (d00a):** cast [992D,6DB4] → Snake model 0. Re-faces on cuts. Some casts don't include Snake (director camera).
- A RAM snapshot of a cutscene was saved on the PC (mgs1mc_vr_probe/ram_*_us10_cutscene.bin), for later analysis.
- **[cutscene] ini:** snake_eyes=1, eye_forward=100, snake_models=992D,34C2. Also a panel item.

## Earlier designs
- **body v1:** GM_PlayerControl → CONTROL (US 0x800AE16C / PAL 0x800ACA4C; rot.vy at +0xA, turn.vy at +0x4E). GM_PlayerStatus: US 0x800AE1C8, PAL 0x800ACAA8.
- **Panel:** hold the left menu button 0.7 s. It is a quad layer only while open, drawn with GDI into a texture. Changes go to the ini and reload at once (g_ini_force).
- **Stereo (milestone 2):**
  - Eye = GV_Clock.
  - Marker: three 3x2 TILEs (eye colour, magenta, pose seq 6 bits), linked into DG_Chanls[2].ot[GV_Clock][0] once per frame and decoded from the captured pixels. It gives the exact eye and the exact head pose (the old ring guess had been 100% wrong by about 4.5 poses, which caused the doubling and the shimmer).
  - SMB at 0x8000A940.
  - Per-eye textures; view pose ±IPD/2; the crop hides rows 0-1.

## Revised timeline to GOG feature parity (made 2026-10-08)
| # | Milestone (GOG ref) | Status | Target | Risk |
|---|---|---|---|---|
| 1 | First playable (B0-B3, B5) | done (wired) | | |
| 2 | Stereo, alternate-eye (B4a) | **done 10-12** | | |
| 4 | Controls (B10) | **done 10-12** | | |
| 3 | Cutscene Snake's eyes (B6) | **done 10-12** | | |
| 5 | Wrist HUD and virtual screen for codec/menus/pause (B9) | wrist panels ported 1:1 + quick-select (v10, awaiting test); floating screen awaiting confirmation; boss bars left | ~Nov 8 | low |
| 6 | True stereo, both eyes per frame (B4b) | 4-6 sessions | ~Nov 18 | **high** |
| 7 | Gameplay Snake's eyes for vehicles, rappel, torture, REX (B7) | 3-5 sessions (scripted scenes partly free already) | ~Nov 25 | medium |
| 8 | Hands, gun in hand, motion aim, throws, melee, wall knock (act call US 0x80015138) (B8) — owner priority | 8-12 sessions | ~Dec 15 | **high** |
| 9 | PSG1 scope, sky lock, small items, PAL pass, SteamVR test, packaging (B11-B12) | 3-4 sessions | ~Dec 28 | low |
| 10 | **Options menu** (owner, 10-13): an in-headset menu for the controls and for changing the ini settings (builds on the settings panel, panel v1/v2) | not started | | low |
| 11 | **Item / weapon selection hologram** (owner, 10-13): new in the GOG mod; port it to MC. Needs the updated GOG source (the kit's src/gog_mod_source predates it) | not started | | medium |
| 12 | **Diorama mode** (owner, 10-13) | not started; check the GOG source for its design | | medium |

## Design notes
- Frame order (decomp):
  1. GV daemon: GV_Time++ and GV_Clock ^= 1.
  2. DG_StartFrame: draws OT[1-W], clears OT[W].
  3. GV_UpdatePadSystem (pad hook).
  4. Actors, including the camera and FrameRunDemo: our routine.
  5. DG_EndFrame: sorts W.
- Addresses: US GV_Clock 0x800AE098, PAL 0x800AC978. DG_Chanls[2] US 0x800B4350, PAL 0x800B2C30; sizeof DG_CHANL is 0x1EC.
- DG_OBJS: flag at +0x28, n_models at +0x2E, objs[] at +0x48 (DG_OBJ stride 0x5C). Joint 6 world matrix at +0x270, t at +0x284.
- Pad routine (asm_pad.py, 0x8000A7A0); pad mailbox 0x8000A780: +0..+13 pad, +14..+1F routine call record.
- Keys: F5 stereo, F6 recentre, F7 walk mode, F8 tracking, F9 RAM snapshot, F10 picture, F11 headset test colour. Hold the left menu button for the settings panel.
- Ini sections: [phase2] [phase3] [walk] [stereo] [cutscene] [controls] [menus] [screen] (codec, pause, menus, screen_distance_m, screen_width_m) [wrist_hud] (enabled, width_mm, gaze_cone_deg, …).
- Build: x86_64-w64-mingw32-g++ -O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ -I openxr-sdk/include mgs1mc_vr_hud2.cpp dinput8.def -ld3d11 -ldxgi -lpdh -luser32 -lgdi32.
  - Template chain: st3 → patch_body1 → patch_panel1 (plus the %ls fix) → patch_cut1 → patch_cut2 → patch_cut3 → patch_hud1 → patch_hud2 (mgs1mc_vr_hud2.template.cpp).
  - Placeholders: @@CAVE11_*@@ / @@PADCAVE_*@@.
  - Emulation harness: scratchpad emu12.py (python3 -I; RA/REC environment variables).

## True stereo design note (B4b, 10-17)
- GOG (render_twice.cpp): snapshot the frame's ordering table, sort the scene twice (eye offset each time) and DrawOTag twice into two eye buffers, all inside one game frame.
- MC: the PS1 runs inside M2's emulator; we cannot call the GPU twice from the DLL. Options, all needing probes:
  1. MIPS: after DG_EndFrame sorts OT[W], re-run DG's sort for the world channel with the eye shifted into a second OT and DrawOTag it to a second VRAM area (needs spare VRAM / a second display area, and 2x GPU time in the emulated GPU at 30 fps).
  2. Emulator side: capture the GPU command list per frame and replay it with a shifted GTE result -- not possible: the transforms are already baked into the primitives.
  3. Keep alternate-eye (current) and tighten the hold (done).
- Risk high; plan: 1-2 probe rounds measuring whether a second DrawOTag fits in the frame (log the emulated frame time), then decide.
- **v24 tested 10-21 (owner):** gloss still in the d16e scene. Log: BLUR lines present (blur.c actors were paused), so blur.c was not the whole story. d16e also uses DEMO_UNSHAPEVIEW = takabe/focus.c (NewFocusView): depth-of-field, the picture shifted 2 px laid semi-transparent over near/far depth ranges every frame. **motion v24, build `mc_motion_v25\` (575,488 bytes, md5 dec9a993..., root md5 verified):** focus.c added to the paused list (same NOPFN act swap, same [cutscene] blur_effect switch, panel item renamed "Game's blur/focus in VR"). Diagnostics: `ACTORS: '<name>' running in stage '<stage>'` logged once per name per stage.
- **True stereo resolution (owner 10-21):** looks lower resolution. Cause: MC upscales the PS1 picture to its window (backbuffer 1920x1061, the 4:3 picture ~1415 px wide). In true stereo the PS1 display is 640 wide (both eyes side by side), so each eye gets ~707 output pixels across, half of normal. Not fixable inside the PS1 side; only a larger MC output (4K window / DSR) gives each eye more. True stereo stays off by default, marked test.
