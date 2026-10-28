# Building zuma-nx

Everything below runs on Windows. You need your own copy of the Xbox Live Arcade game and its Title Update #2; nothing
made from them is committed (`assets/`, `app/generated/` and `out/` are git-ignored).

## Tools

- VS 2022 Build Tools (MSVC libraries + Windows SDK), LLVM clang 20 (portable is fine; by default `..\_tools\llvm`),
  Python 3 with `xxhash` and Pillow, Git. CMake and Ninja come from the VS Build Tools. Paths can be overridden, see
  `tools/win/env.ps1`. Configure and build through `tools\win\run.ps1` (it loads that environment): from a plain
  Git Bash a re-configure finds the wrong compiler and Ninja.
- For the Switch: devkitPro (devkitA64, libnx, deko3d), and uam and SPIRV-Cross built for the PC
  (`bash tools/deko3d_build_tools.sh`, into `out/deko3d_tools`). No Vulkan driver is needed: the NRO draws with deko3d.

## 1. The game files

```powershell
# The game and Title Update #2 (verifies the STFS hash tables, writes nothing to the packages). The update's
# default.xexp goes beside default.xex: the SDK applies it when the game loads, and the code generator reads the
# patched image, so every address in the port is the updated executable's.
python tools\stfs_extract.py "<...>\584107EF\000D0000\90226B760A840A3AFC759094668DA3FBE4DF716D58" assets\game_root
python tools\stfs_extract.py "<...>\584107EF\000B0000\TU_1C421VF_0000104000000.00000000000O1" assets\game_root
```

## 2. The code generator and the PC build

```powershell
# Third-party sources of the SDK (only the files this fork changed are kept in sdk/thirdparty)
python tools\fetch_thirdparty.py

# The code generator (-march=x86-64-v2 is required: the SDK uses SSSE3 intrinsics)
powershell -File tools\win\run.ps1 out\host_configure.log "cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2"
powershell -File tools\win\run.ps1 out\host_build.log "cmake --build out/host --target rexglue"

# The PC build on the emulated GPU first (Renut's renderer needs the game's shaders, section 3)
powershell -File tools\win\run.ps1 out\pc_configure.log "cmake --preset win-pc -S app -DZUMA_RENUT_RENDERER=OFF -DZUMA_PERF_POST=ON"
powershell -File tools\win\bj3_regen_build.ps1
```

`tools\win\bj3_regen_build.ps1` runs the code generator with its post steps (tools/post_codegen.py; it writes the
decrypted executable to `out\default_image.bin`) and then the build, keeping the build from running the generator a
second time (`tools/regen_restat.py`). Use it after every change of the code generation's inputs (overrides, hooks,
the hot lists): a new hook needs the post-codegen passes again (they make direct calls to functions no hook was known
for).

Runs of the PC build (muted and minimized by default; log and shots in `out\renut_tests\<name>`):

```powershell
powershell -File tools\win\bj3_run.ps1 -Name boot1 -Seconds 60 -InputScript "20s:A,26s:A,32s:A"
```

- `-InputScript` drives a scripted pad (`app/src/zuma_input_script.h`), `REX_ZUMA_INPUT_SCRIPT_P2` a second player.
- `REX_ZUMA_CPU_VERIFY=true` checks every call of the native functions against the recompiled code.

## 3. The shaders

The game compiles all of its shaders (7) while it starts, so `default.xex` stores none. Renut's renderer hooks the same functions, so the trace build is a separate configuration (`-DZUMA_RENUT_RENDERER=OFF`): its PC run with `zuma_d3d_trace = <game frame>` writes every shader container the game creates to `out\pc\out\ff_shaders`. Each is
named by its XXH3 (`vs_` / `ps_` + the hash of the container) and copied into the renderer's shader dump:

```powershell
python tools\renut_shader_dump.py --image out\default_image.bin --dumps <the named containers> --out out\renut_shader_dump
powershell -File tools\win\run.ps1 out\pc_configure.log "cmake --preset win-pc -S app -DZUMA_RENUT_RENDERER=ON -DZUMA_PERF_POST=ON"
powershell -File tools\win\bj3_regen_build.ps1
```

For the Switch:

```powershell
$env:BANJO_HLSL_OVERRIDE = "$PWD\out\pc\renut_gpu\early_alpha_hlsl"
$env:BANJO_DEKO3D_SPIRV_DIR = "$PWD\out\deko3d_shaders\spirv"
out\pc\xenos_recomp\build\XenosRecomp\XenosRecomp.exe out\renut_shader_dump out\deko3d_shaders\scratch_cache.cpp app\thirdparty\reblue-XenosRecomp\XenosRecomp\shader_common.h
Remove-Item env:BANJO_HLSL_OVERRIDE, env:BANJO_DEKO3D_SPIRV_DIR
python tools\deko3d_shaders.py --spirv out\deko3d_shaders\spirv --out out\deko3d_shaders
python tools\deko3d_glsl_opt.py --in out\deko3d_shaders\glsl --out out\deko3d_shaders\glsl_opt --check
python tools\deko3d_shader_package.py --glsl out\deko3d_shaders\glsl_opt --out out\renut_switch
python tools\renut_switch_shader_cache.py
```

## 4. The NRO

No installer can make the game's shaders from its files (none is stored there), so the NRO carries them all.
The release is built with link-time optimisation and without profile-guided optimisation:

```powershell
powershell -File tools\win\build_switch.ps1 -Jobs 12 -BuildDir out\nx -Log out\nx_build.log -Pgo none -Define "ZUMA_SWITCH_LTO=ON"
python tools\check_switch_build.py
```

The NRO's icon is `out\icon.jpg` (256x256 JPEG; not committed). The Switch build's settings are compiled in:
`app/zuma_switch_defaults.toml` (Renut on deko3d, no log, 720p) is applied before the SD card's `zuma.toml`, and
`app/zuma_switch.toml` (language, controllers) is written as `zuma.toml` at the first start when there is none. A test
run turns the log on in its `zuma.toml` (`log_level = "info"`, `log_to_file = true`).

## 5. The SD folder

```powershell
python tools\build_asset_pack.py assets\game_root --out-dir out\pack --verify
```

`sdmc:/switch/zuma-nx/` takes `zuma.nro` and `game_root.nxpack` + `game_root.nxidx` from `out\pack`.

In Ryujinx, `tools/win/bj3_emu_run.sh <name> <seconds> [key = value ...]` runs the NRO from `out/emu_sd/zuma-nx`
(see the script for the data folder it expects) and collects its log and shots. The Switch profiler
(`switch_profile_report = true`, `switch_profile_stacks = true`) writes `rex_profile.log`; in Ryujinx, which gives the
NRO no path of its own, that is `sdmc:/switch/rex_profile.log`. `tools/switch_hot_functions.py` with
`out/nx/zuma.map` lists the hottest functions.
