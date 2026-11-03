/* Hardware (OpenGL) rendering for engines that draw with the graphics card (PS2 with LRPS2, later N64, GameCube...).
   The engine renders into a framebuffer object in a context on a hidden window; each finished frame is read back into
   the player's ordinary frame buffer, so presentation, pacing, the pause-menu picture and screenshots stay exactly
   as they are for every other engine. Reading back costs a copy per frame; it can become asynchronous later. */
#include "glhost.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>
#else
#include "desktop.h"
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_opengl_glext.h>
#endif

#ifdef _WIN32
static HWND gl_window;static HDC gl_dc;static HGLRC gl_context;
#else
static void *gl_context;
#endif
#ifndef _WIN32
/* Resolve base GL through SDL too: portable workers have no link-time GL framework dependency. */
static const GLubyte *(APIENTRY *p_glGetString)(GLenum);
static void (APIENTRY *p_glDeleteTextures)(GLsizei,const GLuint*);
static void (APIENTRY *p_glGenTextures)(GLsizei,GLuint*);
static void (APIENTRY *p_glBindTexture)(GLenum,GLuint);
static void (APIENTRY *p_glTexParameteri)(GLenum,GLenum,GLint);
static void (APIENTRY *p_glTexImage2D)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
static void (APIENTRY *p_glClearColor)(GLfloat,GLfloat,GLfloat,GLfloat);
static void (APIENTRY *p_glClear)(GLbitfield);
static void (APIENTRY *p_glGetIntegerv)(GLenum,GLint*);
static void (APIENTRY *p_glPixelStorei)(GLenum,GLint);
static void (APIENTRY *p_glReadBuffer)(GLenum);
static void (APIENTRY *p_glReadPixels)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*);
#define glGetString p_glGetString
#define glDeleteTextures p_glDeleteTextures
#define glGenTextures p_glGenTextures
#define glBindTexture p_glBindTexture
#define glTexParameteri p_glTexParameteri
#define glTexImage2D p_glTexImage2D
#define glClearColor p_glClearColor
#define glClear p_glClear
#define glGetIntegerv p_glGetIntegerv
#define glPixelStorei p_glPixelStorei
#define glReadPixels p_glReadPixels
#define glReadBuffer p_glReadBuffer
#endif
static GLuint fbo,color,depth;static unsigned fbo_w,fbo_h;
static PFNGLGENFRAMEBUFFERSPROC p_glGenFramebuffers;static PFNGLBINDFRAMEBUFFERPROC p_glBindFramebuffer;
static PFNGLFRAMEBUFFERTEXTURE2DPROC p_glFramebufferTexture2D;static PFNGLGENRENDERBUFFERSPROC p_glGenRenderbuffers;
static PFNGLBINDRENDERBUFFERPROC p_glBindRenderbuffer;static PFNGLRENDERBUFFERSTORAGEPROC p_glRenderbufferStorage;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC p_glFramebufferRenderbuffer;static PFNGLCHECKFRAMEBUFFERSTATUSPROC p_glCheckFramebufferStatus;
static PFNGLDELETEFRAMEBUFFERSPROC p_glDeleteFramebuffers;static PFNGLDELETERENDERBUFFERSPROC p_glDeleteRenderbuffers;
static PFNGLBINDBUFFERPROC p_glBindBuffer;static PFNGLGENBUFFERSPROC p_glGenBuffers;static PFNGLBUFFERDATAPROC p_glBufferData;
static PFNGLMAPBUFFERRANGEPROC p_glMapBufferRange;static PFNGLUNMAPBUFFERPROC p_glUnmapBuffer;static PFNGLDELETEBUFFERSPROC p_glDeleteBuffers;
/* Three pixel buffers in a ring, each with a fence: frames are copied as they finish and taken out only once the graphics
   card is done. Waiting for the previous frame instead made the CPU and GPU take turns: 5-7 ms a frame on an integrated GPU. */
#define PBO_SLOTS 3
static GLuint pbo[PBO_SLOTS];static unsigned pbo_w[PBO_SLOTS],pbo_h[PBO_SLOTS];static int pbo_pending[PBO_SLOTS],pbo_write,pbo_read;
static GLsync pbo_fence[PBO_SLOTS];static PFNGLFENCESYNCPROC p_glFenceSync;static PFNGLCLIENTWAITSYNCPROC p_glClientWaitSync;static PFNGLDELETESYNCPROC p_glDeleteSync;

