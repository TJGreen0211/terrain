# geometry: meshes, quad cube, quadtree LOD.
OBJS = $(OBJS) \
	$(OBJDIR)\geometry\Sphere.obj \
	$(OBJDIR)\geometry\Geometry.obj \
	$(OBJDIR)\geometry\QuadCube.obj \
	$(OBJDIR)\geometry\QuadTree.obj

{geometry}.c{$(OBJDIR)\geometry}.obj::
	@if not exist $(OBJDIR)\geometry mkdir $(OBJDIR)\geometry
	$(COMPILE) /Fo$(OBJDIR)\geometry\ /sourceDependencies $(OBJDIR)\geometry\ $<
