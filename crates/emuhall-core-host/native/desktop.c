/* Portable SDL3 desktop surface. The worker owns SDL and all callbacks stay on
   its main thread. The proven Win32 presenter remains the Windows default. */
#include "desktop.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
static void *open_library(void){return (void*)LoadLibraryExW(L"SDL3.dll",NULL,LOAD_LIBRARY_SEARCH_APPLICATION_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);}
static void *symbol(void *lib,const char *name){FARPROC f=GetProcAddress((HMODULE)lib,name);void *p=NULL;memcpy(&p,&f,sizeof(p));return p;}
static void close_library(void *lib){FreeLibrary((HMODULE)lib);}
#else
#include <dlfcn.h>
static void *open_library(void){
#ifdef __APPLE__
    void *lib=dlopen("@executable_path/libSDL3.dylib",RTLD_NOW|RTLD_LOCAL);if(!lib)lib=dlopen("libSDL3.dylib",RTLD_NOW|RTLD_LOCAL);
#else
    void *lib=dlopen("libSDL3.so.0",RTLD_NOW|RTLD_LOCAL);if(!lib)lib=dlopen("libSDL3.so",RTLD_NOW|RTLD_LOCAL);
#endif
    return lib;
}
static void *symbol(void *lib,const char *name){return dlsym(lib,name);}
static void close_library(void *lib){dlclose(lib);}
#endif
#define FUNCTIONS(X) \
 X(void,SDL_SetMainReady,(void)) X(bool,SDL_SetAppMetadata,(const char*,const char*,const char*)) X(bool,SDL_InitSubSystem,(SDL_InitFlags)) X(void,SDL_QuitSubSystem,(SDL_InitFlags)) X(const char*,SDL_GetError,(void)) \
 X(SDL_Window*,SDL_CreateWindow,(const char*,int,int,SDL_WindowFlags)) X(void,SDL_DestroyWindow,(SDL_Window*)) \
 X(SDL_Renderer*,SDL_CreateRenderer,(SDL_Window*,const char*)) X(void,SDL_DestroyRenderer,(SDL_Renderer*)) \
 X(SDL_Texture*,SDL_CreateTexture,(SDL_Renderer*,SDL_PixelFormat,SDL_TextureAccess,int,int)) X(void,SDL_DestroyTexture,(SDL_Texture*)) \
 X(bool,SDL_UpdateTexture,(SDL_Texture*,const SDL_Rect*,const void*,int)) X(bool,SDL_SetTextureScaleMode,(SDL_Texture*,SDL_ScaleMode)) \
 X(bool,SDL_SetRenderDrawColor,(SDL_Renderer*,Uint8,Uint8,Uint8,Uint8)) X(bool,SDL_RenderClear,(SDL_Renderer*)) \
 X(bool,SDL_RenderFillRect,(SDL_Renderer*,const SDL_FRect*)) X(bool,SDL_RenderDebugText,(SDL_Renderer*,float,float,const char*)) X(bool,SDL_SetRenderScale,(SDL_Renderer*,float,float)) \
 X(bool,SDL_RenderTexture,(SDL_Renderer*,SDL_Texture*,const SDL_FRect*,const SDL_FRect*)) X(bool,SDL_RenderPresent,(SDL_Renderer*)) \
 X(bool,SDL_SetRenderVSync,(SDL_Renderer*,int)) X(bool,SDL_GetRenderOutputSize,(SDL_Renderer*,int*,int*)) \
 X(bool,SDL_GetWindowSize,(SDL_Window*,int*,int*)) X(SDL_Window*,SDL_GetKeyboardFocus,(void)) \
 X(const bool*,SDL_GetKeyboardState,(int*)) X(SDL_MouseButtonFlags,SDL_GetMouseState,(float*,float*)) \
 X(bool,SDL_PollEvent,(SDL_Event*)) X(Uint64,SDL_GetTicksNS,(void)) X(void,SDL_Delay,(Uint32)) \
 X(bool,SDL_HideWindow,(SDL_Window*)) X(bool,SDL_ShowWindow,(SDL_Window*)) X(bool,SDL_RaiseWindow,(SDL_Window*)) \
 X(bool,SDL_SetWindowFullscreen,(SDL_Window*,bool)) \
 X(SDL_AudioStream*,SDL_OpenAudioDeviceStream,(SDL_AudioDeviceID,const SDL_AudioSpec*,SDL_AudioStreamCallback,void*)) \
 X(bool,SDL_ResumeAudioStreamDevice,(SDL_AudioStream*)) X(bool,SDL_PauseAudioStreamDevice,(SDL_AudioStream*)) \
 X(bool,SDL_SetAudioStreamFrequencyRatio,(SDL_AudioStream*,float)) X(bool,SDL_ClearAudioStream,(SDL_AudioStream*)) X(void,SDL_DestroyAudioStream,(SDL_AudioStream*)) \
 X(int,SDL_GetAudioStreamQueued,(SDL_AudioStream*)) X(bool,SDL_PutAudioStreamData,(SDL_AudioStream*,const void*,int)) \
 X(bool,SDL_GL_SetAttribute,(SDL_GLAttr,int)) X(SDL_GLContext,SDL_GL_CreateContext,(SDL_Window*)) \
 X(SDL_Window*,SDL_GL_GetCurrentWindow,(void)) X(SDL_GLContext,SDL_GL_GetCurrentContext,(void)) \
 X(bool,SDL_GL_MakeCurrent,(SDL_Window*,SDL_GLContext)) X(bool,SDL_GL_DestroyContext,(SDL_GLContext)) X(SDL_FunctionPointer,SDL_GL_GetProcAddress,(const char*))
