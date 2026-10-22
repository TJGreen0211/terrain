# world/bodies: bodies table, procedural solar system, planet impostors, sun, lit bodies.
OBJS = $(OBJS) \
	$(OBJDIR)\world\bodies\Bodies.obj \
	$(OBJDIR)\world\bodies\SolarSystem.obj \
	$(OBJDIR)\world\bodies\Planets.obj \
	$(OBJDIR)\world\bodies\Sun.obj \
	$(OBJDIR)\world\bodies\BodyDraw.obj

{world\bodies}.c{$(OBJDIR)\world\bodies}.obj::
	@if not exist $(OBJDIR)\world\bodies mkdir $(OBJDIR)\world\bodies
	$(COMPILE) /Fo$(OBJDIR)\world\bodies\ /sourceDependencies $(OBJDIR)\world\bodies\ $<
