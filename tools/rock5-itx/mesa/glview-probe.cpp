// SPDX-License-Identifier: MIT
// Normal Haiku OpenGL Kit rendering; private experimental GLVND/Mesa libraries.
#include <EGL/egl.h>
#include <GLView.h>
#include <Autolock.h>
#include <Screen.h>
#include <StringView.h>
#include <atomic>
#include <initializer_list>
#include <vector>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "CsfQueue.h"
using namespace MaliCSF;

static bool software;
static int failure(const char* what, int line)
{
    fprintf(stderr, "ROCK5_GLVIEW_FAILURE line=%d operation=%s errno=%d\n",
        line, what, errno);
    return 0;
}
#define CHECK(x) do { if (!(x)) return failure(#x, __LINE__); } while (0)
#include "native-observer.h"

static void dump_bytes(const unsigned char* bytes, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) printf("%02x", bytes[i]);
}

static const unsigned sizes[4][2] = {{64,64},{79,47},{128,72},{65,63}};
static unsigned colour(unsigned cycle, unsigned view, unsigned frame, unsigned layer)
{
    static const unsigned values[2][4][3] = {
        {{0,1,2},{4,3,5},{7,6,4},{5,0,3}},
        {{7,4,1},{2,5,0},{3,1,6},{0,6,5}},
    };
    return values[(cycle + view) % 2][frame][layer];
}

static void rectangle(unsigned layer, unsigned width, unsigned height, int r[4])
{
    if (layer == 0) {
        r[0]=width/8; r[1]=height/5; r[2]=width*3/4; r[3]=height*2/3;
    } else {
        r[0]=width/3; r[1]=height/3; r[2]=width*7/8; r[3]=height*7/8;
    }
}

static int verify_pixels(const unsigned char* bytes, unsigned stride, bool screen,
    unsigned cycle, unsigned view, unsigned frame, unsigned width, unsigned height)
{
    CHECK(stride >= width*4);
    const char* kind = screen ? "screen" : "gl";
    printf("ROCK5_GLVIEW_PIXELS_BEGIN kind=%s cycle=%u view=%u frame=%u width=%u height=%u stride=%u format=%s origin=%s\n",
        kind,cycle,view,frame,width,height,stride,screen ? "BGRX8" : "RGBA8",
        screen ? "upper-left" : "lower-left");
    unsigned mismatches=0;
    for (unsigned y=0; y<height; ++y) {
        const unsigned char* row=bytes+y*stride;
        unsigned gl_y=screen ? height-1-y : y;
        for (unsigned x=0; x<width; ++x) {
            unsigned expected=colour(cycle,view,frame,0);
            for (unsigned layer=0; layer<2; ++layer) {
                int r[4]; rectangle(layer,width,height,r);
                if (int(x)>=r[0] && int(x)<r[2] && int(gl_y)>=r[1] && int(gl_y)<r[3])
                    expected=colour(cycle,view,frame,layer+1);
            }
            for (unsigned channel=0; channel<3; ++channel)
                mismatches += row[x*4+channel] !=
                    ((expected & (1u << (screen ? 2-channel : channel))) ? 255 : 0);
            if (!screen) mismatches += row[x*4+3] != 255;
        }
        printf("row=%03u ",y); dump_bytes(row,width*4); printf("\n");
    }
    printf("ROCK5_GLVIEW_PIXELS_END kind=%s cycle=%u view=%u frame=%u mismatches=%u\n",
        kind,cycle,view,frame,mismatches);
    CHECK(mismatches == 0);
    return 1;
}

class ProbeView : public BGLView {
public:
    explicit ProbeView(unsigned index) : BGLView(
        BRect(16+176*index,32,79+176*index,95), "OpenGL Kit frame", B_FOLLOW_NONE,
        B_WILL_DRAW|B_FRAME_EVENTS, BGL_RGB|BGL_DOUBLE|BGL_ALPHA),
        fSizeLock("OpenGL Kit size"), fWidth(64), fHeight(64), fDraws(0), fAttached(false) {}

