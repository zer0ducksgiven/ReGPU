# ReGPU on Linux (Wine, Proton and vkd3d-proton)

ReGPU is a Direct3D 12 renderer, so on Linux it runs the way Windows games do: the Windows build
under Wine or Proton, with vkd3d-proton translating Direct3D 12 to Vulkan. This page shows how to
set that up, using Forza Horizon 1: ReImagined (FH1R, built with ReIGlue, our ReXGlue fork; it loads
ReGPU as `rexgpu-regpu.dll`) as the example, and what we measured on one Linux PC.

**Status (2026-11-03): ReGPU boots under vkd3d-proton on RADV, then hangs the GPU about 2 s after its
first frames.** The game's title screen never appears. The same Windows build reaches free roam
under Wine with FH1R's Vulkan renderer. See [Results](#results).

## What you need

- A Windows build of the game with ReGPU in it. For FH1R that's its ReGPU plugin
  ([FH1R PR #622](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/pull/622); these measurements used
  the earlier [#611](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/pull/611)), cross-built on Linux with
  `tools/build_windows.sh` (clang-cl and xwin).
- Wine with vkd3d-proton and DXVK's `dxgi.dll`. A copy of a Proton install from Steam has all
  three. We used Proton 11.0 (`proton-11.0-2e`: Wine 11.0, vkd3d-proton `212991fc`, DXVK
  `v2.7.1-498`). Copy it rather than pointing a prefix at Steam's own files.
- Microsoft's `d3dcompiler_47.dll` beside the executable. ReGPU loads it from there first, and
  Wine's built-in copy can't compile the translated shaders.
  - Use the Windows SDK's redistributable, `Redist\D3D\x64\d3dcompiler_47.dll`: 10.0.26100.7705,
    SHA-256 `a05f99734f7c4822fefc12b367af21fd0976ed6608752fb1e1e80b6ece7ecbbb`.
  - It ships in the Windows SDK 10.0.26100 cab `61d57a7a82309cd161a854a6f4619e52.cab` (file
    `fil3585cb2ea5db13cc0838f8d06b5c9679`), which Microsoft's Visual Studio download manifest lists
    under package `Win11SDK_10.0.26100`.
  - Don't use winetricks' `d3dcompiler_47` verb. It downloads the Windows 8.1 SDK's 6.3.9600 build,
    which has no shader model 5.1.
- The DLL override `d3dcompiler_47=n`. With it, Wine loads the native DLL even when the game asks
  for it by full path.

## Building on Linux

ReGPU's CMake compiles its FSR 1, depth copy, gamma and enhance compute shaders at build time,
with `py -3 tools/embed_hlsl.py`, which runs `fxc`. A Linux cross-build has neither. For the test
bed we used two small stand-ins on `PATH`:

- **`py`:** drops a leading `-3` and runs `python3`.
- **`fxc`:** runs Microsoft's `fxc.exe` under the same Wine, with its `d3dcompiler_47.dll` beside it.
  The script turns Unix paths into `Z:\` paths, so fxc doesn't read them as `/options`. We took fxc
  from the same SDK, in the "Windows SDK for Windows Store Apps Tools" MSI, cab
  `69661e20556b3ca9456b946c2c881ddd.cab`.
- **Cost:** 13 s for the five entry points.

Shipping the generated `fsr1_cs.h` bytecode would remove both stand-ins.

The rest of the build is the normal cross-build:

- The SDK came from CI's `sdk-overlay-windows` artifact.
- We ran `tools/build_windows.sh --game-only` with `REXSDK_WIN_PREFIX` pointing at that artifact.
- At `-j6` it took 3.5 minutes, and gave a 2.3 MB `rexgpu-regpu.dll`.

## Running it

FH1R's `tools/run_regpu_wine.sh` ([FH1R PR #616](https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined/pull/616)) does the setup and the launch:

```sh
tools/run_regpu_wine.sh setup "$HOME/.steam/steam/steamapps/common/Proton 11.0"  # copies it, makes the prefix
tools/run_regpu_wine.sh run --game_data_root=/path/to/game --user_data_root=/path/to/userdata
tools/run_regpu_wine.sh run --vulkan ...      # the same build on FH1R's Vulkan renderer, for comparison
```

By hand, the essentials are:

```sh
export WINEPREFIX=/scratch/pfx
# vkd3d-proton (d3d12, d3d12core) and DXVK's dxgi copied into $WINEPREFIX/drive_c/windows/system32
export WINEDLLOVERRIDES="d3d12=n;d3d12core=n;dxgi=n;d3dcompiler_47=n;winepulse.drv=d;winealsa.drv=d"
export REGPU_LOG_DIR='Z:\scratch\logs'    # regpu.log, else it goes to the working directory
wine forza.exe --gpu_plugin=regpu --gpu_backend=d3d12 ...
```

Disabling `winepulse.drv` and `winealsa.drv` leaves Wine without an audio driver. Our test runs also
muted the game.

## What ReGPU does differently under vkd3d-proton

- **Guest memory is copied, not mapped.**
  - `OpenExistingHeapFromAddress` succeeds on vkd3d-proton, but the GPU reads zeros through the
    placed buffer.
  - ReGPU's self-test (`PhysicalHeapAliasesGuest`) catches this and every vertex fetch takes the copy
    path. The log shows: `renderer: guest memory self-test at AA860000 wrote ..., the GPU read 00000000
    00000000`, then `vertex data is copied instead`.
  - `MapGuestMemory` retries at frames 30, 120, 600 and 2400, and each retry runs the self-test again.
- **The upload ring grows** to 384 MB a frame, against 128 MB with mapped memory.

## Results

Linux PC: Ryzen (8 cores), Radeon RX 9060 XT, RADV (Mesa 26.2.1), kernel 7.2, KDE Wayland. FH1R
Title Update 4, 1280x720 window, `--frame_rate=240 --vsync=false`, audio muted. Each timed run
used the same scripted drive:

- the title, the main menu and SINGLE PLAYER;
- free roam from a copied career save;
- the throttle held for 90 s on the spawn road.

Inputs came through FH1R's `FORZA_AUTOPRESS_FIFO`, which the Windows build tails as a plain file.

Medians of three warm runs. Each number is free roam with the throttle held. fps and 1% low are the
game's frames (FH1R's `RECOMP_PERF`), and "CPU / frame" is the SDK's GPU minute line: command
processor CPU time per frame and per draw. Other jobs shared the PC (load average 4-11, no other
game), so absolute numbers are low-confidence; the ordering held in every run.

| Renderer | fps | 1% low | CPU / frame | CPU / draw | Draws / frame | First boot |
| --- | --- | --- | --- | --- | --- | --- |
| FH1R Vulkan, native Linux | 198 (155-208) | 86 (54-94) | 4.2 ms | 1.4 us | 3,013 | no wait: pipelines build in the background (cold run: 207 fps, 1% low 80) |
| FH1R Vulkan, Windows build under Wine (winevulkan) | 152 (147-153) | 53 (44-59) | 5.2 ms | 1.7 us | 3,024 | no wait (cold run: 162 fps, 1% low 64) |
| ReGPU, Windows build under Wine + vkd3d-proton | GPU hang after 7-33 frames, see below | | | | | 112 s of shaders, 18 s of pipelines |

Screenshots of the title, the main menu and the spawn point match between native Linux and Wine:
the same picture, with mean differences of 3-13 levels from the clock and the camera settling.
Wine costs the Vulkan renderer about 25% of its frame rate and 25% more command processor time per
draw.

### ReGPU: first boot

- **Static shaders:** 1,689 compiled in 112.1 s. FH1R's Windows test boot took 140 s.
- **Seed pipelines:** 1,183 built in 17.6 s. 602 more wait for shaders the game makes at run time.
- **Total:** about 130 s to the first game frame on an empty cache. A warm boot preloads in 0.4 s.

### ReGPU: the GPU hang

Every run, warm or cold, the game presents 7-33 frames and then stops for good:

- **Kernel:** `amdgpu: [gfxhub] page fault ... Faulty UTCL2 client ID: SQC (data)` on the
  `vkd3d_queue` thread, then `ring gfx_0.0.0 timeout` and a ring reset.
- **vkd3d-proton:** `VK_ERROR_DEVICE_LOST` (`vr -4`) on the next submit.
- **ReGPU:**
  - its draw worker waits forever in `BeginFrameIfNeeded` (`WaitFenceValue(f.fence, INFINITE)`),
    for a frame the lost device will never finish;
  - the command processor waits on the worker (`work::Drain`).
- **Game:**
  - the render thread spins in D3D's wait for the read pointer, so the title screen never appears;
  - the ring stays 3 dwords short of the write pointer.

Each hang resets the GPU ring (amdgpu recovers it, but other GPU work on the PC stalls for those 2 s).

The last thing drawn before each hang is the title's first video frame: two new 1280x720 `k_8`
textures, which look like the video's planes. A scalar-cache (SQC) data fault is a shader reading
a descriptor or constant buffer outside mapped memory. Root CBVs and SRVs are raw GPU addresses in
vkd3d-proton and have no bounds, while Windows drivers often get away with the same read.

These all hang the same way:

| Switch | Frames before the hang |
| --- | --- |
| (defaults) | 7-33 |
| `REGPU_EARLY_FENCES=1` / `GLUE2GPU_NO_EARLY_FENCES=1` | 19 / 7 |
| `REGPU_NO_FENCE_WRITEBACKS=1` | 25 |
| `REGPU_NO_RECORD_THREAD=1` | 13 |
| `GLUE2GPU_NO_DRAW_THREAD=1` | 14 |
| `REGPU_DRAW_WORKERS=0` | 33 |
| all three off (draw thread, early fences, workers) | 31 |
| `GLUE2GPU_VSYNC=1` | 28 |
| `GLUE2GPU_FETCH_BOUNDS=1` (vertex fetches held to the fetch size) | 13 |
| `VKD3D_CONFIG=force_robust_physical_cbv` | 14 |
| FH1R `FORZA_RENDER_SYNC=0` | 24 |

Next step: one run with `VKD3D_CONFIG=breadcrumbs` (or `RADV_DEBUG=hang`) should name the draw and
shader that fault.

### Smaller things found on the way

- **Hang diagnostics crash under Wine.**
  - `Host::logThreadStacks` (FH1R's `stack_dump.cpp`) crashes in a vector `push_back` while it
    collects the thread list.
  - `GpuLogState` reads the device's retire block at `device + 0x2A10` through a guest address
    without translating it, and crashes too.
  - `winedbg` with the plugin's PDB gave the stacks above instead. Link the plugin with `/DEBUG`
    and `winedbg` resolves ReGPU's frames.
- **FH1R's frame statistics are blind to ReGPU.** `RECOMP_PERF` never sees a frame with the plugin,
  so its CSV stays empty. ReGPU's own `REGPU_PERF` lines are the numbers to use, and the GPU minute
  line is SDK-renderer only.
- **The shader cache can lose work.** A first boot that was killed later stored 334 of the 1,689
  compiled shaders, so the next boot recompiled the rest.
