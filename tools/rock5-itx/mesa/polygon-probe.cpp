// Bounded desktop-OpenGL polygon-mode diagnostic. No application modifications.
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

enum { W = 64, H = 64, GUARD = 64, BYTES = W * H * 4 };
struct Case {
    const char* name;
    GLenum front, back, cull;
    bool clockwise, indexed, edge_flag;
    unsigned expected; // 0 empty, 1 filled, 2 lines, 3 points
    bool clipped = false;
};
static const Case cases[] = {
    {"fill", GL_FILL, GL_FILL, 0, false, false, true, 1},
    {"line", GL_LINE, GL_LINE, 0, false, false, true, 2},
    {"fill-restored", GL_FILL, GL_FILL, 0, false, false, true, 1},
    {"point", GL_POINT, GL_POINT, 0, false, false, true, 3},
    {"line-indexed", GL_LINE, GL_LINE, 0, false, true, true, 2},
    {"line-cull-back-front", GL_LINE, GL_LINE, GL_BACK, false, false, true, 2},
    {"line-cull-back-back", GL_LINE, GL_LINE, GL_BACK, true, false, true, 0},
    {"line-cull-front-front", GL_LINE, GL_LINE, GL_FRONT, false, false, true, 0},
    {"line-cull-front-back", GL_LINE, GL_LINE, GL_FRONT, true, false, true, 2},
    {"line-cull-all", GL_LINE, GL_LINE, GL_FRONT_AND_BACK, false, false, true, 0},
    {"split-front", GL_LINE, GL_FILL, 0, false, false, true, 2},
    {"split-back", GL_LINE, GL_FILL, 0, true, false, true, 1},
    {"line-edge-flag", GL_LINE, GL_LINE, 0, false, false, false, 2},
    {"final-fill", GL_FILL, GL_FILL, 0, false, true, true, 1},
    {"clipped-line", GL_LINE, GL_LINE, 0, false, false, true, 4, true},
    {"clipped-fill", GL_FILL, GL_FILL, 0, false, false, true, 5, true},
};

static void dump(const unsigned char* p, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) std::printf("%02x", p[i]);
}

