CC = gcc
CFLAGS = -Wall
PROG = cube

MAKE_DIR = ${PWD}

SRC_DIR := $(MAKE_DIR)
SUB_DIR1 := $(SRC_DIR)/geometry
SUB_DIR2 := $(SRC_DIR)/math

SRCS = Main.c LoadShader.c math/LinearAlg.c arcballCamera.c Window.c geometry/Sphere.c geometry/Geometry.c Obj.c geometry/QuadCube.c geometry/QuadTree.c Water.c SimplexNoise.c Common.c Textures.c Buffers.c

#SRCS := $(shell echo dir1/*.c)
INCLUDE_PATHS := -I/usr/local/include -I/opt/X11/include -I/opt/local/include
INCLUDE_PATHS += -I/$(SRC_DIR)/math	-I$(SRC_DIR)/../Dependencies -I$(SRC_DIR) -I$(SRC_DIR)/geometry
LIBRARY_PATHS = -I/opt/X11/lib -L/Users/tjgreen/Documents/OpenGL/Dependencies/lib -lSOIL -lglfw.3.1
ifeq ($(shell uname),Darwin)
	LIBS = -framework OpenGL -framework GLUT
else
	LIBS = -lglut
endif

all: $(PROG)
	@echo $(SRC_DIR)
	@$(MAKE) -C geometry -f geometry.mk

$(PROG):	$(SRCS)
	$(CC) $(CFLAGS) -o $(PROG) $(SRCS) $(LIBS) $(INCLUDE_PATHS) $(LIBRARY_PATHS)

clean:
	rm -f $(PROG)