    void AttachedToWindow() override
    {
        BGLView::AttachedToWindow();
        fAttached.store(true);
    }
    void Draw(BRect update) override
    {
        BGLView::Draw(update);
        Sync();
        ++fDraws;
    }
    void FrameResized(float width, float height) override
    {
        BGLView::FrameResized(width,height);
        BAutolock lock(&fSizeLock);
        fWidth=unsigned(width)+1; fHeight=unsigned(height)+1;
    }
    int Ready()
    {
        bigtime_t deadline=system_time()+5000000;
        while (!fAttached.load() && system_time()<deadline) snooze(1000);
        CHECK(fAttached.load());
        return 1;
    }
    int Resize(unsigned width, unsigned height)
    {
        CHECK(Window()->Lock());
        ResizeTo(width-1,height-1);
        Window()->Unlock();
        bigtime_t deadline=system_time()+5000000;
        while (system_time()<deadline) {
            {
                BAutolock lock(&fSizeLock);
                if (fWidth==width && fHeight==height) return 1;
            }
            snooze(1000);
        }
        CHECK(false);
    }
    int VerifyScreen(unsigned cycle,unsigned view,unsigned frame,unsigned width,unsigned height)
    {
        CHECK(Window()->Lock());
        uint64_t before=fDraws.load();
        Invalidate();
        BRect bounds=Bounds(); ConvertToScreen(&bounds);
        Window()->Unlock();
        bigtime_t deadline=system_time()+5000000;
        while (fDraws.load()==before && system_time()<deadline) snooze(1000);
        CHECK(fDraws.load()>before);
        BScreen screen(Window());
        CHECK(screen.IsValid());
        BBitmap copy(BRect(0,0,width-1,height-1),0,B_RGB32);
        CHECK(copy.InitCheck()==B_OK);
        CHECK(screen.ReadBitmap(&copy,false,&bounds)==B_OK);
        CHECK(verify_pixels(static_cast<unsigned char*>(copy.Bits()),copy.BytesPerRow(),
            true,cycle,view,frame,width,height));
        printf("ROCK5_GLVIEW_SCREEN_PASS cycle=%u view=%u frame=%u x=%d y=%d width=%u height=%u\n",
            cycle,view,frame,int(bounds.left),int(bounds.top),width,height);
        return 1;
    }
private:
    BLocker fSizeLock;
    unsigned fWidth,fHeight;
    std::atomic<uint64_t> fDraws;
    std::atomic<bool> fAttached;
};

struct LockedGL {
    ProbeView* view;
    explicit LockedGL(ProbeView* v) : view(v) {view->LockGL();}
    ~LockedGL() {view->UnlockGL();}
};
struct WindowOwner {
    BWindow* window;
    ~WindowOwner() {if (window && window->Lock()) window->Quit();}
};

static int render_frame(ProbeView* view,EGLContext expected,unsigned cycle,
    unsigned index,unsigned frame,unsigned width,unsigned height)
{
    CHECK(view->Resize(width,height));
    {
        LockedGL lock(view);
        CHECK(eglGetCurrentContext()==expected);
        glViewport(0,0,width,height);
        glDrawBuffer(GL_BACK); glReadBuffer(GL_BACK);
        glDisable(GL_DITHER); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0,width,0,height,-1,1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        unsigned background=colour(cycle,index,frame,0);
        glClearColor(!!(background&1),!!(background&2),!!(background&4),1);
        glClear(GL_COLOR_BUFFER_BIT);
        for (unsigned layer=0; layer<2; ++layer) {
            int r[4]; rectangle(layer,width,height,r);
            unsigned rgba=colour(cycle,index,frame,layer+1);
            glColor4f(!!(rgba&1),!!(rgba&2),!!(rgba&4),1);
            glBegin(GL_TRIANGLES);
            glVertex2i(r[0],r[1]); glVertex2i(r[2],r[1]); glVertex2i(r[2],r[3]);
            glVertex2i(r[0],r[1]); glVertex2i(r[2],r[3]); glVertex2i(r[0],r[3]);
            glEnd();
        }
        CHECK(glGetError()==GL_NO_ERROR);
        std::vector<unsigned char> pixels(width*height*4+128,0xa5);
        glPixelStorei(GL_PACK_ALIGNMENT,1);
        glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data()+64);
        CHECK(glGetError()==GL_NO_ERROR);
        for (unsigned i=0; i<64; ++i)
            CHECK(pixels[i]==0xa5 && pixels[64+width*height*4+i]==0xa5);
        CHECK(verify_pixels(pixels.data()+64,width*4,false,cycle,index,frame,width,height));
        printf("ROCK5_GLVIEW_GUARDS cycle=%u view=%u frame=%u before=",cycle,index,frame);
        dump_bytes(pixels.data(),64); printf(" after=");
        dump_bytes(pixels.data()+64+width*height*4,64); printf("\n");
        view->SwapBuffers();
        CHECK(eglGetError()==EGL_SUCCESS && glGetError()==GL_NO_ERROR);
    }
    CHECK(eglGetCurrentContext()==EGL_NO_CONTEXT);
    CHECK(view->VerifyScreen(cycle,index,frame,width,height));
    return 1;
}

