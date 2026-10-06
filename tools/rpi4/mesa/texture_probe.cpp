/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
// GPU/CPU texture ownership regression: exact pixels after uploads, GPU
// draws, repeated maps, unaligned subimage writes, and PBO readback.
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <OS.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static void require(bool ok, const char* what)
{
	if (!ok) {
		std::fprintf(stderr, "FAIL %s (GL %#x, EGL %#x)\n", what,
			glGetError(), eglGetError());
		std::exit(1);
	}
}

static GLuint shader(GLenum type, const char* text)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &text, NULL);
	glCompileShader(s);
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	require(ok, "shader compile");
	return s;
}

static void verify(const unsigned char* actual,
	const std::vector<unsigned char>& expected, const char* step)
{
	for (size_t i = 0; i < expected.size(); i++) {
		if (actual[i] != expected[i]) {
			std::fprintf(stderr, "FAIL %s byte %zu expected %u got %u\n",
				step, i, expected[i], actual[i]);
			std::exit(1);
		}
	}
	require(glGetError() == GL_NO_ERROR, step);
}

int main(int argc, char** argv)
{
	int rounds = argc > 1 ? std::atoi(argv[1]) : 40;
	int baseWidth = argc > 2 ? std::atoi(argv[2]) : 257;
	int baseHeight = argc > 3 ? std::atoi(argv[3]) : 131;
	require(rounds > 0 && rounds <= 10000, "round count");
	require(baseWidth >= 128 && baseWidth <= 3840
		&& baseHeight >= 96 && baseHeight <= 2160, "texture dimensions");
	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	require(eglInitialize(display, NULL, NULL), "EGL initialize");
	require(eglBindAPI(EGL_OPENGL_ES_API), "bind API");
	const EGLint configAttributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
	EGLConfig config;
	EGLint count = 0;
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
	GLuint program = glCreateProgram();
	GLuint vs = shader(GL_VERTEX_SHADER,
		"#version 300 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,"
		"gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}");
	GLuint fs = shader(GL_FRAGMENT_SHADER,
		"#version 300 es\nprecision highp float;uniform sampler2D tex;"
		"out vec4 color;void main(){color=texelFetch(tex,ivec2(gl_FragCoord.xy),0);}");
	glAttachShader(program, vs); glAttachShader(program, fs);
	glLinkProgram(program);
	GLint linked;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	require(linked, "program link");
	glUseProgram(program);
	glUniform1i(glGetUniformLocation(program, "tex"), 0);
	glDisable(GL_DITHER);
	GLuint fbo;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	bigtime_t start = system_time();
	for (int round = 0; round < rounds; round++) {
		const int width = baseWidth + (round % 3) * 63;
		const int height = baseHeight + (round % 5) * 37;
		std::vector<unsigned char> pixels(size_t(width) * height * 4);
		std::vector<unsigned char> expected(pixels.size());
		for (size_t i = 0; i < expected.size(); i++)
			expected[i] = (i * 13 + (i / (width * 4)) * 7 + round * 31) & 255;
		GLuint texture[2];
		glGenTextures(2, texture);
		for (int i = 0; i < 2; i++) {
			glBindTexture(GL_TEXTURE_2D, texture[i]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, i == 0 ? expected.data() : NULL);
		}
		glViewport(0, 0, width, height);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, texture[1], 0);
		require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
			"framebuffer");
		glBindTexture(GL_TEXTURE_2D, texture[0]);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		for (int repeat = 0; repeat < 2; repeat++) {
			glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
			verify(pixels.data(), expected, "upload / sample / repeated CPU read");
		}
		// GPU writes followed by two CPU updates without intervening GPU use.
		// The second prepare must preserve the first update's dirty cache lines.
		glEnable(GL_SCISSOR_TEST);
		glScissor(5, 9, 121, 77);
		glClearColor(1, 0, 1, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glDisable(GL_SCISSOR_TEST);
		for (int y = 9; y < 86; y++) for (int x = 5; x < 126; x++) {
			unsigned char* p = expected.data() + 4 * (y * width + x);
			p[0] = 255; p[1] = 0; p[2] = 255; p[3] = 255;
		}
		glBindTexture(GL_TEXTURE_2D, texture[1]);
		for (int update = 0; update < 2; update++) {
			int x0 = 13 + update * 3, y0 = 7 + update * 31;
			std::vector<unsigned char> patch(117 * 29 * 4);
			for (size_t i = 0; i < patch.size(); i++) patch[i] = (i + round + update) & 255;
			glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, 117, 29, GL_RGBA,
				GL_UNSIGNED_BYTE, patch.data());
			for (int y = 0; y < 29; y++)
				std::memcpy(expected.data() + 4 * ((y0 + y) * width + x0),
					patch.data() + y * 117 * 4, 117 * 4);
		}
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, texture[0], 0);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		for (int y = 0; y < height; y += 17)
			glReadPixels(0, y, width, std::min(17, height - y), GL_RGBA,
				GL_UNSIGNED_BYTE, pixels.data() + size_t(y) * width * 4);
		verify(pixels.data(), expected, "GPU / partial CPU writes / GPU / band reads");
		GLuint pbo;
		glGenBuffers(1, &pbo);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
		glBufferData(GL_PIXEL_PACK_BUFFER, pixels.size(), NULL, GL_STREAM_READ);
		glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		void* mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, pixels.size(), GL_MAP_READ_BIT);
		require(mapped != NULL, "PBO map");
		verify((unsigned char*)mapped, expected, "PBO pixels");
		require(glUnmapBuffer(GL_PIXEL_PACK_BUFFER), "PBO unmap");
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		glDeleteBuffers(1, &pbo);
		glDeleteTextures(2, texture);
	}
	std::printf("PASS %d rounds, repeated maps, partial writes, bands and PBOs: %.3f s\n",
		rounds, (system_time() - start) / 1e6);
	glDeleteFramebuffers(1, &fbo); glDeleteProgram(program);
	glDeleteShader(vs); glDeleteShader(fs);
	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(display, context); eglDestroySurface(display, surface);
	eglTerminate(display); eglReleaseThread();
	return 0;
}
