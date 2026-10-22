#include "paths.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Relative to build/, the exe's folder (see paths.h). */
#define SHADER_ROOT "../src/"
#define ASSET_ROOT  "../assets/"
#define BENCH_ROOT  "../bench/"

const char *Paths_ShaderRoot(void) { return SHADER_ROOT; }
const char *Paths_AssetRoot(void)  { return ASSET_ROOT; }
const char *Paths_BenchRoot(void)  { return BENCH_ROOT; }

static char *joinv(char *out, size_t n, const char *root, const char *fmt, va_list ap)
{
	int head = snprintf(out, n, "%s", root);
	if (head >= 0 && (size_t)head < n)
		vsnprintf(out + head, n - head, fmt, ap);
	return out;
}

/* "<module folder>/<file>" -> SHADER_ROOT "<module folder>/shaders/<file>".
   A bare "<file>" would be src/shaders/<file>; every shader now lives in a
   module, so a bare name only shows up in the include-not-found message. */
char *Paths_Shader(char *out, size_t n, const char *fmt, ...)
{
	char name[260];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(name, sizeof name, fmt, ap);
	va_end(ap);
	const char *slash = strrchr(name, '/');
	int folder = slash ? (int)(slash - name) + 1 : 0;
	snprintf(out, n, "%s%.*sshaders/%s", SHADER_ROOT, folder, name, name + folder);
	return out;
}

char *Paths_Asset(char *out, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	joinv(out, n, ASSET_ROOT, fmt, ap);
	va_end(ap);
	return out;
}

char *Paths_Bench(char *out, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	joinv(out, n, BENCH_ROOT, fmt, ap);
	va_end(ap);
	return out;
}
