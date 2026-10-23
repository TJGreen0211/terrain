#include "engine/core/gl.h"
#include "app/app.h"
#include "app/bench.h"
#include "app/render.h"
#include "engine/platform/frame_limiter.h"
/* stb_image_write lives here; bench.c uses it for its PNG captures. */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <string.h>
#include <direct.h>

int main(int argc, char *argv[])
{
	/* Shader and asset paths are relative to the exe's folder (paths.c). */
	{
		char exePath[MAX_PATH];
		GetModuleFileNameA(NULL, exePath, MAX_PATH);
		char *slash = strrchr(exePath, '\\');
		if(slash) *slash = '\0';
		_chdir(exePath);
	}

	static App app;
	int exitCode;
	if (!App_Create(&app, argc, argv, &exitCode))
		return exitCode;

	while(!glfwWindowShouldClose(app.window))
	{
		App_Update(&app);
		Render_Frame(&app);
		if (Bench_NoteFrame())
			glfwSetWindowShouldClose(app.window, GL_TRUE);

		glfwPollEvents();
		if (!Bench_Active())
			FrameLimiter_Wait();
		glfwSwapBuffers(app.window);
	}
	App_Destroy(&app);
	return 0;
}
