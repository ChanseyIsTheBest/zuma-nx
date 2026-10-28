<h1 align=center>Zuma — Nintendo Switch port</h1>

A native Nintendo Switch port of the Xbox 360 (Xbox Live Arcade) version of **Zuma** (2007), by PopCap Games and
Oberon Media: shoot coloured balls from the stone frog idol and match three or more before the chain reaches the skull.

It's not an emulator. The PowerPC code of the game's `default.xex` (with its Title Update #2) is statically recompiled
to C++ with [ReXGlue](https://github.com/rexglue/rexglue-sdk) and compiled for the Switch, and the game is drawn by
Renut's Direct3D-level renderer ([reNut](https://github.com/masterspike52/reNut) by masterspike52, as banjo-nx carries
it) directly on the Switch's own graphics API, deko3d. It is made from the
[Feeding Frenzy port](https://github.com/ChanseyIsTheBest/feedingfrenzy-nx)'s code.

> [!NOTE]
> You need your own copy of the game and its title update, from your own Xbox 360:
> - the **Xbox Live Arcade** game's content package
>   (`Content/0000000000000000/584107EF/000D0000/90226B760A840A3AFC759094668DA3FBE4DF716D58`);
> - **Title Update #2** (`Content/0000000000000000/584107EF/000B0000/TU_1C421VF_0000104000000.00000000000O1`). The
>   port's code is the updated game, so the update is needed.

## How to install

1. Open the installer page: **https://chanseyisthebest.github.io/zuma-nx-installer/**
2. Choose your game and its title update (their packages, or a folder that holds them) and press
   **Create zuma-nx.zip**. Everything runs in your browser and the game files never leave your computer: the page
   checks your copy, downloads the build and makes the game pack from your files.
3. Extract the downloaded `zuma-nx.zip` into `sdmc:/switch/`.

Your `zuma-nx` folder should look like this:

```text
/switch/zuma-nx/
  zuma.nro
  LICENSES.txt
  zuma.toml                 (your settings; written at the first start when it is missing)
  game_root.nxpack          (the game and its title update, in one file, made from your copy)
  game_root.nxidx
```

Launch it with a game override (hold **R** while starting an installed title) or a forwarder. Album applet mode does
not give the game enough memory. If you use a forwarder, set it to a **39-bit address space**.

To build everything yourself instead, see [docs/building.md](docs/building.md).

> [!CAUTION]
> **The first time you play**, the game's shaders are compiled for the console, so **it can stutter for a moment** at
> the start. They are saved in `deko3d_shaders.bin` next to the NRO and later sessions start smoothly.

## Controls

The **left stick** aims the frog and **A** shoots; **A** selects in the menus and **B** goes back. By default the face
buttons work **by position, as on the Xbox 360 pad** the game was made for: the bottom button is the Xbox's A, the
right one the Xbox's B, and so on.

<div align="center">

| Switch | Xbox 360 |
| --- | --- |
| B (bottom) | A |
| A (right) | B |
| Y (left) | X |
| X (top) | Y |
| ZR / ZL | RT / LT |
| R / L | RB / LB |
| + | Start |
| − | Back |
| Sticks / D-pad | Sticks / D-pad |

</div>

To use the buttons by letter instead (the Switch's A acts as A), set `input_xbox_layout = false` in `zuma.toml`.

**Touch screen** (handheld mode; `zuma_touch` in `zuma.toml`, on by default):
- in a level, tap where the ball should go: the frog turns there and shoots;
- or touch and hold: the frog follows your finger without shooting, and stays aimed when you let go; your next tap
  then shoots where it points, wherever you tap;
- tap a menu entry to choose it, and tap anywhere on a screen with only "A" to continue.

Opening the pause menu is the **+** button, and the Options dialog's sliders are moved with the stick.

The game has no multiplayer. Xbox LIVE (leaderboards) is not available.

## Settings

`zuma.toml`, next to the NRO, holds only your options. Everything else is built into the NRO.

<div align="center">

| Setting | What it does |
| --- | --- |
| `zuma_language` | Empty (default): the console's system language (English when the game does not have it). Or `en`, `ja`, `de`, `fr`, `es`, `it`, `ko` or `zh` |
| `input_xbox_layout` | Face buttons by position (`true`, default) or by letter (`false`) |
| `input_joycon_sideways` | A single Joy-Con held sideways (`true`, default) |
| `zuma_touch` | Touch controls (`true`, default) |

</div>

If you delete `zuma.toml`, the game writes the default one again at the next start.

## Resolution and performance

The game draws **1280x720** in handheld and docked mode (its 640x480 picture scaled up in the middle, as on the Xbox
360) and runs at **60 FPS**. No log files are kept; only a crash writes `logs/rex/rex_crash.log`.

## How it works

- **The game's code is translated, not emulated.** [ReXGlue](https://github.com/rexglue/rexglue-sdk) translates every
  function of the game's program (with Title Update #2 applied) to C++, which is then compiled for the Switch. ReXGlue
  also gives the game what it expects from an Xbox 360: its system, files, saves, audio and controllers.
- **Renut's renderer on deko3d.** The game's Direct3D calls (XDK 2135's Direct3D: its device tables and resource
  headers mapped function by function onto Renut's) are drawn directly by Renut's Direct3D-level renderer through
  [plume](https://github.com/renderbag/plume)'s deko3d back end.
- **Shaders.** The game compiles its 7 GPU programs (5 pixel and 2 vertex shaders) while it starts, with the Xbox's
  shader compiler, so none is stored in its files: they were translated with
  [XenosRecomp](https://github.com/hedge-dev/XenosRecomp), DXC and SPIRV-Cross into deko3d's GLSL and the NRO carries
  them. The console compiles each shader variant once with [uam](https://github.com/devkitPro/uam) and keeps it in
  `deko3d_shaders.bin`.
- **Optimization.** The 56 busiest game functions (from an emulator profile of Level 1-1) run as translated native
  copies that keep the game's registers in C++ locals and write back only the registers that changed; two of the
  hottest are written by hand (see [docs/optimising-functions.md](docs/optimising-functions.md)). The release NRO is
  built with link-time optimisation.

## Documentation

<div align="center">

| Document | What it explains |
| --- | --- |
| [docs/building.md](docs/building.md) | How to build the PC test build, the NRO and the game data |
| [docs/zuma-notes.md](docs/zuma-notes.md) | Notes on the game's code and every change of the port, each with its evidence |
| [docs/optimising-functions.md](docs/optimising-functions.md) | How the game's hot functions are made faster, measured and checked |
| [docs/hand-natives.md](docs/hand-natives.md) | The rules for hand-written native functions |
| [docs/feedingfrenzy-notes.md](docs/feedingfrenzy-notes.md) | The Feeding Frenzy port's notes: the code this port is made from |

</div>

## Build

Windows, LLVM clang 20 and the VS 2022 Build Tools for the PC build; devkitA64, libnx and deko3d from
[devkitPro](https://devkitpro.org) for the Switch, with uam and SPIRV-Cross built for the PC
(`tools/deko3d_build_tools.sh`); Python; and your own copy of the game and its title update. The whole process is in
[docs/building.md](docs/building.md).

## Credits

- **PopCap Games and Oberon Media** — creators of Zuma and its Xbox Live Arcade version. This is an unofficial,
  fan-made port with no affiliation.
- **ChanseyIsTheBest** — this port and the ports it is made from (feedingfrenzy-nx, bejeweled3-nx, the Peggle ports).
- **[Tom Clay](https://github.com/rexglue/rexglue-sdk)** — the ReXGlue SDK, built on the work of the
  **[Xenia](https://xenia.jp)** team.
- **[StevensND](https://github.com/StevensND/nfsmw-nx)** — nfsmw-nx: the Switch layer of the SDK and much of the
  method; and [nfsmw-nx-installer](https://github.com/StevensND/nfsmw-nx-installer), the installer page this port's
  is built on.
- **[masterspike52](https://github.com/masterspike52/reNut)** — reNut: the Direct3D-level renderer the port draws
  with (its reuse in this port permitted by its author).
- **banjo-nx** (Banjo-Kazooie: Nuts & Bolts Switch port) — Renut's Switch port: the deko3d back end of plume, the
  shader generator and its optimisations.
- **[renderbag](https://github.com/renderbag/plume)** — plume; **[fincs](https://github.com/devkitPro/uam)** and
  devkitPro — deko3d and uam.
- **[hedge-dev](https://github.com/hedge-dev/XenosRecomp)** — XenosRecomp, the Xenos shader translator.
- **[devkitPro](https://devkitpro.org) and [switchbrew](https://github.com/switchbrew/libnx)** — devkitA64 and libnx.
- The sibling ports, which share their fixes with this one, and the libraries listed in
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal

No affiliation with PopCap Games, Oberon Media, Electronic Arts, Microsoft or Nintendo. "Zuma" is a trademark of
PopCap Games / Electronic Arts; the NRO's icon is the game's artwork, © PopCap Games. This repository contains no
assets or program code from the original game. Users must provide their own legally obtained copy, and everything made
from it (`assets/`, `app/generated/`, the game pack) is made on their own computer. Running homebrew requires custom
firmware, which violates Nintendo's ToS and can get a console banned — your call.

Source code is provided under the GPL-3.0 License (see [LICENSE](LICENSE)), like the ports it builds on. The SDK keeps
its BSD-3-Clause license (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
