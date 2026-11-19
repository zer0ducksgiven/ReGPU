# ReGPU
<img width="2100" height="1500" alt="reGPU society" src="https://github.com/user-attachments/assets/5554e4b0-5b9f-4e8d-9839-314dc3e4eb7f" />

The Xbox 360 GPU (Xenos), on Direct3D 12, for native PC ports of Xbox 360 games made with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

ReGPU takes the GPU's half of a port off your hands. The recompiled game talks to it exactly as it
talked to the console: it writes command packets into a ring buffer and calls the kernel's `Vd*` video
functions. ReGPU runs the command processor, translates the Xenos shader microcode to HLSL, emulates
EDRAM, resolves, MSAA and the texture formats, presents the frames, and can upscale them (FSR 1,
optionally FSR 3 and DLSS).

It came out of a working port, Forza Motorsport 2, where it renders the whole game. Everything
specific to that game has been removed; what remains is the console's behaviour, plus hooks for the
workarounds a particular game needs.

## Requirements

- **XenonRecomp.** ReGPU is for XenonRecomp ports and builds against it: it uses XenonRecomp's
  `PPCContext` and calling convention to provide the console's `__imp__Vd*` video exports and to call
  the game's graphics interrupt. Your recompiled game links them like any other import. (Ports made
  some other way are not supported.)
- Windows 10/11, a Direct3D 12 GPU (feature level 12_0), clang-cl (as XenonRecomp's output needs),
  CMake 3.25+, Ninja, Python 3 (a build step embeds compiled shaders).
- The AMD FidelityFX SDK (MIT) for the FSR 1 shader headers. FSR 3 needs its signed runtime DLLs and
  DLSS needs NVIDIA's DLSS SDK; both are off by default.

## Building

ReGPU is a static library your port links. In your port's CMake:

```cmake
set(REGPU_XENONRECOMP "${CMAKE_SOURCE_DIR}/thirdparty/XenonRecomp")   # XenonRecomp checkout
set(REGPU_PPC_DIR     "${CMAKE_SOURCE_DIR}/out/ppc")                  # your game's XenonRecomp output (ppc_config.h)
set(REGPU_FFX         "${CMAKE_SOURCE_DIR}/thirdparty/fidelityfx/Kits/FidelityFX")
add_subdirectory(thirdparty/ReGPU)
target_link_libraries(game PRIVATE regpu)
```

Options: `REGPU_FSR3` (with the FidelityFX signed DLLs), `REGPU_DLSS` and `REGPU_DLSS_DIR`.

`REGPU_FSR1_CS` takes a pre-built `fsr1_cs.h` (the header `tools/embed_hlsl.py` writes into the build's
`generated` folder). With it the build copies that header and needs no Python, fxc or FidelityFX headers,
which helps cross-builds from Linux. The header has to come from the same `src/gpu/fsr1.hlsl`: rebuild it
when that file or its entry points change.

Ship Microsoft's `d3dcompiler_47.dll` (the Windows SDK's `Redist\D3D\x64`, redistributable) beside
your executable: ReGPU loads it from there first. Under Wine/Proton the system copy is Wine's own,
which can't compile the translated shaders.

To check that ReGPU builds on its own: `cmake --build build --target regpu_linkcheck`.

## Hooking it up

Everything is in [`include/regpu/regpu.h`](include/regpu/regpu.h). Before the game initialises
its graphics:

```cpp
#include <regpu/regpu.h>

regpu::Config& config = regpu::GetConfig();
config.width = 1920; config.height = 1080;
config.msaa = 4;
config.userDir = savesFolder / "regpu";      // shader and pipeline caches
config.windowTitle = L"My Game";

regpu::Host& host = regpu::GetHost();
host.base = guestMemoryBase;                 // the guest's 4 GB address space
host.alloc = [](bool physical, uint32_t size, uint32_t align) { return MyGuestAlloc(physical, size, align); };
host.createThread = [](const char* name, void (*body)(PPCContext&)) { MyCreateHostGuestThread(name, body); };
host.currentPcr = [] { return MyCurrentThread().pcr; };
host.guestCall = [](PPCContext& ctx, uint32_t address) { MyGuestCall(ctx, address); };
host.interruptLockAcquire = [](int cpu) { MyInterruptLock(cpu); };
host.interruptLockRelease = [] { MyInterruptUnlock(); };

regpu::Start();   // window, device, command processor
```

Your runtime already has these: they are the pieces any XenonRecomp port builds for its own threads
and memory. ReGPU never touches the rest of the kernel (files, threads, XAM, audio), which stays yours.

The renderer currently assumes the game draws at 1280x720, as most Xbox 360 games do (see
[`docs/xenos-notes.md`](docs/xenos-notes.md)).

### Your own device and presenter

By default ReGPU creates its own Direct3D 12 device (on the high-performance adapter) and presents
through its own swap chain, on its own window or on `Config::hostWindow`. A host that already draws
(its own menus, overlays, a recorder) can take the device over, or the device and the presenting: a
host device without a host presenter is fine, a host presenter needs the host device and queue.

