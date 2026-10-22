# engine/core: runtime file roots.
OBJS = $(OBJS) \
	$(OBJDIR)\engine\core\paths.obj

{engine\core}.c{$(OBJDIR)\engine\core}.obj::
	@if not exist $(OBJDIR)\engine\core mkdir $(OBJDIR)\engine\core
	$(COMPILE) /Fo$(OBJDIR)\engine\core\ /sourceDependencies $(OBJDIR)\engine\core\ $<
