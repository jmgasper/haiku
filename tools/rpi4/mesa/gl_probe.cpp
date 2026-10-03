/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Offscreen GLES 2 on an EGL pbuffer, step by step, with every pixel read
	back and checked:
	  1. clear only (a render job without a binner job)
	  2. one triangle over the clear (binner and render)
	Prints the renderer; exits non-zero if a step fails. A step number as
	the argument runs only that step. */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>


static const int kWidth = 128;
static const int kHeight = 96;


static bool
check(bool ok, const char* what)
{
	printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
	fflush(stdout);
	return ok;
}


static GLuint
compile(GLenum type, const char* source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	GLint status = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (status == 0) {
		char log[512];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		printf("shader: %s\n", log);
	}
	return shader;
}


int
main(int argc, char** argv)
{
	int only = argc > 1 ? atoi(argv[1]) : 0;

	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!check(eglInitialize(display, NULL, NULL), "eglInitialize"))
		return 1;

	const EGLint configAttributes[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLConfig config;
	EGLint count = 0;
	if (!check(eglChooseConfig(display, configAttributes, &config, 1, &count)
			&& count == 1, "eglChooseConfig")) {
		return 1;
	}

	const EGLint surfaceAttributes[] = {
		EGL_WIDTH, kWidth, EGL_HEIGHT, kHeight, EGL_NONE
	};
	EGLSurface surface = eglCreatePbufferSurface(display, config,
		surfaceAttributes);
	eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
		contextAttributes);
	if (!check(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT
			&& eglMakeCurrent(display, surface, surface, context),
			"pbuffer and context")) {
		return 1;
	}

	printf("renderer: %s\nversion: %s\n", glGetString(GL_RENDERER),
		glGetString(GL_VERSION));
	fflush(stdout);

	bool ok = true;
	static unsigned char pixels[kWidth * kHeight * 4];
	glViewport(0, 0, kWidth, kHeight);

	if (only == 0 || only == 1) {
		glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		memset(pixels, 0xa5, sizeof(pixels));
		glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		int wrong = 0;
		for (int i = 0; i < kWidth * kHeight; i++) {
			const unsigned char* p = pixels + 4 * i;
			if (p[0] != 51 || p[1] != 102 || p[2] != 153 || p[3] != 255)
				wrong++;
		}
		printf("clear: %d wrong pixels, first %u %u %u %u\n", wrong, pixels[0],
			pixels[1], pixels[2], pixels[3]);
		ok &= check(wrong == 0, "clear only");
	}

	if (only == 0 || only == 2) {
		GLuint program = glCreateProgram();
		glAttachShader(program, compile(GL_VERTEX_SHADER,
			"attribute vec2 position;\n"
			"void main() { gl_Position = vec4(position, 0.0, 1.0); }\n"));
		glAttachShader(program, compile(GL_FRAGMENT_SHADER,
			"precision mediump float;\n"
			"void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n"));
		glBindAttribLocation(program, 0, "position");
		glLinkProgram(program);
		glUseProgram(program);

		// the lower left half of the buffer
		static const GLfloat vertices[] = {-1, -1, 1, -1, -1, 1};
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
		glEnableVertexAttribArray(0);

		glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		memset(pixels, 0xa5, sizeof(pixels));
		glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

		int red = 0, blue = 0, other = 0;
		for (int i = 0; i < kWidth * kHeight; i++) {
			const unsigned char* p = pixels + 4 * i;
			if (p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255)
				red++;
			else if (p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255)
				blue++;
			else
				other++;
		}
		printf("triangle: %d red, %d blue, %d other of %d\n", red, blue, other,
			kWidth * kHeight);
		const unsigned char* low = pixels + 4 * (2 * kWidth + 2);
		const unsigned char* high = pixels
			+ 4 * ((kHeight - 3) * kWidth + kWidth - 3);
		ok &= check(other == 0 && red > kWidth * kHeight * 45 / 100
			&& red < kWidth * kHeight * 55 / 100 && low[0] == 255
			&& high[2] == 255, "triangle over a clear");
	}

	printf("%s\n", ok ? "GL PROBE PASSED" : "GL PROBE FAILED");
	return ok ? 0 : 1;
}
