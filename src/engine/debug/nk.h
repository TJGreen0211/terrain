#ifndef NK_H
#define NK_H

/* Nuklear with this project's configuration. Every file that includes
   nuklear must see the same NK_INCLUDE_* set (it changes struct layouts),
   so they all include it through here. The implementation is compiled
   once, in DebugGUI.c. */
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#include <nuklear/nuklear.h>

#endif
