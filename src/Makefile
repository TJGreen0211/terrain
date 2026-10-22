# Build: one .obj per source under ..\build\obj\<folder>, linked once.
#
# Each source folder lists its objects (and its compile rule) in a module.mk;
# adding a folder is one module.mk plus one !include line below.
#
# Header dependencies: cl writes one /sourceDependencies JSON per object, and
# tools\gen_deps.ps1 turns the last build's JSON into $(OBJDIR)\deps.mk before
# every build, so editing a header rebuilds exactly the objects that include
# it. Changing CFLAGS or INCLUDE_PATHS is not tracked: run `nmake clean`.

CC = cl.exe
CFLAGS = /nologo /W3 /O2 /openmp /D_USE_MATH_DEFINES /MD
OUTDIR = ..\build
OBJDIR = $(OUTDIR)\obj
PROG = $(OUTDIR)\cube.exe

INCLUDE_PATHS = -I "..\third_party\include" -I "."
LIBS = ..\third_party\lib\glfw3.lib opengl32.lib user32.lib gdi32.lib shell32.lib

# Used by the batch rules in each module.mk. /MP compiles a folder's
# out-of-date files in parallel.
COMPILE = $(CC) $(CFLAGS) $(INCLUDE_PATHS) /MP /c

OBJS =

!include module.mk
!include engine\core\module.mk
!include engine\math\module.mk
!include engine\platform\module.mk
!include engine\gfx\module.mk
!include engine\camera\module.mk
!include engine\debug\module.mk
!include world\bodies\module.mk
!include world\terrain\module.mk
!include world\ocean\module.mk
!include world\atmosphere\module.mk
!include world\clouds\module.mk
!include world\sky\module.mk
!include world\rings\module.mk
!include world\shadows\module.mk
!include ..\third_party\module.mk

# First target = the default goal, so it must come before deps.mk's targets.
all: $(PROG)

!if [powershell -NoProfile -ExecutionPolicy Bypass -File ..\tools\gen_deps.ps1 -ObjDir $(OBJDIR) -Out $(OBJDIR)\deps.mk]
!endif
!if exist($(OBJDIR)\deps.mk)
!include $(OBJDIR)\deps.mk
!endif

$(PROG): $(OBJS)
	link /nologo /OUT:$(PROG) @<<
$(OBJS) $(LIBS)
<<

clean:
	if exist $(OBJDIR) rmdir /s /q $(OBJDIR)
	if exist $(PROG) del /q $(PROG)
