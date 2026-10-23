# world/clouds: cloud layers (the 2D slab) and the shared cloud volume.
OBJS = $(OBJS) \
	$(OBJDIR)\world\clouds\clouds.obj \
	$(OBJDIR)\world\clouds\cloud_volume.obj

{world\clouds}.c{$(OBJDIR)\world\clouds}.obj::
	@if not exist $(OBJDIR)\world\clouds mkdir $(OBJDIR)\world\clouds
	$(COMPILE) /Fo$(OBJDIR)\world\clouds\ /sourceDependencies $(OBJDIR)\world\clouds\ $<