/* Engines look up every GL function through this, as they would in RetroArch. */
retro_proc_address_t eh_gl_proc(const char *name){
    #ifdef _WIN32
    PROC p=wglGetProcAddress(name);
    /* wglGetProcAddress returns small sentinel values instead of NULL on some drivers for GL 1.1 functions. */
    if(!p||p==(PROC)1||p==(PROC)2||p==(PROC)3||p==(PROC)-1){HMODULE gl=GetModuleHandleW(L"opengl32.dll");p=gl?GetProcAddress(gl,name):NULL;}
    #else
    void *p=eh_desktop_gl_proc(name);
    #endif
    retro_proc_address_t out;memcpy(&out,&p,sizeof(out));return out;
}
#define LOADGL(name) do{retro_proc_address_t f=eh_gl_proc(#name);memcpy(&p_##name,&f,sizeof(p_##name));if(!p_##name)return 0;}while(0)

static void APIENTRY diagnostic_gl_message(GLenum source,GLenum type,GLuint id,GLenum severity,GLsizei length,const GLchar *message,const void *user){
    static unsigned count;(void)source;(void)id;(void)severity;(void)user;
    if(type==GL_DEBUG_TYPE_ERROR&&count++<64){
        fprintf(stderr,"OpenGL diagnostic: %.*s\n",length>1000?1000:length,message);
#ifdef _WIN32
        GLint unit=0,texture=0,width=0,height=0,immutable=0;
        glGetIntegerv(GL_ACTIVE_TEXTURE,&unit);glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);
        if(texture){glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&height);glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_IMMUTABLE_FORMAT,&immutable);}
        fprintf(stderr,"  texture unit=%d name=%d size=%dx%d immutable=%d\n",unit-GL_TEXTURE0,texture,width,height,immutable);
        void *frames[24];USHORT n=CaptureStackBackTrace(0,24,frames,NULL);
        for(USHORT i=0;i<n;i++){
            HMODULE module=NULL;wchar_t file[MAX_PATH];
            if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)frames[i],&module)&&GetModuleFileNameW(module,file,MAX_PATH)&&wcsstr(file,L"mupen64plus_next_libretro"))
                fprintf(stderr,"  N64 core+0x%llx\n",(unsigned long long)((uintptr_t)frames[i]-(uintptr_t)module));
        }
#endif
    }
}

