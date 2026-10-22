#ifndef RENDER_H
#define RENDER_H

#include "app/App.h"

/* One frame's passes, in order (Render.c): shadow maps -> sky -> bodies ->
   terrain -> water -> clouds -> rings -> nebula -> atmosphere -> sun glow,
   into the reversed-Z scene target, then the resolve and the UI into the
   window. Builds the frame's FrameContext from app->cam. Does not swap. */
void Render_Frame(App *app);

#endif
