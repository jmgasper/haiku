// Haiku adaptation of the pinned Linux rendering oracle; see source receipt.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <OS.h>
#include "CsfQueue.h"
#include <errno.h>
#include <initializer_list>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
using namespace MaliCSF;
enum { WIDTH = 64, HEIGHT = 64, PIXEL_BYTES = WIDTH * HEIGHT * 4, GUARD = 64 };
static bool software;
static int failure(const char *what, int line)
{
    fprintf(stderr, "ROCK5_MESA_FAILURE line=%d operation=%s errno=%d egl=%04x gl=%04x\n",
        line, what, errno, eglGetError(), glGetError());
    return 0;
}
#define CHECK(x) do { if (!(x)) return failure(#x, __LINE__); } while (0)

static GLuint shader(GLenum kind, const char *source)
{
    GLuint object = glCreateShader(kind);
    glShaderSource(object, 1, &source, NULL);
    glCompileShader(object);
    GLint status = 0;
    glGetShaderiv(object, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[4096];
        glGetShaderInfoLog(object, sizeof(log), NULL, log);
        fprintf(stderr, "ROCK5_MESA_SHADER_FAILURE %s\n", log);
        glDeleteShader(object);
        return 0;
    }
    return object;
}

static void draw_rectangle(GLint colour, const int rectangle[4], unsigned rgba)
{
    float x0 = rectangle[0] / 32.0f - 1.0f, x1 = rectangle[2] / 32.0f - 1.0f;
    float y0 = rectangle[1] / 32.0f - 1.0f, y1 = rectangle[3] / 32.0f - 1.0f;
    const GLfloat points[] = {x0,y0, x1,y0, x1,y1, x0,y0, x1,y1, x0,y1};
    glBufferData(GL_ARRAY_BUFFER, sizeof(points), points, GL_STREAM_DRAW);
    glUniform4f(colour, !!(rgba & 1), !!(rgba & 2), !!(rgba & 4), 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void dump_bytes(const unsigned char *data, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) printf("%02x", data[i]);
}

static int render_round(unsigned cycle, unsigned round, GLint colour)
{
    static const int rectangles[2][2][4] = {
        {{8,8,48,40}, {24,24,56,56}},
        {{4,12,40,52}, {20,4,60,44}},
    };
    static const unsigned colours[2][2][3] = {
        {{0,1,2}, {4,3,5}}, {{7,6,4}, {5,0,3}},
    };
    unsigned background = colours[cycle][round][0];
    glClearColor(!!(background & 1), !!(background & 2), !!(background & 4), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    for (unsigned i = 0; i < 2; ++i)
        draw_rectangle(colour, rectangles[round][i], colours[cycle][round][i + 1]);
    CHECK(glGetError() == GL_NO_ERROR);
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    CHECK(fence != NULL);
    struct timespec start, end;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    GLenum wait = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    CHECK(wait == GL_ALREADY_SIGNALED || wait == GL_CONDITION_SATISFIED);
    glDeleteSync(fence);
    unsigned char buffer[GUARD + PIXEL_BYTES + GUARD];
    unsigned char canary = 0xa0 + cycle * 2 + round;
    memset(buffer, canary, sizeof(buffer));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, buffer + GUARD);
    CHECK(glGetError() == GL_NO_ERROR);
    unsigned mismatches = 0;
    for (unsigned y = 0; y < HEIGHT; ++y) {
        for (unsigned x = 0; x < WIDTH; ++x) {
            unsigned expected = background;
            for (unsigned i = 0; i < 2; ++i) {
                const int *r = rectangles[round][i];
                if ((int)x >= r[0] && (int)x < r[2] && (int)y >= r[1] && (int)y < r[3])
                    expected = colours[cycle][round][i + 1];
            }
            const unsigned char *pixel = buffer + GUARD + (y * WIDTH + x) * 4;
            for (unsigned c = 0; c < 4; ++c)
                mismatches += pixel[c] != (c == 3 || (expected & (1u << c)) ? 255 : 0);
        }
    }
    unsigned damaged_guards = 0;
    for (unsigned i = 0; i < GUARD; ++i)
        damaged_guards += buffer[i] != canary || buffer[GUARD + PIXEL_BYTES + i] != canary;
    printf("ROCK5_MESA_PIXELS_BEGIN cycle=%u round=%u width=%u height=%u format=RGBA8 origin=lower-left\n",
        cycle, round, WIDTH, HEIGHT);
    printf("guard_before="); dump_bytes(buffer, GUARD); printf("\n");
    for (unsigned y = 0; y < HEIGHT; ++y) {
        printf("row=%02u ", y);
        dump_bytes(buffer + GUARD + y * WIDTH * 4, WIDTH * 4);
        printf("\n");
    }
    printf("guard_after="); dump_bytes(buffer + GUARD + PIXEL_BYTES, GUARD); printf("\n");
    printf("ROCK5_MESA_PIXELS_END cycle=%u round=%u mismatches=%u guard_errors=%u wait=%04x wait_ns=%llu\n",
        cycle, round, mismatches, damaged_guards, wait,
        (unsigned long long)((end.tv_sec - start.tv_sec) * 1000000000ll + end.tv_nsec - start.tv_nsec));
    CHECK(mismatches == 0 && damaged_guards == 0);
    return 1;
}

static int render_context(EGLDisplay display, unsigned cycle)
{
    EGLint major = 0, minor = 0;
    CHECK(eglInitialize(display, &major, &minor));
    CHECK(major == 1 && minor >= 4);
    printf("ROCK5_MESA_EGL cycle=%u version=%d.%d vendor=%s\n", cycle, major, minor,
        eglQueryString(display, EGL_VENDOR));
    CHECK(eglBindAPI(EGL_OPENGL_ES_API));
    const EGLint attributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE,8, EGL_GREEN_SIZE,8,
        EGL_BLUE_SIZE,8, EGL_ALPHA_SIZE,8, EGL_DEPTH_SIZE,0, EGL_STENCIL_SIZE,0,
        EGL_SAMPLE_BUFFERS,0, EGL_NONE };
    EGLConfig config;
    EGLint count = 0;
    CHECK(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1);
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION,3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    CHECK(context != EGL_NO_CONTEXT);
    const EGLint surface_attributes[] = {EGL_WIDTH,WIDTH, EGL_HEIGHT,HEIGHT, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
    CHECK(surface != EGL_NO_SURFACE);
    CHECK(eglMakeCurrent(display, surface, surface, context));
    EGLint actual_width = 0, actual_height = 0;
    CHECK(eglQuerySurface(display, surface, EGL_WIDTH, &actual_width));
    CHECK(eglQuerySurface(display, surface, EGL_HEIGHT, &actual_height));
    CHECK(actual_width == WIDTH && actual_height == HEIGHT);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    printf("ROCK5_MESA_GL cycle=%u renderer=%s version=%s vendor=%s\n", cycle,
        renderer ? renderer : "NULL", version ? version : "NULL", glGetString(GL_VENDOR));
    CHECK(renderer && (software ? strcmp(renderer, "softpipe") == 0
        : strstr(renderer, "Mali-G610") && strstr(renderer, "Panfrost")));
    CHECK(version && strncmp(version, "OpenGL ES 3.", 12) == 0);
    CHECK(version && strstr(version, "Mesa 25.3.6"));
    // Prove the pbuffer itself works before binding a texture-backed FBO.
    glClearColor(0, 1, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    unsigned char pbuffer_pixel[4] = {0};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pbuffer_pixel);
    CHECK(glGetError() == GL_NO_ERROR && pbuffer_pixel[0] == 0
        && pbuffer_pixel[1] == 255 && pbuffer_pixel[2] == 0 && pbuffer_pixel[3] == 255);
    CHECK(eglSwapBuffers(display, surface));
    memset(pbuffer_pixel, 0, sizeof(pbuffer_pixel));
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pbuffer_pixel);
    CHECK(glGetError() == GL_NO_ERROR && pbuffer_pixel[0] == 0
        && pbuffer_pixel[1] == 255 && pbuffer_pixel[2] == 0 && pbuffer_pixel[3] == 255);
    printf("ROCK5_MESA_PBUFFER_PASS cycle=%u width=%d height=%d swap_preserved=1\n",
        cycle, actual_width, actual_height);
    GLuint vertex = shader(GL_VERTEX_SHADER,
        "#version 300 es\nlayout(location=0) in vec2 point;\n"
        "void main(){gl_Position=vec4(point,0.0,1.0);}\n");
    GLuint fragment = shader(GL_FRAGMENT_SHADER,
        "#version 300 es\nprecision highp float;\nuniform vec4 colour;\n"
        "layout(location=0) out vec4 pixel;\nvoid main(){pixel=colour;}\n");
    CHECK(vertex && fragment);
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex); glAttachShader(program, fragment);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    CHECK(linked);
    glDeleteShader(vertex); glDeleteShader(fragment);
    glUseProgram(program);
    GLint colour = glGetUniformLocation(program, "colour");
    CHECK(colour >= 0);
    GLuint texture, framebuffer, vao, vertex_buffer;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, WIDTH, HEIGHT);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vertex_buffer); glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);
    glViewport(0, 0, WIDTH, HEIGHT);
    glDisable(GL_DITHER); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
    CHECK(glGetError() == GL_NO_ERROR);
    for (unsigned round = 0; round < 2; ++round)
        CHECK(render_round(cycle, round, colour));
    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(1, &texture);
    glDeleteBuffers(1, &vertex_buffer); glDeleteVertexArrays(1, &vao);
    glUseProgram(0); glDeleteProgram(program);
    CHECK(glGetError() == GL_NO_ERROR);
    if (cycle == 1) {
        // Termination retires handles, but bound resources must remain usable.
        CHECK(eglTerminate(display));
        CHECK(eglGetCurrentContext() == context && glGetString(GL_VERSION));
        glFinish(); CHECK(glGetError() == GL_NO_ERROR);
        CHECK(eglInitialize(display, &major, &minor));
        CHECK(eglGetCurrentContext() == context);
        CHECK(!eglDestroyContext(display, context) && eglGetError() == EGL_BAD_CONTEXT);
        CHECK(!eglDestroySurface(display, surface) && eglGetError() == EGL_BAD_SURFACE);
        CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
        CHECK(eglTerminate(display));
        printf("ROCK5_MESA_BOUND_TERMINATE_PASS cycle=1 reinitialized=1 retired_handles=1\n");
    } else {
        CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
        CHECK(eglDestroySurface(display, surface));
        CHECK(eglDestroyContext(display, context));
        CHECK(eglTerminate(display));
    }
    CHECK(eglReleaseThread());
    printf("ROCK5_MESA_CONTEXT_PASS cycle=%u rounds=2 destroyed=1\n", cycle);
    return 1;
}


