// SPDX-License-Identifier: MIT
// Keep the qualified offscreen probe and its native allocation observer intact.
#define main rock5_offscreen_main
#include "render-probe.cpp"
#undef main

#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <Locker.h>
#include <Messenger.h>
#include <Screen.h>
#include <StringView.h>
#include <View.h>
#include <Window.h>
#include "hgl_sw_winsys.h"

class WindowView : public BView, public BitmapHook {
public:
    WindowView() : BView(BRect(16,32,79,95), "GPU frame", B_FOLLOW_NONE,
        B_WILL_DRAW | B_FRAME_EVENTS), fLock("GPU window bitmap"),
        fBitmap(nullptr), fWidth(64), fHeight(64), fPublished(0), fDrawn(0) {}

    void GetSize(uint32_t& width, uint32_t& height) override
    {
        BAutolock lock(&fLock);
        width = fWidth; height = fHeight;
    }

    BBitmap* SetBitmap(BBitmap* bitmap) override
    {
        BBitmap* old;
        {
            BAutolock lock(&fLock);
            old = fBitmap; fBitmap = bitmap; ++fPublished;
        }
        BMessenger(this).SendMessage(B_INVALIDATE);
        return old;
    }

    void Draw(BRect) override
    {
        BAutolock lock(&fLock);
        if (fBitmap) DrawBitmap(fBitmap, B_ORIGIN);
        else { SetHighColor(100,100,100); FillRect(Bounds()); }
        Sync();
        fDrawn = fPublished;
    }

    void FrameResized(float width, float height) override
    {
        BAutolock lock(&fLock);
        fWidth = uint32_t(width) + 1; fHeight = uint32_t(height) + 1;
    }

    int Resize(unsigned width, unsigned height)
    {
        CHECK(Window()->Lock());
        ResizeTo(width - 1, height - 1);
        Window()->Unlock();
        // BView::_ResizeBy posts B_VIEW_RESIZED to the window looper.
        uint32_t actual_width = 0, actual_height = 0;
        bigtime_t deadline = system_time() + 5000000;
        while (system_time() < deadline) {
            GetSize(actual_width, actual_height);
            if (actual_width == width && actual_height == height) return 1;
            snooze(1000);
        }
        fprintf(stderr, "ROCK5_WINDOW_RESIZE_TIMEOUT expected=%ux%u actual=%ux%u\n",
            width,height,actual_width,actual_height);
        CHECK(false);
    }

    int WaitDrawn()
    {
        bigtime_t deadline = system_time() + 5000000;
        while (system_time() < deadline) {
            {
                BAutolock lock(&fLock);
                if (fBitmap && fDrawn == fPublished) return 1;
            }
            snooze(10000);
        }
        CHECK(false);
    }

    int Empty()
    {
        BAutolock lock(&fLock);
        CHECK(fBitmap == nullptr);
        return 1;
    }

    int Verify(unsigned cycle, unsigned frame, unsigned width, unsigned height);

private:
    BLocker fLock;
    BBitmap* fBitmap; // borrowed; EGL/displaytarget owns it until retirement
    uint32_t fWidth, fHeight;
    uint64_t fPublished, fDrawn;
};

static void rectangle(unsigned index, unsigned width, unsigned height, int out[4])
{
    if (index == 0) {
        out[0] = width/8; out[1] = height/5;
        out[2] = width*3/4; out[3] = height*2/3;
    } else {
        out[0] = width/3; out[1] = height/3;
        out[2] = width*7/8; out[3] = height*7/8;
    }
}

static unsigned colour(unsigned cycle, unsigned frame, unsigned layer)
{
    static const unsigned colours[2][4][3] = {
        {{0,1,2},{4,3,5},{7,6,4},{5,0,3}},
        {{7,4,1},{2,5,0},{3,1,6},{0,6,5}},
    };
    return colours[cycle][frame][layer];
}

