#ifndef TERRAIN_PANEL_H
#define TERRAIN_PANEL_H

struct nk_context;

/* The Terrain tab of the debug panel (terrain_panel.c): live surface style,
   bake settings (applied by a regenerate), detail knobs and LOD stats. */
void Terrain_DebugPanel(struct nk_context *ctx);
/* First panel frame: snapshots what the reset buttons restore. */
void Terrain_DebugPanelStartup(void);
/* 1 once after "Regenerate planet" / "New seed". */
int Terrain_ConsumeRegenerate(void);

#endif
