/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
// Grow and retire GL buffers while keeping their context alive. Pauses let
// Mesa retire cached BOs, separating cache-table retention from live GPU RAM.
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <OS.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static void
require(bool ok, const char* what)
{
	if (!ok) {
		std::fprintf(stderr, "FAIL %s (GL %#x, EGL %#x)\n", what,
			glGetError(), eglGetError());
		std::exit(1);
	}
}

static void
memory(const char* phase, int step)
{
	ssize_t cookie = 0;
	area_info area;
	uint64 heap = 0, heapVirtual = 0, resident = 0, gpu = 0;
	int areas = 0;
	while (get_next_area_info(B_CURRENT_TEAM, &cookie, &area) == B_OK) {
		resident += area.ram_size;
		areas++;
		if (std::strstr(area.name, "heap") != NULL) {
			heap += area.ram_size;
			heapVirtual += area.size;
		}
		if (std::strstr(area.name, "v3d") != NULL)
			gpu += area.ram_size;
	}
	team_usage_info usage = {};
	get_team_usage_info(B_CURRENT_TEAM, B_TEAM_USAGE_SELF, &usage);
	std::printf("MEM phase=%s step=%d heap=%llu heap_virtual=%llu "
		"resident=%llu gpu_mapped=%llu areas=%d cpu_us=%lld\n", phase, step,
		(unsigned long long)heap, (unsigned long long)heapVirtual,
		(unsigned long long)resident, (unsigned long long)gpu, areas,
		(long long)(usage.user_time + usage.kernel_time));
	std::fflush(stdout);
}

static void
buffer_round(size_t size, unsigned char value)
{
	GLuint buffer;
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, size, NULL, GL_DYNAMIC_DRAW);
	require(glGetError() == GL_NO_ERROR, "allocate buffer");
	const size_t offsets[] = {0, size - 64};
	for (size_t offset : offsets) {
		void* ptr = glMapBufferRange(GL_ARRAY_BUFFER, offset, 64,
			GL_MAP_WRITE_BIT);
		require(ptr != NULL, "write map");
		std::memset(ptr, value, 64);
		require(glUnmapBuffer(GL_ARRAY_BUFFER), "write unmap");
	}
	for (size_t offset : offsets) {
		const unsigned char* ptr = (const unsigned char*)glMapBufferRange(
			GL_ARRAY_BUFFER, offset, 64, GL_MAP_READ_BIT);
		require(ptr != NULL, "read map");
		for (int i = 0; i < 64; i++)
			require(ptr[i] == value, "buffer contents");
		require(glUnmapBuffer(GL_ARRAY_BUFFER), "read unmap");
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDeleteBuffers(1, &buffer);
	require(glGetError() == GL_NO_ERROR, "delete buffer");
}

int
main(int argc, char** argv)
{
	int steps = argc > 1 ? std::atoi(argv[1]) : 1024;
	require(steps >= 1 && steps <= 2048, "steps in 1..2048");
	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	require(eglInitialize(display, NULL, NULL), "EGL initialize");
	require(eglBindAPI(EGL_OPENGL_ES_API), "bind API");
	const EGLint configAttributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
	EGLConfig config;
	EGLint count;
	require(eglChooseConfig(display, configAttributes, &config, 1, &count)
		&& count == 1, "config");
	const EGLint surfaceAttributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
	const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
	EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
	EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
		contextAttributes);
	require(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT
		&& eglMakeCurrent(display, surface, surface, context), "context");
	std::printf("renderer=%s steps=%d\n", glGetString(GL_RENDERER), steps);
	buffer_round(1048576, 0x31);
	memory("warm", 0);
	bigtime_t start = system_time();
	for (int i = 1; i <= steps; i++) {
		buffer_round(1048576 + size_t(i) * 8192, (unsigned char)i);
		if (i % 32 == 0) {
			// BOs older than two whole monotonic seconds are discarded when
			// another BO is retired. Bound their peak RAM to one small batch.
			snooze(3100000);
			buffer_round(4096, 0x52);
		}
		if (i % 128 == 0)
			memory("growing", i);
	}
	snooze(3100000);
	buffer_round(4096, 0x74);
	memory("retired", steps);
	std::printf("elapsed_us=%lld\n", (long long)(system_time() - start));
	require(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
		EGL_NO_CONTEXT), "release context");
	require(eglDestroySurface(display, surface), "destroy surface");
	require(eglDestroyContext(display, context), "destroy context");
	require(eglTerminate(display), "terminate");
	memory("destroyed", steps);
	std::puts("PASS growing buffers, readback and retirement");
	return 0;
}