| Option | Effect |
|---|---|
| `Config::hostDevice`, `Config::hostQueue` | Your `ID3D12Device*` and its direct `ID3D12CommandQueue*`: ReGPU renders on them instead of creating its own, and chooses no adapter (`REGPU_ADAPTER` does nothing). Set both or neither (one alone ends the process at `Start` with a FATAL line in `regpu.log`), and keep both alive until `Start` has returned: ReGPU takes its own references in `Start`. `REGPU_D3D12_DEBUG`, `REGPU_D3D12_GBV` and `REGPU_DRED` then turn nothing on (turn them on when you create the device). |
| `Host::presentImage` | You present. ReGPU opens no window and no swap chain, and hands you each finished picture (`PresentImage`: an R8G8B8A8 image already encoded for display, its resource state, and the optional progress overlay with its place): once for every guest swap once the renderer has started (from the first draw on), after that frame's commands were submitted to the queue (not waited for), and for each hold and first-boot screen. Frames and first-boot screens come on the renderer's thread; a hold comes on whichever thread waits for a shader or pipeline, which with `REGPU_DRAW_WORKERS` can be a draw piece worker. Return the image and overlay in the states given, with your work on them submitted on `Config::hostQueue` (the queue ReGPU renders on) before the callback returns: ReGPU hands over no fence, so that queue is what orders your reads after its writes and its next writes after your reads. Do not keep either pointer past the call: ReGPU writes both again for the next picture. It therefore needs `Config::hostDevice` and `hostQueue`; without them the process ends at `Start` with a FATAL line in `regpu.log` (not with `REGPU_HEADLESS` or `REGPU_NO_HOST_PRESENT=1`, which never call it). `regpu::SetFullscreen` does nothing then (the window is yours); `SetOutputSize` changes the size of the pictures you get. `REGPU_NO_HOST_PRESENT=1` (read at `Start`; flipping it later does nothing) ignores the callback, and ReGPU presents itself: it creates its own swap chain on `Config::hostWindow` (or its own window), which fails if you already have a swap chain on that window. |

### Game-specific fixes

When a game needs something the console did that isn't the GPU's job, or a workaround for one of
its shaders, use the optional hooks instead of changing ReGPU:

| Hook | For |
|---|---|
| `rewriteShader` | Edit a shader's translated HLSL before it compiles, and tag it |
| `onDraw` | Set four `float4` constants (`vuser`) for draws with tagged shaders |
| `resolveWriteback` | Copy a resolved image back to memory because the game reads it on the CPU (screenshots, thumbnails, focus readback) |
| `staticShaders`, `bootScreen` | Compile every shader in the game's files on first boot, with your own progress screen |
| `onChar`, `onSwap` | Typed text (cheat codes), a callback per frame |
| `logThreadStacks`, `currentStack`, `slowFrame`, `cpuUsage` | Diagnostics |
| `SetGameWidescreen(true)` | The game itself draws wider than 16:9, so ReGPU leaves the field of view alone |

## Debugging

Start the game with `REGPU_DEBUG=1` and drive it from outside with
[`tools/regpuctl.py`](tools/regpuctl.py): press buttons, take screenshots, capture a frame, edit
shaders live, flip `REGPU_*` switches. [`CLAUDE.md`](CLAUDE.md) is the playbook.
[`tools/xbdm.py`](tools/xbdm.py) and [`tools/xbox_fb.py`](tools/xbox_fb.py) take reference
screenshots and memory reads from a real (modded) console over XBDM.

## Credits and licences

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (MIT), which ReGPU builds against.
- [Xenia](https://github.com/xenia-project/xenia) (BSD 3-Clause)'s research into the Xenos GPU informed this
  renderer. A few routines follow Xenia's closely: the PWL gamma conversion (`renderer.cpp`), the texture
  tiling and packed-mip offsets (`texture.cpp`). For those:

  > Copyright (c) 2015, Ben Vanik. All rights reserved.
  >
  > Redistribution and use in source and binary forms, with or without modification, are permitted
  > provided that the following conditions are met:
  > - Redistributions of source code must retain the above copyright notice, this list of conditions
  >   and the following disclaimer.
  > - Redistributions in binary form must reproduce the above copyright notice, this list of
  >   conditions and the following disclaimer in the documentation and/or other materials provided
  >   with the distribution.
  > - Neither the name of the project nor the names of its contributors may be used to endorse or
  >   promote products derived from this software without specific prior written permission.
  >
  > THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
  > IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
  > FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
  > CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  > DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  > DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
  > IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
  > OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
- AMD FidelityFX SDK (MIT); NVIDIA DLSS SDK (its own licence, only with `REGPU_DLSS`); Microsoft's
  `d3dcompiler_47.dll` (Windows SDK redistributable).

ReGPU is released under the [MIT licence](LICENSE).
