CC = gcc
CFLAGS = -Wall
PROG = cube

MAKE_DIR = ${PWD}

SRC_DIR := $(MAKE_DIR)


SRCS = Main.c LoadShader.c math/LinearAlg.c arcballCamera.c Window.c geometry/Sphere.c geometry/Geometry.c Obj.c geometry/QuadCube.c geometry/QuadTree.c Water.c SimplexNoise.c Common.c Textures.c

#SRCS := $(shell echo dir1/*.c)
OS_INCLUDE_PATHS = -I/usr/local/include -I/opt/X11/include -I/opt/local/include
SRC_INCLUDE_PATHS = -I/$(SRC_DIR)/math	-I$(SRC_DIR)/../Dependencies -I$(SRC_DIR) -I$(SRC_DIR)/geometry
LIBRARY_PATHS = -I/opt/X11/lib -L/Users/tjgreen/Documents/OpenGL/Dependencies/lib -lSOIL -lglfw.3.1
ifeq ($(shell uname),Darwin)
	LIBS = -framework OpenGL -framework GLUT
else
	LIBS = -lglut
endif

all: $(PROG)
	@echo $(SRC_DIR)

$(PROG):	$(SRCS)
	$(CC) $(CFLAGS) -o $(PROG) $(SRCS) $(LIBS) $(SRC_INCLUDE_PATHS) $(OS_INCLUDE_PATHS) $(LIBRARY_PATHS)

clean:
	rm -f $(PROG)
