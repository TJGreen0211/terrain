# app: composition root (main loop, update, render pass list), bench mode, loading screen.
OBJS = $(OBJS) \
	$(OBJDIR)\app\main.obj \
	$(OBJDIR)\app\app.obj \
	$(OBJDIR)\app\render.obj \
	$(OBJDIR)\app\camera_sliders.obj \
	$(OBJDIR)\app\bench.obj \
	$(OBJDIR)\app\loading_screen.obj

{app}.c{$(OBJDIR)\app}.obj::
	@if not exist $(OBJDIR)\app mkdir $(OBJDIR)\app
	$(COMPILE) /Fo$(OBJDIR)\app\ /sourceDependencies $(OBJDIR)\app\ $<
