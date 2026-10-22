#ifndef PATHS_H
#define PATHS_H

#include <stddef.h>

/* Runtime file roots. main() chdir's to the exe's folder (build/), so every
   root is relative to that folder; moving the exe means editing paths.c only.

   Shaders: loaders take names relative to the shader root ("quad.vert") and
   prefix it themselves (LoadShader.c). Assets and bench files: build the path
   with Paths_Asset / Paths_Bench, which take a printf format. */
const char *Paths_ShaderRoot(void);
const char *Paths_AssetRoot(void);
const char *Paths_BenchRoot(void);

/* root + formatted rel into out (n bytes); returns out. */
char *Paths_Shader(char *out, size_t n, const char *fmt, ...);
char *Paths_Asset(char *out, size_t n, const char *fmt, ...);
char *Paths_Bench(char *out, size_t n, const char *fmt, ...);

#endif
