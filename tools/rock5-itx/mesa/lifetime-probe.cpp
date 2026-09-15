// Reuse the pinned shader, texture and complete-pixel oracle helpers.
#define main pipeline_standalone_main
#include "pipeline-probe.cpp"
#undef main
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

enum { READY_MAGIC = 0x4c495645 };
struct Ready {
    uint32_t magic, child, phase, pattern;
};

static int configure(const char* mode)
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
    return 1;
}

static int worker(const char* mode, unsigned index, int control, int ready)
{
    CHECK(configure(mode));
    auto get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    CHECK(get_display);
    EGLDisplay display = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    CHECK(display != EGL_NO_DISPLAY && eglInitialize(display, NULL, NULL));
    CHECK(eglBindAPI(EGL_OPENGL_ES_API));
    const EGLint attributes[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT, EGL_RED_SIZE,8, EGL_GREEN_SIZE,8,
        EGL_BLUE_SIZE,8, EGL_ALPHA_SIZE,8, EGL_SAMPLE_BUFFERS,0, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    CHECK(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1);
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION,3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    CHECK(context != EGL_NO_CONTEXT);
    const EGLint surface_attributes[] = {EGL_WIDTH,WIDTH, EGL_HEIGHT,HEIGHT, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attributes);
    CHECK(surface != EGL_NO_SURFACE && eglMakeCurrent(display, surface, surface, context));
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    const char* version = (const char*)glGetString(GL_VERSION);
    printf("ROCK5_LIFETIME_CONTEXT child=%u renderer=%s version=%s\n", index,
        renderer ? renderer : "NULL", version ? version : "NULL");
    CHECK(renderer && !strcmp(renderer, software ? "softpipe" : "Mali-G610 (Panfrost)"));
    CHECK(version && strncmp(version, "OpenGL ES 3.", 12) == 0 && strstr(version, "Mesa 25.3.6"));
    GLuint sampled = program("#version 300 es\nprecision highp float;\n"
        "uniform highp sampler2D source_image;\nuniform bool reflected;\n"
        "layout(location=0) out vec4 pixel;\nvoid main(){"
        "vec2 uv=gl_FragCoord.xy/vec2(97.0,67.0);"
        "if(reflected) uv=vec2(1.0)-uv; pixel=texture(source_image,uv);}\n");
    CHECK(sampled);
    GLuint vao, vertex_buffer, texture;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vertex_buffer); glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, WIDTH, HEIGHT);
    texture_parameters();
    glViewport(0, 0, WIDTH, HEIGHT);
    glDisable(GL_DITHER); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
    unsigned phase = 0;
    for (;;) {
        unsigned pattern = (index + phase) % 2;
        unsigned char pixels[PIXEL_BYTES];
        for (unsigned y = 0; y < HEIGHT; ++y) {
            for (unsigned x = 0; x < WIDTH; ++x) {
                unsigned colour = expected(pattern, 0, x, y);
                for (unsigned c = 0; c < 4; ++c)
                    pixels[(y * WIDTH + x) * 4 + c]
                        = c == 3 || (colour & (1u << c)) ? 255 : 0;
            }
        }
        printf("ROCK5_LIFETIME_DRAW child=%u phase=%u pattern=%u\n", index, phase, pattern);
        glBindTexture(GL_TEXTURE_2D, texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WIDTH, HEIGHT,
            GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        sampling(sampled, texture, false);
        CHECK(readback(pattern, 0));
        glFinish(); CHECK(glGetError() == GL_NO_ERROR);
        printf("ROCK5_LIFETIME_LIVE child=%u phase=%u gpu_finished=1 resources_live=1\n", index, phase);
        Ready message = {READY_MAGIC, index, phase, pattern};
        CHECK(write(ready, &message, sizeof(message)) == sizeof(message));
        char command = 0;
        ssize_t bytes;
        do { bytes = read(control, &command, 1); } while (bytes < 0 && errno == EINTR);
        CHECK(bytes == 1);
        if (command == 'x') break;
        CHECK(command == 'r' && index == 0 && phase == 0);
        phase++;
    }
    glUseProgram(0); glDeleteTextures(1, &texture);
    glDeleteBuffers(1, &vertex_buffer); glDeleteVertexArrays(1, &vao);
    glDeleteProgram(sampled); CHECK(glGetError() == GL_NO_ERROR);
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    CHECK(eglDestroySurface(display, surface)); CHECK(eglDestroyContext(display, context));
    CHECK(eglTerminate(display)); CHECK(eglReleaseThread());
    CHECK(close(control) == 0 && close(ready) == 0);
    printf("ROCK5_LIFETIME_WORKER_PASS child=%u draws=%u destroyed=1\n", index, phase + 1);
    return 1;
}