int eh_gl_create(int core_profile,unsigned major,unsigned minor){
    #ifdef _WIN32
    WNDCLASSW wc;memset(&wc,0,sizeof(wc));wc.style=CS_OWNDC;wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(NULL);wc.lpszClassName=L"EmuHallGL";RegisterClassW(&wc);
    /* Never shown: it only carries the context. */
    gl_window=CreateWindowExW(0,wc.lpszClassName,L"EmuHall GL",WS_POPUP,0,0,16,16,NULL,NULL,wc.hInstance,NULL);if(!gl_window)return 0;
    gl_dc=GetDC(gl_window);
    PIXELFORMATDESCRIPTOR pfd;memset(&pfd,0,sizeof(pfd));pfd.nSize=sizeof(pfd);pfd.nVersion=1;pfd.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;
    pfd.iPixelType=PFD_TYPE_RGBA;pfd.cColorBits=32;pfd.cDepthBits=24;pfd.cStencilBits=8;
    int format=ChoosePixelFormat(gl_dc,&pfd);if(!format||!SetPixelFormat(gl_dc,format,&pfd)){fprintf(stderr,"OpenGL: no usable pixel format\n");return 0;}
    HGLRC legacy=wglCreateContext(gl_dc);if(!legacy||!wglMakeCurrent(gl_dc,legacy)){fprintf(stderr,"OpenGL: no context\n");return 0;}
    PFNWGLCREATECONTEXTATTRIBSARBPROC create=(PFNWGLCREATECONTEXTATTRIBSARBPROC)(void*)wglGetProcAddress("wglCreateContextAttribsARB");
    if(create){
        /* The newest version the driver offers that is at least what the engine asked for. */
        static const int versions[][2]={{4,6},{4,5},{4,4},{4,3},{4,2},{4,1},{4,0},{3,3},{3,2},{3,1},{3,0}};
        for(size_t i=0;i<sizeof(versions)/sizeof(versions[0])&&!gl_context;i++){
            const char *cap=getenv("EMUHALL_GL_VERSION_CAP");
            if(cap&&!strcmp(cap,"3.3")&&(versions[i][0]>3||(versions[i][0]==3&&versions[i][1]>3)))continue;
            if(versions[i][0]<(int)major||(versions[i][0]==(int)major&&versions[i][1]<(int)minor))break;
            int attribs[]={WGL_CONTEXT_MAJOR_VERSION_ARB,versions[i][0],WGL_CONTEXT_MINOR_VERSION_ARB,versions[i][1],
                WGL_CONTEXT_PROFILE_MASK_ARB,core_profile?WGL_CONTEXT_CORE_PROFILE_BIT_ARB:WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,0};
            gl_context=create(gl_dc,NULL,attribs);
        }
    }
    if(gl_context){wglMakeCurrent(gl_dc,gl_context);wglDeleteContext(legacy);}
    else if(!core_profile&&major<3){gl_context=legacy;} /* An old-style context is what was asked for. */
    else{wglMakeCurrent(NULL,NULL);wglDeleteContext(legacy);fprintf(stderr,"OpenGL %u.%u%s is not available on this graphics card\n",major,minor,core_profile?" core":"");return 0;}
    #else
    if(!eh_desktop_gl_create(core_profile,major,minor)){return 0;}gl_context=(void*)1;
    LOADGL(glGetString);LOADGL(glDeleteTextures);LOADGL(glGenTextures);LOADGL(glBindTexture);LOADGL(glTexParameteri);LOADGL(glTexImage2D);
    LOADGL(glClearColor);LOADGL(glClear);LOADGL(glGetIntegerv);LOADGL(glPixelStorei);LOADGL(glReadPixels);LOADGL(glReadBuffer);
    #endif
    LOADGL(glGenFramebuffers);LOADGL(glBindFramebuffer);LOADGL(glFramebufferTexture2D);LOADGL(glGenRenderbuffers);LOADGL(glBindRenderbuffer);
    LOADGL(glRenderbufferStorage);LOADGL(glFramebufferRenderbuffer);LOADGL(glCheckFramebufferStatus);LOADGL(glDeleteFramebuffers);
    LOADGL(glDeleteRenderbuffers);LOADGL(glBindBuffer);LOADGL(glGenBuffers);LOADGL(glBufferData);LOADGL(glMapBufferRange);
    LOADGL(glUnmapBuffer);LOADGL(glDeleteBuffers);
    /* Optional (GL 3.2): without fences, frames are read back one frame late as before. */
    {retro_proc_address_t f=eh_gl_proc("glFenceSync"),c=eh_gl_proc("glClientWaitSync"),d=eh_gl_proc("glDeleteSync");
     if(f&&c&&d){memcpy(&p_glFenceSync,&f,sizeof(f));memcpy(&p_glClientWaitSync,&c,sizeof(c));memcpy(&p_glDeleteSync,&d,sizeof(d));}}
    fprintf(stderr,"OpenGL: %s, %s\n",(const char*)glGetString(GL_VERSION),(const char*)glGetString(GL_RENDERER));
    const char *debug=getenv("EMUHALL_GL_DEBUG");
    if(debug&&!strcmp(debug,"1")){
        PFNGLDEBUGMESSAGECALLBACKPROC callback=NULL;retro_proc_address_t fn=eh_gl_proc("glDebugMessageCallback");memcpy(&callback,&fn,sizeof(fn));
        typedef void(APIENTRY *enable_fn)(GLenum);enable_fn enable=NULL;fn=eh_gl_proc("glEnable");memcpy(&enable,&fn,sizeof(fn));
        if(callback&&enable){callback(diagnostic_gl_message,NULL);enable(GL_DEBUG_OUTPUT);enable(GL_DEBUG_OUTPUT_SYNCHRONOUS);}
    }
    return 1;
}

