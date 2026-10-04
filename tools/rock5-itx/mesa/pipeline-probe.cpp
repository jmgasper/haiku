// Bounded GLES pipeline checks with an independent host pixel oracle.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <OS.h>
#include "CsfQueue.h"
#include <errno.h>
#include <fcntl.h>
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

using namespace MaliCSF;
enum { WIDTH = 97, HEIGHT = 67, CASES = 6, GUARD = 64,
    PIXEL_BYTES = WIDTH * HEIGHT * 4, UPLOAD_STRIDE = (WIDTH + 5) * 4,
    UPLOAD_BYTES = UPLOAD_STRIDE * HEIGHT };
static bool software;
static const char* const names[CASES] = {
    "texture", "depth", "stencil", "blend", "scissor", "render_texture"
};
static const int rectangles[2][4] = {
    {WIDTH / 8, HEIGHT / 5, WIDTH * 3 / 4, HEIGHT * 2 / 3},
    {WIDTH / 3, HEIGHT / 3, WIDTH * 7 / 8, HEIGHT * 7 / 8}
};
static int failure(const char* what, int line)
{
    fprintf(stderr, "ROCK5_PIPELINE_FAILURE line=%d operation=%s errno=%d egl=%04x gl=%04x\n",
        line, what, errno, eglGetError(), glGetError());
    return 0;
}
#define CHECK(x) do { if (!(x)) return failure(#x, __LINE__); } while (0)
#include "native-observer.h"

static void dump(const unsigned char* data, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) printf("%02x", data[i]);
}

static GLuint shader(GLenum kind, const char* source)
{
    GLuint object = glCreateShader(kind);
    glShaderSource(object, 1, &source, NULL);
    glCompileShader(object);
    GLint compiled = 0;
    glGetShaderiv(object, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[4096];
        glGetShaderInfoLog(object, sizeof(log), NULL, log);
        fprintf(stderr, "ROCK5_PIPELINE_SHADER_FAILURE %s\n", log);
        glDeleteShader(object);
        return 0;
    }
    return object;
}

static GLuint program(const char* fragment_source)
{
    GLuint vertex = shader(GL_VERTEX_SHADER,
        "#version 300 es\nlayout(location=0) in vec3 point;\n"
        "void main(){gl_Position=vec4(point,1.0);}\n");
    GLuint fragment = shader(GL_FRAGMENT_SHADER, fragment_source);
    CHECK(vertex && fragment);
    GLuint object = glCreateProgram();
    glAttachShader(object, vertex); glAttachShader(object, fragment);
    glLinkProgram(object);
    GLint linked = 0;
    glGetProgramiv(object, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096];
        glGetProgramInfoLog(object, sizeof(log), NULL, log);
        fprintf(stderr, "ROCK5_PIPELINE_LINK_FAILURE %s\n", log);
    }
    glDeleteShader(vertex); glDeleteShader(fragment);
    CHECK(linked);
    return object;
}

