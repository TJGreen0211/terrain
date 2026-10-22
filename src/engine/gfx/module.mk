# engine/gfx: shader loading, textures, buffers, meshes, framebuffer quad.
OBJS = $(OBJS) \
	$(OBJDIR)\engine\gfx\shader.obj \
	$(OBJDIR)\engine\gfx\texture.obj \
	$(OBJDIR)\engine\gfx\framebuffer.obj \
	$(OBJDIR)\engine\gfx\buffer.obj \
	$(OBJDIR)\engine\gfx\mesh.obj

{engine\gfx}.c{$(OBJDIR)\engine\gfx}.obj::
	@if not exist $(OBJDIR)\engine\gfx mkdir $(OBJDIR)\engine\gfx
	$(COMPILE) /Fo$(OBJDIR)\engine\gfx\ /sourceDependencies $(OBJDIR)\engine\gfx\ $<
