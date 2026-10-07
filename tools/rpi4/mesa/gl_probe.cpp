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

#include <OS.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>


// PROBE_WIDTH, PROBE_HEIGHT, PROBE_DEPTH=1 (a depth buffer, depth test on;
// 2 to 5 are variations, see below)
// and PROBE_TRIANGLES=n (the triangle drawn n times) vary the job.
static int kWidth = 128;
static int kHeight = 96;


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
	bigtime_t start = system_time();
	int only = argc > 1 ? atoi(argv[1]) : 0;
	if (getenv("PROBE_WIDTH") != NULL)
		kWidth = atoi(getenv("PROBE_WIDTH"));
	if (getenv("PROBE_HEIGHT") != NULL)
		kHeight = atoi(getenv("PROBE_HEIGHT"));
	bool depth = getenv("PROBE_DEPTH") != NULL;
	int triangles = getenv("PROBE_TRIANGLES") != NULL
		? atoi(getenv("PROBE_TRIANGLES")) : 1;

	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!check(eglInitialize(display, NULL, NULL), "eglInitialize"))
		return 1;

	const EGLint configAttributes[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, depth ? 16 : 0,
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

	EGLint samples = -1, depthBits = -1, stencilBits = -1;
	eglGetConfigAttrib(display, config, EGL_SAMPLES, &samples);
	eglGetConfigAttrib(display, config, EGL_DEPTH_SIZE, &depthBits);
	eglGetConfigAttrib(display, config, EGL_STENCIL_SIZE, &stencilBits);
	printf("renderer: %s\nversion: %s\nconfig: %d samples, %d depth bits, %d "
		"stencil bits\n", glGetString(GL_RENDERER), glGetString(GL_VERSION),
		(int)samples, (int)depthBits, (int)stencilBits);
	fflush(stdout);

	bool ok = true;
	unsigned char* pixels = (unsigned char*)malloc(kWidth * kHeight * 4);
	size_t pixelBytes = kWidth * kHeight * 4;
	// PROBE_DEPTH=2: a depth buffer in the configuration, but no depth test
	if (depth && atoi(getenv("PROBE_DEPTH")) == 1) {
		glEnable(GL_DEPTH_TEST);
		glClearDepthf(1.0f);
	}
	glViewport(0, 0, kWidth, kHeight);

	if (only == 0 || only == 1) {
		glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | (depth ? GL_DEPTH_BUFFER_BIT : 0));
		memset(pixels, 0xa5, pixelBytes);
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
		// PROBE_DEPTH=3: a job that stores nothing but depth
		if (depth && atoi(getenv("PROBE_DEPTH")) == 3) {
			glClear(GL_DEPTH_BUFFER_BIT);
			triangles = 0;
		} else if (depth && atoi(getenv("PROBE_DEPTH")) >= 4) {
			// 4: everything cleared at once (no clear by drawing a quad on
			// V3D 4.2), 5: the same with the depth test on
			if (atoi(getenv("PROBE_DEPTH")) == 5)
				glEnable(GL_DEPTH_TEST);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT
				| GL_STENCIL_BUFFER_BIT);
		} else
			glClear(GL_COLOR_BUFFER_BIT | (depth ? GL_DEPTH_BUFFER_BIT : 0));
		for (int i = 0; i < triangles; i++)
			glDrawArrays(GL_TRIANGLES, 0, 3);
		memset(pixels, 0xa5, pixelBytes);
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
		if (other != 0) {
			// a coarse picture of what came back, top row first
			for (int y = kHeight - 4; y >= 0; y -= 8) {
				for (int x = 4; x < kWidth; x += 8) {
					const unsigned char* p = pixels + 4 * (y * kWidth + x);
					putchar(p[0] == 255 && p[2] == 0 ? 'R'
						: p[0] == 0 && p[2] == 255 ? 'B'
						: p[0] == 0xa5 ? '.' : '?');
				}
				putchar('\n');
			}
		}
		const unsigned char* low = pixels + 4 * (2 * kWidth + 2);
		const unsigned char* high = pixels
			+ 4 * ((kHeight - 3) * kWidth + kWidth - 3);
		ok &= check(other == 0 && red > kWidth * kHeight * 45 / 100
			&& red < kWidth * kHeight * 55 / 100 && low[0] == 255
			&& high[2] == 255, "triangle over a clear");
	}

	printf("render and validation: %.3f ms\n",
		(system_time() - start) / 1000.0);
	free(pixels);
	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(display, context);
	eglDestroySurface(display, surface);
	eglTerminate(display);
	eglReleaseThread();
	printf("%s\n", ok ? "GL PROBE PASSED" : "GL PROBE FAILED");
	return ok ? 0 : 1;
}