static void rectangle(const int* r, float depth)
{
    float x0 = r[0] * (2.0f / WIDTH) - 1.0f;
    float x1 = r[2] * (2.0f / WIDTH) - 1.0f;
    float y0 = r[1] * (2.0f / HEIGHT) - 1.0f;
    float y1 = r[3] * (2.0f / HEIGHT) - 1.0f;
    const GLfloat points[] = {x0,y0,depth, x1,y0,depth, x1,y1,depth,
        x0,y0,depth, x1,y1,depth, x0,y1,depth};
    glBufferData(GL_ARRAY_BUFFER, sizeof(points), points, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void coloured(GLint location, unsigned colour, const int* r, float depth = 0)
{
    glUniform4f(location, !!(colour & 1), !!(colour & 2), !!(colour & 4), 1.0f);
    rectangle(r, depth);
}

static void sampling(GLuint shader_program, GLuint texture, bool reflected)
{
    const int full[4] = {0, 0, WIDTH, HEIGHT};
    glUseProgram(shader_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(shader_program, "source_image"), 0);
    glUniform1i(glGetUniformLocation(shader_program, "reflected"), reflected);
    rectangle(full, 0);
}

static unsigned expected(unsigned cycle, unsigned test, unsigned x, unsigned y)
{
    if (test == 5) {
        x = WIDTH - 1 - x;
        y = HEIGHT - 1 - y;
        test = 1;
    }
    bool a = x >= unsigned(rectangles[0][0]) && x < unsigned(rectangles[0][2])
        && y >= unsigned(rectangles[0][1]) && y < unsigned(rectangles[0][3]);
    bool b = x >= unsigned(rectangles[1][0]) && x < unsigned(rectangles[1][2])
        && y >= unsigned(rectangles[1][1]) && y < unsigned(rectangles[1][3]);
    unsigned colour = ((x / 7) ^ (y / 5) ^ cycle) & 7;
    switch (test) {
        case 1: if (b) colour = 2; else if (a) colour = 1; break;
        case 2: if (a && b) colour = 4; break;
        case 3: colour |= (a ? 1 : 0) | (b ? 2 : 0); break;
        case 4: if (a) colour = b ? 2 : 4; break;
    }
    return colour;
}

static int readback(unsigned cycle, unsigned test)
{
    CHECK(glGetError() == GL_NO_ERROR);
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    CHECK(fence);
    struct timespec start, end;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    GLenum wait = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    CHECK(wait == GL_ALREADY_SIGNALED || wait == GL_CONDITION_SATISFIED);
    glDeleteSync(fence);
    unsigned char data[GUARD + PIXEL_BYTES + GUARD];
    unsigned char canary = 0xa0 + cycle * CASES + test;
    memset(data, canary, sizeof(data));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, data + GUARD);
    CHECK(glGetError() == GL_NO_ERROR);
    unsigned errors = 0, guards = 0;
    for (unsigned y = 0; y < HEIGHT; ++y) {
        for (unsigned x = 0; x < WIDTH; ++x) {
            unsigned colour = expected(cycle, test, x, y);
            for (unsigned c = 0; c < 4; ++c)
                errors += data[GUARD + (y * WIDTH + x) * 4 + c]
                    != (c == 3 || (colour & (1u << c)) ? 255 : 0);
        }
    }
    for (unsigned i = 0; i < GUARD; ++i)
        guards += data[i] != canary || data[GUARD + PIXEL_BYTES + i] != canary;
    printf("ROCK5_PIPELINE_PIXELS_BEGIN cycle=%u case=%s width=%u height=%u format=RGBA8 origin=lower-left\n",
        cycle, names[test], WIDTH, HEIGHT);
    printf("guard_before="); dump(data, GUARD); printf("\n");
    for (unsigned y = 0; y < HEIGHT; ++y) {
        printf("row=%03u ", y);
        dump(data + GUARD + y * WIDTH * 4, WIDTH * 4); printf("\n");
    }
    printf("guard_after="); dump(data + GUARD + PIXEL_BYTES, GUARD); printf("\n");
    printf("ROCK5_PIPELINE_PIXELS_END cycle=%u case=%s mismatches=%u guard_errors=%u wait=%04x wait_ns=%llu\n",
        cycle, names[test], errors, guards, wait,
        (unsigned long long)((end.tv_sec - start.tv_sec) * 1000000000ll + end.tv_nsec - start.tv_nsec));
    CHECK(errors == 0 && guards == 0);
    return 1;
}

static void texture_parameters()
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static int context_round(EGLDisplay display, unsigned cycle)
{
    EGLint major = 0, minor = 0;
    CHECK(eglInitialize(display, &major, &minor));
    CHECK(major == 1 && minor >= 4 && eglBindAPI(EGL_OPENGL_ES_API));
    const EGLint attributes[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT, EGL_RED_SIZE,8, EGL_GREEN_SIZE,8,
        EGL_BLUE_SIZE,8, EGL_ALPHA_SIZE,8, EGL_SAMPLE_BUFFERS,0, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    CHECK(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1);
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION,3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    CHECK(context != EGL_NO_CONTEXT);
    const EGLint surface_attributes[] = {EGL_WIDTH,1, EGL_HEIGHT,1, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
    CHECK(surface != EGL_NO_SURFACE && eglMakeCurrent(display, surface, surface, context));
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    const char* version = (const char*)glGetString(GL_VERSION);
    printf("ROCK5_PIPELINE_GL cycle=%u renderer=%s version=%s\n", cycle,
        renderer ? renderer : "NULL", version ? version : "NULL");
    CHECK(renderer && (software ? strcmp(renderer, "softpipe") == 0
        : strcmp(renderer, "Mali-G610 (Panfrost)") == 0));
    CHECK(version && strncmp(version, "OpenGL ES 3.", 12) == 0 && strstr(version, "Mesa 25.3.6"));
    GLuint solid = program("#version 300 es\nprecision highp float;\nuniform vec4 colour;\n"
        "layout(location=0) out vec4 pixel;\nvoid main(){pixel=colour;}\n");
    GLuint sampled = program("#version 300 es\nprecision highp float;\n"
        "uniform highp sampler2D source_image;\nuniform bool reflected;\n"
        "layout(location=0) out vec4 pixel;\nvoid main(){"
        "vec2 uv=gl_FragCoord.xy/vec2(97.0,67.0);"
        "if(reflected) uv=vec2(1.0)-uv; pixel=texture(source_image,uv);}\n");
    CHECK(solid && sampled);
    GLint colour_location = glGetUniformLocation(solid, "colour");
    CHECK(colour_location >= 0 && glGetUniformLocation(sampled, "source_image") >= 0
        && glGetUniformLocation(sampled, "reflected") >= 0);
    GLuint vao, vertex_buffer, textures[3], fbos[2], depth_stencil;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vertex_buffer); glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);
    glGenTextures(3, textures);
    for (GLuint texture : textures) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, WIDTH, HEIGHT);
        texture_parameters();
    }
    unsigned char upload[GUARD + UPLOAD_BYTES + GUARD], original[sizeof(upload)];
    memset(upload, 0x5d + cycle, sizeof(upload));
    for (unsigned y = 0; y < HEIGHT; ++y) {
        for (unsigned x = 0; x < WIDTH; ++x) {
            unsigned colour = expected(cycle, 0, x, y);
            for (unsigned c = 0; c < 4; ++c)
                upload[GUARD + y * UPLOAD_STRIDE + x * 4 + c]
                    = c == 3 || (colour & (1u << c)) ? 255 : 0;
        }
    }
    memcpy(original, upload, sizeof(upload));
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, WIDTH + 5);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WIDTH, HEIGHT, GL_RGBA,
        GL_UNSIGNED_BYTE, upload + GUARD);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glGenRenderbuffers(1, &depth_stencil);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_stencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, WIDTH, HEIGHT);
    glGenFramebuffers(2, fbos);
    for (unsigned i = 0; i < 2; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, textures[i + 1], 0);
        if (i == 0)
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                GL_RENDERBUFFER, depth_stencil);
        CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbos[0]);
    GLint depth_bits = 0, stencil_bits = 0;
    glGetIntegerv(GL_DEPTH_BITS, &depth_bits); glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);
    CHECK(depth_bits == 24 && stencil_bits == 8);
    printf("ROCK5_PIPELINE_TARGET cycle=%u width=%u height=%u depth_bits=%d stencil_bits=%d upload_stride=%u\n",
        cycle, WIDTH, HEIGHT, depth_bits, stencil_bits, UPLOAD_STRIDE);
    glViewport(0, 0, WIDTH, HEIGHT);
    glDisable(GL_DITHER); glDisable(GL_CULL_FACE);
    for (unsigned test = 0; test < CASES; ++test) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbos[0]);
        glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE); glStencilMask(0xff);
        glClearColor(0, 0, 0, 1); glClearDepthf(1); glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        sampling(sampled, textures[0], false);
        glUseProgram(solid);
        switch (test) {
            case 1:
            case 5:
                glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
                coloured(colour_location, 2, rectangles[1], -0.5f);
                coloured(colour_location, 1, rectangles[0], 0.5f);
                coloured(colour_location, 4, rectangles[0], 0.8f);
                break;
            case 2:
                glEnable(GL_STENCIL_TEST);
                glStencilFunc(GL_ALWAYS, 1, 0xff);
                glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                coloured(colour_location, 1, rectangles[0]);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glStencilFunc(GL_EQUAL, 1, 0xff);
                glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
                coloured(colour_location, 4, rectangles[1]);
                break;
            case 3:
                glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_ONE, GL_ONE);
                coloured(colour_location, 1, rectangles[0]);
                coloured(colour_location, 2, rectangles[1]);
                break;
            case 4:
                glEnable(GL_SCISSOR_TEST);
                glScissor(rectangles[0][0], rectangles[0][1],
                    rectangles[0][2] - rectangles[0][0], rectangles[0][3] - rectangles[0][1]);
                glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
                coloured(colour_location, 2, rectangles[1]);
                break;
        }
        if (test == 5) {
            glDisable(GL_DEPTH_TEST);
            glBindFramebuffer(GL_FRAMEBUFFER, fbos[1]);
            sampling(sampled, textures[1], true);
        }
        CHECK(readback(cycle, test));
    }
    glFinish();
    CHECK(!memcmp(upload, original, sizeof(upload)));
    printf("ROCK5_PIPELINE_UPLOAD_PASS cycle=%u bytes=%zu pixels=%u padding_bytes=%u guard_bytes=%u unchanged=1\n",
        cycle, sizeof(upload), WIDTH * HEIGHT, (UPLOAD_STRIDE - WIDTH * 4) * HEIGHT, GUARD * 2);
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glUseProgram(0);
    glDeleteFramebuffers(2, fbos); glDeleteRenderbuffers(1, &depth_stencil);
    glDeleteTextures(3, textures); glDeleteBuffers(1, &vertex_buffer);
    glDeleteVertexArrays(1, &vao); glDeleteProgram(solid); glDeleteProgram(sampled);
    CHECK(glGetError() == GL_NO_ERROR);
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    CHECK(eglDestroySurface(display, surface)); CHECK(eglDestroyContext(display, context));
    CHECK(eglTerminate(display)); CHECK(eglReleaseThread());
    printf("ROCK5_PIPELINE_CONTEXT_PASS cycle=%u cases=%u destroyed=1\n", cycle, CASES);
    return 1;
}

