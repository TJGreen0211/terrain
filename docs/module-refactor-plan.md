# Module Refactor Plan — Self-Contained Modules

Date: 2026-10-22 (replaces `.kilo/plans/2026-10-21-module-refactor-plan.md`, whose
coupling analysis was stale)

Goal: restructure `src/` into self-contained modules, in preparation for this app
becoming one part of a larger game. No rendering or behaviour changes: every phase
must stay pixel-identical and keep the same frame times.

## 1. Current state (reviewed 2026-10-22)

### Build and layout

- `src/Makefile` compiles all 33 sources in a single `cl.exe` call, so every build
  recompiles everything.
- The `.obj` files, `cube.exe`, `cube_dev.exe` and `glfw3.dll` are all written into `src/`.
- About 24 `.c/.h` pairs sit flat in `src/`. Only `math/`, `camera/`, `common/` and
  `geometry/` are folders.
- All 79 shaders are flat in `src/shaders/`.
- At runtime, `main()` calls `chdir` to the exe's folder. Shaders are then loaded as
  `"shaders/..."` and assets as `"../assets/..."`, both relative to that folder.

### Coupling

| Problem | Evidence |
|---|---|
| `Main.c` (1,642 lines) is the renderer for other modules | It holds terrain LOD (`terrainReliefBounds`, `lodCellKFor`, `terrainViewRange`), `setBodyUniforms`, the shadow depth pass (`drawPlanetDepth`, `getLightViewMatrix`), `drawWater`, `Earth_GroundRadius`, and 12 `init*Shader()` functions for atmosphere, water, quad/terrain, ring, depth, instance and tess shaders. |
| `main()` is 609 lines | There are only about 11 file-scope globals. The real problem is one huge function full of locals. |
| About 30 `extern` globals act as the interface between modules | `g_clouds`, `g_cloudsEnabled`, `g_nebula`, `g_planetGen`, `g_planetStyle`, `g_terrainBake`, `g_cascades`, `g_waveParams`, `g_bodies`, `g_atmosphere`, `planetHeightTex`, `g_qtStats`, the camera speed globals, and others. They exist mostly so that `DebugGUI.c`, which includes 11 module headers, can change tuning values. |
| `Main.h` is a god header that includes 15 headers | Lower modules include upward: `Atmosphere.c/h`, `Rings.c/h` and `TerrainGen.c` include `Main.h`. `camera/Input.c` includes `GUI.h` and `DebugGUI.h`, so input depends on the UI. `geometry/QuadTree.c` includes `Window.h`. |
| `Window.h` is used as the "I need GL types" header | It pulls `windows.h`, glad and GLFW into nearly everything. |
| Engine-level code reads `WorldConfig.h` | `camera/arcballCamera.c` and `geometry/QuadTree.c` include it. |
| Bench hooks are scattered | `Window.c` asks `Bench_FixedWindowSize`. `Water.c` includes `Bench.h` but uses nothing from it. |
| `TerrainGen.c` (2,250 lines) covers about six jobs | See the table below. |
| Naming is inconsistent | Some functions use a module prefix (`TerrainGen_BakeStart`, `Nebula_RunBake`); others don't (`initializeWaves`, `setupGLFW`, `drawWater`, `rotateLight`). |

The six jobs inside `TerrainGen.c`:

| Job | Functions |
|---|---|
| Cube-map math | `faceDir`, `dirFace`, `cubeToEquirect`, ... |
| Erosion | `thermalErode`, `hydraulicErode` |
| Climate | `computeMoisture`, `computeDrainage`, `prevailingWind` |
| Async bake | worker thread and readback |
| Texture upload | |
| Surface and ocean uniforms | |

### Shader include graph (verified by grepping `#include` in `src/shaders`)

`LoadShader.c` resolves `#include "x"` relative to the including file's folder.
Cross-module chunks, grouped by owner:

