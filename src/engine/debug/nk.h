#ifndef NK_H
#define NK_H

/* Nuklear with this project's configuration. Every file that includes
   nuklear must see the same NK_INCLUDE_* set (it changes struct layouts),
   so they all include it through here. The implementation is compiled
   once, in debug_gui.c. */
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
/* C5287 (mixing enum types in |, ==): nuklear does this on purpose. */
#pragma warning(push)
#pragma warning(disable: 5287)
#include <nuklear/nuklear.h>
#pragma warning(pop)

#endif