struct Child {
    pid_t pid = -1;
    int control = -1, ready = -1;
    char log[160] = {};
    bool dumped = false;
    ~Child()
    {
        if (pid > 0) {
            kill(pid, SIGKILL);
            int status;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
        if (control >= 0) close(control);
        if (ready >= 0) close(ready);
        if (!dumped && log[0]) {
            FILE* input = fopen(log, "rb");
            if (input) {
                printf("ROCK5_LIFETIME_ABORT_LOG_BEGIN path=%s\n", log);
                char bytes[4096];
                size_t count, total = 0;
                while ((count = fread(bytes, 1, sizeof(bytes), input)) != 0) {
                    if ((total += count) > 4 * 1024 * 1024) break;
                    fwrite(bytes, 1, count, stdout);
                }
                fclose(input);
                printf("\nROCK5_LIFETIME_ABORT_LOG_END\n");
            }
        }
    }
};

static int start(Child& child, const char* self, const char* mode, unsigned index)
{
    int command[2] = {-1,-1}, report[2] = {-1,-1};
    CHECK(pipe(command) == 0 && pipe(report) == 0);
    for (int fd : {command[0], command[1], report[0], report[1]})
        CHECK(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
    snprintf(child.log, sizeof(child.log), "/tmp/rock5-mali-lifetime-%ld-%u.log", (long)getpid(), index);
    int output = open(child.log, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(output >= 0);
    pid_t pid = fork(); CHECK(pid >= 0);
    if (pid == 0) {
        close(command[1]); close(report[0]);
        if (dup2(output, STDOUT_FILENO) < 0 || dup2(output, STDERR_FILENO) < 0
            || fcntl(command[0], F_SETFD, 0) < 0 || fcntl(report[1], F_SETFD, 0) < 0)
            _exit(125);
        close(output);
        char index_text[16], control_text[16], ready_text[16];
        snprintf(index_text, sizeof(index_text), "%u", index);
        snprintf(control_text, sizeof(control_text), "%d", command[0]);
        snprintf(ready_text, sizeof(ready_text), "%d", report[1]);
        execl(self, self, "--worker", mode, index_text, control_text, ready_text, (char*)NULL);
        _exit(126);
    }
    child.pid = pid; child.control = command[1]; child.ready = report[0];
    CHECK(close(output) == 0 && close(command[0]) == 0 && close(report[1]) == 0);
    printf("ROCK5_LIFETIME_STARTED child=%u pid=%ld\n", index, (long)pid);
    return 1;
}

static int receive_ready(Child& child, unsigned index, unsigned phase)
{
    pollfd watched = {child.ready, POLLIN, 0};
    int result;
    do { result = poll(&watched, 1, 30000); } while (result < 0 && errno == EINTR);
    CHECK(result == 1 && (watched.revents & POLLIN));
    Ready ready = {};
    ssize_t bytes;
    do { bytes = read(child.ready, &ready, sizeof(ready)); } while (bytes < 0 && errno == EINTR);
    CHECK(bytes == sizeof(ready) && ready.magic == READY_MAGIC && ready.child == index
        && ready.phase == phase && ready.pattern == (index + phase) % 2);
    printf("ROCK5_LIFETIME_READY child=%u phase=%u pattern=%u\n", index, phase, ready.pattern);
    return 1;
}

static int finish(Child& child, unsigned index, bool killed)
{
    int status = 0;
    pid_t pid;
    do { pid = waitpid(child.pid, &status, 0); } while (pid < 0 && errno == EINTR);
    CHECK(pid == child.pid);
    child.pid = -1;
    CHECK(killed ? WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL
        : WIFEXITED(status) && WEXITSTATUS(status) == 0);
    printf("ROCK5_LIFETIME_EXIT child=%u killed=%u signal=%d exit=%d\n", index, killed,
        WIFSIGNALED(status) ? WTERMSIG(status) : 0, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    CHECK(close(child.control) == 0 && close(child.ready) == 0);
    child.control = child.ready = -1;
    FILE* log = fopen(child.log, "rb"); CHECK(log);
    printf("ROCK5_LIFETIME_LOG_BEGIN child=%u\n", index);
    char buffer[4096];
    size_t bytes = 0, total = 0;
    while ((bytes = fread(buffer, 1, sizeof(buffer), log)) != 0) {
        total += bytes; CHECK(total <= 4 * 1024 * 1024);
        CHECK(fwrite(buffer, 1, bytes, stdout) == bytes);
    }
    CHECK(!ferror(log) && fclose(log) == 0);
    printf("\nROCK5_LIFETIME_LOG_END child=%u bytes=%zu\n", index, total);
    CHECK(unlink(child.log) == 0);
    child.dumped = true;
    return 1;
}

static int live_snapshot(int fd, const Snapshot& baseline, unsigned clients, unsigned stage)
{
    if (software) return 1;
    Snapshot live;
    CHECK(snapshot(fd, &live));
    CHECK(live.buffers.globalClients == baseline.buffers.globalClients + clients);
    CHECK(live.sync.globalClients == baseline.sync.globalClients + clients);
    CHECK(live.buffers.globalBuffers > 0 && live.buffers.globalBufferBytes > 0
        && live.vms.globalVms >= clients && live.heaps.globalHeaps >= clients
        && live.sync.globalObjects > 0 && live.kernel_areas > 0 && live.user_maps == 0);
    CHECK(same_snapshot(live, live, stage));
    return 1;
}

static int parent(const char* self, const char* mode)
{
    CHECK(configure(mode));
    printf("ROCK5_LIFETIME_PARENT_READY version=1 mode=%s\n", mode);
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
    Child children[3];
    CHECK(start(children[0], self, mode, 0) && receive_ready(children[0], 0, 0));
    CHECK(live_snapshot(fd, before, 1, 10));
    CHECK(start(children[1], self, mode, 1) && receive_ready(children[1], 1, 0));
    CHECK(live_snapshot(fd, before, 2, 11));
    CHECK(kill(children[1].pid, SIGKILL) == 0 && finish(children[1], 1, true));
    CHECK(live_snapshot(fd, before, 1, 12));
    CHECK(write(children[0].control, "r", 1) == 1 && receive_ready(children[0], 0, 1));
    printf("ROCK5_LIFETIME_SURVIVOR_PASS child=0 new_pattern=1 same_context=1\n");
    CHECK(write(children[0].control, "x", 1) == 1 && finish(children[0], 0, false));
    if (!software) CHECK(snapshot(fd, &after) && same_snapshot(before, after, 0));
    CHECK(start(children[2], self, mode, 2) && receive_ready(children[2], 2, 0));
    CHECK(live_snapshot(fd, before, 1, 13));
    CHECK(write(children[2].control, "x", 1) == 1 && finish(children[2], 2, false));
    if (!software) CHECK(snapshot(fd, &after) && same_snapshot(before, after, 1));
    if (fd >= 0) CHECK(close(fd) == 0);
    printf("ROCK5_LIFETIME_PASS processes=3 normal=2 killed=1 frames=4 pixels=25996 guard_bytes=512 software=%u\n", software);
    return 1;
}

static int number(const char* text)
{
    char* end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    return errno == 0 && end != text && *end == '\0' && value >= 0 && value <= INT_MAX
        ? int(value) : -1;
}

int main(int argc, char** argv)
{
    setbuf(stdout, NULL); setbuf(stderr, NULL);
    if (argc == 6 && !strcmp(argv[1], "--worker")) {
        int index = number(argv[3]), control = number(argv[4]), ready = number(argv[5]);
        return index >= 0 && index <= 2 && control > 2 && ready > 2
            && worker(argv[2], unsigned(index), control, ready) ? 0 : 1;
    }
    return argc == 2 && parent(argv[0], argv[1]) ? 0 : 1;
}
