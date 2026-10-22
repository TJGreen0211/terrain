# world/atmosphere: atmospheric scattering.
OBJS = $(OBJS) \
	$(OBJDIR)\world\atmosphere\Atmosphere.obj

{world\atmosphere}.c{$(OBJDIR)\world\atmosphere}.obj::
	@if not exist $(OBJDIR)\world\atmosphere mkdir $(OBJDIR)\world\atmosphere
	$(COMPILE) /Fo$(OBJDIR)\world\atmosphere\ /sourceDependencies $(OBJDIR)\world\atmosphere\ $<