| Owner | Chunk | Included by |
|---|---|---|
| engine | `tonemap.glsl` | quad, water, ring, rock, clouds, nebula_post/sky/vol |
| atmosphere | `atmosurface.glsl` (which includes `atmodensity.glsl`) | quad.frag, water.frag, clouds.frag |
| atmosphere | `atmoscatter.glsl` | atmosphere.frag, clouds.frag |
| clouds | `cloudlayer.glsl` | quad.frag, water.frag, clouds.frag |
| clouds | `cloudnoise.glsl` | cloudmap.frag, clouds.frag, nebula.glsl |
| rings | `ringshadow.glsl` | quad.frag, water.frag, clouds.frag |
| shadows | `shadow.glsl` | quad.frag, water.frag |
| terrain | `planetmap.glsl` | quad.*, water.*, depth.vert |
| terrain | `patch.glsl` | quad.vert, water.vert, light.vert, depth.vert |
| terrain | `detail.glsl` | quad.*, depth.vert |
| ocean | `wavecube.glsl`, `seastate.glsl` | water.* |
| ocean | `watercolumn.glsl` | quad.frag, water.frag |
| sky | `nebula.glsl` | nebula_grid/sky/vol |

### Dead weight

Re-check each item with grep before deleting it.

- **Empty or unused source:** `src/Physics.c/.h` (0 lines) and
  `src/camera/Camera.c/.h` (not in the Makefile).
- **Old copies and leftovers:** `src/backups/`, `src/shaders/backups/`, and
  `src/shaders/MatDefs/` (JMonkeyEngine leftovers, including an `.svn/` folder).
- **Unreferenced shaders:** `depth.geom`, `terrain.glsl`, `waveinit.frag`.
- **`lib/`:** FreeType 2.4.0 sources, SOIL, macOS dylibs, and duplicate GLFW headers.
  Nothing includes or links any of it.
- **Stray files:**
  - `src/geometry/geometry.mk` (a stale macOS make fragment).
  - `.DS_Store` files.
  - Root `temp.glsl`.
  - The `Main.exe` reference in `clean`.
- **Files to move to `docs/`:** root `quadtree.txt`, `personal_notes.txt` and `notes/`.

## 2. Target layout

There are three tiers. **engine** is reusable and knows nothing about planets.
**world** holds the feature modules. **app** is a thin layer that composes them. Each
feature folder owns its C code, its shaders and its debug panel, so it can be
understood, removed or reused as one unit.

```
terrain/
  src/
    engine/                     tier 0
      core/      gl.h (the ONLY place glad/GLFW/windows.h are included),
                 paths (shader/asset roots)
      math/      linalg.c/h, complex.h
      platform/  window.c/h, input.c/h   (input sends events; it never calls the GUI)
      gfx/       shader.c/h (LoadShader + common/Common's createShader helpers),
                 texture.c/h, buffer.c/h (+BUFFER_OFFSET),
                 mesh.c/h (Geometry, Sphere, Obj), framebuffer.c/h (CommonGL),
                 frame.h (FrameContext)
                 shaders/ framebuffer.*, tonemap.glsl
      camera/    arcball.c/h
      debug/     profiler.c/h, gui.c/h (Nuklear host + panel registry)
                 shaders/ UI.*

   Phase 4 moves files under their CURRENT names (engine/gfx/LoadShader.c);
   the lowercase renames and merges above (shader, mesh, ...) are code
   changes and happen with the phase 8 renames.
    world/                      tier 1
      WorldConfig.h             stays a single file (see rule 7)
      bodies/      Bodies, SolarSystem, Planets (impostors), Sun     + shaders/
      terrain/     quadcube, quadtree, terrain_draw (from Main.c), detail,
                   gen/ {cubemap, erosion, climate, bake_job, upload, surface}
                                                                     + shaders/
      ocean/       Water (FFT waves, draw)                           + shaders/
      atmosphere/  Atmosphere                                        + shaders/
      clouds/      Clouds                                            + shaders/
      sky/         Skybox, Nebula (+ --bake-sky entry point)         + shaders/
      rings/       Rings                                             + shaders/
      shadows/     Shadows, cascades, depth pass (from Main.c)       + shaders/
    app/                        tier 2: the only code that knows every module
      main.c       args, chdir, create, loop, destroy (~50 lines)
      app.c/h      App struct: owns module instances and per-frame update
      render.c     ordered pass list: shadow -> sky -> bodies -> terrain
                   -> water -> clouds -> UI
      loading.c    LoadingScreen
      bench.c/h    Bench (app-level; passes window size down)
  third_party/  glad, nuklear, stb, glfw headers + lib (from dependencies/;
                .gitignore already references third_party/)
  assets/  bench/  docs/  tools/check_modules.ps1  build/ (all output)
```