static int run(const char* mode)
{
    software = !strcmp(mode, "--software");
    CHECK(software || !strcmp(mode, "--native"));
    CHECK(unsetenv("MESA_GL_VERSION_OVERRIDE") == 0);
    CHECK(unsetenv("MESA_GLES_VERSION_OVERRIDE") == 0);
    CHECK(unsetenv("MESA_LOADER_DRIVER_OVERRIDE") == 0);
    if (software) {
        CHECK(unsetenv("HAIKU_CSF_DEVICE") == 0);
        CHECK(setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1) == 0);
        CHECK(setenv("GALLIUM_DRIVER", "softpipe", 1) == 0);
    } else {
        CHECK(setenv("HAIKU_CSF_DEVICE", "/dev/graphics/mali_csf/0", 1) == 0);
        CHECK(setenv("HAIKU_CSF_FIRMWARE", "/boot/home/mali_csffw.bin", 1) == 0);
        CHECK(setenv("HAIKU_CSF_TRACE", "1", 1) == 0);
        CHECK(unsetenv("LIBGL_ALWAYS_SOFTWARE") == 0);
        CHECK(unsetenv("GALLIUM_DRIVER") == 0);
    }
    printf("ROCK5_PIPELINE_READY version=1 mode=%s\n", mode);
    auto get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    CHECK(get_display);
    int fd = -1;
    Snapshot before{}, after{};
    if (!software) {
        fd = open("/dev/graphics/mali_csf/0", O_RDONLY | O_CLOEXEC);
        CHECK(fd >= 0 && snapshot(fd, &before));
        CHECK(before.buffers.globalBuffers == 0 && before.vms.globalVms == 0
            && before.heaps.globalHeaps == 0 && before.sync.globalObjects == 0
            && before.kernel_areas == 0 && before.user_maps == 0);
        CHECK(same_snapshot(before, before, 99));
    }
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        EGLDisplay display = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        CHECK(display != EGL_NO_DISPLAY && context_round(display, cycle));
        if (!software) CHECK(snapshot(fd, &after) && same_snapshot(before, after, cycle));
    }
    if (fd >= 0) CHECK(close(fd) == 0);
    printf("ROCK5_PIPELINE_PASS contexts=2 cases=12 pixels=77988 guard_bytes=1536 upload_bytes=54928 software=%u\n", software);
    return 1;
}

int main(int argc, char** argv)
{
    setbuf(stdout, NULL); setbuf(stderr, NULL);
    return argc == 2 && run(argv[1]) ? 0 : 1;
}
