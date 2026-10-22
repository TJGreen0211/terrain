# world/bodies: bodies table, procedural solar system, planet impostors, sun, lit bodies.
OBJS = $(OBJS) \
	$(OBJDIR)\world\bodies\bodies.obj \
	$(OBJDIR)\world\bodies\solar_system.obj \
	$(OBJDIR)\world\bodies\planets.obj \
	$(OBJDIR)\world\bodies\sun.obj \
	$(OBJDIR)\world\bodies\body_draw.obj

{world\bodies}.c{$(OBJDIR)\world\bodies}.obj::
	@if not exist $(OBJDIR)\world\bodies mkdir $(OBJDIR)\world\bodies
	$(COMPILE) /Fo$(OBJDIR)\world\bodies\ /sourceDependencies $(OBJDIR)\world\bodies\ $<
