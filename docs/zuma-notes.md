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
