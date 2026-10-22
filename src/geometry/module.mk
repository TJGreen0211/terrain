# geometry: quad cube, quadtree LOD (moves to world/terrain).
OBJS = $(OBJS) \
	$(OBJDIR)\geometry\QuadCube.obj \
	$(OBJDIR)\geometry\QuadTree.obj

{geometry}.c{$(OBJDIR)\geometry}.obj::
	@if not exist $(OBJDIR)\geometry mkdir $(OBJDIR)\geometry
	$(COMPILE) /Fo$(OBJDIR)\geometry\ /sourceDependencies $(OBJDIR)\geometry\ $<
