# world/clouds: cloud layers.
OBJS = $(OBJS) \
	$(OBJDIR)\world\clouds\Clouds.obj

{world\clouds}.c{$(OBJDIR)\world\clouds}.obj::
	@if not exist $(OBJDIR)\world\clouds mkdir $(OBJDIR)\world\clouds
	$(COMPILE) /Fo$(OBJDIR)\world\clouds\ /sourceDependencies $(OBJDIR)\world\clouds\ $<
