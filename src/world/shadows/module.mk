# world/shadows: cascaded sun shadows.
OBJS = $(OBJS) \
	$(OBJDIR)\world\shadows\Shadows.obj

{world\shadows}.c{$(OBJDIR)\world\shadows}.obj::
	@if not exist $(OBJDIR)\world\shadows mkdir $(OBJDIR)\world\shadows
	$(COMPILE) /Fo$(OBJDIR)\world\shadows\ /sourceDependencies $(OBJDIR)\world\shadows\ $<