static bool draw_case(unsigned cycle, unsigned index)
{
    const Case& c = cases[index];
    glPolygonMode(GL_FRONT, c.front);
    glPolygonMode(GL_BACK, c.back);
    GLint mode[2] = {0, 0};
    glGetIntegerv(GL_POLYGON_MODE, mode);
    if (c.cull) { glEnable(GL_CULL_FACE); glCullFace(c.cull); }
    else glDisable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    // Clip inside the framebuffer so both sides of the newly formed edge
    // remain observable, independent of the boundary pixel convention.
    const GLdouble plane[] = {1, 0, 0, 0.5};
    glClipPlane(GL_CLIP_PLANE0, plane);
    if (c.clipped) glEnable(GL_CLIP_PLANE0); else glDisable(GL_CLIP_PLANE0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor4f(1, 0, 0, 1);
    const GLfloat v[][3] = {{-0.75f,-0.75f,0}, {0.75f,-0.75f,0}, {0,0.75f,0}};
    const unsigned order[3] = {0, c.clockwise ? 2u : 1u, c.clockwise ? 1u : 2u};
    if (c.indexed) {
        // Nonzero start in the element array; extra elements must be ignored.
        const GLushort indices[] = {2, 2, GLushort(order[0]), GLushort(order[1]), GLushort(order[2]), 1};
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(3, GL_FLOAT, sizeof(v[0]), v);
        glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, indices + 2);
        glDisableClientState(GL_VERTEX_ARRAY);
    } else {
        glBegin(GL_TRIANGLES);
        for (unsigned i = 0; i < 3; ++i) {
            glEdgeFlag(i == 0 ? c.edge_flag : GL_TRUE);
            glVertex3fv(v[order[i]]);
        }
        glEnd();
        glEdgeFlag(GL_TRUE);
    }
    glFinish();
    unsigned char data[GUARD + BYTES + GUARD];
    const unsigned char canary = 0x80 + cycle * 16 + index;
    std::memset(data, canary, sizeof(data));
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, data + GUARD);
    const GLenum error = glGetError();
    unsigned coloured = 0, other = 0, interior = 0, edge = 0, clip_edge = 0, guard_errors = 0;
    for (unsigned y = 0; y < H; ++y) {
        for (unsigned x = 0; x < W; ++x) {
            const unsigned char* p = data + GUARD + 4 * (y * W + x);
            const bool red = p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255;
            const bool black = p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 255;
            coloured += red;
            other += !red && !black;
            if (x >= 28 && x < 36 && y >= 22 && y < 30) interior += red;
            if (x >= 20 && x < 44 && y >= 7 && y < 9) edge += red;
            if (x >= 15 && x < 17 && y >= 10 && y < 22) clip_edge += red;
        }
    }
    for (unsigned i = 0; i < GUARD; ++i)
        guard_errors += data[i] != canary || data[GUARD + BYTES + i] != canary;
    bool pixels = false;
    switch (c.expected) {
        case 0: pixels = coloured == 0; break;
        case 1: pixels = coloured >= 1000 && coloured <= 1200 && interior == 64; break;
        case 2: pixels = coloured >= 70 && coloured <= 160 && interior == 0
            && (c.edge_flag ? edge >= 20 : edge == 0); break;
        case 3: pixels = coloured >= 3 && coloured <= 27 && interior == 0 && edge == 0; break;
        case 4: pixels = coloured >= 100 && coloured <= 220 && interior == 0
            && edge >= 20 && clip_edge >= 10; break;
        case 5: pixels = coloured >= 1000 && coloured <= 1100 && interior == 64
            && clip_edge == 12; break;
    }
    bool passed = pixels && other == 0 && guard_errors == 0 && error == GL_NO_ERROR
        && GLenum(mode[0]) == c.front && GLenum(mode[1]) == c.back;
    std::printf("ROCK5_POLYGON_PIXELS_BEGIN cycle=%u case=%u name=%s width=%u height=%u format=RGBA8 origin=lower-left\n",
        cycle, index, c.name, W, H);
    std::printf("guard_before="); dump(data, GUARD); std::printf("\n");
    for (unsigned y = 0; y < H; ++y) {
        std::printf("row=%02u ", y); dump(data + GUARD + y * W * 4, W * 4); std::printf("\n");
    }
    std::printf("guard_after="); dump(data + GUARD + BYTES, GUARD); std::printf("\n");
    std::printf("ROCK5_POLYGON_PIXELS_END cycle=%u case=%u front=%04x back=%04x error=%04x coloured=%u other=%u interior=%u edge=%u clip_edge=%u guard_errors=%u expected=%u pass=%u\n",
        cycle,index,mode[0],mode[1],error,coloured,other,interior,edge,clip_edge,guard_errors,c.expected,passed);
    return passed;
}

