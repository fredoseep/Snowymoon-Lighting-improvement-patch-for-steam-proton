# ETS2 1.61 / Snowymoon / Proton diagnostic ASI

**Testing on proton experimental**

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
DXVK_ASYNC=1 PROTON_NO_FSYNC=1 SNOWY_SKIP_EXTERNAL_DISCARD=1 PROTON_LOG=1 WINEDLLOVERRIDES="dxgi,dinput8,xinput1_4=n,b" %command% -rdevice dx11
```

