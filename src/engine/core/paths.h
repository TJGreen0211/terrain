#ifndef PATHS_H
#define PATHS_H

#include <stddef.h>

/* Runtime file roots. main() chdir's to the exe's folder (build/), so every
   root is relative to that folder; moving the exe means editing paths.c only.

   Shaders are named "<module folder>/<file>", relative to src/: each module
   keeps its shaders in a shaders/ subfolder, which the name leaves out.
       "world/clouds/clouds.frag" -> src/world/clouds/shaders/clouds.frag
       "app/loading.vert"         -> src/app/shaders/loading.vert
   Loaders take shader names and resolve them themselves (shader.c), and
   GLSL #include uses the same names (after trying the includer's folder).
   Assets and bench files: build the path with Paths_Asset / Paths_Bench.
   All three Paths_* builders take a printf format. */
const char *Paths_ShaderRoot(void);
const char *Paths_AssetRoot(void);
const char *Paths_BenchRoot(void);

/* Shader name -> file path, as above. */
char *Paths_Shader(char *out, size_t n, const char *fmt, ...);
/* root + formatted rel into out (n bytes); returns out. */
char *Paths_Asset(char *out, size_t n, const char *fmt, ...);
char *Paths_Bench(char *out, size_t n, const char *fmt, ...);

#endif
