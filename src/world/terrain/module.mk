# world/terrain: quad cube, quadtree LOD, surface detail, surface drawing, debug panel (the bake is in gen/).
OBJS = $(OBJS) \
	$(OBJDIR)\world\terrain\quad_cube.obj \
	$(OBJDIR)\world\terrain\quad_tree.obj \
	$(OBJDIR)\world\terrain\terrain_detail.obj \
	$(OBJDIR)\world\terrain\terrain_draw.obj \
	$(OBJDIR)\world\terrain\terrain_panel.obj

{world\terrain}.c{$(OBJDIR)\world\terrain}.obj::
	@if not exist $(OBJDIR)\world\terrain mkdir $(OBJDIR)\world\terrain
	$(COMPILE) /Fo$(OBJDIR)\world\terrain\ /sourceDependencies $(OBJDIR)\world\terrain\ $<
