# math: vectors, matrices, complex numbers (no GL).
OBJS = $(OBJS) \
	$(OBJDIR)\math\LinearAlg.obj

{math}.c{$(OBJDIR)\math}.obj::
	@if not exist $(OBJDIR)\math mkdir $(OBJDIR)\math
	$(COMPILE) /Fo$(OBJDIR)\math\ /sourceDependencies $(OBJDIR)\math\ $<
