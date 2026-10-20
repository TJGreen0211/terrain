CC = cl.exe
CFLAGS = /nologo /W3 /O2 /openmp /D_USE_MATH_DEFINES
PROG = cube.exe

SRCS = Main.c \
	Sun.c \
	Skybox.c \
	Atmosphere.c \
	Rings.c \
	LoadingScreen.c \
	GUI.c \
	TerrainGen.c \
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
	camera/Input.c

INCLUDE_PATHS = -I "..\dependencies\include" -I "." -I math -I geometry -I common -I camera
LIBS = ..\dependencies\lib\glfw3.lib opengl32.lib user32.lib gdi32.lib shell32.lib

all: $(PROG)

$(PROG): $(SRCS)
	$(CC) /MD $(CFLAGS) $(INCLUDE_PATHS) $(SRCS) /Fe$(PROG) /link $(LIBS)

clean:
	del /q *.obj $(PROG) Main.exe 2>nul
