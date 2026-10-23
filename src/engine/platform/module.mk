# engine/platform: window, GL context, input callbacks, frame limiter.
OBJS = $(OBJS) \
	$(OBJDIR)\engine\platform\window.obj \
	$(OBJDIR)\engine\platform\input.obj \
	$(OBJDIR)\engine\platform\frame_limiter.obj

{engine\platform}.c{$(OBJDIR)\engine\platform}.obj::
	@if not exist $(OBJDIR)\engine\platform mkdir $(OBJDIR)\engine\platform
	$(COMPILE) /Fo$(OBJDIR)\engine\platform\ /sourceDependencies $(OBJDIR)\engine\platform\ $<
