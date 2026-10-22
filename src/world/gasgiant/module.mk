# world/gasgiant: gas giant looks (styles, latitude profiles, stand-in shading).
OBJS = $(OBJS) \
	$(OBJDIR)\world\gasgiant\gas_giant.obj

{world\gasgiant}.c{$(OBJDIR)\world\gasgiant}.obj::
	@if not exist $(OBJDIR)\world\gasgiant mkdir $(OBJDIR)\world\gasgiant
	$(COMPILE) /Fo$(OBJDIR)\world\gasgiant\ /sourceDependencies $(OBJDIR)\world\gasgiant\ $<
