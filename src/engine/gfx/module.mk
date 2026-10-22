# engine/gfx: shader loading, textures, buffers, meshes, framebuffer quad.
OBJS = $(OBJS) \
	$(OBJDIR)\engine\gfx\LoadShader.obj \
	$(OBJDIR)\engine\gfx\Common.obj \
	$(OBJDIR)\engine\gfx\Textures.obj \
	$(OBJDIR)\engine\gfx\CommonGL.obj \
	$(OBJDIR)\engine\gfx\Buffers.obj \
	$(OBJDIR)\engine\gfx\Geometry.obj \
	$(OBJDIR)\engine\gfx\Sphere.obj \
	$(OBJDIR)\engine\gfx\Obj.obj

{engine\gfx}.c{$(OBJDIR)\engine\gfx}.obj::
	@if not exist $(OBJDIR)\engine\gfx mkdir $(OBJDIR)\engine\gfx
	$(COMPILE) /Fo$(OBJDIR)\engine\gfx\ /sourceDependencies $(OBJDIR)\engine\gfx\ $<
