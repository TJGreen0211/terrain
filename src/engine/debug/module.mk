# engine/debug: profiler, slider UI, Nuklear debug panel host.
OBJS = $(OBJS) \
	$(OBJDIR)\engine\debug\profiler.obj \
	$(OBJDIR)\engine\debug\ui.obj \
	$(OBJDIR)\engine\debug\debug_gui.obj

{engine\debug}.c{$(OBJDIR)\engine\debug}.obj::
	@if not exist $(OBJDIR)\engine\debug mkdir $(OBJDIR)\engine\debug
	$(COMPILE) /Fo$(OBJDIR)\engine\debug\ /sourceDependencies $(OBJDIR)\engine\debug\ $<
