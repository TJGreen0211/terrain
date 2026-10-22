# src\ (flat): sources not yet moved into a module folder. Phase 4 of
# docs\module-refactor-plan.md empties this list.
OBJS = $(OBJS) \
	$(OBJDIR)\Main.obj \
	$(OBJDIR)\Bench.obj \
	$(OBJDIR)\LoadingScreen.obj \
	$(OBJDIR)\DebugGUI.obj

{.}.c{$(OBJDIR)}.obj::
	@if not exist $(OBJDIR) mkdir $(OBJDIR)
	$(COMPILE) /Fo$(OBJDIR)\ /sourceDependencies $(OBJDIR)\ $<
