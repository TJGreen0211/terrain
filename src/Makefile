CC = cl.exe
CFLAGS = ""
#Wall - W0 - W4
PROG = cube.exe

MAKE_DIR = %cd%

SRCS = Main.c \
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
	SimplexNoise.c \
	common/Common.c \
	common/CommonGL.c \
	common/Textures.c \
	Buffers.c \
	camera/Input.c

#SRCS := $(shell echo dir1/*.c)
INCLUDE_PATHS = -I "..\dependencies\lib" -I "..\dependencies\include"
INCLUDE_PATHS = $(INCLUDE_PATHS) -I $(MAKE_DIR) -I $(MAKE_DIR)\math -I $(MAKE_DIR)\geometry -I $(MAKE_DIR)\common -I $(MAKE_DIR)\camera
#LIBRARY_PATHS = -I/opt/X11/lib -L/Users/tjgreen/Documents/OpenGL/Dependencies/lib -lSOIL -lglfw.3.1
LIBS = C:\Users\TJ\Documents\programming\terrain\dependencies\lib\glfw3dll.lib C:\Users\TJ\Documents\programming\terrain\dependencies\lib\SOIL.lib opengl32.lib

all: $(PROG)
	@echo $(SRC_DIR)

#$(CC) $(CFLAGS) -o $(PROG) $(SRCS) $(LIBS) $(INCLUDE_PATHS) $(LIBRARY_PATHS)
$(PROG):$(SRCS)
	$(CC) /MDd $(INCLUDE_PATHS) $(LIBRARY_PATHS) $(SRCS) /link $(LIBS) /NODEFAULTLIB:libcmt.lib

clean:
	del main.obj
