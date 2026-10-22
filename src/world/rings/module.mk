# world/rings: planetary rings and ring rocks.
OBJS = $(OBJS) \
	$(OBJDIR)\world\rings\rings.obj

{world\rings}.c{$(OBJDIR)\world\rings}.obj::
	@if not exist $(OBJDIR)\world\rings mkdir $(OBJDIR)\world\rings
	$(COMPILE) /Fo$(OBJDIR)\world\rings\ /sourceDependencies $(OBJDIR)\world\rings\ $<