static void release_framebuffers(void){
    if(fbo)p_glDeleteFramebuffers(1,&fbo);
    if(depth)glDeleteTextures(1,&depth);
    if(color)glDeleteTextures(1,&color);
    for(int i=0;i<PBO_SLOTS;i++)if(pbo_fence[i]){p_glDeleteSync(pbo_fence[i]);pbo_fence[i]=0;}
    if(pbo[0])p_glDeleteBuffers(PBO_SLOTS,pbo);
    fbo=color=depth=0;memset(pbo,0,sizeof(pbo));memset(pbo_pending,0,sizeof(pbo_pending));pbo_write=pbo_read=0;
}
int eh_gl_framebuffer(unsigned width,unsigned height,int with_depth,int with_stencil){
    if(!gl_context||!width||!height||width>4096||height>4096)return 0;
    /* Cores such as GLideN64 cache this name. Resize its attachments in place. */
    GLint read_fbo=0,draw_fbo=0,texture=0,renderbuffer=0,pack=0,unpack=0;
    const GLenum unpack_names[]={GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_ROWS,GL_UNPACK_SKIP_PIXELS,GL_UNPACK_SWAP_BYTES};
    GLint unpack_values[5];for(unsigned i=0;i<5;i++)glGetIntegerv(unpack_names[i],&unpack_values[i]);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read_fbo);glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw_fbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);glGetIntegerv(GL_RENDERBUFFER_BINDING,&renderbuffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&pack);glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&unpack);
    uint32_t *blank=calloc((size_t)width*height,sizeof(uint32_t));if(!blank)return 0;
    if(!color)glGenTextures(1,&color);
    p_glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);glBindTexture(GL_TEXTURE_2D,color);
    for(unsigned i=0;i<5;i++)glPixelStorei(unpack_names[i],i==0?4:0);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,blank);free(blank);
    if(!fbo)p_glGenFramebuffers(1,&fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color,0);
    if(with_depth||with_stencil){
        /* GLideN64's GLSM copies both frontend attachments as GL_TEXTURE_2D.
           A renderbuffer ID belongs to another namespace and breaks depth copies. */
        if(!depth)glGenTextures(1,&depth);
        glBindTexture(GL_TEXTURE_2D,depth);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D,0,with_stencil?GL_DEPTH24_STENCIL8:GL_DEPTH_COMPONENT24,width,height,0,
            with_stencil?GL_DEPTH_STENCIL:GL_DEPTH_COMPONENT,with_stencil?GL_UNSIGNED_INT_24_8:GL_UNSIGNED_INT,NULL);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER,with_stencil?GL_DEPTH_STENCIL_ATTACHMENT:GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,depth,0);
    }
    int ok=p_glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
    if(!pbo[0])p_glGenBuffers(PBO_SLOTS,pbo);
    for(int i=0;i<PBO_SLOTS;i++){
        if(pbo_fence[i]){p_glDeleteSync(pbo_fence[i]);pbo_fence[i]=0;}
        p_glBindBuffer(GL_PIXEL_PACK_BUFFER,pbo[i]);p_glBufferData(GL_PIXEL_PACK_BUFFER,(GLsizeiptr)width*height*4,NULL,GL_STREAM_READ);
    }
    memset(pbo_pending,0,sizeof(pbo_pending));pbo_write=pbo_read=0;fbo_w=width;fbo_h=height;
    glBindTexture(GL_TEXTURE_2D,texture);p_glBindRenderbuffer(GL_RENDERBUFFER,renderbuffer);
    for(unsigned i=0;i<5;i++)glPixelStorei(unpack_names[i],unpack_values[i]);
    p_glBindBuffer(GL_PIXEL_PACK_BUFFER,pack);p_glBindBuffer(GL_PIXEL_UNPACK_BUFFER,unpack);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER,read_fbo);p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw_fbo);
    if(!ok)fprintf(stderr,"OpenGL: the frame buffer is incomplete\n");
    return ok;
}

uintptr_t eh_gl_current_framebuffer(void){return fbo;}

/* Starts copying the engine's finished frame (bottom-left origin) into a pixel buffer, and hands back the previous
   frame from the other one, top row first, as 0x00RRGGBB. Every piece of GL state touched here is put back: engines
   such as LRPS2 cache their bindings. */
