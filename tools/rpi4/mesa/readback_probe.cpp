/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
// Exact readback after RGB/RGBA/sRGB rendering, MSAA resolve and mip levels.
// Check cropped origins, row padding, skipped pixels/rows and byte guards.
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <image.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>


static void
require(bool ok, const char* step)
{
	if (!ok) {
		std::fprintf(stderr, "FAIL %s (GL %#x, EGL %#x)\n", step,
			glGetError(), eglGetError());
		std::exit(1);
	}
}


static void
check_pixels(GLenum format, int samples, int level, int width, int height,
	bool bgra, bool crop)
{
	const int x = crop ? 13 : 0, y = crop ? 7 : 0;
	const int readWidth = crop ? width - 31 : width;
	const int readHeight = crop ? height - 23 : height;
	const int stride = readWidth + 13;
	std::vector<unsigned char> pixels(size_t(stride) * (readHeight + 6) * 4
		+ 128, 0xa5);
	std::vector<unsigned char> expected(pixels);
	for (int row = 0; row < readHeight; row++) {
		for (int column = 0; column < readWidth; column++) {
			const bool cyan = x + column >= 19 && x + column < 392
				&& y + row >= 11 && y + row < 188;
			unsigned char rgba[] = {
				static_cast<unsigned char>(cyan ? 0 : 255),
				static_cast<unsigned char>(cyan ? 255 : 0),
				static_cast<unsigned char>(cyan ? 255 : 0),
				static_cast<unsigned char>(format == GL_RGB8 || !cyan ? 255 : 0)
			};
			if (bgra)
				std::swap(rgba[0], rgba[2]);
			std::memcpy(expected.data() + 64
				+ ((row + 3) * stride + column + 7) * 4, rgba, 4);
		}
	}
	glPixelStorei(GL_PACK_ROW_LENGTH, stride);
	glPixelStorei(GL_PACK_SKIP_PIXELS, 7);
	glPixelStorei(GL_PACK_SKIP_ROWS, 3);
	glReadPixels(x, y, readWidth, readHeight, bgra ? 0x80e1 : GL_RGBA,
		GL_UNSIGNED_BYTE, pixels.data() + 64);
	require(glGetError() == GL_NO_ERROR, "read pixels");
	for (size_t i = 0; i < pixels.size(); i++) {
		if (pixels[i] != expected[i]) {
			std::fprintf(stderr, "FAIL format=%#x samples=%d level=%d "
				"bgra=%d crop=%d byte=%zu expected=%u actual=%u\n",
				format, samples, level, bgra, crop, i, expected[i], pixels[i]);
			std::exit(1);
		}
	}
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_PACK_SKIP_ROWS, 0);
}


static unsigned
check_render(GLenum format, int samples, int level)
{
	// Both the full and cropped reads exceed the GPU path's size threshold.
	const int width = 641, height = 337;
	GLuint texture, framebuffer, multisample = 0, renderbuffer = 0;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexStorage2D(GL_TEXTURE_2D, level + 1, format, width << level,
		height << level);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
		texture, level);
	require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
		"single-sample framebuffer");
	if (samples > 1) {
		glGenFramebuffers(1, &multisample);
		glBindFramebuffer(GL_FRAMEBUFFER, multisample);
		glGenRenderbuffers(1, &renderbuffer);
		glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, format,
			width, height);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_RENDERBUFFER, renderbuffer);
		require(glCheckFramebufferStatus(GL_FRAMEBUFFER)
			== GL_FRAMEBUFFER_COMPLETE, "multisample framebuffer");
	}
	glDisable(GL_DITHER);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(1, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_SCISSOR_TEST);
	glScissor(19, 11, 373, 177);
	glClearColor(0, 1, 1, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
	if (samples > 1) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, multisample);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
		glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	for (bool bgra : {false, true}) {
		for (bool crop : {false, true})
			check_pixels(format, samples, level, width, height, bgra, crop);
	}
	if (samples > 1) {
		glDeleteFramebuffers(1, &multisample);
		glDeleteRenderbuffers(1, &renderbuffer);
	}
	glDeleteFramebuffers(1, &framebuffer);
	glDeleteTextures(1, &texture);
	return 4;
}


int
main()
{
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
	std::printf("renderer: %s\n", glGetString(GL_RENDERER));

	int (*readbackHint)() = NULL;
	image_info info;
	int32 cookie = 0;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		void* symbol = NULL;
		if (std::strstr(info.name, "libEGL_mesa")
			&& get_image_symbol(info.id, "haiku_mesa_readback_band_bytes",
				B_SYMBOL_TYPE_TEXT, &symbol) == B_OK) {
			readbackHint = reinterpret_cast<int (*)()>(symbol);
			break;
		}
	}
	if (const char* expected = std::getenv("PROBE_READBACK_HINT")) {
		require(readbackHint && readbackHint() == std::atoi(expected),
			"driver readback hint");
		std::printf("readback hint: %d\n", readbackHint());
	}
	unsigned cases = 0;
	for (GLenum format : {GL_RGBA8, GL_RGB8, GL_SRGB8_ALPHA8}) {
		for (int samples : {1, 4}) {
			for (int level : {0, 1})
				cases += check_render(format, samples, level);
		}
	}
	require(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
		EGL_NO_CONTEXT), "release context");
	if (readbackHint)
		require(readbackHint() == -1, "hint without a current context");
	eglDestroyContext(display, context);
	eglDestroySurface(display, surface);
	eglTerminate(display);
	eglReleaseThread();
	std::printf("PASS %u format/MSAA/mip/crop/row padding/guard checks\n", cases);
	return 0;
}
