# common: shader program helpers, framebuffer quad, texture loading.
OBJS = $(OBJS) \
	$(OBJDIR)\common\Common.obj \
	$(OBJDIR)\common\CommonGL.obj \
	$(OBJDIR)\common\Textures.obj

{common}.c{$(OBJDIR)\common}.obj::
	@if not exist $(OBJDIR)\common mkdir $(OBJDIR)\common
	$(COMPILE) /Fo$(OBJDIR)\common\ /sourceDependencies $(OBJDIR)\common\ $<
