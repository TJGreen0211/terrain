# AGENTS.md

This file provides guidance to AI coding agents working with code in this repository.

## What this is

A C (no C++) OpenGL renderer for a procedurally generated, Earth-scale planet inside a
procedural solar system: quadtree LOD terrain on a cube sphere, FFT ocean, atmosphere,
clouds, rings, shadow cascades, nebula sky. Windows only, built with MSVC + nmake,
GLFW + glad + Nuklear (in `third_party/`).

## Build and run

- `build.bat` — finds VS 2022 via vswhere, runs `vcvars64`, then `nmake` in `src/`.
  Output is `build\cube.exe` (objects in `build\obj\<folder>\`). Extra args pass to nmake,
  e.g. `build.bat clean`.
- The build first runs `tools\check_modules.ps1 -Strict` (module-boundary lint, see below);
  a violation fails the build. Run it alone with `powershell -File tools\check_modules.ps1`.
- Header deps are tracked automatically (`/sourceDependencies` → `tools\gen_deps.ps1` →
  `build\obj\deps.mk`). Changing `CFLAGS` or include paths is **not** tracked: `build.bat clean`.
- `CFLAGS` has `/we4013`: calling an undeclared function is an error. The build has a known
  warning count (127 at the end of the refactor); don't add new warnings.
- `main()` chdirs to the exe's folder; all shader/asset/bench paths go through
  `engine/core/paths` (`Paths_Shader`, `Paths_Asset`, `Paths_Bench`). Never hard-code
  `"../assets/..."`.
- Runtime: F1 toggles the debug panel (per-pass CPU/GPU ms in Performance; every module's
  tuning is a tab). Command-line flags are parsed in `app/bench.c` (`--bench`, `--frames`,
  `--shots`, `--width/--height`, ...) and `world/sky/nebula.c` (`--bake-sky`, `--nebula`, ...).

## Verification (there are no unit tests)

A change is "green" when all of these pass:

1. `build.bat` builds clean, no new warnings.
2. `powershell -File bench\bench.ps1` — runs each `bench\scenes\*.bench` scripted camera path
   and compares frame times to `bench\baseline.json` (default ±2%). Options: `-Scenes <name>`,
   `-Runs 1`, `-SkipBuild`.
3. `powershell -File bench\visual_check.ps1` — renders shots at fixed frames and compares to
   `bench\reference\*.png` (SSIM/PSNR). `-Scenes grazing-sea` for one scene.
4. `tools\check_modules.ps1` passes.

**Never edit the protected harness files** (`bench\protected.txt` lists them: the scripts,
scenes, baseline and reference images) to make a run pass; `bench.ps1` checks them against a
SHA-256 manifest. Re-baselining (`-UpdateBaseline`, `-UpdateRefs`, `-SealManifest`) is only for
intentional, human-reviewed changes — ask first.

Perf numbers are noisy on the dev laptop (Iris Xe, thermal throttling); compare A/B by
alternating runs, not by a single run.

## Architecture

Three tiers under `src/`, enforced by `tools/check_modules.ps1`:

- **`engine/`** (tier 0, knows nothing about planets): `core/gl.h` is the *only* file that
  includes `windows.h`/glad/GLFW; `paths`; `math/linalg` (GLSL-style names like `addvec3`,
  unprefixed by design); `platform/` window + input; `gfx/` shader loader, textures, meshes,
  framebuffers, `frame.h` (`FrameContext`); `camera/`; `debug/` (Nuklear host, profiler).
- **`world/`** (tier 1, feature modules): `bodies` (Kepler orbits, solar system, impostors,
  sun), `terrain` (quad cube, quadtree LOD, detail, draw; `gen/` is the async CPU bake of
  height/climate/ocean/surface cube maps), `ocean`, `atmosphere`, `clouds`, `sky` (skybox,
  nebula), `rings`, `shadows`. `world/WorldConfig.h` holds every world-scale constant.
- **`app/`** (tier 2, the only code that knows every module): `main.c` (~40 lines) →
  `App_Create` / `App_Update` / `Render_Frame` / `App_Destroy`. `render.c` is the explicit
  ordered pass list (shadow maps → skybox → sun/planets → terrain → water → clouds → rings →
  nebula → atmosphere dome → sun glow → UI) and builds the per-frame `FrameContext`.

Module rules (full text in `docs/module-refactor-plan.md` §3):

- Includes go down a tier or across into another module's public header, never up.
  `*_internal.h` (e.g. `terrain/gen/gen_internal.h`) is private to its folder.
  Engine code never includes `WorldConfig.h`; it gets values via config structs at `_Init`.
- Every public symbol has a `Module_` prefix; file names are the prefix in lower snake_case
  (`QuadTree_` → `quad_tree.c`). Exception: `WorldConfig.h` keeps its name.
- No `extern` globals between modules. Tuning goes through `X_Params()`; each module draws its
  own debug tab via `X_DebugPanel`, registered in `app.c` with `DebugGUI_AddPanel`. Per-frame
  shared data goes in `FrameContext` (keep it per-frame only).
- **Keep all scale constants in `WorldConfig.h`** (one file, standing preference). Units:
  1 world unit = 1 m. Positions/orbits/time are `double` on the CPU; the GPU only sees
  camera-relative or body-local floats.
- Each module owns and loads its own shaders from `<module>/shaders/`. Shader names are
  `<module folder>/<file>` relative to `src/` with `shaders/` implied:
  `"world/clouds/cloudlayer.glsl"` → `src/world/clouds/shaders/cloudlayer.glsl`. GLSL
  `#include` uses the same names, trying the includer's folder first. A bad include prints
  "Shader include not found" on stderr.
- Adding a source file: list its `.obj` in that folder's `module.mk`. Adding a folder: a new
  `module.mk` plus one `!include` line in `src/Makefile`.

## Docs

`docs/` holds design notes and status for in-progress features (cube maps, ocean/terrain
look, volumetric clouds, render scale, solar system, near-ground perf). Read the relevant one
before working on a feature; several features (clouds, nebula) ship disabled by default and
note where to resume.
