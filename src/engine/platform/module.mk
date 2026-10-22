# engine/platform: window, GL context, input callbacks.
OBJS = $(OBJS) \
	$(OBJDIR)\engine\platform\window.obj \
	$(OBJDIR)\engine\platform\input.obj

{engine\platform}.c{$(OBJDIR)\engine\platform}.obj::
	@if not exist $(OBJDIR)\engine\platform mkdir $(OBJDIR)\engine\platform
	$(COMPILE) /Fo$(OBJDIR)\engine\platform\ /sourceDependencies $(OBJDIR)\engine\platform\ $<
