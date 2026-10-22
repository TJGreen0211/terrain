#include "paths.h"
#include <stdarg.h>
#include <stdio.h>

/* Relative to build/, the exe's folder (see paths.h). */
#define SHADER_ROOT "../src/shaders/"
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

char *Paths_Shader(char *out, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	joinv(out, n, SHADER_ROOT, fmt, ap);
	va_end(ap);
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