static int verify_bitmap(BBitmap* bitmap, const char* kind, unsigned cycle,
    unsigned frame, unsigned width, unsigned height)
{
    CHECK(bitmap && bitmap->InitCheck() == B_OK);
    CHECK(bitmap->Bounds().IntegerWidth() + 1 == int(width)
        && bitmap->Bounds().IntegerHeight() + 1 == int(height));
    CHECK(bitmap->ColorSpace() == B_RGBA32 || bitmap->ColorSpace() == B_RGB32);
    CHECK(bitmap->BytesPerRow() >= int(width * 4));
    printf("ROCK5_WINDOW_PIXELS_BEGIN kind=%s cycle=%u frame=%u width=%u height=%u stride=%d format=BGRX8 origin=upper-left\n",
        kind, cycle, frame, width, height, bitmap->BytesPerRow());
    unsigned mismatches = 0;
    for (unsigned y = 0; y < height; ++y) {
        const unsigned char* row = static_cast<const unsigned char*>(bitmap->Bits())
            + y * bitmap->BytesPerRow();
        for (unsigned x = 0; x < width; ++x) {
            unsigned expected = colour(cycle, frame, 0);
            for (unsigned layer = 0; layer < 2; ++layer) {
                int r[4]; rectangle(layer, width, height, r);
                if (int(x) >= r[0] && int(x) < r[2]
                    && int(height - 1 - y) >= r[1] && int(height - 1 - y) < r[3])
                    expected = colour(cycle, frame, layer + 1);
            }
            for (unsigned channel = 0; channel < 3; ++channel)
                mismatches += row[4*x+channel] != ((expected & (1u << (2-channel))) ? 255 : 0);
            if (!strcmp(kind, "bitmap")) mismatches += row[4*x+3] != 255;
        }
        printf("row=%03u ", y); dump_bytes(row, width * 4); printf("\n");
    }
    printf("ROCK5_WINDOW_PIXELS_END kind=%s cycle=%u frame=%u mismatches=%u\n",
        kind, cycle, frame, mismatches);
    CHECK(mismatches == 0);
    return 1;
}

int WindowView::Verify(unsigned cycle, unsigned frame, unsigned width, unsigned height)
{
    CHECK(WaitDrawn());
    {
        BAutolock lock(&fLock);
        CHECK(verify_bitmap(fBitmap, "bitmap", cycle, frame, width, height));
    }
    CHECK(Window()->Lock());
    BRect bounds = Bounds();
    ConvertToScreen(&bounds);
    Window()->Unlock();
    BScreen screen(Window());
    CHECK(screen.IsValid());
    BBitmap copy(BRect(0,0,width-1,height-1), 0, B_RGB32);
    CHECK(copy.InitCheck() == B_OK);
    CHECK(screen.ReadBitmap(&copy, false, &bounds) == B_OK);
    CHECK(verify_bitmap(&copy, "screen", cycle, frame, width, height));
    printf("ROCK5_WINDOW_SCREEN_PASS cycle=%u frame=%u x=%d y=%d width=%u height=%u\n",
        cycle, frame, int(bounds.left), int(bounds.top), width, height);
    return 1;
}

struct WindowOwner {
    BWindow* window;
    ~WindowOwner() { if (window && window->Lock()) window->Quit(); }
};

