// Reuse the pinned shader, texture and complete-pixel oracle helpers.
#define main pipeline_standalone_main
#include "pipeline-probe.cpp"
#undef main
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

// Synchronized applications; timestamps exclude all barrier waits and logging.
static const unsigned pressure_quads[] = {8192, 16384};
static const unsigned pressure_colours[] = {1, 2, 4, 6};
static unsigned rounds() { return software ? 8 : 64; }
enum { READY_MAGIC = 0x434f4e43 };
struct Ready {
    uint32_t magic, child, frame, reserved;
    bigtime_t start, end;
};
struct Command {
    uint32_t operation, frame;
    bigtime_t start_at;
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
    CHECK(unsetenv("PAN_MESA_DEBUG") == 0);
    return 1;
}

static int worker(const char* mode, unsigned index, int control, int ready)
{
    CHECK(configure(mode));
    auto get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    CHECK(get_display);
    EGLDisplay display = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    CHECK(display != EGL_NO_DISPLAY);
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
    printf("ROCK5_CONCURRENT" "_GL child=%u renderer=%s version=%s\n", index,
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
        fprintf(stderr, "ROCK5_CONCURRENT" "_LINK_FAILURE %s\n", log);
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
    printf("ROCK5_CONCURRENT" "_VERTICES child=%u vertices=%u bytes=%zu explicit_buffer=1\n",
        index, vertices, upload_bytes);

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
    CHECK(glGetError() == GL_NO_ERROR);
    Ready initialized = {READY_MAGIC, index, UINT32_MAX, 0, 0, 0};
    CHECK(write(ready, &initialized, sizeof(initialized)) == sizeof(initialized));
    unsigned frame = 0;
    for (;;) {
        pollfd watched = {control, POLLIN, 0};
        int result;
        do { result = poll(&watched, 1, 30000); } while (result < 0 && errno == EINTR);
        CHECK(result == 1 && (watched.revents & POLLIN));
        Command command{};
        CHECK(read(control, &command, sizeof(command)) == sizeof(command));
        CHECK(command.frame == frame);
        if (command.operation == 'x') break;
        CHECK(command.operation == 'r' && frame <= rounds());
        bigtime_t now = system_time();
        CHECK(command.start_at > 0 && command.start_at - now < 1000000);
        if (command.start_at > now)
            CHECK(snooze_until(command.start_at, B_SYSTEM_TIMEBASE) == B_OK);
        unsigned colour = pressure_colours[(index + frame) % 4];
        unsigned quads = pressure_quads[(index + frame) % 2];
        bigtime_t start = system_time();
        glUniform4f(colour_location, colour & 1 ? 1 : 0,
            colour & 2 ? 1 : 0, colour & 4 ? 1 : 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, quads * 6);
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        CHECK(fence);
        GLenum wait = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
        CHECK(wait == GL_ALREADY_SIGNALED || wait == GL_CONDITION_SATISFIED);
        glDeleteSync(fence);
        unsigned char data[GUARD + PIXEL_BYTES + GUARD];
        unsigned char canary = 0x40 + (index * 53 + frame) % 191;
        memset(data, canary, sizeof(data));
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, data + GUARD);
        glFinish();
        bigtime_t end = system_time();
        CHECK(glGetError() == GL_NO_ERROR && end > start && start >= command.start_at);
        unsigned errors = 0, guards = 0;
        for (unsigned pixel = 0; pixel < WIDTH * HEIGHT; ++pixel)
            for (unsigned channel = 0; channel < 4; ++channel)
                errors += data[GUARD + pixel * 4 + channel]
                    != (channel == 3 || (colour & (1u << channel)) ? 255 : 0);
        for (unsigned i = 0; i < GUARD; ++i)
            guards += data[i] != canary || data[GUARD + PIXEL_BYTES + i] != canary;
        printf("ROCK5_CONCURRENT_FRAME child=%u frame=%u quads=%u start_at=%lld start=%lld end=%lld wait=%04x mismatches=%u guard_errors=%u guard_before=",
            index, frame, quads, (long long)command.start_at, (long long)start,
            (long long)end, wait, errors, guards);
        dump(data, GUARD); printf(" guard_after=");
        dump(data + GUARD + PIXEL_BYTES, GUARD); printf(" rgba_runs=");
        for (unsigned first = 0; first < WIDTH * HEIGHT;) {
            unsigned last = first + 1;
            while (last < WIDTH * HEIGHT
                && !memcmp(data + GUARD + first * 4, data + GUARD + last * 4, 4)) ++last;
            if (first) printf(",");
            printf("%u:", last - first); dump(data + GUARD + first * 4, 4);
            first = last;
        }
        printf("\n");
        CHECK(errors == 0 && guards == 0);
        Ready message = {READY_MAGIC, index, frame, 0, start, end};
        CHECK(write(ready, &message, sizeof(message)) == sizeof(message));
        ++frame;
    }
    glUseProgram(0); glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &texture);
    glDeleteBuffers(1, &vertex_buffer);
    glDeleteVertexArrays(1, &vao); glDeleteProgram(object);
    CHECK(glGetError() == GL_NO_ERROR);
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    CHECK(eglDestroySurface(display, surface)); CHECK(eglDestroyContext(display, context));
    CHECK(eglTerminate(display)); CHECK(eglReleaseThread());
    CHECK(close(control) == 0 && close(ready) == 0);
    printf("ROCK5_CONCURRENT_WORKER_PASS child=%u frames=%u destroyed=1\n", index, frame);
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
                printf("ROCK5_CONCURRENT_ABORT_LOG_BEGIN path=%s\n", log);
                char bytes[4096];
                size_t count, total = 0;
                while ((count = fread(bytes, 1, sizeof(bytes), input)) != 0) {
                    if ((total += count) > 4 * 1024 * 1024) break;
                    fwrite(bytes, 1, count, stdout);
                }
                fclose(input);
                printf("\nROCK5_CONCURRENT_ABORT_LOG_END\n");
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
    snprintf(child.log, sizeof(child.log), "/tmp/rock5-mali-concurrent-%ld-%u.log", (long)getpid(), index);
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
    printf("ROCK5_CONCURRENT_STARTED child=%u pid=%ld\n", index, (long)pid);
    return 1;
}

