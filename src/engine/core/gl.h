#ifndef ENGINE_CORE_GL_H
#define ENGINE_CORE_GL_H

/* The one place the platform and GL headers are included; everything else
   includes this (tools/check_modules.ps1 enforces it). windows.h comes first:
   glad and GLFW need its APIENTRY. */
#include <windows.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#endif
