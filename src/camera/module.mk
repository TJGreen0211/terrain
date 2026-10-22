# camera: arcball camera and input callbacks.
OBJS = $(OBJS) \
	$(OBJDIR)\camera\arcballCamera.obj \
	$(OBJDIR)\camera\Input.obj

{camera}.c{$(OBJDIR)\camera}.obj::
	@if not exist $(OBJDIR)\camera mkdir $(OBJDIR)\camera
	$(COMPILE) /Fo$(OBJDIR)\camera\ /sourceDependencies $(OBJDIR)\camera\ $<
