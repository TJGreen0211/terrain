# engine/camera: arcball camera.
OBJS = $(OBJS) \
	$(OBJDIR)\engine\camera\arcballCamera.obj

{engine\camera}.c{$(OBJDIR)\engine\camera}.obj::
	@if not exist $(OBJDIR)\engine\camera mkdir $(OBJDIR)\engine\camera
	$(COMPILE) /Fo$(OBJDIR)\engine\camera\ /sourceDependencies $(OBJDIR)\engine\camera\ $<
