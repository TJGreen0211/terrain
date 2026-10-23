#ifndef FRAME_LIMITER_H
#define FRAME_LIMITER_H

/* Caps the frame rate by waiting out the rest of each frame's period before
   the swap. Vsync is off (FrameLimiter_Init), so this is the only pacing:
   a coarse high-resolution timer sleep, then a short spin to the deadline.
   The deadline advances by whole periods, so an occasional late frame does
   not shift every later one. */

struct nk_context;

/* The cap choices the debug tab offers; 0 = uncapped. */
#define FRAME_LIMITER_DEFAULT_FPS 120

typedef struct FrameLimiterParams {
	int fps;   /* 30, 60, 90, 120, or 0 (uncapped) */
} FrameLimiterParams;

FrameLimiterParams *FrameLimiter_Params(void);

void FrameLimiter_Init(void);      /* after the GL context is current */
void FrameLimiter_Wait(void);      /* once per frame, just before the swap */
void FrameLimiter_Shutdown(void);

void FrameLimiter_DebugPanel(struct nk_context *ctx);

#endif
