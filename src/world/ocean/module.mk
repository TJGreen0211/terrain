# world/ocean: FFT waves and the water surface.
OBJS = $(OBJS) \
	$(OBJDIR)\world\ocean\water.obj \
	$(OBJDIR)\world\ocean\water_draw.obj

{world\ocean}.c{$(OBJDIR)\world\ocean}.obj::
	@if not exist $(OBJDIR)\world\ocean mkdir $(OBJDIR)\world\ocean
	$(COMPILE) /Fo$(OBJDIR)\world\ocean\ /sourceDependencies $(OBJDIR)\world\ocean\ $<
