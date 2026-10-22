CC = cl.exe
CFLAGS = /nologo /W3 /O2 /openmp /D_USE_MATH_DEFINES
OUTDIR = ..\build
OBJDIR = $(OUTDIR)\obj
PROG = $(OUTDIR)\cube.exe

SRCS = Main.c \
	Sun.c \
	Skybox.c \
	Atmosphere.c \
	Clouds.c \
	Nebula.c \
	Rings.c \
	Bodies.c \
	Profiler.c \
	Bench.c \
	LoadingScreen.c \
	GUI.c \
	DebugGUI.c \
	TerrainGen.c \
	TerrainDetail.c \
	Shadows.c \
	Planets.c \
	SolarSystem.c \
	glad.c \
	LoadShader.c \
	math/LinearAlg.c \
	camera/arcballCamera.c \
	Window.c \
	geometry/Sphere.c \
	geometry/Geometry.c \
	Obj.c \
	geometry/QuadCube.c \
	geometry/QuadTree.c \
	Water.c \
	common/Common.c \
	common/CommonGL.c \
	common/Textures.c \
	Buffers.c \
	camera/Input.c \
	engine/core/paths.c

INCLUDE_PATHS = -I "..\dependencies\include" -I "." -I math -I geometry -I common -I camera
LIBS = ..\dependencies\lib\glfw3.lib opengl32.lib user32.lib gdi32.lib shell32.lib

all: $(PROG)

$(PROG): $(SRCS)
	@if not exist $(OBJDIR) mkdir $(OBJDIR)
	$(CC) /MD $(CFLAGS) $(INCLUDE_PATHS) $(SRCS) /Fo$(OBJDIR)\ /Fe$(PROG) /link $(LIBS)

clean:
	if exist $(OBJDIR) rmdir /s /q $(OBJDIR)
	if exist $(PROG) del /q $(PROG)
