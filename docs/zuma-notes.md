# Zuma port: notes

This file records the port: what each step found, with its evidence. The project is a copy of feedingfrenzy-nx 1.0.0
(`68df7bf`: Feeding Frenzy 1's port, itself a copy of bejeweled3-nx 1.0.1). Identifiers were renamed
feedingfrenzy -> zuma with `tools/port_rename.py`. Feeding Frenzy's notes (`feedingfrenzy-notes.md`), Bejeweled 3's,
Peggle 1's and Peggle 2's stay as the reference for everything taken from those ports; comments that say
"feedingfrenzy-nx", "bejeweled3-nx", "peggle1-nx" or "Peggle 2" record findings from those ports.

The user's request (2026-10-26): "please work on porting zuma in the same manner you porting feeding frenzy 1 and 2".
The renderer is Renut on deko3d, as for both Feeding Frenzy ports.

## The game (2026-10-26)

The user's files are `D:\peggle 2\Zuma.rar` (the XBLA package) and `D:\peggle 2\Zuma (World) (v2).zip` (its title
update), extracted to `D:\peggle 2\zuma`:

| Package | Content type | What | Extracted by `tools/stfs_extract.py` to |
|---|---|---|---|
| `Zuma/584107EF/000D0000/90226B76...` | `0x000D0000` (XBLA) | "Zuma", 6,676 blocks: 31 entries, 26 MiB | `assets/game_root` |
| `TU_1C421VF_0000104000000...` | `0x000B0000` (title update) | "Zuma Title Update #2": `default.xexp` | `assets/game_root` |

