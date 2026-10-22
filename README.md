# Metal Gear Solid — Master Collection VR

A VR mod for **Metal Gear Solid (PS1) in METAL GEAR SOLID: MASTER COLLECTION Vol. 1 on Steam**. It is the Master Collection port of the [MGS1 Integral VR mod for the GOG PC release](https://github.com/mgs1IntegralVR/Metal-Gear-Solid-Intergral-VR) and brings the same features to the Master Collection, where the game runs as the original PS1 code inside Konami's emulator.

The mod is a `dinput8.dll` proxy. It finds the emulated PS1 RAM, places small MIPS routines in unused memory (camera, controller pad, renderer hooks) and shows the game in your headset through OpenXR.

> Tested on a Meta Quest 2 over Virtual Desktop, with the **US (SLUS-00594) version** of Metal Gear Solid. The PAL tables are kept in step but untested. Steam Link (SteamVR) on the Quest 2 also tested. Other headsets and Quest Link are untested.

## Features

- **Snake's eyes**: head-tracked first person in normal gameplay. Walk where you look, with smooth or snap turning, and the body follows your head. Lean with 6DoF.
- **Stereo 3D**: alternate-eye stereo, with every picture tagged with the exact head pose it was drawn with.
- **Cutscenes through Snake's eyes**: the director's camera is used when Snake is off set. **Subtitles** are lifted onto a panel just below your gaze. Snake's-eyes cutscenes are shown in mono by default ([cutscene] stereo=1 turns stereo on); the picture also goes mono for a moment while your hands move fast (the hands are part of the game picture).
- **Set pieces in Snake's eyes**: jeep, rappel rope, torture rack and cell.
- **Diorama mode**: the level as a miniature on a table. Walk round it, lean in, and turn or zoom it with the right stick.
- **Motion controls**:
  - The gun in your hand, aimed by pointing.
  - Throw grenades from your hand.
  - Punch by swinging; chokehold with your hands together.
  - Floating hands that stop at walls; knock on walls.
  - PSG1 and Stinger aimed with your head, with a round PSG1 lens.
- **Wrist HUD** (as in the GOG mod): LIFE, O2, radar, boss LIFE bars and the equipped item on the left wrist, the weapon and ammo on the right.
- **Selection hologram**: hold a grip to choose. Your weapons or items float above the wrist as the game's own 3D models in a 3D carousel, with the game's descriptions. Swipe with the other hand or flick the stick, then release the grip to equip. A uses an item. A 2D wheel and a plain list are also available. The game pauses while you choose.
- **Floating screen**: for codec calls, the pause screen, menus and the Master Collection launcher.
- **In-headset VR Settings**: seven pages, including full button remapping. Every option is saved to `mgs1mc_vr.ini`.

## Install (players)

You need:
- **METAL GEAR SOLID: MASTER COLLECTION Vol. 1** on Steam, with Metal Gear Solid (US version).
- A 64-bit OpenXR runtime. **Virtual Desktop** and **Steam Link** (SteamVR) are tested; Quest Link should work but is untested.

Steps:
1. Download `MGS1-MC-VR-vX.Y.Z.zip` from the **Releases** page.
2. Open the game folder. In Steam, right-click Metal Gear Solid, then **Manage → Browse local files**. It is usually `C:\Program Files (x86)\Steam\steamapps\common\MGS1`, the folder that holds `METAL GEAR SOLID.exe`.
3. Copy `dinput8.dll` and `openxr_loader.dll` from the zip into that folder.
4. Start your VR runtime (for example Virtual Desktop), then launch the game from Steam.

On first launch the mod writes `mgs1mc_vr.ini`, where every option is documented, and a log, `mgs1mc_vr.log`, into the game folder.

**Uninstall:** delete `dinput8.dll`, `openxr_loader.dll`, `mgs1mc_vr.ini` and `mgs1mc_vr.log` from the game folder.

## Controls (defaults, Quest Touch)

| Control | Action |
|---|---|
| Left stick | Move Snake (push further to run) |
| Right stick | Turn (smooth or snap). In diorama: turn the table (left/right) and zoom (up/down) |
| A / B | Circle (action) / Triangle (first-person view) |
| X / Y | L1 / R1 |
| Right trigger / Left trigger | Square (weapon) / Cross (crawl) |
| Right grip (hold) | Weapon selection (release to equip) |
| Left grip (hold) | Item selection (release to equip; A uses the item). At your left ear: Codec |
| Menu button (left) | Start |
| **X + Y together** | Start (on every runtime; under SteamVR / Steam Link the left menu button opens the SteamVR dashboard instead) |
| **L3 tap** | Select |
| **L3 hold (1 s)** | VR Settings (L3 tap or B closes it) |
| **R3 tap or hold** | Recentre |
| **R3 double-press** | View mode: VR → Diorama → Flat screen (the game untouched) → VR |
| **L3 + R3 together** | Back to the Master Collection menu (sends E + M) |

Inside VR Settings: the triggers change page, the stick picks and changes an option, A presses, and Y resets an option to its default.

Keyboard: F5 stereo on/off, F6 recentre, F7 walk mode, F8 head tracking, F9 RAM snapshot, F10 save picture, F11 headset test colour.

## Build (developers)

The whole mod is one C++17 file (`src/mgs1mc_vr.cpp`). It is built with **MinGW-w64** (x86_64) and needs no runtime DLLs.

- Linux / WSL: `./build.sh`. Needs `x86_64-w64-mingw32-g++`.
- Windows: `build.bat` from an **MSYS2 MinGW64** shell, with `g++` on PATH.

Output: `dinput8.dll`. The OpenXR headers are in `third_party/openxr/include`.

The MIPS routines that run inside the emulated PS1 are generated by the Python scripts in `tools/mips`, which also test them in unicorn. See [tools/mips/README.md](tools/mips/README.md). The generated words are already embedded in the source.

`docs/` holds the port notes: addresses, design decisions and the test history.

## Credits

- The FoxdieTeam decompilation of MGS1: https://github.com/FoxdieTeam/mgs_reversing. Every PS1 address and structure here was found or checked against it.
- The MGS1 Integral VR mod for GOG, whose design and code (wrist panels, selection wheel and carousel, model parser) this port follows.

## Legal

This project contains no game code or assets. Models, icons and texts are read at runtime from the game's own memory. You must own METAL GEAR SOLID: MASTER COLLECTION Vol. 1. Metal Gear Solid is a trademark of Konami Digital Entertainment; this project is not affiliated with or endorsed by Konami.

Licensed under the MIT License (see `LICENSE`). Third-party notices are in `THIRD_PARTY_NOTICES.md`.
