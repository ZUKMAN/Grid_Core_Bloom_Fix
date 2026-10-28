# Grid_Core_Bloom_Fix

A `dinput8.dll` proxy for **Race Driver: GRID** (2008, EGO engine) that makes the game run on
modern multi-core CPUs and Windows 10/11, and removes the yellow filter without touching any game
files. Sister project of [Dirt1_Core_Bloom_Fix](https://github.com/ZUKMAN/Dirt1_Core_Bloom_Fix).

- No `workerMap` swapping, no `hardware_settings_restrictions.xml` edits, no `-novirtual`
- Full resolution list on cards with 2+ GB of VRAM (no 1280x1024 cap)
- Starts with Windows' "Beta: Use Unicode UTF-8" option enabled
- Yellow tint, bloom and vignette adjusted on the fly in `tracks/<track>/effects.xml`
  (text and binary), configurable in an `.ini`. Tone mapping / night lighting stay intact.
- Longer track surface LOD distance (`tracks/tracklodsettings.xml`, 30 m -> 1000 m)

Targets the Steam `grid.exe` (SteamStub-wrapped, `TimeDateStamp 0x4ACE02BC`), also a DRM-free
copy of it. Other builds get everything except the automatic `-novirtual`. Start the Steam version
from the Steam library: launching `grid.exe` directly fails with the DRM's
"Application load error 5:..." (the SteamStub can't attach to Steam), unrelated to this fix.

## Installation

1. Build it (see below) or take `dinput8.dll` from a release.
2. Copy `dinput8.dll` and `Grid_Core_Bloom_Fix.ini` into the game folder, next to `grid.exe`.
3. Undo old workarounds: restore the original `system/workerMap*.xml` and
   `hardware_settings_restrictions.xml`, remove `-novirtual`, turn off compatibility mode.
4. Start the game. `Grid_Core_Bloom_Fix.log` shows the CPU, the worker map, the display modes,
   and every rewritten file.

To uninstall, delete `dinput8.dll`.

## Configuration

| Section | Key | Default | Meaning |
|---|---|---|---|
| `Threads` | `Affinity` | `1` | `1` map ideal processors to physical P-cores, `0` none, `2` original |
| | `WorkerMap` | `1` | Redirect every `workerMap*.xml` to the game's 4/2/1 core map |
| `Memory` | `VirtualMemory` | `-1` | `-1` auto (off without LAA, like `-novirtual`), `0` off, `1` on |
| | `HardwareSettingsPoolKB` | `256` | Size of the `HARDWARE_SETTINGS` pool (16 KB in the game) |
| `PostProcess` | `Enable` | `1` | Rewrite track `effects.xml` files on load |
| | `Bloom` | `0.30` | `bloomAmount`, game uses ~1.0-1.1, `0` = off |
| | `Tint` | `0` | `0` removes the yellow/orange tone (`tintColour` -> white), `keep` = game |
| | `Vignette` | `0` | Dark screen edges |
| | `Brightness`, `Contrast`, `Saturation`, `ColourAmount`, `Gamma`, `MotionBlur` | `keep` | Optional overrides (`Saturation` = `colour`) |
| `PostProcessOverride` | *any `<Param>` name* | | Free-form overrides, e.g. `lumMin=0.40` |
| `Graphics` | `SurfaceLodDistance` | `1000` | `SURFACELOD loddistance` (game: 30), `0` = keep |
| | `SurfaceLodBlend` | `100` | `SURFACELOD blend` (game: 10), `0` = keep |
| `Video` | `MaxVideoMemoryMB` | `1024` | Video memory reported to the game |
| | `FilterModes` | `1` | Filter the D3D9 display mode list |
| | `MaxResolutions` | `12` | Max distinct resolutions (largest + desktop) |
| | `MinWidth` | `800` | Smaller resolutions are dropped |
| `Misc` | `FixUtf8CodePage` | `1` | Code page 1252 for the game when Windows uses UTF-8 |
| | `DPIAware` | `1` | Disable DPI scaling |
| | `Log` | `1` | Write the log file |

## What was wrong (analysis of grid.exe)

The Steam executable is wrapped in SteamStub 2.0 (`.bind` section, AES-encrypted `.text`). For the
analysis the code section was decrypted offline (XOR-chained header and payload, XTEA-CBC
`SteamDRMP.dll`, AES-256 with the key from the payload). The fix itself works with the original
wrapped executable: Windows resolves the import table before the stub runs, so the import hooks
are installed from `DllMain`.

### Threads and worker maps

- GRID never calls `SetThreadAffinityMask` (DiRT does). Workers only get
  `SetThreadIdealProcessor(core % dwNumberOfProcessors)`. Several pools and per-thread caches are
  sized by `GetSystemInfo().dwNumberOfProcessors`, the source of the "6 & 8 thread CPU" crashes.
- `hardware_settings_restrictions.xml` picks `workerMap2Core.xml` / `workerMap4Core.xml` by `cores`,
  and `2Core` again for `hyperThreading` (the CPUID HTT bit is set on every modern Intel CPU).
- `workerMap4Core.xml` is the real multi-threaded config: unlike DiRT, GRID supports workers in the
  physics `solver`, `integrator` and `cloth` sets. `workerMap8Core.xml` references the undefined
  workers `view_manager_2` / `view_manager_3`.

**Fix:** the engine sees 4 CPUs (2/1 on small CPUs) and always loads the game's `workerMap4Core.xml`;
ideal processors are mapped to physical P-cores (topology code ported from DiRT 2).

Tested on Windows 11 24H2 (build 26100), 8-core/16-thread CPU, 3 GB GPU, 3840x2160 @ 120 Hz desktop:
Windows reported 246 display modes (24 kept), the game asked for `workerMap16Core.xml` left over in
the config by another fix and got `workerMap4Core.xml`.

### 1280x1024 resolution cap

VRAM is read with `IDirectDraw7::GetAvailableVidMem` (DWORD) and treated as signed. 4 GB cards fall
into `<res mem="270" maxWidth="1280" />`. **Fix:** the value is clamped to 1024 MB.

### Display modes and the HARDWARE_SETTINGS pool

Every D3D9 display mode is a node in `hardware_settings_info.xml`, stored in the `HARDWARE_SETTINGS`
pool of `system/applicationMemoryMap.xml`, only **16 KB**. **Fix:** the mode list is filtered (as in
Dirt1_Core_Bloom_Fix) and `applicationMemoryMap.xml` is rewritten on load with a 256 KB pool.

### Fails to start: the 815 MB reservation (`-novirtual`)

At startup the "virtual memory manager" reserves one contiguous block of `(0x325 + 0xA) << 20` =
815 MB of address space (10 MB with `-novirtual`). Without the Large Address Aware flag the process
has 2 GB, and after modern drivers and overlays load such a block often doesn't exist.
**Fix:** without LAA the flag byte at `0xDAFF08` is cleared from `DllMain`, which is exactly what
`-novirtual` does. With LAA the virtual memory manager stays on.

### UTF-8 system code page

`WinMain` converts the command line with `WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, ...)`.
With ANSI code page 65001 that flag is invalid and the call fails; the CRT also relies on
`GetACP`. **Fix:** `GetACP`, `MultiByteToWideChar` and `WideCharToMultiByte` use 1252 instead of UTF-8.

### The yellow filter

Track `effects.xml` files set `tintColour` to ~`255, 220, 140` with `tintAmount` 0.15-0.5, a strong
`bloomAmount` (~1.0-1.1) and vignettes. Disabling post processing in `hardware_settings_config.xml`
also removes tone mapping, so night tracks get dark. **Fix:** only the configured `<Param>` values of
`chain="Type2"` effects in `tracks/` are replaced when the game opens a file.

Some tracks (Japan) use the engine's **binary XML** format, which is rewritten too:

```
[u32 tag = 0x72522200 | type][u32 size][data]
0x1A document { 0x17 { 0x1D string pool, 0x1E u32 string offsets }, 0x1B nodes, 0x1C attributes }
node      = u32 name, text, attrCount, firstAttr, childCount, firstChild
attribute = u32 name, value
```

Strings are shared, so a value is changed by appending a new string and pointing the `Param` node at
it; the pool stays padded to 32 bytes.

### Track surface LOD

`tracks/tracklodsettings.xml` switches the track surface to the low detail version 30 m in front of
the car (`<SURFACELOD loddistance="30" blend="10" />`). **Fix:** rewritten on load with the values
from `[Graphics]` (1000 / 100 by default, as suggested on PCGamingWiki).

## DLC and "Content pack(s) missing"

Not part of the fix, but found while testing with the 8 Ball / Prestige packs and the unofficial
V8 Supercars (Bathurst) content:

- A content pack N counts as installed **only** if `dlc_info/dlcN.xml` exists with `pack="N"`
  (bit N in the DLC manager's mask). `database/dlc-database-aN.bin`, `frontend/*_DLC_N.pssg` and
  `language/*_dlc_N.lng` don't register anything.
- A save stores the pack mask it was made with (`dlc_level`); loading compares it with the installed
  mask and shows "Content pack(s) missing" if a pack is gone.
- `dlc_info` also takes `<feature name="..."/>`: `achievement1`..`achievement10` and `V8`.
- If the V8 content (database `a3`/`p3`, `b_DLC_3`/`fe_DLC_3`, `language_*_dlc_3`) is present but
  `dlc_info/dlc3.xml` is missing, create it:

```xml
<dlc_info
pack="3" name="V8 SUPERCARS">
	<feature name="V8" />
</dlc_info>
```

### Not addressed

- Alt+Tab crashes in fullscreen (use windowed / borderless).
- Flashback length depends on the frame rate and the menu is capped at 40 FPS: limit the frame
  rate to 60 for the intended flashback time.

## Building

**Visual Studio 2022:** open `Grid_Core_Bloom_Fix.sln`, configuration `Release|x86` (static CRT).

**llvm-mingw:**

```sh
./build.sh          # i686-w64-mingw32-clang++ from PATH, output in build/
```

## Credits

- Known workarounds collected on [PCGamingWiki](https://www.pcgamingwiki.com/wiki/Race_Driver:_Grid).
- Threading/topology code shared with [Dirt1_Core_Bloom_Fix](https://github.com/ZUKMAN/Dirt1_Core_Bloom_Fix).

## License

MIT, see [LICENSE](LICENSE).