#define DECLARE(ret,name,args) static ret (SDLCALL *p_##name)args;
FUNCTIONS(DECLARE)
static void *library;static int video_ready,audio_ready,fullscreen;
static SDL_Window *window,*gl_window;static SDL_GLContext gl_context;
static SDL_Renderer *renderer;static SDL_Texture *texture;static SDL_AudioStream *audio;
static unsigned texture_w,texture_h;static unsigned source_rate=48000;static int touch_system;static SDL_FRect picture;static int output_w,output_h;
static int ensure(void){
    if(library){return 1;}
    library=open_library();if(!library){fprintf(stderr,"SDL3 runtime is unavailable\n");return 0;}
    int ok=1;
#define LOAD(ret,name,args) {void *f=symbol(library,#name);if(!f)ok=0;else memcpy(&p_##name,&f,sizeof(p_##name));}
    FUNCTIONS(LOAD)
    if(!ok){close_library(library);library=NULL;fprintf(stderr,"SDL3 runtime is too old or incomplete\n");return 0;}return 1;
}
static int video_init(void){if(!ensure())return 0;if(!video_ready){p_SDL_SetMainReady();p_SDL_SetAppMetadata("EmuHall","0.1.0-dev","dev.emuhall.player");if(!p_SDL_InitSubSystem(SDL_INIT_VIDEO))return 0;video_ready=1;}return 1;}
double eh_desktop_now(void){return ensure()?p_SDL_GetTicksNS()/1000000.0:0;}
int eh_desktop_open(double rate,int hidden){
    if(!video_init())return 0;
    const char *system=getenv("EMUHALL_SYSTEM");touch_system=system&&(!strcmp(system,"nds")||!strcmp(system,"3ds"));
    source_rate=(unsigned)rate;
    window=p_SDL_CreateWindow("EmuHall",960,720,SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY|(hidden?SDL_WINDOW_HIDDEN:0));if(!window)return 0;
    renderer=p_SDL_CreateRenderer(window,NULL);if(!renderer)return 0;
    p_SDL_SetRenderVSync(renderer,0); /* Core-rate scheduling is independent of monitor refresh. */
    if(p_SDL_InitSubSystem(SDL_INIT_AUDIO)){
        audio_ready=1;SDL_AudioSpec spec;memset(&spec,0,sizeof(spec));spec.format=SDL_AUDIO_S16;spec.channels=2;spec.freq=(int)rate;
        audio=p_SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,NULL,NULL);
        if(audio)p_SDL_ResumeAudioStreamDevice(audio);else fprintf(stderr,"SDL audio unavailable: %s\n",p_SDL_GetError());
    }
    if(gl_context)p_SDL_GL_MakeCurrent(gl_window,gl_context);
    return 1;
}
int eh_desktop_focus(void){return window&&p_SDL_GetKeyboardFocus()==window;}
int eh_desktop_poll(void){
    if(!window){return 0;}
    SDL_Event e;int result=0;
    while(p_SDL_PollEvent(&e)){
        if(e.type==SDL_EVENT_QUIT||e.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED)result|=2;
        if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN&&e.button.button==SDL_BUTTON_LEFT&&e.button.clicks==2&&!touch_system){fullscreen=!fullscreen;p_SDL_SetWindowFullscreen(window,fullscreen);}
        if(e.type==SDL_EVENT_KEY_DOWN&&!e.key.repeat){
            if(e.key.scancode==SDL_SCANCODE_ESCAPE||e.key.scancode==SDL_SCANCODE_SPACE)result|=1;
            if(e.key.scancode==SDL_SCANCODE_F11){fullscreen=!fullscreen;p_SDL_SetWindowFullscreen(window,fullscreen);}
        }
    }return result;
}
int eh_desktop_key(int key){
    if(!eh_desktop_focus()){return 0;}
    SDL_Scancode sc=SDL_SCANCODE_UNKNOWN;
    if(key>='A'&&key<='Z')sc=(SDL_Scancode)(SDL_SCANCODE_A+key-'A');
    else if(key>='1'&&key<='9')sc=(SDL_Scancode)(SDL_SCANCODE_1+key-'1');
    else if(key==48)sc=SDL_SCANCODE_0;
    else if(key>=96&&key<=105)sc=key==96?SDL_SCANCODE_KP_0:(SDL_Scancode)(SDL_SCANCODE_KP_1+key-97);
    else if(key>=112&&key<=123)sc=(SDL_Scancode)(SDL_SCANCODE_F1+key-112);
    else switch(key){
        case 13:sc=SDL_SCANCODE_RETURN;break;case 8:sc=SDL_SCANCODE_BACKSPACE;break;case 9:sc=SDL_SCANCODE_TAB;break;
        case 37:sc=SDL_SCANCODE_LEFT;break;case 38:sc=SDL_SCANCODE_UP;break;case 39:sc=SDL_SCANCODE_RIGHT;break;case 40:sc=SDL_SCANCODE_DOWN;break;
        case 160:sc=SDL_SCANCODE_LSHIFT;break;case 161:sc=SDL_SCANCODE_RSHIFT;break;case 162:sc=SDL_SCANCODE_LCTRL;break;case 163:sc=SDL_SCANCODE_RCTRL;break;
        case 164:sc=SDL_SCANCODE_LALT;break;case 165:sc=SDL_SCANCODE_RALT;break;case 188:sc=SDL_SCANCODE_COMMA;break;case 190:sc=SDL_SCANCODE_PERIOD;break;
        case 191:sc=SDL_SCANCODE_SLASH;break;case 186:sc=SDL_SCANCODE_SEMICOLON;break;case 222:sc=SDL_SCANCODE_APOSTROPHE;break;
        case 219:sc=SDL_SCANCODE_LEFTBRACKET;break;case 221:sc=SDL_SCANCODE_RIGHTBRACKET;break;case 220:sc=SDL_SCANCODE_BACKSLASH;break;
        case 189:sc=SDL_SCANCODE_MINUS;break;case 187:sc=SDL_SCANCODE_EQUALS;break;case 192:sc=SDL_SCANCODE_GRAVE;break;
    }
    int count=0;const bool *keys=p_SDL_GetKeyboardState(&count);
    if(key==16)return keys&&count>SDL_SCANCODE_RSHIFT&&(keys[SDL_SCANCODE_LSHIFT]||keys[SDL_SCANCODE_RSHIFT]);
    return keys&&sc>SDL_SCANCODE_UNKNOWN&&(int)sc<count&&keys[sc];
}
int16_t eh_desktop_pointer(unsigned id){
    if(id==3)return window?1:0;
    if(!window||!picture.w||!picture.h)return id==2?0:-32768;
    float x=0,y=0;SDL_MouseButtonFlags buttons=p_SDL_GetMouseState(&x,&y);int ww=0,wh=0;
    p_SDL_GetWindowSize(window,&ww,&wh);if(ww<=0||wh<=0)return 0;x=x*output_w/ww-picture.x;y=y*output_h/wh-picture.y;
    int inside=x>=0&&y>=0&&x<picture.w&&y<picture.h;
    if(id==2)return inside&&(buttons&SDL_BUTTON_LMASK)?1:0;
    if(!inside)return -32768;
    return id==0?(int16_t)(x*65534/picture.w-32767):id==1?(int16_t)(y*65534/picture.h-32767):0;
}
int eh_desktop_video(const uint32_t *pixels,unsigned w,unsigned h,int integer,int smooth){
    if(!renderer||!pixels||!w||!h||w>4096||h>4096)return 0;
    if(w!=texture_w||h!=texture_h){if(texture)p_SDL_DestroyTexture(texture);texture=p_SDL_CreateTexture(renderer,SDL_PIXELFORMAT_XRGB8888,SDL_TEXTUREACCESS_STREAMING,(int)w,(int)h);texture_w=w;texture_h=h;}
    if(!texture||!p_SDL_UpdateTexture(texture,NULL,pixels,(int)w*4))return 0;
    p_SDL_SetTextureScaleMode(texture,smooth?SDL_SCALEMODE_LINEAR:SDL_SCALEMODE_NEAREST);
    if(!p_SDL_GetRenderOutputSize(renderer,&output_w,&output_h)||output_w<=0||output_h<=0)return 0;
    float scale=fminf((float)output_w/w,(float)output_h/h);if(integer&&scale>=1)scale=floorf(scale);
    picture.w=w*scale;picture.h=h*scale;picture.x=(output_w-picture.w)/2;picture.y=(output_h-picture.h)/2;
    extern int eh_bezel_style(void);int bezel=eh_bezel_style();
    int ok=p_SDL_SetRenderDrawColor(renderer,bezel==2?22:0,bezel==1?17:0,bezel?30:0,255)&&p_SDL_RenderClear(renderer)&&p_SDL_RenderTexture(renderer,texture,NULL,&picture);
    if(bezel){SDL_FRect left={picture.x-4,picture.y,2,picture.h},right={picture.x+picture.w+2,picture.y,2,picture.h};p_SDL_SetRenderDrawColor(renderer,bezel==2?135:30,bezel==2?50:100,190,255);if(picture.x>=6){p_SDL_RenderFillRect(renderer,&left);p_SDL_RenderFillRect(renderer,&right);}}
    extern int eh_pause_hint_visible(void);
    if(eh_pause_hint_visible()){
        float scale=output_w>=500?1.5f:1.0f;p_SDL_SetRenderScale(renderer,scale,scale);
        SDL_FRect card={12,12,fminf(370,(float)output_w/scale-24),48};
        p_SDL_SetRenderDrawColor(renderer,8,18,32,255);p_SDL_RenderFillRect(renderer,&card);
        p_SDL_SetRenderDrawColor(renderer,225,241,255,255);p_SDL_RenderDebugText(renderer,22,24,"Pressione Select + Start\npara abrir o menu de pausa.");
        p_SDL_SetRenderScale(renderer,1,1);
    }
    extern int eh_fps_overlay(void);int measured=eh_fps_overlay();if(measured>=0){char label[40];snprintf(label,sizeof(label),"%.1f FPS",measured/1000.0);SDL_FRect card={(float)output_w-112,16,96,28};p_SDL_SetRenderDrawColor(renderer,8,18,32,255);p_SDL_RenderFillRect(renderer,&card);p_SDL_SetRenderDrawColor(renderer,225,241,255,255);p_SDL_RenderDebugText(renderer,card.x+8,card.y+9,label);}
    ok=ok&&p_SDL_RenderPresent(renderer);
    if(gl_context)p_SDL_GL_MakeCurrent(gl_window,gl_context);
    return ok;
}
void eh_desktop_audio(const int16_t *samples,size_t frames,unsigned volume,int mute,unsigned speed){
    if(!audio||!samples||frames>1024*1024)return;
    // Bound queued input to 100 ms. A stalled device must not grow memory forever.
    p_SDL_SetAudioStreamFrequencyRatio(audio,speed/100.0f);
    int queued=p_SDL_GetAudioStreamQueued(audio);if(queued>(int)(source_rate*4*speed/100/10)){p_SDL_ClearAudioStream(audio);}
    if(mute)return;
    if(volume==100){p_SDL_PutAudioStreamData(audio,samples,(int)frames*4);return;}
    int16_t block[2048];size_t offset=0;
    while(offset<frames){size_t count=frames-offset;if(count>1024)count=1024;for(size_t i=0;i<count*2;i++)block[i]=(int16_t)(samples[offset*2+i]*volume/100);p_SDL_PutAudioStreamData(audio,block,(int)count*4);offset+=count;}
}
void eh_desktop_pause(int paused){if(audio){p_SDL_ClearAudioStream(audio);if(paused)p_SDL_PauseAudioStreamDevice(audio);else p_SDL_ResumeAudioStreamDevice(audio);}if(window){if(paused)p_SDL_HideWindow(window);else{p_SDL_ShowWindow(window);p_SDL_RaiseWindow(window);}}}
int eh_desktop_gl_create(int core,unsigned major,unsigned minor){
    if(gl_context){return 1;}
    if(!video_init())return 0;
#ifdef __APPLE__
    if(core&&(major<4||(major==4&&minor<=1))){major=4;minor=1;}
#endif
    p_SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,(int)major);p_SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,(int)minor);
    p_SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,core?SDL_GL_CONTEXT_PROFILE_CORE:SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
