# third_party: vendored sources (glad GL loader). Paths are relative to src\,
# where nmake runs.
OBJS = $(OBJS) \
	$(OBJDIR)\third_party\glad.obj

{..\third_party\src}.c{$(OBJDIR)\third_party}.obj::
	@if not exist $(OBJDIR)\third_party mkdir $(OBJDIR)\third_party
	$(COMPILE) /Fo$(OBJDIR)\third_party\ /sourceDependencies $(OBJDIR)\third_party\ $<
