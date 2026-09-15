// Exercise real CSF tiler-heap growth with bounded, nondegenerate geometry.
#define main pipeline_standalone_main
#include "pipeline-probe.cpp"
#undef main

static const unsigned pressure_quads[] = {8192, 16384};
static const unsigned pressure_colours[] = {1, 2, 4, 6};

static int heap_state(int fd, unsigned cycle, unsigned phase)
{
    if (software) return 1;
    Snapshot value{};
    CHECK(snapshot(fd, &value));
    printf("ROCK5_PRESSURE_HEAP cycle=%u phase=%u clients=%u heaps=%u chunks=%u bytes=%llu\n",
        cycle, phase, value.buffers.globalClients, value.heaps.globalHeaps,
        value.heaps.globalChunks, (unsigned long long)value.heaps.globalBytes);
    CHECK(value.buffers.globalClients == 2 && value.heaps.globalHeaps == 1);
    CHECK(value.heaps.globalChunks >= 1 && value.heaps.globalChunks <= 32);
    CHECK(value.heaps.globalBytes == 4096ull + value.heaps.globalChunks * 262144ull);
    // Initial allocation is not evidence of firmware-requested growth.
    CHECK(phase == 0 ? value.heaps.globalChunks == 1 : value.heaps.globalChunks > 1);
    return 1;
}

static int pressure_readback(unsigned cycle, unsigned round, bigtime_t start)
{
    CHECK(glGetError() == GL_NO_ERROR);
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    CHECK(fence);
    GLenum wait = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
    CHECK(wait == GL_ALREADY_SIGNALED || wait == GL_CONDITION_SATISFIED);
    glDeleteSync(fence);
    unsigned char data[GUARD + PIXEL_BYTES + GUARD];
    unsigned char canary = 0xa0 + cycle * 2 + round;
    memset(data, canary, sizeof(data));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, data + GUARD);
    glFinish();
    bigtime_t elapsed = system_time() - start;
    CHECK(glGetError() == GL_NO_ERROR && elapsed >= 0);
    unsigned errors = 0, guards = 0;
    unsigned colour = pressure_colours[cycle * 2 + round];
    for (unsigned pixel = 0; pixel < WIDTH * HEIGHT; ++pixel) {
        for (unsigned c = 0; c < 4; ++c)
            errors += data[GUARD + pixel * 4 + c]
                != (c == 3 || (colour & (1u << c)) ? 255 : 0);
    }
    for (unsigned i = 0; i < GUARD; ++i)
        guards += data[i] != canary || data[GUARD + PIXEL_BYTES + i] != canary;
    printf("ROCK5_PRESSURE_PIXELS_BEGIN cycle=%u round=%u quads=%u width=%u height=%u format=RGBA8 origin=lower-left\n",
        cycle, round, pressure_quads[round], WIDTH, HEIGHT);
    printf("guard_before="); dump(data, GUARD); printf("\n");
    for (unsigned y = 0; y < HEIGHT; ++y) {
        printf("row=%03u ", y);
        dump(data + GUARD + y * WIDTH * 4, WIDTH * 4); printf("\n");
    }
    printf("guard_after="); dump(data + GUARD + PIXEL_BYTES, GUARD); printf("\n");
    printf("ROCK5_PRESSURE_PIXELS_END cycle=%u round=%u mismatches=%u guard_errors=%u wait=%04x render_us=%lld\n",
        cycle, round, errors, guards, wait, (long long)elapsed);
    CHECK(errors == 0 && guards == 0);
    return 1;
}