#ifdef __APPLE__
    p_SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    gl_window=p_SDL_CreateWindow("EmuHall graphics",16,16,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);if(!gl_window)return 0;
    gl_context=p_SDL_GL_CreateContext(gl_window);if(!gl_context){fprintf(stderr,"SDL OpenGL %u.%u unavailable: %s\n",major,minor,p_SDL_GetError());p_SDL_DestroyWindow(gl_window);gl_window=NULL;return 0;}return 1;
}
void *eh_desktop_gl_proc(const char *name){if(!library)return NULL;SDL_FunctionPointer fn=p_SDL_GL_GetProcAddress(name);void *out=NULL;memcpy(&out,&fn,sizeof(out));return out;}
void eh_desktop_gl_close(void){if(gl_context)p_SDL_GL_DestroyContext(gl_context);gl_context=NULL;if(gl_window)p_SDL_DestroyWindow(gl_window);gl_window=NULL;}
void eh_desktop_close(void){
    if(audio){p_SDL_DestroyAudioStream(audio);}audio=NULL;
    if(texture){p_SDL_DestroyTexture(texture);}texture=NULL;
    if(renderer){p_SDL_DestroyRenderer(renderer);}renderer=NULL;
    if(window){p_SDL_DestroyWindow(window);}window=NULL;
    if(audio_ready){p_SDL_QuitSubSystem(SDL_INIT_AUDIO);}
    if(video_ready){p_SDL_QuitSubSystem(SDL_INIT_VIDEO);}audio_ready=video_ready=0;
    if(library){close_library(library);}library=NULL;
}
static SDL_Window *shader_window,*shader_previous_window;static SDL_GLContext shader_context,shader_previous_context;
int eh_desktop_shader_begin(void){
 if(!video_init()){return 0;}
 shader_previous_window=p_SDL_GL_GetCurrentWindow();shader_previous_context=p_SDL_GL_GetCurrentContext();
 if(!shader_context){p_SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
 p_SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,4);p_SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,1);
#else
 p_SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);p_SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
#endif
 p_SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT,0);shader_window=p_SDL_CreateWindow("EmuHall shaders",16,16,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);if(!shader_window)return 0;shader_context=p_SDL_GL_CreateContext(shader_window);if(!shader_context)return 0;
 }return p_SDL_GL_MakeCurrent(shader_window,shader_context);
}
void eh_desktop_shader_end(void){if(library)p_SDL_GL_MakeCurrent(shader_previous_window,shader_previous_context);}
void eh_desktop_shader_close(void){if(shader_context)p_SDL_GL_DestroyContext(shader_context);shader_context=NULL;if(shader_window)p_SDL_DestroyWindow(shader_window);shader_window=NULL;}
int eh_desktop_smoke(void){
    if(!eh_desktop_open(48000,1)){eh_desktop_close();return 0;}
    uint32_t pixels[16];for(int i=0;i<16;i++)pixels[i]=(i%2)?0x289eff:0x102844;
    int ok=eh_desktop_video(pixels,4,4,1,0);int16_t samples[128]={0};eh_desktop_audio(samples,64,100,0,100);eh_desktop_close();return ok;
}