static bool context_test(EGLDisplay display, EGLConfig config, unsigned cycle, bool software)
{
    const EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    const EGLint sa[] = {EGL_WIDTH,W,EGL_HEIGHT,H,EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, ca);
    EGLSurface surface = eglCreatePbufferSurface(display, config, sa);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE
        || !eglMakeCurrent(display, surface, surface, context)) {
        std::fprintf(stderr, "ROCK5_POLYGON_CONTEXT_FAILURE cycle=%u error=%04x\n", cycle, eglGetError());
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        return false;
    }
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    std::printf("ROCK5_POLYGON_GL cycle=%u renderer=%s version=%s\n", cycle,
        renderer ? renderer : "NULL", glGetString(GL_VERSION));
    const bool identity = renderer && (software ? std::strcmp(renderer,"softpipe") == 0
        : std::strstr(renderer,"Mali-G610") && std::strstr(renderer,"Panfrost"));
    glViewport(0,0,W,H);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DITHER); glDisable(GL_BLEND); glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST); glDisable(GL_LINE_SMOOTH); glDisable(GL_POINT_SMOOTH);
    glDisable(GL_POLYGON_SMOOTH); glDisable(GL_SCISSOR_TEST);
    glDisable(GL_LINE_STIPPLE); glDisable(GL_POLYGON_STIPPLE);
    glLineWidth(1); glPointSize(1); glClearColor(0,0,0,1);
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    bool pass = identity && glGetError() == GL_NO_ERROR;
    unsigned passed = 0;
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) passed += draw_case(cycle,i);
    pass &= passed == sizeof(cases)/sizeof(cases[0]);
    // Release normally even when the pixel oracle finds a rendering failure.
    glFinish();
    pass &= glGetError() == GL_NO_ERROR;
    bool cleanup = eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    cleanup &= eglDestroySurface(display, surface);
    cleanup &= eglDestroyContext(display, context);
    pass &= cleanup;
    std::printf("ROCK5_POLYGON_CONTEXT_END cycle=%u cases=%zu passed=%u normal_cleanup=%u pass=%u\n",
        cycle,sizeof(cases)/sizeof(cases[0]),passed,cleanup,pass);
    return pass;
}

int main(int argc, char** argv)
{
    if (argc != 2 || (std::strcmp(argv[1],"--software") && std::strcmp(argv[1],"--native"))) return 2;
    const bool software = !std::strcmp(argv[1],"--software");
    unsetenv("MESA_GL_VERSION_OVERRIDE"); unsetenv("MESA_GLES_VERSION_OVERRIDE");
    unsetenv("MESA_EXTENSION_OVERRIDE");
    unsetenv("MESA_LOADER_DRIVER_OVERRIDE");
    unsetenv("HAIKU_CSF_DEVICE"); unsetenv("HAIKU_CSF_FIRMWARE"); unsetenv("HAIKU_CSF_TRACE");
    if (software) {
        setenv("LIBGL_ALWAYS_SOFTWARE","1",1); setenv("GALLIUM_DRIVER","softpipe",1);
    } else {
        unsetenv("LIBGL_ALWAYS_SOFTWARE"); unsetenv("GALLIUM_DRIVER");
        setenv("HAIKU_CSF_DEVICE","/dev/graphics/mali_csf/0",1);
        setenv("HAIKU_CSF_FIRMWARE","/boot/home/mali_csffw.bin",1);
        setenv("HAIKU_CSF_TRACE","1",1);
    }
    setenv("DRAW_USE_LLVM","0",1);
    setvbuf(stdout,nullptr,_IOLBF,0); setvbuf(stderr,nullptr,_IOLBF,0);
    auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!get_display) return 3;
    EGLDisplay display = get_display(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,nullptr);
    EGLint major=0,minor=0;
    if (!eglInitialize(display,&major,&minor) || !eglBindAPI(EGL_OPENGL_API)) {
        std::fprintf(stderr,"ROCK5_POLYGON_EGL_FAILURE error=%04x\n",eglGetError());
        return 3;
    }
    const EGLint attributes[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,0,
        EGL_STENCIL_SIZE,0,EGL_SAMPLE_BUFFERS,0,EGL_NONE};
    EGLConfig config; EGLint count=0;
    if (!eglChooseConfig(display,attributes,&config,1,&count) || count != 1) { eglTerminate(display); return 4; }
    bool pass = true;
    for (unsigned i=0; i<2; ++i) pass &= context_test(display,config,i,software);
    pass &= eglTerminate(display);
    pass &= eglReleaseThread();
    std::printf("ROCK5_POLYGON_RESULT contexts=2 cases=%zu pass=%u\n",2*sizeof(cases)/sizeof(cases[0]),pass);
    return pass ? 0 : 1;
}