When the larger game arrives, `engine/` becomes its shared library, and `world/` is
one feature set among siblings such as `world/physics/` or `game/`. A future game
state can own a `World` instead of `main()` owning everything directly.

Tess shaders (`tess.*`) are currently disabled. They go under `world/terrain/` as-is;
rewriting them is out of scope.

## 3. Rules that keep modules self-contained

1. **Includes point down a tier, or across into another module's public header.**
   Never up. `Main.h` is deleted. `tools/check_modules.ps1` enforces this and runs
   as a build prerequisite.
2. **Each module has a public header and private internals.**
   - `world/clouds/clouds.h` is the module's API.
   - Helpers are `static`, or live in a `clouds_internal.h` that only that folder
     includes.
   - Each module has one lifecycle: `Clouds_Init(const CloudsConfig*)`,
     `Clouds_Update(dt)`, `Clouds_Draw(const FrameContext*)`, `Clouds_Shutdown()`.
   - Every public symbol uses the `Module_` prefix.
3. **Each module owns its shaders.** It loads them in its own `_Init`. `app/` never
   names another module's shader file.
4. **`FrameContext` replaces cross-module globals for per-frame data.**
   - It carries view/projection, the camera-relative origin (today's `g_cam`), sun
     direction, sim time, shadow cascades and the reversed-Z flag.
   - `render.c` builds it once per frame.
   - Keep it to per-frame values only, so it doesn't become a new god-struct.
     Long-lived state stays in its owning module.
5. **Each module owns its tuning.**
   - Each module exposes `X_Params()`, which returns a pointer to its params struct,
     plus `X_DebugPanel(struct nk_context*)`. The panel is registered with
     `engine/debug/gui`.
   - `DebugGUI.c` becomes a host and stops including module headers.
   - The `extern g_*` set drops to roughly zero.
6. **Shared GLSL chunks stay with the module that owns their data, and includes name
   that module.**
   - Shader names are `<module folder>/<file>`, relative to `src/`. The module's
     `shaders/` subfolder is implied: `world/clouds/cloudlayer.glsl` is
     `src/world/clouds/shaders/cloudlayer.glsl`. The tier is part of the name, so
     there is no search and no ambiguity (implemented in phase 3, `Paths_Shader`).
   - Loaders and `#include` both use these names. Example: `quad.frag` writes
     `#include "world/clouds/cloudlayer.glsl"`, and `Sun.c` loads
     `"world/bodies/sun.vert"`.
   - An include tries the including file's folder first, so chunks inside one
     module include each other by bare file name.
   - This keeps ownership clear and makes real coupling visible: terrain shaders do
     depend on clouds, atmosphere and rings.
7. **`WorldConfig.h` stays one file.** This is a standing preference: keep
   scale constants together. Engine code never includes it. The camera and quadtree
   get their limits through the config structs passed to their `_Init`.

## 4. Phases

Each phase is one or a few commits. Never mix file moves with logic edits.

A phase is green only when all four checks pass:

1. `build.bat` builds clean, with no new warnings.
2. `bench/bench.ps1` frame times are within noise of `bench/baseline.json` for every
   scene.
3. `bench/visual_check.ps1` matches `bench/reference/` pixel for pixel.
4. `tools/check_modules.ps1` passes (from phase 5 onward).

**Phase 0 — checkpoint.**
- Commit the current WIP first: 24 modified files, the new shaders (`groundtex.*`,
  `planetmap.glsl`, `seastate.glsl`, `watercolumn.glsl`, `wavecube.glsl`), `bench/`
  (scripts, scenes, baseline, reference) and `notes/`. A refactor on top of
  uncommitted feature work can't be bisected.
- Re-record `baseline.json` and the reference shots from that commit.
- Add `tools/check_modules.ps1`, reporting only at this stage.

**Phase 1 — hygiene.**
- Delete the dead weight listed in §1, then move the docs into `docs/`.
- First add `engine/core/paths` (`Paths_ShaderRoot()`, `Paths_AssetRoot()`) and route
  every `"shaders/..."` and `"../assets/..."` string through it.
- Then point build output at `build/`, copying `glfw3.dll` there and removing it from
  `src/`.

**Phase 2 — build system.**
- Compile each file to `build/obj/<module>/*.obj` with nmake inference rules, and
  link once.
- Add one `module.mk` per folder that lists its sources. Adding a module is then one
  file plus one `!include` line.
- Move `dependencies/` to `third_party/`.
- Do this before the big moves, so every later phase builds incrementally.

**Phase 3 — shader loader include root.**
- Teach `LoadShader.c` to resolve an include as a shader name (rule 6) after the
  includer's folder.
- Commit it on its own and verify it with `visual_check` before moving any shader.
- Done 2026-10-22. It was also checked with a throwaway move: `tonemap.glsl` went
  to `engine/gfx/shaders/` and the sun shaders to `world/bodies/shaders/`, and the
  result was pixel-identical. A missing include prints the includer and both
  paths tried.

**Phase 4 — directory moves.**
- `git mv` everything into the §2 layout, including the shaders into each module's
  `shaders/`. Then update includes, `module.mk` files and shader path strings.
- No logic edits. Make one commit per tier: engine, then world, then app.
- Done 2026-10-22. Notes:
  - Files kept their names. `src/shaders/` and the old `math/`, `camera/`,
    `common/` and `geometry/` folders are gone.
  - The only `-I` paths left are `src/` and `third_party/include`; every
    include is a path from `src/`.
  - Dead files found during the move, kept for a later deletion:
    `app/shaders/instance.*` (`drawInstanced` is never called) and
    `world/terrain/shaders/tess.geom` (used only in commented-out code).
  - `check_modules.ps1` lost its legacy map, resolves includes exactly, and
    now checks shader includes too (tier rule, plus a "missing" rule).
- Watch stderr for "Shader include not found".

**Phase 5 — kill `Main.h` and the upward includes.**
- Add `engine/core/gl.h`. Each `.c` includes only what it uses.
- `Atmosphere`, `Rings` and `TerrainGen` stop including app headers.
- `Input` gets an event callback, and `app` connects it to the GUI.
- `Window_Create` takes its size as a parameter, so Bench passes it in. Drop the
  unused `Bench.h` include in `Water.c`.
- Switch `check_modules.ps1` to failing the build.
- Done 2026-10-22:
  - `Water.c` did use `Bench.h`: its `App_Time()` was the wave clock. The
    time is now a parameter of `initializeWaves` and `generateWaves`, and
    `Main` passes `App_Time()` in.
  - The camera takes its scales through `CameraConfig` / `Camera_Init`, filled
    from `WorldConfig.h` by `Main`.
  - `Input` forwards to the GUI through `InputUiHooks` / `Input_SetUiHooks`.
  - `setupGLFW(title, fixedW, fixedH)` replaces the Bench call.
  - The build runs `check_modules.ps1 -Strict` first (Makefile `check`), and
    `CFLAGS` has `/we4013`: an undeclared function is now an error. Removing
    `Main.h` exposed six implicit declarations, including `float`-returning
    window getters that would have read as `int`.
  - `gl.h` includes `windows.h` first, which removed the two `APIENTRY`
    redefinition warnings. The `__APPLE__` guards were dropped.

**Phase 6 — move rendering out of `Main.c` into its owners.**
- `init*Shader` functions go to their modules.
- `setBodyUniforms`, `rotateLight` and `drawOrbit` go to `bodies/`.
- Terrain LOD bounds, `Earth_GroundRadius` and the quad draw go to `terrain/`.
- `drawWater` and the wave test grid go to `ocean/`.
- The depth pass and light view matrix go to `shadows/`.
- Introduce `FrameContext`.
- Done 2026-10-22, pixel-identical at every step. `Main.c` went from 1,680
  to 746 lines. Where things went:
  - **Dead code deleted first:** the tess path, the tess planet shader, the
    instancing path, the framebuffer shader, the `wtest` FFT test grid, the
    ring mesh, `draw()`, the `drawWater` stub and the hot-reload helpers,
    with their shaders.
  - **`engine/gfx/Frame.h`:** `FrameContext` (cam, proj, view, viewProj,
    near plane, viewport height, light, time), built once per frame.
  - **`shadows/`:** owns the global map, its light view (`Shadows_FitGlobal`),
    receiver binding (`Shadows_BindMaps`), the depth program and mesh casters.
  - **`terrain/TerrainDraw.c`:** surface programs, relief/LOD helpers,
    `TerrainDraw_SetBodyUniforms` (the recipe shared with the water),
    planet casters, planet draw, `planetHeightTex`, `Earth_GroundRadius`.
  - **`ocean/WaterDraw.c`:** water program, `Water_GeomScale` /
    `Water_Excursion`, the water relief bounds and draw. `g_waveParams` is now
    defined in `Water.c`.
  - **`bodies/BodyDraw.c`:** the lit program, the sun marker and the Moon.
  - **`atmosphere/`:** owns its dome program (`Atmosphere_Program`).
  - Left in `Main.c` for phase 7: the scene target,
    reversed-Z switching, the projection, the mesh helpers (`initSphere`,
    `initQuadCube`, `initObjectBuffer`), the camera sliders, and the frame
    loop's order and per-frame decisions (shadow box size, which bodies get
    full detail).
  - Found along the way: the sun marker draws into the window framebuffer
    before the scene target is bound, and the final blit overwrites it, so
    it never shows. It still sets uniforms on the shared lit program and
    was kept as-is; it's a candidate for removal later.

