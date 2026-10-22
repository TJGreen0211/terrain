# app: composition root (main loop), bench mode, loading screen, debug panels.
OBJS = $(OBJS) \
	$(OBJDIR)\app\Main.obj \
	$(OBJDIR)\app\App.obj \
	$(OBJDIR)\app\Render.obj \
	$(OBJDIR)\app\CameraSliders.obj \
	$(OBJDIR)\app\Bench.obj \
	$(OBJDIR)\app\LoadingScreen.obj \
	$(OBJDIR)\app\DebugGUI.obj

{app}.c{$(OBJDIR)\app}.obj::
	@if not exist $(OBJDIR)\app mkdir $(OBJDIR)\app
	$(COMPILE) /Fo$(OBJDIR)\app\ /sourceDependencies $(OBJDIR)\app\ $<
