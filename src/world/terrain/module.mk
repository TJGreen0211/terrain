# world/terrain: quad cube, quadtree LOD, terrain bake, surface detail, surface drawing.
OBJS = $(OBJS) \
	$(OBJDIR)\world\terrain\QuadCube.obj \
	$(OBJDIR)\world\terrain\QuadTree.obj \
	$(OBJDIR)\world\terrain\TerrainGen.obj \
	$(OBJDIR)\world\terrain\TerrainDetail.obj \
	$(OBJDIR)\world\terrain\TerrainDraw.obj

{world\terrain}.c{$(OBJDIR)\world\terrain}.obj::
	@if not exist $(OBJDIR)\world\terrain mkdir $(OBJDIR)\world\terrain
	$(COMPILE) /Fo$(OBJDIR)\world\terrain\ /sourceDependencies $(OBJDIR)\world\terrain\ $<