**Phase 7 — break up `main()`.**
- Split it into `App_Create`, `App_Update`, `Render_Frame` and `App_Destroy`, with
  the explicit pass list in `render.c`.

**Phase 8 — debug panels and tuning globals.**
- Move each `DebugGUI.c` section into its module as `X_DebugPanel`.
- Replace the `extern g_*` globals with `X_Params()` getters.
- Rename the remaining unprefixed public functions to the `Module_` style.

**Phase 9 — split `TerrainGen.c`.**
- Move its code, without changes, into `world/terrain/gen/{cubemap, erosion, climate,
  bake_job, upload, surface}.c`, with a private `gen_internal.h`.
- This is last because it is the largest move.

## 5. Non-goals

- No shader, algorithm or look changes. `visual_check` proves this.
- No C++.
- No CMake unless the larger game needs IDE integration.
- No per-module static libraries until a second executable exists. Folder and
  `module.mk` boundaries are enough until then.

## 6. Risks

- **Runtime paths:** shader and asset paths are relative to the exe's folder, so they
  break when the exe moves to `build/`. Phase 1 handles this by centralising them in
  `engine/core/paths` first.
- **GLSL include resolution:** the chunk graph crosses seven modules. Phase 3 lands
  the loader change, and verifies it, before phase 4 moves any shader.
- **Bench harness:** it launches the exe from a fixed location. Update `bench.ps1`
  and `visual_check.ps1` in phase 1, at the same time as the exe moves.
- **Load-bearing globals:** if a global turns out to be read by three or more modules
  in phase 8, decide explicitly. Use `FrameContext` if it changes every frame, or the
  owner's `X_Params()` if it is tuning. Don't add a new global.
