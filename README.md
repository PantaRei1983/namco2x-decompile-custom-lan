
# [Namco 2x version 0.10.4f](https://github.com/PantaRei1983/namco2x-decompile-custom/releases/tag/v0.10.4f): Arcade Custom Decompiled Version with various game fixes made by [LuKa-86](https://github.com/LuKa-86) and me, plus Ridge Racer full decompiled and advanced texture filtering (bilinear/sharp bilinear/trilinear or sharp trilinear+anisotropic 2x-4x-8x-16x) made by [PantaRei1983](https://github.com/PantaRei1983), aka myself, with Claude Opus 5.5 Support.

**A fork of [namco22-decompile](https://github.com/spacestate1/namco22-decompile) by
[spacestate1](https://github.com/spacestate1), made to keep real arcade cabinets running.**

namco22-decompile rebuilds Namco's 1990s 3d arcade games (System 21, 22, Gorgon and 23) so they run natively on a PC. All of that
work is spacestate1's. The [LuKa-86](https://github.com/LuKa-86/polygonizer-arcade-lan)'s fork puts those games into real cabinets: a PC in place of the original boards, the
cabinets linked over a wired network the way the boards were linked by their cable, starting by themselves when the
cabinet is switched on, added full decompiled version of two Namco Gorgon (early System 23 family) games (Rapid River and Final Furlong), one new System 23 game (Gunmen Wars), all other Super System 22 games (Air Combat 22, Alpine Racer, Alpine Racer 2, Alpine Surfer, Aqua Jet, Cyber Cycles) and one new System 22 game (Ace Driver: Victory Lap). My fork added a full decompiled version of Ridge Racer (based on latest "ridgerac" main romset of MAME v0.289), a full decompiled version of Panic Park (based on latest "panicprk" main romset of MAME v0.289), various game fixes (especially on Prop Cycle, that now have full attract mode and correct replay working) and introduce new filter options on texture game (bilinear/sharp bilinear/trilinear or sharp trilinear+anisotropic 2x-4x-8x-16x). In this fork all Namco System 22/Super System 22 games and Namco Gorgon/System 22.5 are decompiled! Huge respect and thanks to spacestate1 and LuKa-86; please star and support
[the original project](https://github.com/spacestate1/namco22-decompile), and use it for playing at home or online.

**You need your own copy of the games.** This repository contains no ROMs or game data.

## Why this exists

The original boards in these cabinets are 25 to 30 years old. They fail, spares are scarce, and a linked set is only as
good as its weakest board. The LuKa-86's project started as a way to **restore a 4-player linked Tokyo Wars set-up** and has
grown into a way to **keep other Namco machines of that era running** with a modern PC in each cabinet. My project starts as a way to **fix some bugs present in the original repo (especially Prop Cycle)**, try to decompile new games on these system ( Ridge Racer and Panic Park at the moment) and added **new graphics options likes texture and Sprite 2d effects filtering**.


> **Project status:** a hobby project, with no regular updates or support.
> Bug reports and pull requests are welcome but may not get picked up. You are welcome to fork it under the same licence
> (MIT).

## Fixes and improvements over the original project

### Game fixes made by LuKa-86

- **Victory Lap:** collision tables and race crashes fixed; test mode works; the rev needle is no longer dimmed.
- **Air Combat 22:** crashes in attract, at mission end and after the mission 2 refuel fixed; stuck sprites fixed; pitch
  and throttle can be rebound; the gunsight's pitch ladder no longer cuts thin lines through the TIME LIMIT / SCORE text
  in the second attract demo (the polygon-over-text mask now stays inside each polygon's clip window).
- **Crisis Zone:** widescreen HUD with the score bars at the screen edges; 4:3 with fading bars outside play; tank
  shadow colours fixed; HUD icons drawn over the bars; zone-select ticker scrolls on its own.
- **Gunmen Wars:** the namco logo matched to real-cabinet footage; widescreen HUD; NamCam from a webcam or a picture.
- **Angler King:** better english subtitles implementation.
- **System 23 engine:** about 4 times faster priority drawing; correct texture sampling for some polygons.
- **Super 22 / System 22:** translucent sprites over text; priority of some polygons over the text layer; guard-band
  polygons drawn.
- **Menus:** quick clicks and touchpad taps are no longer lost; a controller's D-pad moves one row, not two.

### New games, Texture filters and fixes from my namco22-decompile fork

- **Ridge Racer (new port, System 22) -- fully playable:** MAME's `ridgerac` set (World, RR2 Ver.B), on the Ridge Racer 2 / Rave Racer runtime, with the
  master DSP and sound MCU interpreted (as Victory Lap). Two fixes it needed, shared by every 68020 port:
  - The translator (`tools/gen/lift68k.py`) rewrites `cmp2` / `chk2`: Ghidra reads the bounds through a pointer it loads from
    the effective address (they are AT it), compares the whole data register, and reads a PC-relative pair 2 bytes early.
    Ridge Racer's course culling uses it, and drew no road or ground until it was fixed. Re-run a game's `tools/regen.sh`
    to get the fix in its translation.
  - The translator also keeps the 68000's X flag: CMP / CMPI / CMPA / CMPM leave it alone and ADDX sets it (Ghidra does the
    opposite). Ridge Racer's BCD lap time gained a minute at every lap -- the same bug Ridge Racer 2's first translation had
    (fixed there after the fact).
  - `abcd` / `sbcd` in their memory form (`-(Ay),-(Ax)`) are decimal too: Ghidra's p-code left them binary, so Ridge
    Racer's gold TIME countdown went 50 -> 4F -> 4E ... and drew the wrong digit tiles. Now 50 -> 49.
  - Vertex lighting (point ROM packet 0x10) follows MAME's current `namcos22_v.cpp` (after 0.289): flat or Gouraud, the mesh
    width, triangles and the packet's own intensity, the normal index advancing once per quad. The waving flag in the
    attract mode and the rotating sign before the second tunnel are smoothly lit; the same code draws Prop Cycle's
    score / time, Time Crisis' knives and Armadillo Racing's title logo.
  - Build: `ridgerac/build.sh ridgerac.zip namcoc74.zip` (unpacks the ROMs), then `ridgerac/tools/import.sh` once with
    `N22TOOLS` set (Ghidra translates the program), then `ridgerac/build.sh` again. Start it with `./launch.sh rr1`.
  <p align="center">  
  <img width="80%" alt="Ridge Racer Flag Title" src="docs/images/ridgeracer-arcade-flag-title-widescreen.png" />
  <img width="80%" alt="Ridge Racer Start" src="docs/images/ridgeracer-start-race-widescreen.png" />
  <img width="80%" alt="Ridge Racer Ingame" src="docs/images/ridgeracer-ingame-race-widescreen.png" />
  </p>
- **Panic Park (new port, System 23) -- fully playable:** every mini-game runs to the end with the arcade's picture and sound;
  only a few very minor graphics glitches remain. In widescreen, UFO Panic's night sky (a flat picture painted for 4:3)
  is stretched to fill the wide screen instead of leaving black bars at the sides, and the stone frame of the Panic King
  attract screens is widened to the full screen width (both only in widescreen; 4:3 is unchanged). The HUD (score / lives cans, timers), the Panic King
  intro's START button and the PANIC PARK plate are drawn over the scene, as on the arcade (a priority the game sets per
  viewport, unknown to MAME). **Before the first game, calibrate both players' levers:** press F2 to enter
  the service mode and run the lever calibration there (uncalibrated, the centred lever puts the character off to one side).
  Decompiled from MAME's `panicprk` set (World, PNP2 Ver. A, with the ASCA-3A I/O board in
  `namco_asca3a.zip`) the same way as Rapid River: `panicprk/build.sh panicprk.zip namco_asca3a.zip` translates the program
  from your ROM with Ghidra (`N22TOOLS`) and builds it, `./launch.sh panic` starts it. Two players: the joystick on Left / Right
  (player 2 on A / D), START on 1 / 2, coin on 5; pads work (a second pad is player 2). No light gun, so no crosshair options.
  The lever is analogue: a pad's stick gives every position, a key moves it steadily and lets it come back to the centre.
  Fixed against the arcade footage and the TeknoParrot longplay: the C435 interrupt is now taken at once, as on the board
  (its handler sends the next part of the frame's state, which came late every few frames: heavy polygon flicker after the
  coins / START); the C435's state 9 (unknown to MAME) read as a signed depth offset for its part of the list (the practice's
  castle gate, the title's big PANIC PARK letters over the island, the Panic King's scores in their cans); Display > Depth
  "exact" is not offered here (it undid the game's own order); the red namco logo; the monitor's RULE and NEEDED
  pictures (an unnormalised clip window that overflowed MAME's viewport); the PANIC PARK plate swinging in over the monitor;
  exact depth inside a model (the close-ups).
  <p align="center">  
  <img width="80%" alt="Panic Park Title" src="docs/images/panicpark_title.png" />
  <img width="80%" alt="Panic Park Attract Mode" src="docs/images/panicpark_attractmode_widescreen.png" />
  <img width="80%" alt="Enter to Panic Park" src="docs/images/panicpark_enter_to_panic_park_widescreen.png" />
  <img width="80%" alt="Balloon Panic Stage" src="docs/images/panicpark_balloon_panic_stage_widescreen.png" />
  <img width="80%" alt="Ring Panic Stage" src="docs/images/panicpark_ring_panic_stage_widescreen.png" />
  </p>
- **Downhill Bikers (new port, System 23) -- work in progress, playable:** boots, runs the whole attract mode (title, How to
  Play, the Course Preview with its full HUD and messages, the rankings) and a race from coin to finish, with the in-race HUD
  (rest time, position, speed, the course bar, the section messages). Decompiled from MAME's `downhillu` set (US, DH3 Ver. A
  -- the one MAME runs; it needs its parent `downhill.zip`) with the ASCA-3 I/O board in `namco_asca3.zip`:
  `downhill/build.sh downhill.zip namco_asca3.zip` (a merged `downhill.zip` holds downhillu's chips; with split sets add
  `downhillu.zip` to the command; `N22TOOLS` set, as for Panic Park), then `./launch.sh downhill`.
  Controls: Left / Right = handlebar, Up (or W) = pedal, Z / X = left / right brake (Down: both), 1 / Enter = START (the
  confirm button), 5 = coin, PgUp / PgDn / Home = the service Select up / down / Enter, F2 = test mode, 9 = service coin.
  Pads: left stick = handlebar, right trigger (or A) = pedal, LB / RB = brakes (left trigger: both), Start = START. The
  default coin setting is 4 coins a credit (change it in the test menu's COIN OPTIONS).
  Fixed on the way: switch tables Ghidra missed (a crash at the first course preview), the C417 point-ROM read port (the game
  reads its model directory through it: every model was wrong), the C435 model blend factor is signed (the dust puffs were
  screen-high slabs), the HUD glyph upload's time budget (the course preview had no text). Known: the initials on the
  RANKING screen and the "ready" letters at the start are drawn with coloured noise instead of plain green.
- **3D texture filter (Display > 3D texture filter)** for every textured game: **Off, Bilinear, Sharp bilinear, Trilinear,
  Sharp trilinear**, with **anisotropic filtering 2x / 4x / 8x / 16x** in the trilinear modes. Only the 3D polygons are
  filtered: the HUD, the text layer and the 2D sprites stay sharp. It replaces the Texture smoothing switch: Alpine
  Racer 2 and Alpine Surfer still start with Bilinear, and an older `texture_smoothing` line in a game's cfg is still read.
  - Super 22 / System 22 and Prop Cycle: atlas pages with mip levels; the sharp modes run in a small fragment shader.
    Config: `texture_filter` = `off | bilinear | sharp | trilinear | sharp-trilinear`, `texture_aniso`. Environment:
    `ENG_TEX_FILTER` = 0..4, `ENG_TEX_ANISO`. Fixed: with the trilinear filter, textures that were already sharp
    (Rave Racer's City skyscrapers, above all) could flash very blurry for a frame when the texture memory filled up. The
    textures on screen are now kept, and a texture already in memory is always found again.
  - System 23 / Gorgon (Time Crisis II, Crisis Zone, Angler King, Gunmen Wars, Panic Park, Rapid River, Final Furlong): filtered per fragment in the polygon
    shader, kept inside each polygon's own texels; never Gunmen Wars' photo page. Immediate polygons are filtered when they are 3D
    (corners at different depths: Final Furlong's horses and saddle-cloth numbers); flat screen panels stay sharp. Environment: `S23_TEX_FILTER`,
    `S23_TEX_ANISO`.
  - Rave Racer has its own Display rows for the same filters. Cyber Sled has no textures, so no filter rows.
- **Effects (Display > Effects):** smoothing for the 2D effect sprites of Air Combat 22 (smoke and explosions), Cyber Commando
  (smoke and explosions), Dirt Dash (dust, mud, spray and a softer headlight glow on the car select screen), Prop Cycle, Time Crisis,
  Tokyo Wars (smoke, fire, sparks), Armadillo Racing (dust), Alpine Racer 2 (the snow spray, ice chips and clumps of earth
  thrown up by the skis) and Aqua Jet (the spray thrown over the screen's edges while riding, and the wall of water under the waterfall).
  - Effects are matched by the tile a sprite actually draws. Alpine Racer 2 draws its spray as linked sprites (tile 0
    plus a word from the link table), so the base tile alone would not tell them apart. `ENG_SPRLOG=-1` lists every
    sprite of every update, with both numbers.
  - Cyber Commando (System 22): a Display > Effects row smooths the explosions, which on System 22 are textured polygons (texture bank 5,
  u 0..511, v 1024..1535; `fx_tex` in the game table).
  - Rapid River (Gorgon): the water spray and foam sprites. With the option on, each sprite tile is drawn as a filtered
    quad at the output resolution, with no seams between tiles, instead of being stretched with nearest-neighbour
    sampling. Other sprites (HUD, title, text) stay sharp. Config: `effects_filter` = `off | on`. Environment:
    `S23_FX_FILTER` = 0 | 1.
  <p align="center">  
  <img width="80%" alt="Rapid River with no filter (Original Arcade)" src="docs/images/rapidriver-no-filter.png" />
  <img width="80%" alt="Rapid River with trilinear + 16x anisotropic filtering" src="docs/images/rapidriver-with-filter.png" />
  </p>
- **Crisis Zone:**
  - The zone select's beam counter is read in System 23 Evolution 2's format (a line count from the start of vblank),
    so the game's own wait puts the ticker's scroll on the ticker rows only. This replaces the fixed row cut-off.
  - That wait is fast-forwarded line by line, so the screen holds 60 fps.
  - The attract demo's RULES panels ("PRESS PEDAL : GO&SHOOT", "OUT OF AMMO", ...) are shown. They are direct quads,
    and the buffer is decoded again; the full-screen quad and letterbox bars that the game keeps there with bit 15 set
    are skipped.
- **Prop Cycle:** the end-of-game replay of the player's own run is in sync again.
- **Prop Cycle:** the story intermissions and the good ending draw their whole cast. The rewrite stopped every walk of
  the animation chain at 64 nodes; the ROM has no limit, and these scenes go past it (68 nodes after day 2, up to 81
  after day 3, 71 in the ending). The last group loaded lost its last nodes: the boy's head after day 2, the old man's
  head, arms or whole upper body after day 3. `PROPCYCL_LEVEL=inter:<day>` jumps straight to the intermission after a
  day, and `PROPCYCL_NODEDUMP=<frame>` lists the chain's nodes at that frame.
- **Prop Cycle:** the attract mode plays the story introduction again ("IN THE FUTURE PEOPLE LIVE IN PEACE..." to "IS
  THE HERO OF SOLITAR!", nine scenes, about 35 seconds). It was never ported: the attract's step 14 called another
  scene's code, which ended at once, so the story was skipped. `src/game_story.c` ports it from the ROM (0x0300AC..
  0x03164C), and the sacred altar picture gets its frame number again. `PROPCYCL_SHOT_FROM` / `PROPCYCL_SHOT_TO` limit
  `PROPCYCL_SHOT_EVERY` captures to a range of frames.
- **Prop Cycle: no more flickering fountain spray and black river in the attract's ranking flight over the town** (before the
  Savior of Solitar plate): the water surface's animation frame was read wrongly (a 68020 memory-indirect address), so it
  cycled through other water models -- spray, flow, nothing -- instead of its eight still-water frames.
- **Ridge Racer 2:** the lap time no longer gains a minute (the 68000 X flag in the lifted code).
- **Ridge Racer 2:** the test menu's GAME OPTIONS and COIN OPTIONS change again (gas held, wheel left / right) and are saved when the
  Test switch goes off: the four `cmp2.b` of the lifted code read their bounds from a wrong address, so every step "wrapped" and the
  value flickered between two settings (namco22-decompile issue #101). `ridgera2/tools/gen/fix_cmp2b.py` rewrites them.
- **Ridge Racer 2: two extra camera views (chase near / chase far), not on the original cabinet.** The **View** button (default
  key **V**, gamepad **Y**; Controls > "Camera (extra views)") cycles cockpit -> chase (near) -> chase (far) -> cockpit, with a
  hint at the bottom of the window. They are the camera offsets of MAME's "Camera 1" / "Camera 2" cheats, applied only during a
  race (the attract demos and menus keep their own cameras); the rear-view mirror is hidden in the chase views and back in the
  cockpit. The cheats' third word (0x10000C28 = 0) is the world's priority band: at 0 the whole scene was taken for HUD pieces --
  in 16:9 the car and the track broke into pieces at the sides, and the 3D texture filter switched off -- so band 5 is used
  instead (it hides the mirror the same way). The choice is remembered (`camera = 0 | 1 | 2` in `r2_controls.cfg`).
  <p align="center">  
  <img width="80%" alt="Ridge Racer 2: Default view" src="docs/images/ridgeracer2_defaultview.png" />
  <img width="80%" alt="Ridge Racer 2: Chase view 1" src="docs/images/ridgeracer2_chaseview-1.png" />
  <img width="80%" alt="Ridge Racer 2: Chase view 2" src="docs/images/ridgeracer2_chaseview-2.png" />
  </p>
  
  [![Ridge Racer 2 alternate chase view demo gameplay](https://img.youtube.com/vi/S4O9Z_8-mvM/maxresdefault.jpg)](https://www.youtube.com/watch?v=S4O9Z_8-mvM)
- **Ridge Racer 2 / Rave Racer (and every System 22 / Super System 22 sub-window): the rear-view mirror stays inside its
  frame.** The picture spilled one pixel past the frame on the left and top: the window's edges kept their half pixel
  (`engine/slave_list.c`), where MAME holds them as whole pixels. Now the same as MAME at every resolution and in 4:3 or 16:9.
- **Alpine Surfer: READY slides in and GO! slides out, row by row, as on the board.** The Super System 22 engine now has the
  scanline interrupt (posirq) and a text layer scrolled line by line: the game moves each pixel row of the word in its
  interrupt handler (MAME: "alpines changes x scroll mid-screen"). Other games that enable the interrupt (Air Combat 22,
  Alpine Racer, Aqua Jet, Dirt Dash) draw exactly as before. `SS22_NO_POSIRQ=1` turns it off for a comparison.
- **Rapid River and Gunmen Wars build on Linux in one step:** `rapidrvr/build.sh` and `gunwars/build.sh` (new) unpack the ROM
  zips, translate the program with Ghidra when `N22TOOLS` is set, and build; `./launch.sh rapid` / `./launch.sh gunmen` start
  them. Their import / regen scripts were Windows-only. Every `.sh` in the repository is now executable (several were not, so
  `./launch.sh` itself failed on Linux with "Permission denied").
- **Sprites of the VICS games (Air Combat 22, Armadillo Racing, Dirt Dash, Time Crisis, Tokyo Wars):** the VICS chip now works as
  on the board (MAME PR #16658): in the vblank it copies the sprites the game wrote into the sprite chip, which draws its own list. The
  guessed sprite counts and the per-game fixes are gone; sprites show one frame after the game wrote them, as on the hardware (visible in
  Air Combat 22's gunsight). `SS22_VICS_DMA=0` brings the old drawing back for a comparison.
- **Effects 2d smooth for Alpine Racer, Alpine Surfer and Cyber Cycles:** Display > Effects now smooths the snow the skis and the board throw
  up, and Cyber Cycles' sparks, gravel and tyre smoke -- every Super System 22 game has the option now. Alpine Surfer's and Cyber
  Cycles' translation scripts also run on Linux.
- **Widescreen (all Super System 22 games):** a sprite that runs past the 4:3 edges now goes on into the wider picture instead of
  stopping in a straight line. Aqua Jet's riding spray, which the game lays exactly across the 640-px screen, is continued (mirrored) out to
  the picture's edges (`edge_fill` in the game table).
- **Air Combat 22:** Display > Effects smooths the explosions and the smoke (sprite art 2100..3583). Its translation scripts
  (`airco22/tools/*.sh`) also run on Linux now.
- **Widescreen fog (System 22 games):** the depth fog of a quad past the 4:3 sides is taken as far as the frame's edge would
  see it. The fog level is the depth along the view axis, so the sky panels out to the side -- seen only in a wider picture --
  had less of it: Ridge Racer's sunset (red) and dawn (white), which are fog on the sky dome, left a blue strip of sky at the
  picture's edges. Inside the 4:3 frame nothing changes, and 4:3 is untouched.
- **Alpine Racer 2:** the title's ice block turning into the "Alpine Racer 2" ice logo is drawn. It is a point-RAM object
  (code 5) that a PDP block inside the display list writes on the fly: the block is run (MAME `simulate_slavedsp`), Super 22
  geometry reads point RAM (0xF80000), and a model's list address comes from its point word for every code, as MAME's
  `blit_polyobject`.
- **Alpine Racer from any of MAME's sets:** `alpinerd` (World AR2 Ver.D, the parent), `alpinerc` or `alpinerjc` -- the port
  was written for alpinerjc only, and MAME's parent set is a different program. `alpiner/build.sh alpinerd.zip`, then
  `alpiner/tools/import.sh` once with `N22TOOLS` set, then `alpiner/build.sh` again; `./launch.sh al` starts it. The
  seeds are per set (`seeds_<set>.txt`); alpinerd needs one (0xC688) and runs 12000 autoplay frames without a trap.
  namcoc71.zip is not needed (the DSP BIOS is built in).
- **Victory Lap builds the same way, on Windows (MSYS2) and Linux:** `victlap/build.sh victlap.zip namcoc74.zip`, then
  `victlap/tools/import.sh` once with `N22TOOLS` set, then `victlap/build.sh` again; `./launch.sh vl` starts it. Its
  scripts were Windows-only, the ROMs had to be set up by hand first, and the first import built nothing; on Linux the
  link also failed (`--stack` is a Windows-only linker option).
- **Gunmen Wars:** Added recent MAME three sets builds: `gunwars` (World, GM2 Ver. B, MAME's parent), `gunwarsj` (Japan, GM1 Ver. B)
  and `gunwarsa` (Japan, GM1 Ver. A); the program is translated from the one you have (`GW_SET=gunwarsj gunwars/build.sh ...` picks
  one when you have several).

  [![Rapid Riper Decomp longplay video](https://img.youtube.com/vi/__qW4OUsvcM/maxresdefault.jpg)](https://www.youtube.com/watch?v=__qW4OUsvcM)

## Building

On Windows, in an MSYS2 *MINGW64* shell with `mingw-w64-x86_64-toolchain`, `-cmake`, `-ninja`, `-pkgconf`, `-SDL2`,
`-zlib`, `-python` and `-python-numpy` installed. Each game's ROM files must be unpacked in its `extracted/` folder first:
its sound and DSP programs are translated from them at build time (see the original README).

```
cmake -S tokyowar -B tokyowar/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DZLIB_LIBRARY=/mingw64/lib/libz.a
ninja -C tokyowar/build          # the same for every game folder (cybsled: -j1)
```

Game folders: `tokyowar raverace acedriver victlap cybrcomm adillor airco22 alpiner alpines alpinr2 aquajet cybrcycc
dirtdash timecris cybsled aking finfurl crszone gunwars panicprk downhill tc2 ridgerac`.

On Linux (any distribution: `./install-deps.sh` installs the compiler, CMake, SDL2, zlib and OpenGL headers), the games
with their own `build.sh` build in one step from the ROM zips, e.g. Rapid River and Gunmen Wars:

```
export N22TOOLS=~/n22tools       # Ghidra 12.1.4 (ghidra_12.1.4_PUBLIC/), an empty work/ folder; Java 21 from your distribution
rapidrvr/build.sh rapidrvr.zip namco_asca1.zip     # the first time: unpacks the ROMs, translates the program, builds
gunwars/build.sh gunwars.zip namco_asca5.zip       # or a clone: gunwarsj.zip / gunwarsa.zip with gunwars.zip
panicprk/build.sh panicprk.zip namco_asca3a.zip    # Panic Park
downhill/build.sh downhill.zip namco_asca3.zip     # Downhill Bikers (split sets: add downhillu.zip)
./launch.sh rapid                                  # or: ./launch.sh gunmen / panic   (a number for the window size, --fullscreen)
```

The new 68020 ports are translated from each game's own program with Ghidra and this fork's translator
(`tools/gen/lift68k.py`, `tools/gen/ghidra/`); Gunmen Wars with its MIPS translator (`tools/gen/liftmips.py`,
`gunwars/tools/regen.sh`); their `tools/import.sh` / `regen.sh` need `N22TOOLS` set to the folder with Ghidra and a JDK.
The link tests run four copies on one PC over 127.0.0.1, with network faults injected:
`tokyowar/tools/link_tests.sh` and `raverace/tools/link_tests.sh` (`gather`, `missing`, `bad`, `skew`, `crash`).

## Licence and legal

MIT License; see `LICENSE`. Copyright of the original project is its author's; this fork's additions are under the same
licence.

This project is not affiliated with or endorsed by Bandai Namco. Tokyo Wars, Ridge Racer, Rave Racer, Ace Driver, 
Ace Driver: Victory Lap, Time Crisis, Cyber Cycles, Cyber Commando, Cyber Sled, Armadillo Racing, Alpine Racer, 
Alpine Surfer, Aqua Jet, Air Combat, Rapid River, Final Furlong, Angler King, Gunmen Wars, Panic Park, Downhill Bikers, Crisis Zone, Prop Cycle and Dirt Dash are trademarks 
of Bandai Namco Entertainment Inc. You need your own legally obtained copy of each game.

## Credits

- **[spacestate1](https://github.com/spacestate1)** for [namco22-decompile](https://github.com/spacestate1/namco22-decompile),
  the rebuilt games this fork is built on, and the long reverse-engineering work behind them. This fork would not exist
  without it. Their README is kept in [docs/ORIGINAL_README.md](docs/ORIGINAL_README.md).
- **[LuKa-86](https://github.com/LuKa-86)** for [LAN VERSION](https://github.com/LuKa-86/namco22-arcade-lan) of namco22-decompile, and decompiled Ace Driver: Victory Lap, Gunmen Wars, Final Furlong, Rapid River and all other Super System 22 games.
- **Claude** (Anthropic's AI, through Claude Code) wrote the fork's changes, the new ports and the launcher.
- [MAME](https://www.mamedev.org) served as the reference for how the boards behave, and [Ghidra](https://ghidra-sre.org)
  made the analysis of the games' programs possible.
- Namco, for the games and the cabinets.