struct Snapshot {
    ClientInfo buffers;
    VmInfo vms;
    HeapInfo heaps;
    SyncInfo sync;
    unsigned kernel_areas, user_maps;
};
static int snapshot(int fd, Snapshot *s)
{
    memset(s, 0, sizeof(*s));
    s->buffers.version = s->vms.version = s->heaps.version = s->sync.version = 1;
    CHECK(ioctl(fd, kGetClientInfo, &s->buffers, sizeof(s->buffers)) == 0);
    CHECK(ioctl(fd, kGetVmInfo, &s->vms, sizeof(s->vms)) == 0);
    CHECK(ioctl(fd, kGetHeapInfo, &s->heaps, sizeof(s->heaps)) == 0);
    CHECK(ioctl(fd, kGetSyncInfo, &s->sync, sizeof(s->sync)) == 0);
    for (team_id team : {team_id(B_SYSTEM_TEAM), team_id(B_CURRENT_TEAM)}) {
        ssize_t cookie = 0;
        area_info area;
        unsigned visited = 0;
        while (get_next_area_info(team, &cookie, &area) == B_OK) {
            visited++;
            if (team == B_SYSTEM_TEAM && !strncmp(area.name, "Mali CSF ", 9))
                s->kernel_areas++;
            if (team == B_CURRENT_TEAM && !strcmp(area.name, "Mali CSF buffer mapping"))
                s->user_maps++;
        }
        CHECK(visited > 0);
    }
    return 1;
}
static int same_snapshot(const Snapshot& before, const Snapshot& after, unsigned cycle)
{
    printf("ROCK5_MESA_NATIVE_LIFETIME cycle=%u clients=%u buffers=%u bytes=%llu vms=%u generations=%u vm_pages=%u heaps=%u chunks=%u heap_bytes=%llu heap_pages=%u heap_generations=%u sync_clients=%u sync_objects=%u sync_points=%u sync_events=%u sync_exports=%u sync_waits=%u kernel_areas=%u user_maps=%u\n",
        cycle, after.buffers.globalClients, after.buffers.globalBuffers,
        (unsigned long long)after.buffers.globalBufferBytes,
        after.vms.globalVms, after.vms.globalGenerations, after.vms.globalTablePages,
        after.heaps.globalHeaps, after.heaps.globalChunks,
        (unsigned long long)after.heaps.globalBytes, after.heaps.globalTablePages,
        after.heaps.globalHeapGenerations, after.sync.globalClients, after.sync.globalObjects,
        after.sync.globalPoints, after.sync.globalEvents, after.sync.globalExports,
        after.sync.globalWaits, after.kernel_areas, after.user_maps);
    CHECK(!memcmp(&before, &after, sizeof(before)));
    return 1;
}
static int run(const char *mode)
{
    software = !strcmp(mode, "--software");
    bool absent = !strcmp(mode, "--absent-device");
    CHECK(software || absent || !strcmp(mode, "--native"));
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
    printf("ROCK5_MESA_PROBE_READY version=2 mode=%s\n", mode);
    const char *extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    CHECK(extensions && strstr(extensions, "EGL_MESA_platform_surfaceless"));
    printf("ROCK5_MESA_CLIENT_EXTENSIONS %s\n", extensions);
    auto get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    CHECK(get_display);
    if (absent) {
        CHECK(access("/dev/graphics/mali_csf/0", F_OK) != 0 && errno == ENOENT);
        EGLDisplay display = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        CHECK(display != EGL_NO_DISPLAY);
        CHECK(!eglInitialize(display, NULL, NULL) && eglGetError() == EGL_NOT_INITIALIZED);
        CHECK(eglReleaseThread());
        printf("ROCK5_MESA_ABSENT_DEVICE_PASS software_fallback=0\n");
        return 1;
    }
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
        CHECK(display != EGL_NO_DISPLAY && render_context(display, cycle));
        if (!software)
            CHECK(snapshot(fd, &after) && same_snapshot(before, after, cycle));
    }
    if (fd >= 0) CHECK(close(fd) == 0);
    printf("ROCK5_MESA_RENDER_PASS contexts=2 rounds=4 pixels=16384 guard_bytes=512 software=%u\n", software);
    return 1;
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (argc != 2) return 2;
    return run(argv[1]) ? 0 : 1;
}