/* Takes one copy out of its pixel buffer (top row first, 0x00RRGGBB): 1 delivered, 0 not ready, -1 failed. */
static int pbo_take(int slot,int wait,int bottom_left,uint32_t *out,unsigned *out_w,unsigned *out_h){
    if(pbo_fence[slot]){
        GLenum state=p_glClientWaitSync(pbo_fence[slot],GL_SYNC_FLUSH_COMMANDS_BIT,wait?(GLuint64)5000000000u:0);
        if(state==GL_TIMEOUT_EXPIRED)return 0;
        p_glDeleteSync(pbo_fence[slot]);pbo_fence[slot]=0;
    }
    unsigned w=pbo_w[slot],h=pbo_h[slot];
    p_glBindBuffer(GL_PIXEL_PACK_BUFFER,pbo[slot]);
    const uint32_t *pixels=p_glMapBufferRange(GL_PIXEL_PACK_BUFFER,0,(GLsizeiptr)w*h*4,GL_MAP_READ_BIT);
    pbo_pending[slot]=0;pbo_read=(slot+1)%PBO_SLOTS;
    if(!pixels)return -1;
    for(unsigned y=0;y<h;y++){
        const uint32_t *src=pixels+(size_t)(bottom_left?h-1-y:y)*w;uint32_t *dst=out+(size_t)y*w;
        for(unsigned x=0;x<w;x++)dst[x]=src[x]&0xffffff;
    }
    p_glUnmapBuffer(GL_PIXEL_PACK_BUFFER);*out_w=w;*out_h=h;return 1;
}
int eh_gl_read(unsigned width,unsigned height,int bottom_left,uint32_t *out,unsigned *out_w,unsigned *out_h){
    if(!gl_context||!width||!height||width>fbo_w||height>fbo_h)return -1;
    GLint read_fbo=0,pack_buffer=0,alignment=4,row_length=0,skip_rows=0,skip_pixels=0,swap_bytes=0,read_buffer=0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read_fbo);glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&pack_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT,&alignment);glGetIntegerv(GL_PACK_ROW_LENGTH,&row_length);
    glGetIntegerv(GL_PACK_SKIP_ROWS,&skip_rows);glGetIntegerv(GL_PACK_SKIP_PIXELS,&skip_pixels);glGetIntegerv(GL_PACK_SWAP_BYTES,&swap_bytes);
    glPixelStorei(GL_PACK_ALIGNMENT,4);glPixelStorei(GL_PACK_ROW_LENGTH,0);
    glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);glPixelStorei(GL_PACK_SWAP_BYTES,0);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER,fbo);glGetIntegerv(GL_READ_BUFFER,&read_buffer);glReadBuffer(GL_COLOR_ATTACHMENT0);
    int now=pbo_write,delivered=0;
    /* All copies still in flight: wait for the oldest and show it, so no frame is ever dropped. This only waits when
       the graphics card is two frames behind, and then it is the bottleneck anyway. */
    if(pbo_pending[now])delivered=pbo_take(now,1,bottom_left,out,out_w,out_h);
    if(delivered>=0){
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER,fbo);
        p_glBindBuffer(GL_PIXEL_PACK_BUFFER,pbo[now]);
        glReadPixels(0,0,width,height,GL_BGRA,GL_UNSIGNED_INT_8_8_8_8_REV,NULL); /* Queued; returns at once. */
        if(p_glFenceSync)pbo_fence[now]=p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
        pbo_w[now]=width;pbo_h[now]=height;pbo_pending[now]=1;pbo_write=(now+1)%PBO_SLOTS;
        /* Otherwise the oldest finished copy, one per frame for an even cadence; never wait for it. Without fences
           (GL before 3.2) a copy is taken one frame later. */
        if(!delivered&&pbo_pending[pbo_read]&&(pbo_fence[pbo_read]||pbo_read!=now))delivered=pbo_take(pbo_read,0,bottom_left,out,out_w,out_h);
    }
    glReadBuffer(read_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT,alignment);glPixelStorei(GL_PACK_ROW_LENGTH,row_length);
    glPixelStorei(GL_PACK_SKIP_ROWS,skip_rows);glPixelStorei(GL_PACK_SKIP_PIXELS,skip_pixels);glPixelStorei(GL_PACK_SWAP_BYTES,swap_bytes);
    p_glBindBuffer(GL_PIXEL_PACK_BUFFER,pack_buffer);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER,read_fbo);
    return delivered;
}

void eh_gl_destroy(void){
    if(gl_context){
        release_framebuffers();
        #ifdef _WIN32
        wglMakeCurrent(NULL,NULL);wglDeleteContext(gl_context);
        #else
        eh_desktop_gl_close();
        #endif
        gl_context=NULL;
    }
    #ifdef _WIN32
    if(gl_window){ReleaseDC(gl_window,gl_dc);DestroyWindow(gl_window);gl_window=NULL;}
    #endif
    fbo=color=depth=0;memset(pbo,0,sizeof(pbo));memset(pbo_pending,0,sizeof(pbo_pending));memset(pbo_fence,0,sizeof(pbo_fence));pbo_write=pbo_read=0;
}
