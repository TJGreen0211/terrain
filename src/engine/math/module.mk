# engine/math: vectors, matrices, complex numbers (no GL).
OBJS = $(OBJS) \
	$(OBJDIR)\engine\math\linalg.obj

{engine\math}.c{$(OBJDIR)\engine\math}.obj::
	@if not exist $(OBJDIR)\engine\math mkdir $(OBJDIR)\engine\math
	$(COMPILE) /Fo$(OBJDIR)\engine\math\ /sourceDependencies $(OBJDIR)\engine\math\ $<