static int receive(Child& child, unsigned index, unsigned frame, Ready* value)
{
    pollfd watched = {child.ready, POLLIN, 0};
    int result;
    do { result = poll(&watched, 1, 30000); } while (result < 0 && errno == EINTR);
    CHECK(result == 1 && (watched.revents & POLLIN));
    CHECK(read(child.ready, value, sizeof(*value)) == sizeof(*value));
    CHECK(value->magic == READY_MAGIC && value->child == index
        && value->frame == frame && value->reserved == 0);
    if (frame == UINT32_MAX) CHECK(value->start == 0 && value->end == 0);
    else CHECK(value->start > 0 && value->end > value->start);
    printf("ROCK5_CONCURRENT_READY child=%u frame=%u start=%lld end=%lld\n",
        index, frame, (long long)value->start, (long long)value->end);
    return 1;
}

static int send(Child& child, unsigned operation, unsigned frame, bigtime_t start_at = 0)
{
    Command command = {operation, frame, start_at};
    CHECK(write(child.control, &command, sizeof(command)) == sizeof(command));
    return 1;
}

static int finish(Child& child, unsigned index, bool killed)
{
    int status = 0;
    pid_t pid;
    bigtime_t deadline = system_time() + 10000000;
    do {
        pid = waitpid(child.pid, &status, WNOHANG);
        if (pid == child.pid) break;
        CHECK(pid == 0 || (pid < 0 && errno == EINTR));
        CHECK(system_time() < deadline);
        snooze(1000);
    } while (true);
    CHECK(pid == child.pid);
    child.pid = -1;
    CHECK(killed ? WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL
        : WIFEXITED(status) && WEXITSTATUS(status) == 0);
    printf("ROCK5_CONCURRENT_EXIT child=%u killed=%u signal=%d exit=%d\n", index, killed,
        WIFSIGNALED(status) ? WTERMSIG(status) : 0, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    CHECK(close(child.control) == 0 && close(child.ready) == 0);
    child.control = child.ready = -1;
    FILE* log = fopen(child.log, "rb"); CHECK(log);
    printf("ROCK5_CONCURRENT_LOG_BEGIN child=%u\n", index);
    char buffer[4096];
    size_t bytes = 0, total = 0;
    while ((bytes = fread(buffer, 1, sizeof(buffer), log)) != 0) {
        total += bytes; CHECK(total <= 4 * 1024 * 1024);
        CHECK(fwrite(buffer, 1, bytes, stdout) == bytes);
    }
    CHECK(!ferror(log) && fclose(log) == 0);
    printf("\nROCK5_CONCURRENT_LOG_END child=%u bytes=%zu\n", index, total);
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
    CHECK(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    printf("ROCK5_CONCURRENT_PARENT_READY version=1 mode=%s rounds=%u required_overlap=%u\n",
        mode, rounds(), software ? 0 : 32);
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
    Ready values[2]{};
    CHECK(start(children[0], self, mode, 0) && receive(children[0], 0, UINT32_MAX, &values[0]));
    CHECK(live_snapshot(fd, before, 1, 10));
    CHECK(start(children[1], self, mode, 1) && receive(children[1], 1, UINT32_MAX, &values[1]));
    CHECK(live_snapshot(fd, before, 2, 11));
    unsigned overlaps = 0;
    bigtime_t overlap_total = 0;
    for (unsigned frame = 0; frame < rounds(); ++frame) {
        bigtime_t start_at = system_time() + 10000;
        CHECK(send(children[0], 'r', frame, start_at) && send(children[1], 'r', frame, start_at));
        CHECK(receive(children[0], 0, frame, &values[0]) && receive(children[1], 1, frame, &values[1]));
        bigtime_t begin = values[0].start > values[1].start ? values[0].start : values[1].start;
        bigtime_t end = values[0].end < values[1].end ? values[0].end : values[1].end;
        bigtime_t overlap = end > begin ? end - begin : 0;
        CHECK(values[0].start >= start_at && values[1].start >= start_at);
        overlaps += overlap > 0;
        overlap_total += overlap;
        printf("ROCK5_CONCURRENT_ROUND frame=%u start_at=%lld overlap_us=%lld\n",
            frame, (long long)start_at, (long long)overlap);
    }
    CHECK(software || overlaps >= 32);
    CHECK(live_snapshot(fd, before, 2, 12));
    CHECK(send(children[1], 'x', rounds()) && finish(children[1], 1, false));
    CHECK(live_snapshot(fd, before, 1, 13));
    CHECK(send(children[0], 'r', rounds(), system_time()+10000)
        && receive(children[0], 0, rounds(), &values[0]));
    printf("ROCK5_CONCURRENT_SURVIVOR_PASS child=0 same_context=1\n");
    CHECK(send(children[0], 'x', rounds()+1) && finish(children[0], 0, false));
    if (!software) CHECK(snapshot(fd, &after) && same_snapshot(before, after, 0));
    CHECK(start(children[2], self, mode, 2) && receive(children[2], 2, UINT32_MAX, &values[0]));
    CHECK(live_snapshot(fd, before, 1, 14));
    CHECK(send(children[2], 'r', 0, system_time()+10000) && receive(children[2], 2, 0, &values[0]));
    CHECK(send(children[2], 'x', 1) && finish(children[2], 2, false));
    if (!software) CHECK(snapshot(fd, &after) && same_snapshot(before, after, 1));
    if (fd >= 0) CHECK(close(fd) == 0);
    printf("ROCK5_CONCURRENT_PASS processes=3 normal=3 frames=%u pixels=%u guard_bytes=%u overlapping_rounds=%u overlap_us=%lld software=%u\n",
        2*rounds()+2, (2*rounds()+2)*WIDTH*HEIGHT, (2*rounds()+2)*GUARD*2,
        overlaps, (long long)overlap_total, software);
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