static int window_context(unsigned cycle)
{
    WindowOwner owner{new BWindow(BRect(96,96,415,319), "Haiku Mali rendering",
        B_TITLED_WINDOW, B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_NOT_CLOSABLE)};
    BWindow* window = owner.window;
    WindowView* view = new WindowView();
    window->AddChild(new BStringView(BRect(16,8,304,27), "description",
        software ? "Mesa EGL window: softpipe fixture" : "Mali-G610: GPU-rendered EGL window"));
    window->AddChild(view);
    window->Show();
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    CHECK(display != EGL_NO_DISPLAY);
    CHECK(eglInitialize(display, nullptr, nullptr));
    CHECK(eglBindAPI(EGL_OPENGL_ES_API));
    const EGLint attributes[] = {EGL_SURFACE_TYPE,EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT, EGL_RED_SIZE,8, EGL_GREEN_SIZE,8,
        EGL_BLUE_SIZE,8, EGL_ALPHA_SIZE,8, EGL_SAMPLE_BUFFERS,0, EGL_NONE};
    EGLConfig config; EGLint count = 0;
    CHECK(eglChooseConfig(display,attributes,&config,1,&count) && count == 1);
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    EGLContext context = eglCreateContext(display,config,EGL_NO_CONTEXT,context_attributes);
    CHECK(context != EGL_NO_CONTEXT);
    EGLSurface surface = eglCreateWindowSurface(display,config,
        reinterpret_cast<EGLNativeWindowType>(static_cast<BitmapHook*>(view)), nullptr);
    CHECK(surface != EGL_NO_SURFACE);
    CHECK(eglMakeCurrent(display,surface,surface,context));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    CHECK(renderer && (software ? !strcmp(renderer,"softpipe") : strstr(renderer,"Mali-G610") != nullptr));
    printf("ROCK5_WINDOW_GL cycle=%u renderer=%s version=%s\n",cycle,renderer,glGetString(GL_VERSION));
    GLuint vs = shader(GL_VERTEX_SHADER,
        "#version 300 es\nlayout(location=0) in vec2 point;\n"
        "void main(){gl_Position=vec4(point,0.0,1.0);}\n");
    GLuint fs = shader(GL_FRAGMENT_SHADER,
        "#version 300 es\nprecision highp float;\nuniform vec4 colour;\n"
        "out vec4 pixel;\nvoid main(){pixel=colour;}\n");
    CHECK(vs && fs);
    GLuint program = glCreateProgram();
    glAttachShader(program,vs); glAttachShader(program,fs); glLinkProgram(program);
    GLint linked = 0; glGetProgramiv(program,GL_LINK_STATUS,&linked); CHECK(linked);
    glUseProgram(program);
    GLint uniform = glGetUniformLocation(program,"colour"); CHECK(uniform >= 0);
    GLuint vao, vertices; glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glGenBuffers(1,&vertices); glBindBuffer(GL_ARRAY_BUFFER,vertices);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr); glEnableVertexAttribArray(0);
    glDisable(GL_DITHER);
    static const unsigned sizes[][2] = {{64,64},{79,47},{128,72},{65,63}};
    for (unsigned frame = 0; frame < 4; ++frame) {
        unsigned width = sizes[frame][0], height = sizes[frame][1];
        CHECK(view->Resize(width,height));
        // Query dimensions and render while retaining the same context binding.
        EGLint actual_width = 0, actual_height = 0;
        CHECK(eglQuerySurface(display,surface,EGL_WIDTH,&actual_width));
        CHECK(eglQuerySurface(display,surface,EGL_HEIGHT,&actual_height));
        CHECK(actual_width == int(width) && actual_height == int(height));
        glViewport(0,0,width,height);
        unsigned background = colour(cycle,frame,0);
        glClearColor(!!(background&1),!!(background&2),!!(background&4),1);
        glClear(GL_COLOR_BUFFER_BIT);
        for (unsigned layer = 0; layer < 2; ++layer) {
            int r[4]; rectangle(layer,width,height,r);
            float x0=2.0f*r[0]/width-1, x1=2.0f*r[2]/width-1;
            float y0=2.0f*r[1]/height-1, y1=2.0f*r[3]/height-1;
            const GLfloat points[]={x0,y0,x1,y0,x1,y1,x0,y0,x1,y1,x0,y1};
            glBufferData(GL_ARRAY_BUFFER,sizeof(points),points,GL_STREAM_DRAW);
            unsigned rgba=colour(cycle,frame,layer+1);
            glUniform4f(uniform,!!(rgba&1),!!(rgba&2),!!(rgba&4),1);
            glDrawArrays(GL_TRIANGLES,0,6);
        }
        CHECK(glGetError() == GL_NO_ERROR);
        CHECK(eglSwapBuffers(display,surface));
        CHECK(view->Verify(cycle,frame,width,height));
    }
    if (cycle == 1) {
        printf("ROCK5_WINDOW_VISIBLE cycle=1 frame=3\n");
        snooze(6000000);
    }
    glDeleteBuffers(1,&vertices); glDeleteVertexArrays(1,&vao);
    glDeleteProgram(program); glDeleteShader(vs); glDeleteShader(fs);
    CHECK(glGetError() == GL_NO_ERROR);
    CHECK(eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT));
    CHECK(eglDestroySurface(display,surface));
    CHECK(view->Empty());
    CHECK(eglDestroyContext(display,context));
    CHECK(eglTerminate(display));
    CHECK(eglReleaseThread());
    printf("ROCK5_WINDOW_CONTEXT_PASS cycle=%u frames=4 resize=3 retired_bitmap=1\n",cycle);
    return 1;
}

