# src\ (flat): sources not yet moved into a module folder. Phase 4 of
# docs\module-refactor-plan.md empties this list.
OBJS = $(OBJS) \
	$(OBJDIR)\Main.obj \
	$(OBJDIR)\Sun.obj \
	$(OBJDIR)\Skybox.obj \
	$(OBJDIR)\Atmosphere.obj \
	$(OBJDIR)\Clouds.obj \
	$(OBJDIR)\Nebula.obj \
	$(OBJDIR)\Rings.obj \
	$(OBJDIR)\Bodies.obj \
	$(OBJDIR)\Profiler.obj \
	$(OBJDIR)\Bench.obj \
	$(OBJDIR)\LoadingScreen.obj \
	$(OBJDIR)\GUI.obj \
	$(OBJDIR)\DebugGUI.obj \
	$(OBJDIR)\TerrainGen.obj \
	$(OBJDIR)\TerrainDetail.obj \
	$(OBJDIR)\Shadows.obj \
	$(OBJDIR)\Planets.obj \
	$(OBJDIR)\SolarSystem.obj \
	$(OBJDIR)\LoadShader.obj \
	$(OBJDIR)\Window.obj \
	$(OBJDIR)\Obj.obj \
	$(OBJDIR)\Water.obj \
	$(OBJDIR)\Buffers.obj

{.}.c{$(OBJDIR)}.obj::
	@if not exist $(OBJDIR) mkdir $(OBJDIR)
	$(COMPILE) /Fo$(OBJDIR)\ /sourceDependencies $(OBJDIR)\ $<