static int pressure_context(EGLDisplay display, unsigned cycle, int fd)
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
    printf("ROCK5_PRESSURE_GL cycle=%u renderer=%s version=%s\n", cycle,
        renderer ? renderer : "NULL", version ? version : "NULL");
    CHECK(renderer && !strcmp(renderer, software ? "softpipe" : "Mali-G610 (Panfrost)"));
    CHECK(version && !strncmp(version, "OpenGL ES 3.", 12) && strstr(version, "Mesa 25.3.6"));
    GLuint vertex = shader(GL_VERTEX_SHADER,
        "#version 300 es\nlayout(location=0) in vec2 point;\n"
        "void main(){gl_Position=vec4(point/vec2(97,67)*2.0-1.0,0,1);}\n");
    GLuint fragment = shader(GL_FRAGMENT_SHADER,
        "#version 300 es\nprecision highp float;\nuniform vec4 colour;\n"
        "layout(location=0) out vec4 pixel;\nvoid main(){pixel=colour;}\n");
    CHECK(vertex && fragment);
    GLuint object = glCreateProgram();
    glAttachShader(object, vertex); glAttachShader(object, fragment); glLinkProgram(object);
    GLint linked = 0;
    glGetProgramiv(object, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096];
        glGetProgramInfoLog(object, sizeof(log), NULL, log);
        fprintf(stderr, "ROCK5_PRESSURE_LINK_FAILURE %s\n", log);
    }
    glDeleteShader(vertex); glDeleteShader(fragment);
    CHECK(linked);
    glUseProgram(object);
    GLint colour_location = glGetUniformLocation(object, "colour");
    CHECK(colour_location >= 0);
    GLuint vao, vertex_buffer, texture, fbo;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    // Explicit coordinates avoid the software interpreter's split-draw
    // gl_VertexID limitation, retained with the first candidate's evidence.
    const unsigned vertices = pressure_quads[1] * 6;
    const size_t upload_bytes = vertices * 2 * sizeof(GLfloat);
    GLfloat* points = (GLfloat*)malloc(upload_bytes);
    CHECK(points != NULL);
    const unsigned corners[6][2] = {{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
    for (unsigned i = 0; i < vertices; ++i) {
        unsigned q = i / 6;
        points[i * 2] = q % WIDTH + corners[i % 6][0];
        points[i * 2 + 1] = (q / WIDTH) % HEIGHT + corners[i % 6][1];
    }
    glGenBuffers(1, &vertex_buffer); glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, upload_bytes, points, GL_STATIC_DRAW);
    free(points);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);
    CHECK(glGetError() == GL_NO_ERROR);
    printf("ROCK5_PRESSURE_VERTICES cycle=%u vertices=%u bytes=%zu explicit_buffer=1\n",
        cycle, vertices, upload_bytes);

    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, WIDTH, HEIGHT);
    texture_parameters();
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glViewport(0, 0, WIDTH, HEIGHT);
    glDisable(GL_DITHER); glDisable(GL_CULL_FACE); glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); glFinish();
    CHECK(glGetError() == GL_NO_ERROR && heap_state(fd, cycle, 0));
    for (unsigned round = 0; round < 2; ++round) {
        unsigned colour = pressure_colours[cycle * 2 + round];
        glUniform4f(colour_location, colour & 1 ? 1 : 0,
            colour & 2 ? 1 : 0, colour & 4 ? 1 : 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        bigtime_t start = system_time();
        glDrawArrays(GL_TRIANGLES, 0, pressure_quads[round] * 6);
        CHECK(pressure_readback(cycle, round, start));
        CHECK(heap_state(fd, cycle, round + 1));
    }
    glUseProgram(0); glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &texture);
    glDeleteBuffers(1, &vertex_buffer);
    glDeleteVertexArrays(1, &vao); glDeleteProgram(object);
    CHECK(glGetError() == GL_NO_ERROR);
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    CHECK(eglDestroySurface(display, surface)); CHECK(eglDestroyContext(display, context));
    CHECK(eglTerminate(display)); CHECK(eglReleaseThread());
    printf("ROCK5_PRESSURE_CONTEXT_PASS cycle=%u rounds=2 destroyed=1\n", cycle);
    return 1;
}

static int pressure_run(const char* mode)
{
    software = !strcmp(mode, "--software");
    CHECK(software || !strcmp(mode, "--native"));
    CHECK(unsetenv("MESA_GL_VERSION_OVERRIDE") == 0);
    CHECK(unsetenv("MESA_GLES_VERSION_OVERRIDE") == 0);
    CHECK(unsetenv("MESA_LOADER_DRIVER_OVERRIDE") == 0);
    CHECK(setenv("pan_csf_chunk_size", "262144", 1) == 0);
    CHECK(setenv("pan_csf_initial_chunks", "1", 1) == 0);
    CHECK(setenv("pan_csf_max_chunks", "32", 1) == 0);
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
    printf("ROCK5_PRESSURE_READY version=1 mode=%s chunk_size=262144 initial_chunks=1 max_chunks=32\n", mode);
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
        CHECK(display != EGL_NO_DISPLAY && pressure_context(display, cycle, fd));
        if (!software) CHECK(snapshot(fd, &after) && same_snapshot(before, after, cycle));
    }
    if (fd >= 0) CHECK(close(fd) == 0);
    printf("ROCK5_PRESSURE_PASS contexts=2 frames=4 pixels=25996 guard_bytes=512 software=%u\n", software);
    return 1;
}

int main(int argc, char** argv)
{
    setbuf(stdout, NULL); setbuf(stderr, NULL);
    return argc == 2 && pressure_run(argv[1]) ? 0 : 1;
}