static int glview_cycle(unsigned cycle)
{
    WindowOwner owner{new BWindow(BRect(96,96,463,319), "Haiku OpenGL Kit on Mali",
        B_TITLED_WINDOW,B_NOT_ZOOMABLE|B_NOT_MINIMIZABLE|B_NOT_CLOSABLE)};
    ProbeView* views[2]={new ProbeView(0),new ProbeView(1)};
    owner.window->AddChild(new BStringView(BRect(16,8,344,27),"description",
        software ? "BGLView: two software contexts" : "Mali-G610: two OpenGL Kit contexts"));
    for (ProbeView* view:views) owner.window->AddChild(view);
    owner.window->Show();
    EGLContext contexts[2]={EGL_NO_CONTEXT,EGL_NO_CONTEXT};
    for (unsigned i=0; i<2; ++i) {
        CHECK(views[i]->Ready());
        LockedGL lock(views[i]);
        contexts[i]=eglGetCurrentContext();
        CHECK(contexts[i]!=EGL_NO_CONTEXT);
        const char* renderer=reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        CHECK(renderer && (software ? !strcmp(renderer,"softpipe") : !strcmp(renderer,"Mali-G610 (Panfrost)")));
        CHECK(views[i]->GetGLProcAddress("glClearColor")!=nullptr);
        {
            LockedGL recursive(views[i]);
            CHECK(eglGetCurrentContext()==contexts[i]);
        }
        CHECK(eglGetCurrentContext()==contexts[i]);
        printf("ROCK5_GLVIEW_CONTEXT cycle=%u view=%u context=%p renderer=%s version=%s recursive_lock=1\n",
            cycle,i,contexts[i],renderer,glGetString(GL_VERSION));
    }
    CHECK(contexts[0]!=contexts[1]);
    CHECK(eglGetCurrentContext()==EGL_NO_CONTEXT);
    for (unsigned frame=0; frame<4; ++frame) {
        for (unsigned turn=0; turn<2; ++turn) {
            unsigned i=(frame+turn)%2;
            unsigned size=i ? 3-frame : frame;
            CHECK(render_frame(views[i],contexts[i],cycle,i,frame,sizes[size][0],sizes[size][1]));
        }
    }
    if (cycle==1) {
        printf("ROCK5_GLVIEW_VISIBLE cycle=1 frames=8\n");
        snooze(6000000);
    }
    printf("ROCK5_GLVIEW_CYCLE_PASS cycle=%u views=2 frames=8\n",cycle);
    return 1;
}

static int glview_run(const char* mode)
{
    software=!strcmp(mode,"--software");
    CHECK(software || !strcmp(mode,"--native"));
    for (const char* name:{"MESA_GL_VERSION_OVERRIDE","MESA_GLES_VERSION_OVERRIDE",
            "MESA_LOADER_DRIVER_OVERRIDE","LIBGL_ALWAYS_SOFTWARE","GALLIUM_DRIVER","HAIKU_CSF_DEVICE"})
        CHECK(unsetenv(name)==0);
    if (software) {
        CHECK(setenv("LIBGL_ALWAYS_SOFTWARE","1",1)==0);
        CHECK(setenv("GALLIUM_DRIVER","softpipe",1)==0);
    } else {
        CHECK(setenv("HAIKU_CSF_DEVICE","/dev/graphics/mali_csf/0",1)==0);
        CHECK(setenv("HAIKU_CSF_FIRMWARE","/boot/home/mali_csffw.bin",1)==0);
        CHECK(setenv("HAIKU_CSF_TRACE","1",1)==0);
    }
    printf("ROCK5_GLVIEW_READY version=1 mode=%s\n",mode);
    int fd=-1; Snapshot before{},after{};
    if (!software) {
        fd=open("/dev/graphics/mali_csf/0",O_RDONLY|O_CLOEXEC);
        CHECK(fd>=0 && snapshot(fd,&before) && same_snapshot(before,before,99));
    }
    for (unsigned cycle=0; cycle<2; ++cycle) {
        CHECK(glview_cycle(cycle));
        if (!software) CHECK(snapshot(fd,&after) && same_snapshot(before,after,cycle));
    }
    if (fd>=0) CHECK(close(fd)==0);
    printf("ROCK5_GLVIEW_PASS cycles=2 contexts=4 frames=16 gl_pixels=84480 screen_pixels=84480 guard_bytes=2048 software=%d\n",software);
    return 1;
}
static int32 worker(void* mode)
{
    int result=glview_run(static_cast<const char*>(mode));
    be_app->PostMessage(B_QUIT_REQUESTED);
    return result;
}
int main(int argc,char** argv)
{
    setvbuf(stdout,nullptr,_IOLBF,0);
    if (argc!=2) return 2;
    BApplication app("application/x-vnd.rock5-mesa-glview-probe");
    if (app.InitCheck()!=B_OK) return 1;
    thread_id thread=spawn_thread(worker,"OpenGL Kit test",B_NORMAL_PRIORITY,argv[1]);
    if (thread<0 || resume_thread(thread)!=B_OK) return 1;
    app.Run();
    status_t result=0;
    return wait_for_thread(thread,&result)==B_OK && result==1 ? 0 : 1;
}