static int window_run(const char* mode)
{
    software = !strcmp(mode,"--software");
    CHECK(software || !strcmp(mode,"--native"));
    for (const char* name : {"MESA_GL_VERSION_OVERRIDE","MESA_GLES_VERSION_OVERRIDE",
            "MESA_LOADER_DRIVER_OVERRIDE","LIBGL_ALWAYS_SOFTWARE","GALLIUM_DRIVER","HAIKU_CSF_DEVICE"})
        CHECK(unsetenv(name) == 0);
    if (software) {
        CHECK(setenv("LIBGL_ALWAYS_SOFTWARE","1",1) == 0);
        CHECK(setenv("GALLIUM_DRIVER","softpipe",1) == 0);
    } else {
        CHECK(setenv("HAIKU_CSF_DEVICE","/dev/graphics/mali_csf/0",1) == 0);
        CHECK(setenv("HAIKU_CSF_FIRMWARE","/boot/home/mali_csffw.bin",1) == 0);
        CHECK(setenv("HAIKU_CSF_TRACE","1",1) == 0);
    }
    printf("ROCK5_WINDOW_READY version=1 mode=%s\n",mode);
    int fd=-1; Snapshot before{},after{};
    if (!software) {
        fd=open("/dev/graphics/mali_csf/0",O_RDONLY|O_CLOEXEC);
        CHECK(fd>=0 && snapshot(fd,&before) && same_snapshot(before,before,99));
    }
    for (unsigned cycle=0;cycle<2;++cycle) {
        CHECK(window_context(cycle));
        if (!software) CHECK(snapshot(fd,&after) && same_snapshot(before,after,cycle));
    }
    if (fd>=0) CHECK(close(fd) == 0);
    printf("ROCK5_WINDOW_PASS contexts=2 frames=8 resize=6 bitmap_pixels=42240 screen_pixels=42240 software=%d\n",software);
    return 1;
}

static int32 window_worker(void* mode)
{
    int result=window_run(static_cast<const char*>(mode));
    be_app->PostMessage(B_QUIT_REQUESTED);
    return result;
}

int main(int argc,char** argv)
{
    setvbuf(stdout,nullptr,_IOLBF,0);
    if (argc!=2) return 2;
    BApplication app("application/x-vnd.rock5-mesa-window-probe");
    if (app.InitCheck()!=B_OK) return 1;
    thread_id worker=spawn_thread(window_worker,"Mesa window test",B_NORMAL_PRIORITY,argv[1]);
    if (worker<0 || resume_thread(worker)!=B_OK) return 1;
    app.Run();
    status_t result=0;
    return wait_for_thread(worker,&result)==B_OK && result==1 ? 0 : 1;
}
