#ifndef MAIN_H
#define MAIN_H
#ifdef __APPLE__
#define __gl_h_
#define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED
#endif

#include <windows.h>
#include <glad/glad.h> 
#include <GLFW/glfw3.h>

#include "engine/gfx/LoadShader.h"
#include "engine/math/LinearAlg.h"
#include "engine/camera/arcballCamera.h"
#include "engine/platform/Window.h"
#include "engine/gfx/Sphere.h"
#include "engine/gfx/Geometry.h"
#include "engine/gfx/Obj.h"
#include "world/terrain/QuadCube.h"
#include "world/terrain/QuadTree.h"
#include "world/ocean/Water.h"
#include "engine/gfx/Common.h"
#include "engine/gfx/Textures.h"
#include "engine/gfx/Buffers.h"
#include "engine/gfx/CommonGL.h"
#include "engine/platform/Input.h"

#define BUFFER_OFFSET(offset) ((char*)NULL+(offset))

#endif
