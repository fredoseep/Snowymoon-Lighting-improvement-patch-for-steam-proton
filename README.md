# ETS2 1.61 / Snowymoon / Proton diagnostic ASI

This is a logging probe with an optional experimental workaround. By default it identifies whether the game's D3D11 device context reaches `DiscardView` or `ClearView` shortly before the DXVK crash and forwards every intercepted call unchanged. This revision logs detailed view addresses only for the first 18 calls and then every 1000th call, avoiding file flushes on the rendering thread. A heartbeat reports call totals every five seconds.

## Build on Ubuntu

Install `cmake`, `ninja-build`, `git`, and the x86_64 MinGW-w64 POSIX C/C++ compilers. From this directory:

```bash
mkdir -p external
git clone --depth 1 https://github.com/TsudaKageyu/minhook.git external/minhook
cmake -S . -B build -G Ninja \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The output is `build/SnowymoonProbe.asi`. The MinHook source is not bundled.

## Install for the first test

Use a *64-bit* Ultimate ASI Loader distribution from its official GitHub release. Extract its `xinput1_4.dll` to `Euro Truck Simulator 2/bin/win_x64/` and place `SnowymoonProbe.asi` in the same folder. Leave Snowymoon's own `dxgi.dll` and `dinput8.dll` in place. If an `xinput1_4.dll` already exists in that folder, stop and inspect it before replacing it.

Set Steam launch options to:

```text
PROTON_LOG=1 WINEDLLOVERRIDES="dxgi,dinput8,xinput1_4=n,b" %command% -rdevice dx11
```

The supplied log shows this process loads `xinput1_4.dll` before DXVK's `d3d11.dll`. This is specific to the observed launch; confirm it again in the next Proton log. An ASI loader may also support `winhttp.dll`, but the observed game loads that later, so it could miss the device creation.

Run until the crash. Send `bin/win_x64/SnowymoonProbe.log` and the top of `game.crash.txt`. A useful log has `Probe loaded`, `d3d11 base`, a successful `D3D11CreateDevice...`, `Context QI`, and the last `DiscardView` / `ClearView` records. If there is no `Probe loaded`, confirm the ASI loader was loaded as *native* in the Proton log. If there is no device creation record, the loader arrived too late or the game used another creation path. If the log ends in a view record, the last `view` and `vtable` addresses let us locate the exact object in a debugger.

## Experimental test after confirming the original crash

The September 29 crash and probe both identify `view=0x473fec50` as the first crashing `DiscardView` argument. Its vtable is outside the DXVK module, and the crash occurs during a C++ runtime type conversion. To test whether bypassing that discard hint lets rendering proceed, **rebuild and install this updated probe**, and set launch options to:

```text
SNOWY_SKIP_EXTERNAL_DISCARD=1 PROTON_LOG=1 WINEDLLOVERRIDES="dxgi,dinput8,xinput1_4=n,b" %command% -rdevice dx11
```

This skips `DiscardView` only when the view's vtable address is outside the loaded DXVK `d3d11.dll` image. The log will say `SKIP DiscardView`. It still forwards `ClearView` and all other operations. This is a diagnostic experiment, not a validated rendering fix. Check the actual 3D world and lighting for artifacts. If it crashes at `ClearView`, send the new probe and crash logs. Remove `SNOWY_SKIP_EXTERNAL_DISCARD=1` to disable the experiment without rebuilding.

If the game stops responding while loading a save, leave it for 20–30 seconds and inspect successive `heartbeat` lines in `SnowymoonProbe.log`. Increasing `discard_calls` means the rendering thread is still making these calls; a fixed count with continuing heartbeats means only this worker thread is responding. Record whether Steam marks the process as running and send both `game.log.txt` and the fresh Proton log. The heartbeat's `uptime_ms` is the system uptime, not elapsed game time.

To revert, remove `xinput1_4.dll` and `SnowymoonProbe.asi` from the game folder and remove `xinput1_4` from `WINEDLLOVERRIDES`. Keep your original `dxgi.dll` and `dinput8.dll`.

## IDA / Wine debugger correlation

For the matched September 29 DXVK binary, the PE timestamp is `6ab4f7b7` and image size is `0x669000`. It loaded at `0x6ffff7db0000`. The actual RVA (absolute address minus module base) is `0x4ff1e6`, while the game's crash report's `0001:004fe1e6` is section-relative and differs by `0x1000`. The caller's return address is RVA `0x126b4a`; the preceding call at RVA `0x126b45` invokes the C++ RTTI routine starting at RVA `0x4ff1a0`. The faulting instruction is `mov rax,[rsi]`, with `rsi=0x80750017473fec60`. Do not patch this instruction. With the matching 64-bit DXVK DLL loaded in IDA at PE image base `0x359050000`, press `G` and enter `0x35954f1e6`, or `0x359176b45` for the call site. These numbers only apply to this exact DXVK binary.
