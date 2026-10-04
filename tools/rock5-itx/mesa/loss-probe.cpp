// Two live shared GLES contexts observe native runtime reset and retire.
#define main queue_standalone_main
#include "../mali_queue_probe.cpp"
#undef main
#include "native-observer.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

enum { WIDTH = 97, HEIGHT = 67, GUARD = 64, PIXELS = WIDTH * HEIGHT,
    PIXEL_BYTES = PIXELS * 4, READ_BYTES = PIXEL_BYTES + GUARD * 2 };
static bool software;
static PFNGLGETGRAPHICSRESETSTATUSKHRPROC reset_status;

static void Semaphore(sem_id semaphore)
{
    CHECK(acquire_sem_etc(semaphore, 1, B_RELATIVE_TIMEOUT, 10000000) == B_OK);
}

static GLuint Shader(GLenum kind, const char* source)
{
    GLuint value = glCreateShader(kind);
    CHECK(value);
    glShaderSource(value, 1, &source, NULL); glCompileShader(value);
    GLint okay = 0; glGetShaderiv(value, GL_COMPILE_STATUS, &okay);
    if (!okay) {
        char log[4096]; glGetShaderInfoLog(value, sizeof(log), NULL, log);
        fprintf(stderr, "ROCK5_LOSS_SHADER_ERROR %s\n", log);
    }
    CHECK(okay); return value;
}

static GLuint Program()
{
    GLuint vertex = Shader(GL_VERTEX_SHADER,
        "#version 300 es\nlayout(location=0) in vec2 point;\n"
        "void main(){gl_Position=vec4(point,0,1);}\n");
    GLuint fragment = Shader(GL_FRAGMENT_SHADER,
        "#version 300 es\nprecision highp float;\nprecision highp int;\n"
        "uniform uint seed;\nlayout(location=0) out vec4 pixel;\n"
        "void main(){uvec2 p=uvec2(gl_FragCoord.xy);"
        "uint v=((p.x/7u)^(p.y/5u)^seed)&7u;"
        "pixel=vec4(float(v&1u),float((v>>1u)&1u),float((v>>2u)&1u),1);}\n");
    GLuint program = glCreateProgram(); CHECK(program);
    glAttachShader(program, vertex); glAttachShader(program, fragment);
    glLinkProgram(program); GLint okay = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &okay); CHECK(okay);
    glDeleteShader(vertex); glDeleteShader(fragment); return program;
}

static void Dump(unsigned child, const char* kind, unsigned seed,
    const unsigned char* bytes, unsigned char canary)
{
    printf("ROCK5_LOSS_PIXELS_BEGIN child=%u kind=%s seed=%u width=97 height=67 canary=%02x\n",
        child, kind, seed, canary);
    printf("guard_before=");
    for (unsigned i = 0; i < GUARD; i++) printf("%02x", bytes[i]);
    printf("\n");
    for (unsigned y = 0; y < HEIGHT; y++) {
        printf("row=%03u ", y);
        for (unsigned x = 0; x < WIDTH * 4; x++)
            printf("%02x", bytes[GUARD + y * WIDTH * 4 + x]);
        printf("\n");
    }
    printf("guard_after=");
    for (unsigned i = 0; i < GUARD; i++) printf("%02x", bytes[GUARD + PIXEL_BYTES + i]);
    printf("\nROCK5_LOSS_PIXELS_END child=%u kind=%s\n", child, kind);
}

struct LiveContext {
    unsigned child;
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    GLuint program, vertices;
    sem_id start, ready, finish, done;
    thread_id thread;
};