- **`default.xex`** (`tools/xex_info.py`):
  - XEX2, base `0x82000000`, image size `0x004D0000`; version 0.0.1.1, built 2006.
  - Imports: `xam.xex` (78) and `xboxkrnl.exe` (252), version 2.0.2135.
  - Static libraries: XDK **2.0.2135** (XAPILIB, XBOXKRNL, LIBCPMT, D3D9, D3DX9, XGRAPHC, XONLINE, XMP, XAUD): older
    than every port before (Feeding Frenzy's 2571 was the oldest).
- **Title Update #2** (`default.xexp`): a delta patch to version **0.0.3.1**. The SDK applies a `default.xexp` lying
  next to `default.xex` by itself, in the code generator and in the game ("XEX patch applied successfully: base
  version: 0.0.1.1, new version: 0.0.3.1"), as in Peggle 1. So `assets/game_root` holds both files, and every address
  in this port is the patched game's. The patch moves the entry point: `0x820CE650` (the PE header of the patched
  image; the base XEX header's `0x820CDF48` is now inside another function).
- **Sections** (the PE header in the loaded image):

  | Section | Range |
  |---|---|
  | `.rdata` | `82000400-82055300` |
  | `.pdata` | `82055400-82064AF8` |
  | `.text` | `82070000-823855AC` |
  | `.data` | `82390000-82492114` |
  | `.XEXID` | `82492200` |
  | `.idata` | `824A0000` |
  | `.XBLD` | `824B0000` |
  | `.reloc` | `824B0200-824D52EC` |

  The PE header's ImageBase field holds `0x00400000` (the linker's), not the `0x82000000` the XEX loads it at and its
  code is linked for. Tools that took the base from that field (`ro_constants.py`, `flush_mode_flow.py`,
  `crt_signatures.py`) now fall back to `0x82000000`.
- **Data:** PopCap's game ported by Oberon Media, as Feeding Frenzy (`Images/Logo_PopCap_and_Oberon.jpg`): one Xbox
  packed resource (`Resources/resources.xpr`, 22 MB), the achievement and rating pictures, `ArcadeInfo.xml` (one
  player, 12 achievements).

## Code generation (2026-10-26)

- Futari's VMX keys stay on (`app/perf_overrides.toml`), as in both Feeding Frenzy ports.
- **First run:** one "target not in any function", `0x8236E658` (a this-adjusting thunk's tail-call target, `addi
  r3,r3,4; b` at `0x82382B0C`), declared in `app/overrides.toml`. **Jump tables** (`tools/switch_check.py --code
  82070000:823855AC`): none truncated, wrong or missed; the two "odd shapes" are indirect calls through a table of
  12-byte records (generated as indirect calls). **Pointer sweep** (`tools/pointer_fixpoint.py`): 48 functions, fixpoint
  in pass 2 (5 tail branches into function bodies and 2 SEH scope labels rejected).
- **The first PC build** stopped on two `goto`s without a label: two catch funclets (a loop of destructor calls, then
  the rethrow `bl _CxxThrowException(0, 0)`, which does not return) are followed by two data words that decode as loads,
  so the generator ran on into the next funclet. Their sizes come from `.pdata` (22 words) in `app/overrides.toml`.
- **Addresses:**

  | What | Zuma | How |
  |---|---|---|
  | setjmp | `0x8235E260` | `stfd f14,0(r3); stfd f15,8(r3)` (one match); a hook word at `0x82491FE0` first; 7 call sites |
  | longjmp | `0x8235E010` | `lfd f14,0(r7)` (one match), the check of +312, `mr r7,r3`; 20 call sites |
  | memset | `0x82112990` | `tools/crt_signatures.py scan`: 160 bytes, FNV-1a 64 `0x54D201DC8A21DABA` (Peggle 2's) |
  | memcpy | `0x82113040` | Feeding Frenzy's `sub_822CD480` byte for byte (1,056 bytes, `0xA7EAD6E565BDE9F2`) |
  | memmove | `0x821147D0` | Feeding Frenzy's `sub_822CEDF0` (the same 48 instructions; its tail call reaches memcpy here) |
  | _stricmp | `0x82114C00` | Feeding Frenzy's `sub_822CFFE0` byte for byte (88 bytes) |
  | srand / rand | `0x82112918` / `0x82112948` | Feeding Frenzy's code (branches masked) |
  | `_CxxThrowException` | `sub_82112178` | 84 call sites; copies the 32-byte template at `0x820141C8`; raises through `sub_822EDF18` |
  | `D3DDevice_Present` | `sub_821221C0` | ends with `D3DDevice_Swap` `sub_82121BE0`, the only `VdSwap` caller |
  | entry point | `0x820CE650` | the patched image's PE header |

  The CRT natives (`zuma_hot_guest.cpp`, `hand.txt`) are registered with these sizes and hashes; Feeding Frenzy's
  strstr has no match here (not even with the branches masked), and its hot lists and hand-written native were
  removed.

## First boots on the PC (2026-10-26, emulated GPU)

- `boot1` / `boot2`: the game ran 3-8 s, to its first audio frame, then the process ended with `0xC000008F` (an
  inexact-result trap) and no log line. `app/src/zuma_crash_log.cpp` now logs floating-point traps with the x87 control
  word and MXCSR: `x87 027F, MXCSR 00008060`, every SSE exception unmasked, at a `vcfsx` in `sub_823736A0` (called from
  `sub_8237B5C8`, reached only through a pointer).
- **The cause is in the SDK:** the audio system calls the game's render driver callback on its worker, an
  `XHostThread`, whose guest context never had `fpscr.InitHost()` (guest threads get it in `XThread::Execute`). Its
  FPSCR cache `csr` stayed 0, and the first `enableFlushMode` (vector code) wrote `0 | FlushMask` to MXCSR. Feeding
  Frenzy's callbacks never switched flush mode on, so they never wrote the word. `XHostThread::Execute` now calls
  `InitHost()` first (`sdk/src/system/xthread.cpp`). On the Switch a zero `csr` was harmless (the ARM exception enable
  bits are 0), but the fix is the same code.
- `boot3` (75 s, A presses): the PopCap / Oberon Media splash, the title "Zuma Deluxe" ("PRESS A TO START"), the main
  menu (Adventure, Gauntlet, Leaderboards, Achievements, Help & Options, Return to Arcade), Temple of Zukulkan, the
  instructions, Level 1-1 "Spiral of Doom" with the balls rolling. 60 game frames a second; the game asks for
  presentation interval 1 (every vblank), so no 60 fps forcing is needed. The audio plays (SDL: no missing frames);
  the generated-code self-checks 0 differences. The log has no unimplemented call; only the splash variant
  `_Logo_PopCap_and_Oberon.jpg`, which the game tries first, is missing (as on the 360).

## Renut's renderer at Zuma's addresses (2026-10-26/27)

The renderer's Direct3D layer is Feeding Frenzy's (Bejeweled 3's, N&B's layout); its hook table was moved to XDK
2135's addresses with `tools/renut_hook_remap_zuma.py` (evidence in the tool and here).

- **The trace** (`app/src/zuma_d3d_trace.cpp`, feedingfrenzy2-nx's with Zuma's addresses; PC test aid, off by default):
  the Direct3D library is `0x82121000-0x8213B000`; game code calls 82 of its functions (`out/zuma_d3d_api.txt`), 56 of
  them by game frame 2,400 (`trace1`). The device (`0x40003C80`) is created once (`CreateDevice(0, 1, 0, 0, params,
  renderer+52)`, from `sub_820E1DE0`); a frame is Clear, the sprites (vertex buffer Lock of 0x80 bytes, Unlock,
  SetStreamSource, SetVertexDeclaration, SetPixelShader, SetTexture, DrawVertices(6, 0, 4)), Present.
- **The API functions** were paired with Feeding Frenzy's by an anchor alignment of the two libraries
  (`out/renut_map/anchor.txt`) and by reading both: CreateDevice `sub_82121838`, Clear `sub_821287F8`, Present
  `sub_821221C0` (the frame hook's shared address), DrawVertices `sub_8212AA88`, the shader and declaration creators and
  setters, the vertex buffer's (Lock returns base + offset, Feeding Frenzy's adapter), the shader constants B / I.
  XDK 2135's Set* functions keep references on what they set (AddRef the new, release the old); Renut's do not.
- **The states:** the game never calls a state function directly. Its D3D_InitializeApiState (`sub_8212F960`) copies
  97 render-state records {getter, setter, default} from `0x823924B8` into the device at `+0x60` (setters) / `+0x234`
  (getters), 20 sampler-state records from `0x82392948` into `+0x1E4` / `+0x3B8`, and the game calls through those
  (N&B: 101 render states at `+0x40` / `+0x224`). Renut's `InitializeStateTables` now writes the game's table words at
  XDK 2135's offsets; every one of those functions is hooked. Against N&B's order (the template index of Renut's
  hooks): render states 0-56 are the same, Zuma's 57 is a state N&B lacks (`sub_8212C260`: the value in four device
  words; its own hook keeps it on the host), N&B's 57-95 are Zuma's 58-96, N&B's 96-100 (HiZ, LastPixel, LineWidth,
  Buffer2Frames) do not exist; the samplers are in the same order (`out/renut_map/zuma_state_table.txt`). The getters
  of Wrap10-15 are the game's shared `li r3,0; blr` (`sub_821939F0`), left alone. An alignment of the two state blocks
  (`out/renut_map/state_align.txt`) gave the same functions but for 18, where the table decides.
- **The presentation interval:** the game's `SetRenderState_PresentInterval` (`sub_8212C8F0`: `stw r4,13772(r3)`) asks
  for 1. With Renut's renderer the interval is Renut's word (`0x36A0` of its device), which the frame hook reads.
- **The game's own textures.** XDK 2135's resource headers are not N&B's: the resource head is 12 bytes (Common with the
  reference count in its low byte and the type in bits 16-18, the fence at +4), a texture is 40 bytes with its fetch
  constant at +16, a vertex buffer 20 bytes with its fetch constant at +12, a texture level's surface 64 bytes (its
  texture at +44, face at +24, level at +32). The game's D3DX code (its surface loader `sub_8220BBA8` and the
  functions around it, which Zuma's image loading goes through) reads those fields itself, and the game's own lock
  writes cache-flush packets into the device's ring at +0 / +8 (on Renut's device, its dirty masks). So:
  - the game's own CreateTexture, GetSurfaceLevel, GetDesc, GetLevelDesc, GetLevelCount, GetType, AddRef and Release
    run on its textures and surfaces (pure header work and XMemAlloc / XMemFree; the destroy's fence wait returns at
    once for the fence 0 these keep, and only render targets free EDRAM);
  - what touches the device goes to Renut through a **shadow header** per texture (N&B's layout, a copy of the fetch
    constant; the key of Renut's texture cache): SetTexture, the texture's and the surface's LockRect / UnlockRect;
    when the game's Release destroys a texture, Renut drops the shadow's host texture;
  - Renut's own objects (vertex buffers: `OwnsHeader`; declarations and shaders: records) keep Renut's AddRef and
    Release; GetType reports them in XDK 2135's numbers (surface 1 ... index buffer 7, N&B's 4 ... 2);
  - the device's own count (XDK 2135 at +12) goes to Renut's AddRef / Release / GetDevice, so the game's GetDevice /
    Release pairs cannot shut the device down.
- **Guest data:** the build qword `0x82392A38` (`0x0002000008574000`, XDK 2135), the `VdGlobalDevice` slot `0x8200056C`,
  the caps `0x82016688` (GetDeviceCaps `sub_82136DB8`; Feeding Frenzy's but for five words), the format table
  `0x82015E38`, XMemFree `sub_820CEAA8`, GetCurrentThreadId `sub_820CDF50`.
- **Shaders:** the game compiles 7 at start (5 pixel, 2 vertex) with XDK 2135's compiler: version 0x0E containers, as
  Feeding Frenzy's (`container_0e.h` converts them). The trace build wrote them; named by XXH3 they are
  `out/renut_shader_dump` (`out/zuma_shaders`).
- **PC, Renut on Vulkan** (`renut3`, 90 s): the splash, title, main menu, Temple of Zukulkan, the instructions and Level
  1-1 as with the emulated GPU (the menu compared side by side: the same image), 60 game frames a second, no shader
  missing, no warning.
- **"Unlock Full Game"** stays in the main menu: the license is the full game's (`license_mask = 1`: the game's check
  `sub_820F3168` leaves its trial byte 0, and IsTrial `sub_820F3160` returns 0 to every caller, the menu's
  `sub_820A2DB0` too). In the full game the menu marks that button with its flags +31 / +32 and links the menu's
  navigation past it (`100 -> 92`); the button is still drawn (its widget has no image of its own for that state).
  The game's behaviour, not the port's.

## The first Switch build (2026-10-27)

- **Shaders:** XenosRecomp with `BANJO_HLSL_OVERRIDE` / `BANJO_DEKO3D_SPIRV_DIR` (docs/building.md, section 3), then
  `deko3d_shaders.py` (7 of 7 compile with uam: 2 vertex of 2 KB, 5 pixel of 1 KB), `deko3d_glsl_opt.py --check`,
  `deko3d_shader_package.py` (13 KB), `renut_switch_shader_cache.py`. All 7 are the game's run-time shaders, so the
  NRO carries them all.
- **Build:** `tools/win/build_switch.ps1 -Pgo none -Define "ZUMA_SWITCH_LTO=OFF"` (a development build: no LTO, no
  PGO). NRO 23.8 MB, "Zuma" 0.1.0 with the game's `title_image.png` (64x64) scaled to 256x256 as the development icon
  (`out/icon_dev.jpg`; game data, not committed). The tomls' headers name Zuma (the code generator's manifest with this
  game's default.xex and Title Update #2).
- **`tools/check_switch_build.py`:** all checks passed, with:
  - KNOWN_HALTS: memcpy `sub_82113040` (Feeding Frenzy's byte for byte: its three `b .` at 0x82113204, 0x82113334,
    0x82113444);
  - STREAMING_COPIES: none (as in Feeding Frenzy);
  - the vector fused multiply-adds of `vmx_fused_madd`: 135 in 28 functions, the generated code's `vmx_madd_ps` sites;
  - feedingfrenzy2-nx's later rule for the vector fused multiply-adds LTO inlines from a callee (for the release).
- **Ryujinx** (`emu1`, 150 s; SD `switch\zuma-nx` a junction to `out/emu_sd/zuma-nx` in the Peggle data folder;
  `tools/build_asset_pack.py` made the game pack, 28 files with `default.xexp`, 26 MB; `out/zuma_emu_script.txt`: A to
  the level, then the stick swept with a shot every second): the title, Temple of Zukulkan, Level 1-1 played (matches,
  "COMBO X2", the score to 1,140, a lost chain and the level again); 59.3-60.0 game frames a second in most 10 s
  windows (53.9 and 55.5 in the last two, the busiest part of the level), Renut's render thread 1.2-5.3 ms a frame, no
  shader missing, no error in the log.
- **Delivered:** `out/dist/dev_20261027_zuma/zuma-nx/` (NRO `e6416a95...`, the game pack, README.txt); ELF and map
  `out/nx/elf/zuma_dev_20261027_0045.*`.

## Optimising the hot functions (2026-10-27, from the emulator)

The user: "optimise zuma's functions in the same manner you did feeding frenzy 1 and 2's". The method is
docs/optimising-functions.md, as in both Feeding Frenzy ports.

- **Profile** (Ryujinx `prof1`: 330 s, Level 1-1 played with `out/zuma_emu_script.txt`, `switch_profile_report` /
  `switch_profile_stacks`; `out/renut_tests/emu/prof1/rex_profile.log`, map `out/nx/elf/zuma_dev_20261027_0045.map`;
  `tools/switch_hot_functions.py --top 5000 --from-report 4`, all guest threads): Zuma is light. The game's main thread
  is 5.6 % of the busy samples (3-4 ms of a 16.7 ms frame), and only **62** generated functions were sampled at all;
  56 are eligible (the other 6 are tiny leaves the generator already inlines). On the main thread: the string compare
  `sub_82073B70` 10.1 %, memcpy (already native) 5.3 %, `sub_82071BD8` 3.8 %, `sub_820E4300` 2.5 %. The second hottest
  overall, `sub_8236BB48`, is on the audio side (1,781 instructions, not the frame's).
- **Hand-written natives:**
  - `h_82073B70.h`, MSVC's `std::string::compare(pos, n, s, count)` (205 samples, the hottest): the bytes compared
    eight at a time on host memory, the first difference from the XOR; the throw and ranges the guest mapping splits
    (at 0xE0000000, past 4 GB) run the copy, decided before the native's one store (the back chain). Its copy may call
    (the throw), so its hook is unguarded; in verify mode the native checks every call itself against the copy (the
    registers the copy writes back, lr, the stored word).
  - `h_820E4300.h`, a 3x3 matrix product (51 samples; the main thread's third): Bejeweled 3's h_822DFF80 method with
    Zuma's own order of operations (two loops: fmuls of the k=1 term, then fmadds with k=0, then k=2): both inputs
    read once, the nine results stored together; an overlap with an input takes the copy.
  - Not hand-written: `sub_82071BD8` (`std::string::_Grow`: its common path already makes no call; the copy is near
    the minimum) and `sub_8236BB48` (under docs/hand-natives.md every one of its 293 stores stays, and it is the audio
    thread's).
- **Localized copies:** all 56 eligible (35 in `localized.txt`, 21 in `localized_bulk.txt`). **Write-backs of the
  changed registers only:** 2,737 register stores kept of 5,654 in the 49 copies it limits (51 % removed); smart sync at
  62 of 216 call sites. Seven copies keep every write-back (five jump tables, an unaligned vector store loop, one use of
  XER); 16 inline tiny callees.
- **Checks:**
  - PC verify mode (`verify1`, Renut, Level 1-1 played through a lost life, 150 s): 42.8 million native calls, every
    leaf copy's and the C runtime natives' checked, the string compare's 4.0 million and the matrix product's 1.7
    million among them: 0 differences. 60 fps even in verify mode.
  - `tools/check_switch_build.py`: all checks passed (the memcpy halts, 135 vector fused multiply-adds).
- **Measured:**
  - Ryujinx, the same NRO with the 56 natives on (`prof2`) and their keys off (`prof3`), the same script and length:
    the main thread's median 3.14 -> 2.78 ms a game frame; 54.7 -> 57.5 fps on average (19 -> 8 of 30 windows below
    58; Ryujinx's dips are the emulator host's). Per function the emulator's samples (which land on function entries)
    disagree with themselves: the matrix product 54 -> 24 and `_Grow` 73 -> 55, but the string compare 185 -> 276.
  - So the two hand-written ones were measured on the PC with the same script and length (`out/renut_tests/pcab`,
    the PC profiler at 4,000 Hz, call counters on): the string compare 175 -> 146 samples (-17 %; 3.7 million calls in
    the run), the matrix product 223 -> 67 (-70 %).
- Delivered: the optimised NRO (`6ed8b747...`) in `out/dist/dev_20261027_zuma/zuma-nx/`, replacing the first one; ELF
  and map `out/nx/elf/zuma_dev_opt_20261027.*`. A development build (no LTO, no PGO).

## Languages (2026-10-27)

- **The game's eight:** `Resources/resources.xpr` holds `Images/<name>` and `Fonts/<name>` for English, Japanese,
  German, French, Spanish, Italian, Korean and Chinese (traditional: `_cht` fonts); no Portuguese. The game names its
  language from XGetLanguage (`sub_820F3960`, a virtual method: 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish,
  6 Italian, 7 Korean, 8 Chinese, anything else English; the strings at `0x82011A88`, `0x82003B08`, ...).
- **The port's option** (`zuma_language`, from Bejeweled 3's) offered en, ja, de, fr, es, it and pt: now en, ja, de, fr,
  es, it, ko and zh (XDK languages 1-8). An empty option follows the system: the Switch's Korean gives ko, both
  Chinese scripts zh, Portuguese en (Windows: the same by the display language).
- **PC runs** `lang_<xx>` (40 s each, `REX_ZUMA_LANGUAGE=<xx>`; `out/renut_tests/languages.jpg`): each of the eight
  shows its own title prompt ("DRÜCKEN SIE A, UM ZU STARTEN", "A POUR COMMENCER", the Japanese, Korean and Chinese
  ones), main menu ("Willkommen bei Zuma!", "¡Bienvenido a Zuma!", "Benvenuto a Zuma!") and adventure screen ("TEMPEL
  VON ZUKULKAN", the Japanese, Korean and Chinese names).
- **Switch** (development build, no LTO / PGO; `check_switch_build.py` passed): Ryujinx `lang_ko` (70 s,
  `zuma_language = "ko"`) shows the Korean title prompt, main menu ("Zuma에 오신 것을 환영합니다!") and adventure
  screen. Delivered in `out/dist/dev_20261027_zuma/zuma-nx/` (NRO `296f5866...`, the README with the languages); ELF
  and map `out/nx/elf/zuma_dev_lang_20261027.*`.

## Touch controls (2026-10-27)

The user: "Is there symbols in the code to add touch support to zuma and zuma's revenged like bejeweled 3 ... I want all
menu items clickable and tapping in levels shoots the ball in that direction". Zuma's framework is an early
SexyAppFramework cut down by the Xbox 360 port (a 39-slot Widget vtable, no RTTI names, no mouse handling left), so
Bejeweled 3's method (the framework's touch path) does not apply: `app/src/zuma_touch.cpp` drives the pad's way
(`input_script::Press`, the touch controls' pad) on what the game keeps.

- **Found with a PC test aid** (`app/src/zuma_memdiff.cpp`, `zuma_diag_memdiff = "f0,f1,f2"`: the guest words that
  change between f1 and f2 only, with the stick moved in between; `zuma_diag_memdump`): the Board (vtable `0x82001220`,
  built by `sub_82077878`) holds its Gun at +600, whose +4 changed (0 -> 2.172 with the stick right): the angle in
  radians, the shot's direction (sin a, cos a) on the screen (0 down); its centre at +8 / +12 ((327, 233) in level 1-1).
  The Board's +164 is the app, a static object at `0x823D13B8`: +1620 the Board (`sub_820C7F08` stores it), +744 the
  WidgetManager, whose focus is at +56 (SetFocus `sub_820DE870`). The game's 640x480 is drawn at 1.25x from (240, 60)
  of the 1280x720 screen (the frog at (649, 351)).
- **A tap:** in a level (the Board has the focus) the Gun is turned towards it, then A; on a menu (the focused widget
  has neighbours: the entries are linked at +56 / +60 / +64 / +68, their place and size at +8 / +12 / +16 / +20) the
  entry under it is focused, then A; a screen of button prompts alone (the adventure screen's "A Play" / "B Main
  Menu", 0 wide) or with no neighbours ("PRESS A TO START", the instructions): A.
- **Checked:** the PC, touch alone (`touch2`): the title, Adventure from the main menu, the adventure screen, the
  instructions, level 1-1 shot by taps (170 -> 290); Help & Options from the main menu opens its Options dialog
  (`touch3`). Ryujinx, the Switch build, touch alone (`touch_emu1`): the same path, level 1-1 to 870 points.
- **Not by touch:** opening the pause menu (the + button); the Options dialog's sliders take a tap as focus and A only.
- **Delivered:** `out/dist/dev_20261027_zuma/zuma-nx/` (NRO `6eae2902...`, the README with the touch controls); ELF and
  map `out/nx/elf/zuma_dev_touch_20261027.*`.

## Touch: hold to aim (2026-10-28)

The user: "For Zuma and Zuma's revenge I'd like to if you do a tap held down you can rotate the frog/aiming reticle and
when you tap again you fire". A touch in a level (`LevelTouch` in `app/src/zuma_touch.cpp`) is now followed to its end:

- **A tap** turns the frog to it (Gun +4) and shoots (A) when it ends (before: at once when it began).
- **A hold**, a touch longer than 12 frames (0.2 s) or one that moves more than 24 screen pixels, turns the frog after
  the finger each frame and shoots nothing; the frog stays aimed, and the next tap on that level shoots where it points,
  wherever it lands (a hold then aims again). The level ending or the pause menu taking the focus ends the touch.
- The Switch's touch screen now reports the finger's moves (each frame while it is down), the PC's mouse its moves
  while the button is down.
- **Checked** (PC, `hold1`, touch alone): taps shot at their angle (-0.059 straight down, -2.117, 2.113, 0.902); a hold
  dragged from (528, 392) to (128, 392) left the frog at -0.897 with no shot, and the next tap at (528, 112) shot at
  -0.897 (the shot's picture: the frog and its aim line to the lower left); a still hold (0.5 s) at (320, 48) aimed at
  -3.104 and the next tap shot there; the tap after shot at its own place again.

## 1.0.0 (2026-10-28)

The user: "make this image in downloads zuma 1's icon ... make the version 1.0.0 the author ChanseyIsTheBest do LTO and
upload it to github alongside making a web installer for it", "make sure all logging is off for the release build".

- **The NRO:** version 1.0.0, author ChanseyIsTheBest, the icon `out/icon.jpg` (the user's art, 256x256; not committed),
  logging off (the built-in settings; only a crash writes `logs/rex/rex_crash.log`), link-time optimisation on, no
  profile-guided optimisation (`build_switch.ps1 -Pgo none -Define "ZUMA_SWITCH_LTO=ON"`). NRO `6dcc3268...`;
  `tools/check_switch_build.py`: all checks passed. The player's `zuma.toml` has the language, the controllers and
  `zuma_touch`.
- **Ryujinx** (`rel2`): the release NRO with the zip the installer's code made (`test/package_node.mjs`), a first start
  with no `zuma.toml`: the game wrote `zuma.toml`, `deko3d_shaders.bin` and `pipeline_list.bin` and nothing else (no
  `logs/`, no `deko3d.log`); no emulator error. The user tested the NRO on the console ("it's perfect").
- **The installer** (zuma-nx-installer, from feedingfrenzy-nx-installer): finds the game and Title Update #2 (a package
  or `default.xexp`), checks `default.xex` and `default.xexp` by their SHA-256, and packs the update's `default.xexp`
  with the game. Its game pack is `tools/build_asset_pack.py`'s byte for byte apart from the pack id; a game without
  the update is refused ("Title Update #2 was not found").
- **Published** (the user: "yeah upload zuma 1 and it's web installer to github make sure all logging is turned off for
  it it's perfect"): https://github.com/ChanseyIsTheBest/zuma-nx (release v1.0.0 with `zuma.nro`, `6dcc3268...`) and
  the installer page https://chanseyisthebest.github.io/zuma-nx-installer/ (repository zuma-nx-installer; its
  deployment downloads the NRO from the release and checks it against `release/manifest.json`; the live page serves
  `6dcc3268...`).
