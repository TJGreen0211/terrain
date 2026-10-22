# engine/debug: profiler, Nuklear GUI host.
OBJS = $(OBJS) \
	$(OBJDIR)\engine\debug\Profiler.obj \
	$(OBJDIR)\engine\debug\GUI.obj

{engine\debug}.c{$(OBJDIR)\engine\debug}.obj::
	@if not exist $(OBJDIR)\engine\debug mkdir $(OBJDIR)\engine\debug
	$(COMPILE) /Fo$(OBJDIR)\engine\debug\ /sourceDependencies $(OBJDIR)\engine\debug\ $<