static int32 Worker(void* argument)
{
    LiveContext& live = *(LiveContext*)argument;
    Semaphore(live.start);
    CHECK(eglMakeCurrent(live.display, live.surface, live.surface, live.context));
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    CHECK(renderer && strcmp(renderer, software ? "softpipe" : "Mali-G610 (Panfrost)") == 0);
    CHECK(glIsProgram(live.program) && glIsBuffer(live.vertices));
    GLint strategy = 0;
    if (!software) {
        glGetIntegerv(GL_RESET_NOTIFICATION_STRATEGY_KHR, &strategy);
        CHECK(strategy == GL_LOSE_CONTEXT_ON_RESET_KHR);
        CHECK(reset_status() == GL_NO_ERROR);
    }
    printf("ROCK5_LOSS_CONTEXT child=%u shared=1 strategy=%04x renderer=%s\n",
        live.child, strategy, renderer);
    GLuint vao, texture, framebuffer;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, live.vertices);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, WIDTH, HEIGHT);
    glGenFramebuffers(1, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glViewport(0, 0, WIDTH, HEIGHT); glDisable(GL_DITHER);
    glUseProgram(live.program);
    unsigned seed = live.child ? 6 : 1;
    GLint location = glGetUniformLocation(live.program, "seed"); CHECK(location >= 0);
    glUniform1ui(location, seed); glDrawArrays(GL_TRIANGLES, 0, 3);
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0); CHECK(fence);
    GLenum waited = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
    CHECK(waited == GL_ALREADY_SIGNALED || waited == GL_CONDITION_SATISFIED);
    unsigned char bytes[READ_BYTES];
    unsigned char canary = 0xa0 + live.child;
    memset(bytes, canary, sizeof(bytes));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, bytes + GUARD);
    CHECK(glGetError() == GL_NO_ERROR);
    unsigned errors = 0;
    for (unsigned y = 0; y < HEIGHT; y++) for (unsigned x = 0; x < WIDTH; x++) {
        unsigned colour = ((x / 7) ^ (y / 5) ^ seed) & 7;
        for (unsigned c = 0; c < 4; c++)
            errors += bytes[GUARD + (y * WIDTH + x) * 4 + c]
                != (c == 3 || (colour & (1u << c)) ? 255 : 0);
    }
    for (unsigned i = 0; i < GUARD; i++)
        errors += bytes[i] != canary || bytes[GUARD + PIXEL_BYTES + i] != canary;
    Dump(live.child, "before", seed, bytes, canary); CHECK(errors == 0);
    printf("ROCK5_LOSS_READY child=%u pixels=6499 guards=128\n", live.child);
    CHECK(release_sem(live.ready) == B_OK);
    Semaphore(live.finish);
    if (!software) {
        bigtime_t start = system_time();
        GLenum first = reset_status(), second = reset_status();
        printf("ROCK5_LOSS_STATUS child=%u first=%04x second=%04x\n",
            live.child, first, second);
        CHECK(first == GL_UNKNOWN_CONTEXT_RESET_KHR && second == GL_NO_ERROR);
        canary = 0xd0 + live.child; memset(bytes, canary, sizeof(bytes));
        glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, bytes + GUARD);
        GLenum readError = glGetError(); CHECK(readError == GL_CONTEXT_LOST_KHR);
        errors = 0;
        for (unsigned i = 0; i < sizeof(bytes); i++) errors += bytes[i] != canary;
        Dump(live.child, "ignored", seed, bytes, canary); CHECK(errors == 0);
        glClear(GL_COLOR_BUFFER_BIT);
        GLenum clearError = glGetError(); CHECK(clearError == GL_CONTEXT_LOST_KHR);
        GLint signaled = -1;
        glGetSynciv(fence, GL_SYNC_STATUS, 1, NULL, &signaled);
        GLenum syncError = glGetError();
        CHECK(signaled == GL_SIGNALED && syncError == GL_CONTEXT_LOST_KHR);
        CHECK(system_time() - start < 5000000);
        printf("ROCK5_LOSS_NOTIFIED child=%u first=%04x second=%04x read_error=%04x clear_error=%04x sync_error=%04x signaled=%04x unchanged=%u\n",
            live.child, first, second, readError, clearError, syncError, signaled, unsigned(sizeof(bytes)));
    } else {
        glDeleteSync(fence); glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &texture); glDeleteVertexArrays(1, &vao);
        CHECK(glGetError() == GL_NO_ERROR);
    }
    CHECK(eglMakeCurrent(live.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    CHECK(release_sem(live.done) == B_OK);
    return B_OK;
}

static QueueResetInfo ResetInfo(int fd, uint32 handle)
{
    QueueResetInfo info{}; info.version = 1; info.handle = handle;
    CHECK(Call(fd, kGetQueueResetInfo, info) == 0);
    return info;
}

static void Fault()
{
    int fd = open(kDevice, O_RDWR | O_CLOEXEC); CHECK(fd >= 0);
    Context raw(fd, 0x84573921, NULL); raw.Store(0, true);
    auto healthy = ResetInfo(fd, raw.queue);
    CHECK(healthy.state == kQueueResetNone && healthy.error == B_OK);
    printf("ROCK5_LOSS_RESET_READY handle=%u state=0 error=0\n", raw.queue);
    Buffer nops; nops.Allocate(fd, 1024 * 1024); memset(nops.Data(), 0, 1024 * 1024);
    const uint64_t address = UINT64_C(0x130000000);
    raw.generation = Bind(fd, raw.vm, {{kVmMapOperation, kVmNoExecute, address,
        1024 * 1024, nops.handle, 0, 0}});
    const uint64_t instruction = UINT64_C(0xff00000000000000);
    memcpy(raw.stream.Data(), &instruction, sizeof(instruction));
    uint32_t output = MakeSync(fd);
    for (unsigned i = 0; i < 32; i++) raw.Submit(address, 1024 * 1024);
    uint64_t sequence = SubmitSync(raw, kStreamAddress, sizeof(instruction), {}, {{output, 0, 1}});
    int fence = ExportSync(fd, output, true, 1);
    printf("ROCK5_LOSS_ARMED handle=%u sequence=%" B_PRIu64 " instruction=%016" B_PRIx64 "\n",
        raw.queue, sequence, instruction);
    bool pending = false;
    bigtime_t deadline = system_time() + 10000000;
    QueueResetInfo state{};
    do {
        state = ResetInfo(fd, raw.queue);
        if (state.state == kQueueResetPending && !pending) {
            printf("ROCK5_LOSS_RESET_PENDING handle=%u state=%u error=%" B_PRId32 "\n",
                raw.queue, state.state, state.error);
            pending = true;
        }
        if (state.state == kQueueResetQuiescent) break;
        CHECK(state.state == kQueueResetNone || state.state == kQueueResetPending);
        CHECK(state.error == (state.state == kQueueResetNone ? B_OK : B_IO_ERROR));
        snooze(500);
    } while (system_time() < deadline);
    CHECK(state.state == kQueueResetQuiescent && state.error == B_IO_ERROR);
    status_t result = B_ERROR;
    CHECK(WaitSyncFd(fence, 1000000, &result) == 0 && result == B_IO_ERROR);
    printf("ROCK5_LOSS_FENCE result=%" B_PRId32 "\n", result);
    auto info = Info(fd, raw.queue);
    CHECK(info.state == kQueueFailed && info.error == B_IO_ERROR && info.pending == 1
        && info.submitted == sequence && info.completed == sequence - 1 && info.failedSequence == sequence);
    printf("ROCK5_LOSS_FAULT handle=%u submitted=%" B_PRIu64 " completed=%" B_PRIu64
        " failed=%" B_PRIu64 " pending=%u reset=%u error=%" B_PRId32 " pending_observed=%u\n",
        raw.queue, info.submitted, info.completed, info.failedSequence, info.pending,
        state.state, state.error, pending);
    CHECK(close(fence) == 0); DropSync(fd, output);
    nops.Drop(fd); nops.Unmap(); raw.Finish(); CHECK(close(fd) == 0);
    printf("ROCK5_LOSS_RAW_CLOSED handle=%u\n", raw.queue);
}

int main(int argc, char** argv)
{
    setbuf(stdout, NULL); setbuf(stderr, NULL);
    CHECK(argc == 2 && (!strcmp(argv[1], "--native") || !strcmp(argv[1], "--software")));
    software = !strcmp(argv[1], "--software");
    CHECK(setenv("HAIKU_CSF_TRACE", "1", 1) == 0);
    if (software) {
        int absent = open(kDevice, O_RDWR | O_CLOEXEC); CHECK(absent < 0 && errno == ENOENT);
        CHECK(unsetenv("HAIKU_CSF_DEVICE") == 0);
        CHECK(setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1) == 0);
        CHECK(setenv("GALLIUM_DRIVER", "softpipe", 1) == 0);
    } else {
        CHECK(setenv("HAIKU_CSF_DEVICE", kDevice, 1) == 0);
        CHECK(setenv("HAIKU_CSF_FIRMWARE", "/boot/home/mali_csffw.bin", 1) == 0);
        CHECK(unsetenv("LIBGL_ALWAYS_SOFTWARE") == 0 && unsetenv("GALLIUM_DRIVER") == 0);
    }
    int observer = -1; Snapshot baseline{}, after{};
    if (!software) {
        observer = open(kDevice, O_RDWR | O_CLOEXEC); CHECK(observer >= 0);
        CHECK(snapshot(observer, &baseline) && baseline.buffers.globalBuffers == 0
            && baseline.vms.globalVms == 0 && baseline.heaps.globalHeaps == 0
            && baseline.sync.globalObjects == 0 && baseline.kernel_areas == 0 && baseline.user_maps == 0);
        CHECK(same_snapshot(baseline, baseline, 99));
    }
    auto getDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    CHECK(getDisplay);
    EGLDisplay display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint major, minor; CHECK(display != EGL_NO_DISPLAY && eglInitialize(display, &major, &minor));
    CHECK(major == 1 && minor >= 4 && eglBindAPI(EGL_OPENGL_ES_API));
    if (!software) {
        const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
        CHECK(extensions && strstr(extensions, "EGL_EXT_create_context_robustness"));
        reset_status = (PFNGLGETGRAPHICSRESETSTATUSKHRPROC)eglGetProcAddress("glGetGraphicsResetStatusKHR");
        CHECK(reset_status);
    }
    const EGLint configAttrs[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT, EGL_RED_SIZE,8, EGL_GREEN_SIZE,8,
        EGL_BLUE_SIZE,8, EGL_ALPHA_SIZE,8, EGL_SAMPLE_BUFFERS,0, EGL_NONE};
    EGLConfig config; EGLint count;
    CHECK(eglChooseConfig(display, configAttrs, &config, 1, &count) && count == 1);
    const EGLint nativeAttrs[] = {EGL_CONTEXT_CLIENT_VERSION,3,
        EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT,EGL_LOSE_CONTEXT_ON_RESET_EXT,EGL_NONE};
    const EGLint softAttrs[] = {EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    const EGLint surfaceAttrs[] = {EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};
    LiveContext live[2]{};
    for (unsigned i = 0; i < 2; i++) {
        live[i].child = i; live[i].display = display;
        live[i].context = eglCreateContext(display, config, i ? live[0].context : EGL_NO_CONTEXT,
            software ? softAttrs : nativeAttrs);
        live[i].surface = eglCreatePbufferSurface(display, config, surfaceAttrs);
        CHECK(live[i].context != EGL_NO_CONTEXT && live[i].surface != EGL_NO_SURFACE);
        live[i].start = create_sem(0, "loss start"); live[i].ready = create_sem(0, "loss ready");
        live[i].finish = create_sem(0, "loss finish"); live[i].done = create_sem(0, "loss done");
        CHECK(live[i].start >= 0 && live[i].ready >= 0 && live[i].finish >= 0 && live[i].done >= 0);
    }
    CHECK(eglMakeCurrent(display, live[0].surface, live[0].surface, live[0].context));
    GLuint program = Program(), vertices;
    const GLfloat points[] = {-1,-1, 3,-1, -1,3};
    glGenBuffers(1, &vertices); glBindBuffer(GL_ARRAY_BUFFER, vertices);
    glBufferData(GL_ARRAY_BUFFER, sizeof(points), points, GL_STATIC_DRAW);
    CHECK(glGetError() == GL_NO_ERROR);
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    for (auto& item : live) {
        item.program = program; item.vertices = vertices;
        item.thread = spawn_thread(Worker, "live Mesa context", B_NORMAL_PRIORITY, &item);
        CHECK(item.thread >= 0 && resume_thread(item.thread) == B_OK);
        CHECK(release_sem(item.start) == B_OK); Semaphore(item.ready);
    }
    if (!software) Fault(); else puts("ROCK5_LOSS_SOFTWARE_SKIP_RESET");
    for (auto& item : live) {
        CHECK(release_sem(item.finish) == B_OK); Semaphore(item.done);
        status_t result; CHECK(wait_for_thread(item.thread, &result) == B_OK && result == B_OK);
        CHECK(eglDestroySurface(display, item.surface));
        CHECK(eglDestroyContext(display, item.context));
        for (sem_id semaphore : {item.start, item.ready, item.finish, item.done}) CHECK(delete_sem(semaphore) == B_OK);
    }
    CHECK(eglTerminate(display)); CHECK(eglReleaseThread());
    if (!software) {
        CHECK(snapshot(observer, &after) && same_snapshot(baseline, after, 0));
        CHECK(close(observer) == 0);
    }
    printf("ROCK5_LOSS_PASS contexts=2 shared=1 before_pixels=12998 before_guards=256 notifications=%u unchanged_bytes=%u software=%u\n",
        software ? 0 : 2, software ? 0 : READ_BYTES * 2, software);
    return 0;
}
