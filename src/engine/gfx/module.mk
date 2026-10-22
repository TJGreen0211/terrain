# engine/gfx: shader loading, textures, buffers, meshes, framebuffer quad,
# the main scene target and its tone map.
OBJS = $(OBJS) \
	$(OBJDIR)\engine\gfx\shader.obj \
	$(OBJDIR)\engine\gfx\texture.obj \
	$(OBJDIR)\engine\gfx\framebuffer.obj \
	$(OBJDIR)\engine\gfx\buffer.obj \
	$(OBJDIR)\engine\gfx\mesh.obj \
	$(OBJDIR)\engine\gfx\scene_target.obj \
	$(OBJDIR)\engine\gfx\tone_map.obj

{engine\gfx}.c{$(OBJDIR)\engine\gfx}.obj::
	@if not exist $(OBJDIR)\engine\gfx mkdir $(OBJDIR)\engine\gfx
	$(COMPILE) /Fo$(OBJDIR)\engine\gfx\ /sourceDependencies $(OBJDIR)\engine\gfx\ $<
