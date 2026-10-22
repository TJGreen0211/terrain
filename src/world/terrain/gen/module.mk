# world/terrain/gen: terrain bake (terrain_gen.h): cube map helpers, erosion, climate, surface, upload, height bounds, bake job.
OBJS = $(OBJS) \
	$(OBJDIR)\world\terrain\gen\cubemap.obj \
	$(OBJDIR)\world\terrain\gen\erosion.obj \
	$(OBJDIR)\world\terrain\gen\climate.obj \
	$(OBJDIR)\world\terrain\gen\surface.obj \
	$(OBJDIR)\world\terrain\gen\upload.obj \
	$(OBJDIR)\world\terrain\gen\bounds.obj \
	$(OBJDIR)\world\terrain\gen\bake_job.obj

{world\terrain\gen}.c{$(OBJDIR)\world\terrain\gen}.obj::
	@if not exist $(OBJDIR)\world\terrain\gen mkdir $(OBJDIR)\world\terrain\gen
	$(COMPILE) /Fo$(OBJDIR)\world\terrain\gen\ /sourceDependencies $(OBJDIR)\world\terrain\gen\ $<
