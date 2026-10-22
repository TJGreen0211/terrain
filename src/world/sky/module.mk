# world/sky: skybox and nebula sky (bake and runtime).
OBJS = $(OBJS) \
	$(OBJDIR)\world\sky\Skybox.obj \
	$(OBJDIR)\world\sky\Nebula.obj

{world\sky}.c{$(OBJDIR)\world\sky}.obj::
	@if not exist $(OBJDIR)\world\sky mkdir $(OBJDIR)\world\sky
	$(COMPILE) /Fo$(OBJDIR)\world\sky\ /sourceDependencies $(OBJDIR)\world\sky\ $<
