#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
/* EmuHall's own host; the Libretro ABI is defined only by the pinned upstream header. */
#include "libretro.h"
#include "gamepad.h"
#include "shaders.h"
#include "runtime.h"
#include "desktop.h"
#include "glhost.h"
#include "d3dhost.h"
#include "vkhost.h"
#include "timestretch.h"
#include "audio_pacing.h"
#include "picture_layout.h"
#include "achievements.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
static atomic_uint_fast64_t pause_hint_until;
static int pause_hint_offered;
static const char *hw_renderer_error;
static atomic_int fps_visible,fps_milli,bezel_style;
static int max_pending=2;
int eh_bezel_style(void){return atomic_load(&bezel_style);}
int eh_fps_overlay(void){return atomic_load(&fps_visible)?atomic_load(&fps_milli):-1;}
static eh_pad pad;
static int frame_ready;
static unsigned repeat_reset=1;
static unsigned hitches,underruns,audio_gaps;static double worst_frame_ms,emulate_total_ms,emulate_worst_ms,audio_api_total_ms;
static int pad_map[16]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
static uint16_t mapped_pad_value(const eh_pad *input){uint16_t result=0;for(unsigned i=0;i<16;i++)if(input->buttons&(1u<<pad_map[i]))result|=1u<<i;return result;}
static uint16_t mapped_pad(void){return mapped_pad_value(&pad);}
static int key_map[16]={'Z','A',16,13,38,40,37,39,'X','S','Q','W','E','R','D','F'};
static const char *const key_names[16]={"b","y","select","start","up","down","left","right","a","x","l","r","l2","r2","l3","r3"};
static int system_keys[21]={9,'1','2','3','4','5','6','7','8','9','0',112,113,114,115,116,117,118,119,120,121};
static const char *system_names[21]={"fast-forward","save.1","save.2","save.3","save.4","save.5","save.6","save.7","save.8","save.9","save.10","load.1","load.2","load.3","load.4","load.5","load.6","load.7","load.8","load.9","load.10"};
static int system_key(int key){for(unsigned i=0;i<21;i++)if(system_keys[i]==key)return 1;return key==27||key==32;}
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <io.h>
#include <dwmapi.h>
static HWND window,parent_window;
static HWAVEOUT wave;
/* Audio is resampled to 48 kHz into a ring and handed to waveOut in small blocks. The sound card's play position
   is the session's clock: see pace_frame(). */
#define OUT_RATE 48000
#define BLOCK_FRAMES 256
#define BLOCKS 32
#define RING_FRAMES 32768
#define AUDIO_TARGET_MS 96.0
static int16_t audio_ring[RING_FRAMES*2],audio_blocks[BLOCKS][BLOCK_FRAMES*2];
static size_t ring_read,ring_count;
static WAVEHDR audio_headers[BLOCKS];
static uint64_t frames_submitted,position_base;
static uint32_t last_position;
/* Blocks go to the sound card from a thread of their own: waveOut calls cost about 1 ms a frame, and inside the engine's
   frame that time is taken from emulation. The ring and counters are shared under audio_lock. */
static CRITICAL_SECTION audio_lock;static HANDLE audio_wake,audio_thread;static volatile LONG audio_stop;
static double core_rate,resample_pos,rate_ratio=1.0;
/* Below 1 when emulation falls behind real time: the sound is stretched (timestretch.c) instead of running dry. */
static double audio_tempo=1.0;
static double audio_work_ratio=1.0;
static int audio_prime_enabled,audio_priming;
static int16_t prev_l,prev_r;
static int vsync_mode;
/* The display's real vertical blank, via the kernel graphics thunks in gdi32. DwmFlush is only a fallback: the
   compositor skips passes when nothing changes, which makes it an unreliable clock. */
typedef struct{HDC hDc;UINT hAdapter;LUID AdapterLuid;UINT VidPnSourceId;}EhOpenAdapterFromHdc;
typedef struct{UINT hAdapter;UINT hDevice;UINT VidPnSourceId;}EhWaitForVblank;
typedef LONG (APIENTRY *OpenAdapterFn)(EhOpenAdapterFromHdc*);
typedef LONG (APIENTRY *WaitVblankFn)(const EhWaitForVblank*);
static WaitVblankFn wait_vblank;static EhWaitForVblank vblank;
static double refresh_ms;
/* Interactive playback must not be treated as background work. The worker never owns the foreground window when
   embedded, so without this Windows may power-throttle it on battery, and ordinary threads can preempt a frame. */
static void prefer_realtime_scheduling(void){
    typedef struct{ULONG Version,ControlMask,StateMask;}EhPowerThrottling;
    typedef BOOL (WINAPI *SetProcessInformationFn)(HANDLE,int,LPVOID,DWORD);
    FARPROC symbol=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"SetProcessInformation");
    if(symbol){SetProcessInformationFn set_info;memcpy(&set_info,&symbol,sizeof(set_info));
        EhPowerThrottling state={1,1,0}; /* Execution-speed throttling: controlled by us, and off. */
        if(!set_info(GetCurrentProcess(),4 /* ProcessPowerThrottling */,&state,sizeof(state)))fprintf(stderr,"Power throttling opt-out failed: %lu\n",GetLastError());}
    SetPriorityClass(GetCurrentProcess(),ABOVE_NORMAL_PRIORITY_CLASS);
    HMODULE avrt=LoadLibraryExW(L"avrt.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(avrt){typedef HANDLE (WINAPI *AvSetFn)(LPCWSTR,LPDWORD);FARPROC av_symbol=GetProcAddress(avrt,"AvSetMmThreadCharacteristicsW");
        if(av_symbol){AvSetFn av_set;memcpy(&av_set,&av_symbol,sizeof(av_set));DWORD task=0;av_set(L"Games",&task);}}
}
static void open_vblank(HWND nearby){
    HMODULE gdi=GetModuleHandleW(L"gdi32.dll");if(!gdi)return;
    FARPROC open_symbol=GetProcAddress(gdi,"D3DKMTOpenAdapterFromHdc"),wait_symbol=GetProcAddress(gdi,"D3DKMTWaitForVerticalBlankEvent");if(!open_symbol||!wait_symbol)return;
    OpenAdapterFn open_adapter;memcpy(&open_adapter,&open_symbol,sizeof(open_adapter));
    MONITORINFOEXW monitor;memset(&monitor,0,sizeof(monitor));monitor.cbSize=sizeof(monitor);
    if(!GetMonitorInfoW(MonitorFromWindow(nearby,MONITOR_DEFAULTTOPRIMARY),(MONITORINFO*)&monitor))return;
    HDC dc=CreateDCW(L"DISPLAY",monitor.szDevice,NULL,NULL);if(!dc)return;
    EhOpenAdapterFromHdc request;memset(&request,0,sizeof(request));request.hDc=dc;
    if(open_adapter(&request)==0){vblank.hAdapter=request.hAdapter;vblank.VidPnSourceId=request.VidPnSourceId;memcpy(&wait_vblank,&wait_symbol,sizeof(wait_vblank));}
    DeleteDC(dc);
}
/* Smoothness measurements reported when the session ends. */

static double ms_vblank,ms_audio_wait,ms_run,ms_present,ms_input;static char hitch_log[16][160]; /* Where the last frame's time went, for hitch reports on stderr. */
static double now_ms(void){LARGE_INTEGER f,t;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&t);return (double)t.QuadPart*1000.0/f.QuadPart;}

/* Where the picture was last drawn in the game window (client pixels), so the mouse can touch the DS screen. */
static volatile LONG view_x,view_y,view_w,view_h,view_nds_layout;
/* Keyboard keys (Windows virtual-key codes) for the RetroPad buttons, in RetroPad order: B Y Select Start Up Down
   Left Right A X L R L2 R2 L3 R3. "set key.<button> <code>" changes one. */
static CRITICAL_SECTION paint_lock;
static char system_notice[128];static double system_notice_until;
static CRITICAL_SECTION queue_lock;
#define LOCK() EnterCriticalSection(&queue_lock)
#define UNLOCK() LeaveCriticalSection(&queue_lock)
static void nap(unsigned ms){Sleep(ms);}
#define SEP "\\"
static wchar_t *wide(const char *s){int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,NULL,0);if(!n)return NULL;wchar_t *p=calloc(n,sizeof(wchar_t));if(p)MultiByteToWideChar(CP_UTF8,0,s,-1,p,n);return p;}
static FILE *open_file(const char *path,const char *mode){wchar_t *p=wide(path),*m=wide(mode);FILE *f=p&&m?_wfopen(p,m):NULL;free(p);free(m);return f;}
#else
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
static pthread_mutex_t queue_lock=PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&queue_lock)
#define UNLOCK() pthread_mutex_unlock(&queue_lock)
static void nap(unsigned ms){struct timespec t={(time_t)(ms/1000),(long)(ms%1000)*1000000L};while(nanosleep(&t,&t)&&errno==EINTR){}}
/* Frame pacing sleeps until the frame's deadline on CLOCK_MONOTONIC, the clock now_ms() reads. Not whole milliseconds
   (truncating 16.7 ms to 16 made frames uneven), and not SDL's clock: SDL reads CLOCK_MONOTONIC_RAW, which the kernel
   does not correct, and on machines with a poorly calibrated timer (measured in WSL: 5.6% fast) games ran too fast. */
/* EMUHALL_PACE_DEBUG=1: once a second, where each frame's time went (stderr). */
static int pace_debug;static unsigned pace_frames,pace_late,pace_resets;static double pace_present_ms,pace_sleep_ms,pace_since;
static double now_ms(void);
static void sleep_until_ms(double deadline_ms){
    if(deadline_ms<=now_ms())return;
    struct timespec t;t.tv_sec=(time_t)(deadline_ms/1000.0);t.tv_nsec=(long)((deadline_ms-t.tv_sec*1000.0)*1000000.0);
    if(t.tv_nsec<0)t.tv_nsec=0;if(t.tv_nsec>999999999L)t.tv_nsec=999999999L;
    while(clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,NULL)==EINTR){}
}
/* The system's monotonic clock, so timing works headless and before (or without) SDL. */
static double now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000.0+t.tv_nsec/1e6;}
#define SEP "/"
#define open_file fopen
#endif
/* Modes: 0 headless, 1 standalone window, 2 window embedded in the launcher, 3 headless with controls. */
enum{MODE_HEADLESS,MODE_WINDOW,MODE_EMBED,MODE_CONTROL,MODE_INSPECT};
static unsigned pixel_format,video_frames,polls;
static size_t audio_frames;
static uint32_t frame_hash=2166136261u;
static int quit,fault,show_window,controlled,embedded,paused,fast_forward;
static int diagnostic_mode,diagnostic_mask=-1;
static int variable_states;
static unsigned controller_ports=1,controller_devices[4]={RETRO_DEVICE_JOYPAD,RETRO_DEVICE_NONE,RETRO_DEVICE_NONE,RETRO_DEVICE_NONE};
static int temporal_supported,runahead_supported,rewind_enabled,runahead_enabled,speculating;
static void eventf(const char*,...);
static unsigned screen_filter;static int shader_failed;
static void presentation_filter(uint32_t *pixels,unsigned width,unsigned height){if(screen_filter<3)eh_video_filter(pixels,width,height,screen_filter);else if(!shader_failed&&!eh_shader_apply(pixels,width,height,screen_filter-2)){shader_failed=1;eventf("{\"event\":\"error\",\"message\":\"This graphics device could not run the shader preset. Showing the original picture.\"}");}}
static int cached_keys[16];
static int netplay_active;static uint16_t netplay_inputs[2];
extern int eh_netplay_start(void);
extern void eh_netplay_finish(void);
extern int64_t eh_netplay_frame(unsigned frame,uint16_t input,uint32_t picture);
static unsigned fast_forward_speed=200;
static int fast_forward_mute=1;
static volatile int menu_requested;
static uint32_t *framebuffer;
/* Frame buffers hold the largest frame the engine declares (at least 2048, at most 4096 a side): PCEE2 declares
   2560x1920 for its 4x upscale. */
#define FRAME_LIMIT 4096
static unsigned frame_cap_w=2048,frame_cap_h=2048;
static unsigned frame_width,frame_height;
static const char *system_dir=".";
/* Engines such as PCSX2 lay states out differently per renderer; say so instead of a generic failure. */
#ifdef _WIN32
#define WINDOWS_RENDERERS ", Vulkan, Direct3D 11 or Direct3D 12"
#else
#define WINDOWS_RENDERERS " or Vulkan"
#endif
#define SETTINGS_STATE_MESSAGE "This state was saved with different emulator settings (the renderer, for example). Switch them back in Game engines to load it."
static char profile_dir[4096];
static char option_keys[128][128],option_values[128][256];
/* Engines that draw with the graphics card (OpenGL, glhost.c). */
static void *resume_state;static size_t resume_size;static char resume_path[4300]; /* a verified state applied after the first frame */
/* Direct3D 11 frames shown on the graphics card (d3dhost.c) instead of copied back: see gpu_frame(). */
static int gpu_present;static int gpu_frame(unsigned width,unsigned height);
static const char *callback_error;
#ifndef _WIN32
static int gpu_frame(unsigned width,unsigned height){(void)width;(void)height;return 0;}
#endif
static struct retro_hw_render_callback hw;static int hw_active,hw_vk;static unsigned hw_d3d; /* RETRO_HW_CONTEXT_D3D11/_D3D12 when Direct3D draws */
static uintptr_t RETRO_CALLCONV hw_framebuffer(void){return eh_gl_current_framebuffer();}
static unsigned option_count;
static char option_labels[128][256],choice_values[128][128][256],choice_labels[128][128][256];
static unsigned choice_counts[128];
static void copy_choices(unsigned index,const char *label,const struct retro_core_option_value *values){
    snprintf(option_labels[index],256,"%s",label?label:option_keys[index]);choice_counts[index]=0;
    for(unsigned j=0;j<128&&values[j].value;j++){
        snprintf(choice_values[index][j],256,"%s",values[j].value);
        snprintf(choice_labels[index][j],256,"%s",values[j].label?values[j].label:values[j].value);choice_counts[index]++;
    }
}
static struct retro_game_geometry current_geometry;static int geometry_reported;
static _Atomic float display_aspect;
static struct retro_disk_control_callback disks;
static int audio_volume=100,audio_muted;
typedef void (RETRO_CALLCONV *cheat_set_fn)(unsigned,bool,const char*);
typedef void (RETRO_CALLCONV *cheat_reset_fn)(void);
typedef bool (*typed_cheat_fn)(unsigned,bool,unsigned,const char*);
static typed_cheat_fn typed_cheat;
static cheat_set_fn set_cheat;static cheat_reset_fn reset_cheats;
/* Cheats sent before the first frame are replayed once after it: some engines create their console only on the first
   frame (melonDS DS with a graphics-card renderer) and silently drop earlier cheats. Replaying is safe because engines
   replace cheats by index. */
static char *early_cheat[32];static unsigned char early_enabled[32];static int early_cheats_pending;
static char *copy_text(const char *text){size_t n=strlen(text)+1;char *copy=malloc(n);if(copy)memcpy(copy,text,n);return copy;}
static void early_cheats_clear(void){for(unsigned i=0;i<32;i++){free(early_cheat[i]);early_cheat[i]=NULL;}early_cheats_pending=0;}
/* Engine settings the launcher chose, in RetroArch's format (key = "value"), from core-options.cfg in the profile
   folder. They win over the engine's defaults. */
static char override_keys[256][128],override_values[256][256];
static unsigned override_count;
#ifdef _WIN32 /* read only by the Windows presenter's screen layouts so far */
static const char *core_option(const char *key,const char *fallback){
    for(unsigned i=0;i<override_count;i++)if(!strcmp(key,override_keys[i]))return override_values[i];
    for(unsigned i=0;i<option_count;i++)if(!strcmp(key,option_keys[i]))return option_values[i];
    return fallback;
}
#endif
/* Set by "core-option KEY VALUE" while playing; the engine reads it through GET_VARIABLE_UPDATE and re-reads its options. */
static int options_changed;
static void set_option_override(const char *key,const char *value){
    if(!key[0]||strlen(key)>=128||strlen(value)>=256)return;
    for(unsigned i=0;i<override_count;i++)if(!strcmp(key,override_keys[i])){
        if(strcmp(override_values[i],value)){snprintf(override_values[i],256,"%s",value);options_changed=1;}
        return;
    }
    if(override_count<256){snprintf(override_keys[override_count],128,"%s",key);snprintf(override_values[override_count],256,"%s",value);override_count++;options_changed=1;}
}
static void load_option_overrides(void){
    char path[4200];snprintf(path,sizeof(path),"%s" SEP "core-options.cfg",system_dir);
    FILE *f=open_file(path,"rb");if(!f)return;char line[512];
    while(override_count<256&&fgets(line,sizeof(line),f)){
        char key[128],value[256];
        if(sscanf(line," %127[^= \t] = \"%255[^\"]\"",key,value)==2){
            snprintf(override_keys[override_count],128,"%s",key);snprintf(override_values[override_count],256,"%s",value);override_count++;}
    }
    fclose(f);
}

/* Commands arrive on stdin from the launcher, one per line, and run between frames on this thread. */
static char queue[128][8192];
static unsigned queue_head,queue_tail;
static void queue_push(const char *line){LOCK();if((queue_tail+1)%128!=queue_head){snprintf(queue[queue_tail],sizeof(queue[0]),"%s",line);queue_tail=(queue_tail+1)%128;}UNLOCK();}
static int queue_pop(char *out){LOCK();int ok=queue_head!=queue_tail;if(ok){memcpy(out,queue[queue_head],sizeof(queue[0]));queue_head=(queue_head+1)%128;}UNLOCK();return ok;}
/* Watchdog: the engine runs on the main thread, which also drives the window. If a frame never returns (a graphics
   driver fault inside the engine, for one) or quitting does not finish, the window would freeze and outlive Alt+F4 and
   the launcher's quit. The watchdog ends the process instead; the launcher then reports the session as ended. */
static volatile int quit_asked,in_size_move,watchdog_armed;static volatile unsigned heartbeat;
#define STALL_MS 15000 /* one frame this long is a hang; shader compiles and loads take far less */
#define QUIT_MS 8000
static void watchdog_end(int code,const char *why){
    fprintf(stderr,"%s\n",why);fflush(stderr);fflush(stdout);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(),(UINT)code);
#else
    _exit(code);
#endif
}
static void watchdog_loop(void){
    unsigned seen=heartbeat;double still=0,quitting=0;
    for(;;){
#ifdef _WIN32
        Sleep(250);
#else
        nap(250);
#endif
        double now=now_ms();
        if(!watchdog_armed){still=quitting=0;continue;}
        if(quit||quit_asked){if(!quitting)quitting=now;else if(now-quitting>QUIT_MS)watchdog_end(fault||eh_vk_lost()?1:0,"The engine did not stop within 8 seconds; the player was closed");}
        if(heartbeat!=seen||in_size_move){seen=heartbeat;still=now;continue;}
        if(!still)still=now;
        else if(now-still>STALL_MS)watchdog_end(1,"The engine stopped responding for 15 seconds (often a graphics driver fault); the game was stopped");
    }
}
#ifdef _WIN32
static DWORD WINAPI watchdog_thread(LPVOID unused){(void)unused;watchdog_loop();return 0;}
#else
static void *watchdog_thread(void *unused){(void)unused;watchdog_loop();return NULL;}
#endif
static void take_command(char *line){line[strcspn(line,"\r\n")]=0;char *text=line;if(!memcmp(text,"\xEF\xBB\xBF",3))text+=3; /* Tolerate a UTF-8 byte order mark. */if(text[0]){if(!strcmp(text,"quit"))quit_asked=1;queue_push(text);}}
static void read_commands(void){
    char line[8192];
#ifdef _WIN32
    while(fgets(line,sizeof(line),stdin))take_command(line);
#else
    /* read(2), not stdio: a thread waiting in fgets holds stdin's lock, and exit() on glibc 2.36 and later flushes every
       stream under its lock, so every game's player hung at exit until the watchdog closed it (all but Ubuntu 22.04). */
    char chunk[4096];size_t used=0;
    for(;;){
        ssize_t n=read(0,chunk,sizeof(chunk));
        if(n<0&&errno==EINTR)continue;
        if(n<=0)break;
        for(ssize_t i=0;i<n;i++){
            if(chunk[i]=='\n'){line[used]=0;take_command(line);used=0;}
            else if(used<sizeof(line)-1)line[used++]=chunk[i];
        }
    }
    if(used){line[used]=0;take_command(line);}
#endif
    quit_asked=1;queue_push("quit"); /* The launcher went away. */
}
#ifdef _WIN32
static DWORD WINAPI command_reader(LPVOID unused){(void)unused;read_commands();return 0;}
#else
static void *command_reader(void *unused){(void)unused;read_commands();return NULL;}
#endif
/* Events go to stdout as one JSON object per line; the final statistics line has no "event" key. */
/* RetroAchievements (achievements.c). */
void eh_ra_set_core(const char *name,const char *version);
int eh_ra_start(const char *args,const char *path,const void *data,size_t size,void *(*memory_data)(unsigned),size_t (*memory_size)(unsigned));
void eh_ra_memory_map(const struct retro_memory_map *map);
int eh_ra_temporal_safe(void);
void eh_ra_frame(void);void eh_ra_idle(void);void eh_ra_reset(void);int eh_ra_hardcore(void);void eh_ra_list(void);void eh_ra_shutdown(void);
int eh_ra_save_progress(const char *state_path);void eh_ra_load_progress(const char *state_path);int eh_ra_toast(char *out,size_t cap);
/* One event line at a time: achievement events can arrive from network threads. */
#ifdef _WIN32
static CRITICAL_SECTION event_lock;static int event_lock_ready;
#define EVENT_LOCK() do{if(event_lock_ready)EnterCriticalSection(&event_lock);}while(0)
#define EVENT_UNLOCK() do{if(event_lock_ready)LeaveCriticalSection(&event_lock);}while(0)
#else
static pthread_mutex_t event_lock=PTHREAD_MUTEX_INITIALIZER;
#define EVENT_LOCK() pthread_mutex_lock(&event_lock)
#define EVENT_UNLOCK() pthread_mutex_unlock(&event_lock)
#endif
void eh_events_begin(void){EVENT_LOCK();}
void eh_events_end(void){fflush(stdout);EVENT_UNLOCK();}
void eh_event(const char *json){EVENT_LOCK();fputs(json,stdout);putchar('\n');fflush(stdout);EVENT_UNLOCK();}
static void eventf(const char *format,...){char line[2048];va_list args;va_start(args,format);vsnprintf(line,sizeof(line),format,args);va_end(args);eh_event(line);}

static void system_message(const char *format,...){
#ifdef _WIN32
    EnterCriticalSection(&paint_lock);va_list args;va_start(args,format);vsnprintf(system_notice,sizeof(system_notice),format,args);va_end(args);system_notice_until=now_ms()+2500;LeaveCriticalSection(&paint_lock);
#else
    (void)format;
#endif
}

static void RETRO_CALLCONV core_log(enum retro_log_level level,const char *format,...){
    (void)level;char buffer[2048];va_list args;va_start(args,format);vsnprintf(buffer,sizeof(buffer),format,args);va_end(args);fputs(buffer,stderr);
}

/* Core-owned option definitions remain valid until the next registration. JSON output is escaped,
   and only values advertised by the engine are exposed to the launcher. */
static void json_string(const char *text){
    putchar('"');for(const unsigned char *p=(const unsigned char*)(text?text:"");*p;p++){
        if(*p=='"'||*p=='\\'){putchar('\\');putchar(*p);}else if(*p<32)printf("\\u%04x",*p);else putchar(*p);
    }putchar('"');
}
static void disc_json(void){
    int supported=disks.get_num_images&&disks.get_image_index&&disks.set_eject_state&&disks.get_eject_state&&disks.set_image_index;
    printf("{\"supported\":%s,\"count\":%u,\"index\":%u,\"ejected\":%s}",supported?"true":"false",supported?disks.get_num_images():0,supported?disks.get_image_index():0,supported&&disks.get_eject_state()?"true":"false");
}
static void emit_properties(void){
    EVENT_LOCK();printf("{\"event\":\"properties\",\"discs\":");disc_json();printf(",\"cheatsSupported\":%s,\"typedCheats\":%s,\"options\":[",set_cheat&&reset_cheats?"true":"false",typed_cheat?"true":"false");
    for(unsigned i=0;i<option_count;i++){
        if(i){putchar(',');}
        printf("{\"key\":");json_string(option_keys[i]);printf(",\"label\":");json_string(option_labels[i]);
        printf(",\"value\":");json_string(option_values[i]);printf(",\"options\":[");
        for(unsigned j=0;j<choice_counts[i];j++){if(j)putchar(',');putchar('[');json_string(choice_values[i][j]);putchar(',');json_string(choice_labels[i][j]);putchar(']');}
        printf("]}");
    }
    printf("],\"runtime\":{\"controllerPorts\":%u,\"runaheadSupported\":%s,\"temporalSupported\":%s,\"rewind\":%s,\"runahead\":%s,\"rewindSteps\":%zu}}\n",controller_ports,runahead_supported&&eh_ra_temporal_safe()?"true":"false",temporal_supported&&eh_ra_temporal_safe()?"true":"false",rewind_enabled&&eh_ra_temporal_safe()?"true":"false",runahead_enabled&&eh_ra_temporal_safe()?"true":"false",eh_history_size());fflush(stdout);EVENT_UNLOCK();
}
static int has_input_focus(void);
static bool rumble_callback(unsigned port,enum retro_rumble_effect effect,uint16_t strength){return !speculating&&has_input_focus()&&eh_pad_rumble(port,(unsigned)effect,strength);}
static bool environment(unsigned cmd,void *data){
    switch(cmd){
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:if(!data)return false;disks=*(struct retro_disk_control_callback*)data;return true;
    case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:*(unsigned*)data=1;return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:{
        const struct retro_disk_control_ext_callback *d=data;if(!d)return false;
        disks.set_eject_state=d->set_eject_state;disks.get_eject_state=d->get_eject_state;disks.get_image_index=d->get_image_index;disks.set_image_index=d->set_image_index;disks.get_num_images=d->get_num_images;return true;}
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:((struct retro_log_callback*)data)->log=core_log;return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:{unsigned f=*(unsigned*)data;if(f>RETRO_PIXEL_FORMAT_RGB565)return false;pixel_format=f;return true;}
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:*(bool*)data=true;return true;
    /* Bit 0 video, bit 1 audio. Some cores read this without initializing it, so always answer. */
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:*(int*)data=speculating?1:3;return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:*(bool*)data=fast_forward;return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:{
        const char *shared=getenv("EMUHALL_CORE_SYSTEM_DIRECTORY");
        *(const char**)data=shared&&*shared?shared:system_dir;return true;}
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:*(const char**)data=system_dir;return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:*(bool*)data=options_changed!=0;if(options_changed)fprintf(stderr,"Engine options changed while playing; the engine re-reads them now\n");options_changed=0;return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:return true;
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS_2:{
        uint64_t *quirks=data;if(!quirks)return false;
        if(*quirks&RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE){*quirks|=RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE;variable_states=1;}
        return true;
    }
    case RETRO_ENVIRONMENT_GET_LANGUAGE:*(unsigned*)data=RETRO_LANGUAGE_ENGLISH;return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:*(unsigned*)data=2;return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:{
        const struct retro_core_options_v2 *options=cmd==RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2?data:((struct retro_core_options_v2_intl*)data)->us;
        option_count=0;if(!options||!options->definitions)return false;
        const struct retro_core_option_v2_definition *v=options->definitions;
        for(;v->key&&option_count<128;v++){const char *value=v->default_value?v->default_value:v->values[0].value;if(!value)continue;
            snprintf(option_keys[option_count],128,"%s",v->key);snprintf(option_values[option_count],256,"%s",value);copy_choices(option_count,v->desc,v->values);option_count++;}
        return false; /* Defaults accepted; this host does not display option categories yet. */
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:{
        const struct retro_core_option_definition *v=cmd==RETRO_ENVIRONMENT_SET_CORE_OPTIONS?data:((struct retro_core_options_intl*)data)->us;
        option_count=0;if(!v)return false;
        for(;v->key&&option_count<128;v++){const char *value=v->default_value?v->default_value:v->values[0].value;if(!value)continue;
            snprintf(option_keys[option_count],128,"%s",v->key);snprintf(option_values[option_count],256,"%s",value);copy_choices(option_count,v->desc,v->values);option_count++;}return true;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES:{const struct retro_variable *v=data;option_count=0;for(;v&&v->key&&option_count<128;v++){
        const char *p=v->value?strstr(v->value,"; "):NULL;if(!p)continue;p+=2;size_t n=strcspn(p,"|");if(n>=256)n=255;
        snprintf(option_keys[option_count],128,"%s",v->key);memcpy(option_values[option_count],p,n);option_values[option_count][n]=0;
        snprintf(option_labels[option_count],256,"%s",v->value);char *semi=strchr(option_labels[option_count],';');if(semi)*semi=0;
        choice_counts[option_count]=0;
        while(*p&&choice_counts[option_count]<128){unsigned j=choice_counts[option_count]++;size_t length=strcspn(p,"|");if(length>255)length=255;
            memcpy(choice_values[option_count][j],p,length);choice_values[option_count][j][length]=0;snprintf(choice_labels[option_count][j],256,"%s",choice_values[option_count][j]);p+=strcspn(p,"|");if(*p)p++;}
        option_count++;}return true;}
    case RETRO_ENVIRONMENT_GET_VARIABLE:{struct retro_variable *v=data;v->value=NULL;
        for(unsigned i=0;i<override_count;i++)if(!strcmp(v->key,override_keys[i])){v->value=override_values[i];return true;}
        for(unsigned i=0;i<option_count;i++)if(!strcmp(v->key,option_keys[i])){v->value=option_values[i];return true;}
        return false;}
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:if(!data)return false;((struct retro_rumble_interface*)data)->set_rumble_state=rumble_callback;return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:return true;
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:{
        if(!data)return false;
        const struct retro_controller_info *ports=(const struct retro_controller_info*)data;unsigned count=0;
        for(unsigned i=0;i<4&&ports[i].types&&ports[i].num_types;i++){
            unsigned selected=RETRO_DEVICE_NONE,dualshock=RETRO_DEVICE_NONE;
            for(unsigned j=0;j<ports[i].num_types&&j<64;j++){
                unsigned type=ports[i].types[j].id;const char *desc=ports[i].types[j].desc;
                /* A DualShock keeps digital games working (the engine switches analog mode on per game, or the player
                   toggles it like the real Analog button) while a plain pad would ignore the sticks entirely. */
                if(desc&&(strstr(desc,"DualShock")||strstr(desc,"dualshock"))&&((type&RETRO_DEVICE_MASK)==RETRO_DEVICE_ANALOG||(type&RETRO_DEVICE_MASK)==RETRO_DEVICE_JOYPAD))dualshock=type;
                if(type==RETRO_DEVICE_JOYPAD){selected=type;continue;}
                if(selected==RETRO_DEVICE_NONE&&(type&RETRO_DEVICE_MASK)==RETRO_DEVICE_JOYPAD)selected=type;
            }
            controller_devices[i]=dualshock!=RETRO_DEVICE_NONE?dualshock:selected;count++;
        }
        if(count)controller_ports=count;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY:{
        struct retro_game_geometry *g=data;
        if(!g||!g->base_width||!g->base_height||g->base_width>frame_cap_w||g->base_height>frame_cap_h||g->max_width<g->base_width||g->max_height<g->base_height||g->max_width>8192||g->max_height>8192)return false;
        if(hw_active&&!hw_d3d&&!hw_vk&&geometry_reported&&(g->base_width!=current_geometry.base_width||g->base_height!=current_geometry.base_height)){
            if(!eh_gl_framebuffer(g->base_width,g->base_height,hw.depth,hw.stencil))return false;
        }
        current_geometry=*g;atomic_store(&display_aspect,g->aspect_ratio);geometry_reported=1;return true;
    }
    case RETRO_ENVIRONMENT_SHUTDOWN:quit=1;return true;
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:eh_ra_memory_map(data);return true;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:*(unsigned*)data=RETRO_HW_CONTEXT_OPENGL_CORE;return true;
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
        if(!data||(!hw_d3d&&!hw_vk)){return false;}
        *(const void**)data=hw_vk?eh_vk_interface():eh_d3d_interface();return *(const void**)data!=NULL;
    /* Context negotiation: version 1 for Vulkan; none for Direct3D, whose version-1 interfaces need no locking here. */
    case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:{
        struct retro_hw_render_context_negotiation_interface *n=data;if(!n)return false;
        n->interface_version=n->interface_type==RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN?1:0;return true;
    }
    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:{
        const struct retro_hw_render_context_negotiation_interface *n=data;
        if(!n||n->interface_type!=RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)return false;
        eh_vk_set_negotiation(n);return true;
    }
    case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:return true; /* The player never draws in the engine's context. */
    case RETRO_ENVIRONMENT_SET_HW_RENDER:{
        struct retro_hw_render_callback *cb=data;if(!cb)return false;
        if(cb->context_type==RETRO_HW_CONTEXT_VULKAN){
            /* Vulkan (vkhost.c): the device is made once the game is loaded, through the engine's negotiation. */
            if(hw_active&&!hw_vk)return false;
            hw_renderer_error=NULL;cb->get_current_framebuffer=hw_framebuffer;cb->get_proc_address=NULL;hw=*cb;hw_active=1;hw_vk=1;return true;
        }
        if(cb->context_type==RETRO_HW_CONTEXT_D3D11||cb->context_type==RETRO_HW_CONTEXT_D3D12){
            /* Direct3D (Windows): the engine draws with our device; frames are copied back like OpenGL's. */
            if(hw_active&&hw_d3d!=cb->context_type)return false;
            if(!hw_active&&!eh_d3d_create(cb->context_type)){hw_renderer_error="Direct3D is not available on this graphics card. Choose OpenGL or Software in the game's console options.";return false;}
            hw_renderer_error=NULL;cb->get_current_framebuffer=hw_framebuffer;cb->get_proc_address=NULL;hw=*cb;hw_active=1;hw_d3d=cb->context_type;return true;
        }
        int core_profile=cb->context_type==RETRO_HW_CONTEXT_OPENGL_CORE;
        if(!core_profile&&cb->context_type!=RETRO_HW_CONTEXT_OPENGL){hw_renderer_error="This renderer is unavailable in EmuHall. Choose OpenGL" WINDOWS_RENDERERS " in the game's console options.";return false;}
        if(!hw_active&&!eh_gl_create(core_profile,core_profile?cb->version_major:2,core_profile?cb->version_minor:0))return false;
        hw_renderer_error=NULL;cb->get_current_framebuffer=hw_framebuffer;cb->get_proc_address=eh_gl_proc;hw=*cb;hw_active=1;return true;
    }
    default:return false;
    }
}
static void video(const void *data,unsigned width,unsigned height,size_t pitch){
    /* Duplicate of the previous frame: show it again, so pacing stays one frame per frame. */
    if(!data){
        if(video_frames)frame_ready=1;
        return;
    }
    size_t bpp=pixel_format==RETRO_PIXEL_FORMAT_XRGB8888?4:2;
    /* An empty hardware frame while the engine's output starts (the PS2 engine's first frame on OpenGL is 0x0): nothing
       to show yet. It ended the game whenever it arrived. */
    if(data==RETRO_HW_FRAME_BUFFER_VALID&&hw_active&&(!width||!height)){
        if(video_frames)frame_ready=1;
        return;
    }
    if(data==RETRO_HW_FRAME_BUFFER_VALID&&hw_active&&gpu_present){
        if(width&&height&&width<=frame_cap_w&&height<=frame_cap_h&&gpu_frame(width,height)<0){callback_error="Graphics presentation failed; see renderer details above";fault=1;quit=1;}
        return;
    }
    if(data==RETRO_HW_FRAME_BUFFER_VALID&&hw_active){
        /* A frame drawn by the graphics card: read it back into the frame buffer every other engine uses. */
        /* The copy runs one frame behind, so the CPU never waits for the graphics card. */
        unsigned w=0,h=0;int got=width&&height&&width<=frame_cap_w&&height<=frame_cap_h?(hw_vk?eh_vk_read(width,height,framebuffer,&w,&h):hw_d3d?eh_d3d_read(width,height,framebuffer,&w,&h):eh_gl_read(width,height,hw.bottom_left_origin,framebuffer,&w,&h)):-1;
        if(got<0){fault=1;quit=1;return;}
        if(!got)return;
        frame_width=w;frame_height=h;unsigned width=w,height=h;
        for(size_t i=0,n=(size_t)width*height;i<n;i+=97)if(!speculating)frame_hash=(frame_hash^framebuffer[i])*16777619u; /* Sampled. */
        if(!speculating)video_frames++;
        frame_ready=1;
        return;
    }
    if(data==RETRO_HW_FRAME_BUFFER_VALID||!width||!height||width>frame_cap_w||height>frame_cap_h||pitch<width*bpp||pitch>1024*1024){fault=1;quit=1;return;}
    frame_width=width;frame_height=height;
    for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++){
        const uint8_t *p=(const uint8_t*)data+y*pitch+x*bpp;uint32_t rgb;
        if(bpp==4){memcpy(&rgb,p,4);rgb&=0xffffff;}else{uint16_t v;memcpy(&v,p,2);unsigned r,g,b;
            if(pixel_format==RETRO_PIXEL_FORMAT_RGB565){r=(v>>11)&31;g=(v>>5)&63;b=v&31;rgb=((r*255/31)<<16)|((g*255/63)<<8)|(b*255/31);}
            else{r=(v>>10)&31;g=(v>>5)&31;b=v&31;rgb=((r*255/31)<<16)|((g*255/31)<<8)|(b*255/31);}}
        framebuffer[y*width+x]=rgb;if(!speculating)frame_hash=(frame_hash^rgb)*16777619u;
    }
    if(!speculating)video_frames++;
    frame_ready=1;
}
#ifdef _WIN32
static void ring_put(int16_t l,int16_t r){if(ring_count>=RING_FRAMES)return;size_t i=(ring_read+ring_count)%RING_FRAMES;audio_ring[i*2]=l;audio_ring[i*2+1]=r;ring_count++;}
/* Moves whole blocks from the ring to free waveOut headers, in order. */
static double now_ms(void);
/* Evidence for choppy sound in the session log. A gap is a moment the sound card had nothing left to play; it is noticed
   at the next write, so its length is a lower bound. */
static unsigned window_gaps,window_frames;static double window_start,window_gap_ms,gap_started,queue_min=1e9,queue_max;
static void audio_submit(void){double started=now_ms();
    if(frames_submitted&&!gap_started&&!fast_forward&&!paused&&!audio_priming){
        int playing=0;for(unsigned k=0;k<BLOCKS;k++)if(audio_headers[k].dwFlags&WHDR_INQUEUE){playing=1;break;}
        if(!playing){gap_started=started;audio_gaps++;window_gaps++;}
    }
    uint64_t before=frames_submitted;
    for(unsigned k=0;k<BLOCKS&&ring_count>=BLOCK_FRAMES;k++){
        WAVEHDR *h=&audio_headers[k];if(h->dwFlags&WHDR_INQUEUE)continue;
        for(unsigned i=0;i<BLOCK_FRAMES;i++){size_t j=(ring_read+i)%RING_FRAMES;audio_blocks[k][i*2]=audio_ring[j*2];audio_blocks[k][i*2+1]=audio_ring[j*2+1];}
        ring_read=(ring_read+BLOCK_FRAMES)%RING_FRAMES;ring_count-=BLOCK_FRAMES;
        /* Reuse prepared fixed buffers. Locking/unlocking driver memory for
           every 5 ms block added work and jitter under the audio lock. */
        MMRESULT result=waveOutWrite(wave,h,sizeof(*h));
        if(result!=MMSYSERR_NOERROR){fprintf(stderr,"Audio output submission failed (%u)\n",(unsigned)result);callback_error="Audio output submission failed";fault=1;quit=1;return;}
        frames_submitted+=BLOCK_FRAMES;
    }
    if(gap_started&&frames_submitted>before){window_gap_ms+=now_ms()-gap_started;gap_started=0;}
    /* PS2 startup/state restoration can compile shaders for hundreds of ms.
       Start its device clock only after a normal queue is ready, rather than
       playing tiny BIOS fragments followed by repeated empty-buffer gaps. */
    if(audio_priming&&!paused&&frames_submitted*1000.0/OUT_RATE>=(max_pending==1?64.0:AUDIO_TARGET_MS)){
        MMRESULT result=waveOutRestart(wave);
        if(result!=MMSYSERR_NOERROR){callback_error="Audio output could not start";fault=1;quit=1;return;}
        audio_priming=0;
        if(getenv("EMUHALL_AUDIO_DIAGNOSTICS"))fprintf(stderr,"Audio: playback started with %.0f ms buffered\n",frames_submitted*1000.0/OUT_RATE);
    }
    audio_api_total_ms+=now_ms()-started;
}
/* Every 5 s with gaps or slow emulation: how many, how long silent, and whether emulation kept up. */
static void audio_window(double fps){
    /* Speed from the sound produced, not frames run: an engine may emulate two console frames in one (PCEE2 skips
       presenting duplicates), and every emulated second makes a second of sound. */
    static size_t window_audio;(void)fps;
    double now=now_ms();if(!window_start){window_start=now;window_frames=0;window_audio=audio_frames;return;}
    window_frames++;double elapsed=now-window_start;if(elapsed<5000)return;
    double speed=core_rate>0?(double)(audio_frames-window_audio)/core_rate*1000.0/elapsed*100.0:0.0;window_audio=audio_frames;
    if((window_gaps||speed<97.0||getenv("EMUHALL_AUDIO_DIAGNOSTICS"))&&!fast_forward&&!paused)fprintf(stderr,"Audio: %u gaps (at least %.0f ms silent) in %.0f s; emulation at %.0f%% of full speed; sound queue %.0f-%.0f ms; sound at %.0f%% tempo; core work %.0f%% of guest budget\n",
        window_gaps,window_gap_ms,elapsed/1000.0,speed,queue_min<1e9?queue_min:0.0,queue_max,audio_tempo*100.0,audio_work_ratio*100.0);
    window_start=now;window_frames=0;window_gaps=0;window_gap_ms=0;queue_min=1e9;queue_max=0;
}
/* Pause duration is not emulation work and must not pollute the next speed window. */
static void audio_reset_window(void){window_start=0;window_frames=0;window_gaps=0;window_gap_ms=0;queue_min=1e9;queue_max=0;}
static void audio_reprime(void){
    if(!wave||!audio_prime_enabled)return;
    EnterCriticalSection(&audio_lock);
    waveOutReset(wave);audio_priming=waveOutPause(wave)==MMSYSERR_NOERROR;
    ring_count=ring_read=0;frames_submitted=position_base=last_position=0;gap_started=0;
    resample_pos=0;eh_stretch_reset();audio_tempo=1.0;
    LeaveCriticalSection(&audio_lock);audio_reset_window();
}
/* Milliseconds of sound written but not yet heard, from the device's own play position. */
static double audio_queued_ms(void){
    MMTIME t;memset(&t,0,sizeof(t));t.wType=TIME_SAMPLES;waveOutGetPosition(wave,&t,sizeof(t));
    uint32_t p=t.wType==TIME_SAMPLES?t.u.sample:t.wType==TIME_BYTES?t.u.cb/4:0;
    if(p<last_position){position_base+=(uint64_t)1<<32;}
    last_position=p;
    uint64_t played=position_base+p,queued=frames_submitted>played?frames_submitted-played:0;
    return (double)(queued+ring_count)*1000.0/OUT_RATE;
}
#endif
#ifdef _WIN32
static DWORD WINAPI audio_loop(LPVOID unused){
    (void)unused;SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST);
    /* MMCSS registration belongs to the thread doing the work: registering
       the emulation thread as Games does not prioritize audio submissions.
       Keep the ordinary priority fallback when the service is unavailable. */
    typedef HANDLE (WINAPI *AudioTaskFn)(LPCWSTR,LPDWORD);
    typedef BOOL (WINAPI *AudioRevertFn)(HANDLE);
    HMODULE avrt=LoadLibraryExW(L"avrt.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    HANDLE task=NULL;AudioRevertFn revert=NULL;
    if(avrt){
        FARPROC set_symbol=GetProcAddress(avrt,"AvSetMmThreadCharacteristicsW");
        FARPROC revert_symbol=GetProcAddress(avrt,"AvRevertMmThreadCharacteristics");
        if(set_symbol&&revert_symbol){AudioTaskFn set_task;memcpy(&set_task,&set_symbol,sizeof(set_task));memcpy(&revert,&revert_symbol,sizeof(revert));DWORD index=0;task=set_task(L"Audio",&index);}
    }
    if(getenv("EMUHALL_AUDIO_DIAGNOSTICS"))fprintf(stderr,"Audio scheduling: %s\n",task?"MMCSS Audio":"thread-priority fallback");
    while(!audio_stop){
        WaitForSingleObject(audio_wake,4);
        EnterCriticalSection(&audio_lock);audio_submit();LeaveCriticalSection(&audio_lock);
    }
    if(task&&revert)revert(task);
    if(avrt)FreeLibrary(avrt);
    return 0;
}
#endif
static size_t audio_batch(const int16_t *data,size_t frames){
    if(speculating)return frames;
    if(!data||frames>1024*1024){fault=1;quit=1;return 0;}
    audio_frames+=frames;
#ifdef _WIN32
    if(wave&&(!fast_forward||!fast_forward_mute)){
        /* Linear resampling; rate_ratio nudges the output rate by at most 0.5% to hold the queue at its target. */
        double speed=fast_forward?fast_forward_speed/100.0:1.0;
        double step=core_rate*speed/(OUT_RATE*(fast_forward?1.0:rate_ratio));
        EnterCriticalSection(&audio_lock);
        for(size_t i=0;i<frames;i++){int16_t l=audio_muted?0:(int16_t)(data[i*2]*audio_volume/100),r=audio_muted?0:(int16_t)(data[i*2+1]*audio_volume/100);
            while(resample_pos<1.0){eh_stretch_push((int16_t)(prev_l+(l-prev_l)*resample_pos),(int16_t)(prev_r+(r-prev_r)*resample_pos),fast_forward?1.0:audio_tempo,ring_put);resample_pos+=step;}
            resample_pos-=1.0;prev_l=l;prev_r=r;}
        LeaveCriticalSection(&audio_lock);
        SetEvent(audio_wake);
    }
#else
    if(show_window)eh_desktop_audio(data,frames,(unsigned)audio_volume,audio_muted||(fast_forward&&fast_forward_mute),fast_forward?fast_forward_speed:100);
#endif
    return frames;
}
static void audio_sample(int16_t l,int16_t r){int16_t data[2]={l,r};audio_batch(data,1);}
#ifdef _WIN32
/* melonDS declares the screens' layout through its core options. R3 cycles that declared sequence.
   Keep the gesture exclusion in the same coordinate space as pointer_state(), including letterboxing.
   Rotation is deliberately unsupported by this renderer; those layouts remain a vertical pair. */
static int nds_pointer_layout,nds_cycle_down;
static unsigned nds_layout_index;
static unsigned nds_layout_count(void){unsigned n=(unsigned)atoi(core_option("melonds_number_of_screen_layouts","2"));return n>=1&&n<=8?n:2;}
static const char *nds_layout(unsigned layout_index){
    static const char *defaults[]={"top-bottom","left-right","largescreen-top","largescreen-bottom","hybrid-top","hybrid-bottom","top","bottom"};
    char key[40];unsigned index=layout_index%nds_layout_count();snprintf(key,sizeof(key),"melonds_screen_layout%u",index+1);
    return core_option(key,defaults[index]);
}
static int in_screen(double x,double y,double left,double top,double width,double height){return x>=left&&y>=top&&x<left+width&&y<top+height;}
static int nds_touch_at(int client_x,int client_y){
    LONG width=view_w,height=view_h;if(!nds_pointer_layout||width<=0||height<=0)return 0;
    double x=(double)(client_x-view_x)/width,y=(double)(client_y-view_y)/height;
    if(x<0||y<0||x>=1||y>=1)return 0;
    const char *layout=nds_layout((unsigned)view_nds_layout);
    double gap=atoi(core_option("melonds_screen_gap","0")),scale=atoi(core_option("melonds_secondary_screen_scale","100"))/100.0;
    if(gap<0||gap>126)gap=0;
    if(scale<0.5||scale>1)scale=1;
    double bw=256,bh=384+gap,tx=0,ty=192+gap,tw=256,th=192;
    if(!strcmp(layout,"top"))return 0;
    if(!strcmp(layout,"bottom"))return 1;
    if(!strcmp(layout,"left-right")||!strcmp(layout,"right-left")){
        bw=512;bh=192;tx=!strcmp(layout,"left-right")?256:0;ty=0;
    }else if(strstr(layout,"largescreen")||strstr(layout,"hybrid")){
        unsigned ratio=(unsigned)atoi(core_option("melonds_hybrid_ratio","2"));if(ratio<2||ratio>3)ratio=2;
        int flipped=!strncmp(layout,"flipped-",8),large_touch=strstr(layout,"bottom")!=NULL,hybrid=strstr(layout,"hybrid")!=NULL;
        bw=256*(ratio+1);bh=192*ratio;tx=flipped?0:256*ratio;ty=hybrid?192*(ratio-1):192*(ratio-1)/2.0;
        if(large_touch){
            double large_x=flipped?256:0;
            if(in_screen(x*bw,y*bh,large_x,0,256*ratio,192*ratio))return 1;
            if(!hybrid||!strcmp(core_option("melonds_hybrid_small_screen","both"),"one"))return 0;
        }
    }else if(!strcmp(layout,"bottom-top")){
        bh=192+gap+192*scale;tx=ty=0;
    }else if(!strcmp(layout,"top-bottom")){
        bh=192+gap+192*scale;tw=256*scale;th=192*scale;tx=(256-tw)/2;
    }else return 1; /* Unknown DS layout: preserve touch input until its geometry is known. */
    return in_screen(x*bw,y*bh,tx,ty,tw,th);
}
/* Input counts while the person is using EmuHall: any window of the launcher's process or of this player is in
   front. (Comparing against one exact window was too strict: the launcher's web view and its popups are other
   windows of the same program.) Changes are written to the session log. */
static int has_input_focus(void){
    if(!show_window||paused||!window)return 0;
    static DWORD launcher_pid;static HWND last_front;static int last_answer=-1;
    if(!launcher_pid&&embedded)GetWindowThreadProcessId(parent_window,&launcher_pid);
    HWND front=GetForegroundWindow();DWORD pid=0;if(front)GetWindowThreadProcessId(front,&pid);
    int answer=front==window||front==parent_window||(pid&&(pid==GetCurrentProcessId()||(embedded&&pid==launcher_pid)));
    if(answer!=last_answer||(!answer&&front!=last_front)){
        wchar_t cls[64]=L"";if(front)GetClassNameW(front,cls,64);
        fprintf(stderr,"Input %s (window in front: %ls)\n",answer?"on":"off: another program is in front",cls);
        last_answer=answer;last_front=front;}
    return answer;
}
#else
static int has_input_focus(void){return show_window&&!paused&&eh_desktop_focus();}
#endif
static int physical_key(int code){
#ifdef _WIN32
    return code&&(GetAsyncKeyState(code)&0x8000);
#else
    return eh_desktop_key(code);
#endif
}
static uint64_t hint_now(void){return (uint64_t)now_ms();}
int eh_pause_hint_visible(void){return hint_now()<atomic_load(&pause_hint_until);}
static void input_poll(void){if(speculating)return;polls++;
#ifdef _WIN32
    double t0=now_ms();eh_pad_read(&pad);ms_input=now_ms()-t0;
#else
    eh_pad_read(&pad);
#endif
    if(!pause_hint_offered&&controlled&&has_input_focus()&&getenv("EMUHALL_PAUSE_HINT")&&!strcmp(getenv("EMUHALL_PAUSE_HINT"),"1")){
        int activity=0;for(unsigned port=0;port<4;port++){eh_pad input;eh_pad_read_port(port,&input);if(input.buttons||abs(input.lx)>8000||abs(input.ly)>8000||abs(input.rx)>8000||abs(input.ry)>8000)activity=1;}
        if(activity){pause_hint_offered=1;atomic_store(&pause_hint_until,hint_now()+5000);eventf("{\"event\":\"pause-hint\"}");}
    }
    static int previous_keys[21];
    for(unsigned i=0;i<21;i++){
        int down=system_keys[i]&&physical_key(system_keys[i]);
        if(controlled&&!paused&&has_input_focus()&&down&&!previous_keys[i]){
            char command[64];if(i==0)snprintf(command,sizeof(command),"fast-forward");
            else snprintf(command,sizeof(command),"%s %u",i<=10?"save-state":"load-state",i<=10?i:i-10);
            queue_push(command);
        }
        previous_keys[i]=down;
    }
    static int keyboard_seen;
    if(!keyboard_seen&&has_input_focus())for(unsigned i=0;i<16;i++)if(key_map[i]&&physical_key(key_map[i])){fprintf(stderr,"Keyboard input seen\n");keyboard_seen=1;break;}
    /* Holding Back+Start opens the menu from a controller, without stealing a quick Select+Start press; the Guide
       (PS, Xbox, Home) button opens it at once. */
    static unsigned held;static int guide_down;unsigned gestures=eh_pad_gestures();
    held=has_input_focus()&&(gestures&2)?held+1:0;if(held==45&&controlled){menu_requested=1;atomic_store(&pause_hint_until,0);if(!netplay_active)eventf("{\"event\":\"pause-shortcut-learned\"}");}
    if((gestures&1)&&!guide_down&&has_input_focus()&&controlled)menu_requested=1;
    guide_down=gestures&1;
    #ifdef _WIN32
    if(nds_pointer_layout){
        int cycle=has_input_focus()&&((pad.buttons&(1u<<EH_R3))||(key_map[15]&&(GetAsyncKeyState(key_map[15])&0x8000)));
        if(cycle&&!nds_cycle_down)nds_layout_index=(nds_layout_index+1)%nds_layout_count();
        nds_cycle_down=cycle;
    }
#endif
}
static int key_down(unsigned id){if(speculating)return id<16?cached_keys[id]:0;return id<16&&key_map[id]&&!system_key(key_map[id])&&physical_key(key_map[id]);}
#ifdef _WIN32
/* The mouse on the picture, as a Libretro pointer: -32767..32767 across the frame, pressed with the left button. */
static int16_t pointer_state(unsigned id){
    POINT p;LONG w=view_w,h=view_h;if(!window||w<=0||h<=0||!GetCursorPos(&p)||!ScreenToClient(window,&p))return 0;
    LONG x=p.x-view_x,y=p.y-view_y;int inside=x>=0&&y>=0&&x<w&&y<h;
    switch(id){
    case RETRO_DEVICE_ID_POINTER_X:return inside?(int16_t)(x*65534/w-32767):-32768;
    case RETRO_DEVICE_ID_POINTER_Y:return inside?(int16_t)(y*65534/h-32767):-32768;
    case RETRO_DEVICE_ID_POINTER_PRESSED:return inside&&(GetAsyncKeyState(VK_LBUTTON)&0x8000)?1:0;
    case RETRO_DEVICE_ID_POINTER_COUNT:return 1;
    default:return 0;}
}
#else
static int16_t pointer_state(unsigned id){return eh_desktop_pointer(id);}
#endif
/* Network waits remain cancellable and keep the player window responsive. */
int eh_netplay_cancelled(void){
#ifdef _WIN32
    if(show_window){MSG msg;while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
#endif
    if(controlled){LOCK();for(unsigned i=queue_head;i!=queue_tail;i=(i+1)%128)if(!strcmp(queue[i],"quit"))quit=1;UNLOCK();}
    return quit;
}
static int16_t input_state(unsigned port,unsigned device,unsigned index,unsigned id){
    if(netplay_active){if(port>1||device!=RETRO_DEVICE_JOYPAD||index)return 0;return id==RETRO_DEVICE_ID_JOYPAD_MASK?(int16_t)netplay_inputs[port]:id<16&&(netplay_inputs[port]>>id&1);}
    if(diagnostic_mode&&diagnostic_mask>=0&&!port&&device==RETRO_DEVICE_JOYPAD&&!index)
        return id==RETRO_DEVICE_ID_JOYPAD_MASK?(int16_t)diagnostic_mask:id<16&&(diagnostic_mask>>id&1);
    if(port>=4||!has_input_focus())return 0;
    eh_pad input;eh_pad_read_port(port,&input);
    switch(device){
    case RETRO_DEVICE_POINTER:return port||index?0:pointer_state(id);
    case RETRO_DEVICE_MOUSE:
        // Citra combines absolute pointer position with the mouse button for its touch screen.
        return !port&&!index&&id==RETRO_DEVICE_ID_MOUSE_LEFT?pointer_state(RETRO_DEVICE_ID_POINTER_PRESSED):0;
    case RETRO_DEVICE_ANALOG:
        /* Sticks and pressure-sensitive buttons (PS1 DualShock, PS2). Only triggers are analog here; other buttons
           read 0 so engines fall back to the digital state. */
        if(index==RETRO_DEVICE_INDEX_ANALOG_LEFT&&id<=RETRO_DEVICE_ID_ANALOG_Y)return (id==RETRO_DEVICE_ID_ANALOG_Y?input.ly:input.lx);
        if(index==RETRO_DEVICE_INDEX_ANALOG_RIGHT&&id<=RETRO_DEVICE_ID_ANALOG_Y)return (id==RETRO_DEVICE_ID_ANALOG_Y?input.ry:input.rx);
        if(index==RETRO_DEVICE_INDEX_ANALOG_BUTTON&&id==RETRO_DEVICE_ID_JOYPAD_L2)return pad_map[id]==12&&input.lt>3855?input.lt:pad_map[id]==13&&input.rt>3855?input.rt:((mapped_pad_value(&input)&(1u<<id))||(!port&&key_down(id))?32767:0);
        if(index==RETRO_DEVICE_INDEX_ANALOG_BUTTON&&id==RETRO_DEVICE_ID_JOYPAD_R2)return pad_map[id]==13&&input.rt>3855?input.rt:pad_map[id]==12&&input.lt>3855?input.lt:((mapped_pad_value(&input)&(1u<<id))||(!port&&key_down(id))?32767:0);
        return 0;
    case RETRO_DEVICE_JOYPAD:{
        if(index)return 0;
        uint32_t pressed=mapped_pad_value(&input);for(unsigned i=0;i<16;i++)if(!port&&key_down(i))pressed|=1u<<i;
        if(id==RETRO_DEVICE_ID_JOYPAD_MASK)return (int16_t)pressed; /* All sixteen buttons at once. */
        return id<16&&(pressed>>id&1);}
    default:return 0;}
}
/* Player settings the launcher sends with "set <name> <value>" before the first frame (or at any time). */
static int integer_scaling,smooth_scaling;
#ifdef _WIN32
static void overlay_box(HDC dc,const RECT *box,COLORREF background,COLORREF border,int radius){
    HGDIOBJ brush=SelectObject(dc,GetStockObject(DC_BRUSH)),pen=SelectObject(dc,GetStockObject(DC_PEN));
    SetDCBrushColor(dc,background);SetDCPenColor(dc,border);RoundRect(dc,box->left,box->top,box->right,box->bottom,radius,radius);
    SelectObject(dc,brush);SelectObject(dc,pen);
}
static void overlay_badge(HDC dc,const uint32_t *pixels,int x,int y,int size){
    BITMAPINFO bi;memset(&bi,0,sizeof(bi));bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=EH_RA_BADGE_SIZE;bi.bmiHeader.biHeight=-EH_RA_BADGE_SIZE;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    SetStretchBltMode(dc,HALFTONE);SetBrushOrgEx(dc,0,0,NULL);
    StretchDIBits(dc,x,y,size,size,0,0,EH_RA_BADGE_SIZE,EH_RA_BADGE_SIZE,pixels,&bi,DIB_RGB_COLORS,SRCCOPY);
}
static void overlay_text(HDC dc,const char *text,RECT bounds,HFONT font,COLORREF color){
    wchar_t wide_text[512];MultiByteToWideChar(CP_UTF8,0,text,-1,wide_text,512);
    HGDIOBJ old=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);
    DrawTextW(dc,wide_text,-1,&bounds,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);SelectObject(dc,old);
}
/* RA snapshots contain only copied bytes. The renderer never evaluates memory, fetches images or calls the
   achievements client. Challenge badges disappear as soon as its official evaluator clears the challenge. */
static void overlay_achievements(HDC dc,int x,int y,int width,int height){
    if(width<160||height<120)return;
    int saved=SaveDC(dc),margin=width<640?12:24;
    int notification_y=y+margin;
    int measured=eh_fps_overlay();if(measured>=0){char value[40];snprintf(value,sizeof(value),"%.1f FPS",measured/1000.0);RECT bounds={x+width-margin-100,y+margin,x+width-margin,y+margin+30};overlay_box(dc,&bounds,RGB(8,18,32),RGB(45,140,205),7);HFONT font=CreateFontW(17,0,0,0,FW_MEDIUM,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");bounds.left+=10;bounds.top+=5;overlay_text(dc,value,bounds,font,RGB(225,241,255));DeleteObject(font);}

    if(eh_pause_hint_visible()){
        int card_width=width-2*margin;if(card_width>600)card_width=600;
        RECT hint={x+margin,y+margin,x+margin+card_width,y+margin+62};
        overlay_box(dc,&hint,RGB(8,18,32),RGB(45,140,205),10);
        HFONT font=CreateFontW(width<640?15:18,0,0,0,FW_MEDIUM,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        RECT text={hint.left+14,hint.top+10,hint.right-12,hint.top+30};
        overlay_text(dc,"Pressione Select + Start",text,font,RGB(225,241,255));text.top+=23;text.bottom+=23;overlay_text(dc,"para abrir o menu de pausa.",text,font,RGB(225,241,255));DeleteObject(font);
        notification_y=hint.bottom+10;
    }
    eh_ra_notification_t notification;
    if(eh_ra_notification(&notification)){
        int text_height=height/36;if(text_height<16)text_height=16;if(text_height>24)text_height=24;
        int card_width=width-2*margin;if(card_width>540)card_width=540;
        int card_height=text_height*4+24;
        RECT card={x+margin,notification_y,x+margin+card_width,notification_y+card_height};
        overlay_box(dc,&card,RGB(8,18,32),RGB(42,75,111),12);
        RECT accent={card.left+1,card.top+14,card.left+4,card.bottom-14};
        HBRUSH blue=CreateSolidBrush(RGB(0,174,255));FillRect(dc,&accent,blue);DeleteObject(blue);
        int icon=text_height*2,left=card.left+18;
        if(notification.badge_ready&&card_width>300){overlay_badge(dc,notification.badge_pixels,left,card.top+18,icon);left+=icon+14;}
        HFONT bold=CreateFontW(text_height,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        HFONT regular=CreateFontW(text_height-2,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        RECT line={left,card.top+16,card.right-18,card.top+16+text_height+4};
        overlay_text(dc,notification.heading,line,bold,RGB(238,246,255));
        line.top+=text_height+7;line.bottom+=text_height+7;
        overlay_text(dc,notification.detail,line,regular,RGB(168,190,215));
        if(notification.kind==1&&notification.total){
            char progress[160];snprintf(progress,sizeof(progress),"%u / %u achievements  ·  %u / %u points%s",notification.unlocked,notification.total,notification.points,notification.points_total,notification.hardcore?"  ·  Hardcore":"");
            line.top=card.bottom-text_height-19;line.bottom=card.bottom-15;
            overlay_text(dc,progress,line,regular,RGB(136,166,198));
            RECT track={left,card.bottom-11,card.right-18,card.bottom-8};
            HBRUSH track_brush=CreateSolidBrush(RGB(30,48,70));FillRect(dc,&track,track_brush);DeleteObject(track_brush);
            if(notification.unlocked){track.right=track.left+(int)((uint64_t)(track.right-track.left)*notification.unlocked/notification.total);HBRUSH fill=CreateSolidBrush(RGB(0,174,255));FillRect(dc,&track,fill);DeleteObject(fill);}
        }
        DeleteObject(bold);DeleteObject(regular);
    }
    eh_ra_challenge_t challenges[EH_RA_MAX_CHALLENGES];size_t count=eh_ra_challenges(challenges,EH_RA_MAX_CHALLENGES);
    int size=height/14;if(size<32)size=32;if(size>64)size=64;
    int gap=8,right=x+width-margin,bottom=y+height-margin;
    eh_ra_progress_t progress;
    if(eh_ra_progress(&progress)){
        int card_width=width-2*margin;if(card_width>380)card_width=380;
        int text_height=height/42;if(text_height<14)text_height=14;if(text_height>20)text_height=20;
        int card_bottom=bottom-(count?size+20:0),card_height=text_height*3+34;
        RECT card={right-card_width,card_bottom-card_height,right,card_bottom};
        overlay_box(dc,&card,RGB(8,18,32),RGB(42,75,111),10);
        int badge_size=text_height*2,left=card.left+14;
        if(progress.badge_ready&&card_width>=270){overlay_badge(dc,progress.badge_pixels,left,card.top+15,badge_size);left+=badge_size+12;}
        HFONT bold=CreateFontW(text_height,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        HFONT regular=CreateFontW(text_height-2,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        RECT line={left,card.top+12,card.right-14,card.top+12+text_height};
        overlay_text(dc,progress.pinned?"Tracking":"Achievement progress",line,regular,RGB(136,176,212));
        line.top+=text_height+3;line.bottom+=text_height+3;
        overlay_text(dc,progress.title,line,bold,RGB(238,246,255));
        line.top+=text_height+5;line.bottom+=text_height+5;
        char measured[96];if(progress.has_counts)snprintf(measured,sizeof(measured),"%u / %u",progress.current,progress.target);
        else snprintf(measured,sizeof(measured),"%s",progress.progress);
        overlay_text(dc,measured,line,regular,RGB(168,190,215));
        if(progress.progress[0]&&isfinite(progress.percent)){
            float percent=progress.percent;if(percent<0)percent=0;if(percent>100)percent=100;
            RECT track={card.left+14,card.bottom-10,card.right-14,card.bottom-7};
            HBRUSH brush=CreateSolidBrush(RGB(30,48,70));FillRect(dc,&track,brush);DeleteObject(brush);
            track.right=track.left+(int)((track.right-track.left)*percent/100.0f);
            brush=CreateSolidBrush(RGB(0,174,255));FillRect(dc,&track,brush);DeleteObject(brush);
        }
        DeleteObject(bold);DeleteObject(regular);
    }
    for(size_t i=0;i<count;i++){
        if(!challenges[i].badge_ready)continue;
        int left=right-size-8;if(left<x+margin)break;
        RECT badge={left,bottom-size-8,right,bottom};
        overlay_box(dc,&badge,RGB(8,18,32),RGB(0,174,255),8);
        overlay_badge(dc,challenges[i].badge_pixels,left+4,bottom-size-4,size);
        right=left-gap;
    }
    if(system_notice[0]&&now_ms()<system_notice_until){
        int font_height=height/40;if(font_height<16)font_height=16;if(font_height>24)font_height=24;
        int card_width=width-2*margin;if(card_width>340)card_width=340;
        RECT card={x+margin,y+height-margin-font_height-28,x+margin+card_width,y+height-margin};
        overlay_box(dc,&card,RGB(8,18,32),RGB(42,75,111),8);
        HFONT font=CreateFontW(font_height,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        RECT line={card.left+14,card.top+12,card.right-12,card.bottom-8};overlay_text(dc,system_notice,line,font,RGB(230,242,255));DeleteObject(font);
    }
    RestoreDC(dc,saved);
}
/* Draws a frame aspect-correct and fills only the bars around it. */
static void present_frame(HWND hwnd,HDC dc,const uint32_t *pixels,unsigned frame_width,unsigned frame_height,unsigned layout_index){
    RECT rect;GetClientRect(hwnd,&rect);HBRUSH black=(HBRUSH)GetStockObject(BLACK_BRUSH);
    if(!pixels||!frame_width||!frame_height){FillRect(dc,&rect,black);return;}
    int dw,dh;eh_picture_fit(rect.right,rect.bottom,frame_width,frame_height,atomic_load(&display_aspect),integer_scaling,&dw,&dh);
    int x=(rect.right-dw)/2,y=(rect.bottom-dh)/2;
    view_x=x;view_y=y;view_w=dw;view_h=dh;view_nds_layout=(LONG)layout_index;
    RECT bars[4]={{0,0,rect.right,y},{0,y+dh,rect.right,rect.bottom},{0,y,x,y+dh},{x+dw,y,rect.right,y+dh}};
    int bezel=eh_bezel_style();HBRUSH frame_brush=bezel?CreateSolidBrush(bezel==2?RGB(22,10,28):RGB(5,17,30)):black;
    for(int i=0;i<4;i++)if(bars[i].right>bars[i].left&&bars[i].bottom>bars[i].top)FillRect(dc,&bars[i],frame_brush);
    if(bezel){DeleteObject(frame_brush);if(x>=6){HBRUSH accent=CreateSolidBrush(bezel==2?RGB(135,50,190):RGB(30,100,190));RECT left={x-4,y,x-2,y+dh},right={x+dw+2,y,x+dw+4,y+dh};FillRect(dc,&left,accent);FillRect(dc,&right,accent);DeleteObject(accent);}}

    BITMAPINFO bi;memset(&bi,0,sizeof(bi));bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=frame_width;bi.bmiHeader.biHeight=-(int)frame_height;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    SetStretchBltMode(dc,smooth_scaling?HALFTONE:COLORONCOLOR);if(smooth_scaling)SetBrushOrgEx(dc,0,0,NULL);
    StretchDIBits(dc,x,y,dw,dh,0,0,frame_width,frame_height,pixels,&bi,DIB_RGB_COLORS,SRCCOPY);
    overlay_achievements(dc,x,y,dw,dh);
}
/* Compose game and achievements offscreen, then commit once. Drawing the game directly to
   the window first exposed a bare frame between overlay draws on every refresh. */
static void present(HWND hwnd,HDC dc,const uint32_t *pixels,unsigned width,unsigned height,unsigned layout){
    static HDC canvas;static HBITMAP bitmap;static HGDIOBJ original;static int canvas_w,canvas_h;
    EnterCriticalSection(&paint_lock);
    RECT rect;GetClientRect(hwnd,&rect);int w=rect.right,h=rect.bottom;
    if(w>0&&h>0){
        if(!canvas)canvas=CreateCompatibleDC(dc);
        if(canvas&&(canvas_w!=w||canvas_h!=h)){
            if(bitmap){SelectObject(canvas,original);DeleteObject(bitmap);bitmap=NULL;}
            bitmap=CreateCompatibleBitmap(dc,w,h);
            if(bitmap){original=SelectObject(canvas,bitmap);canvas_w=w;canvas_h=h;}
        }
        if(canvas&&bitmap){present_frame(hwnd,canvas,pixels,width,height,layout);BitBlt(dc,0,0,w,h,canvas,0,0,SRCCOPY);}
    }
    LeaveCriticalSection(&paint_lock);
}
/* Frame queue between emulation (this thread) and the presenter thread. Up to two finished frames wait while a
   third is on screen, so one slow frame (a laptop CPU ramping its clock, the OS preempting us) is absorbed instead of
   showing as a stutter. Costs one frame of latency; RetroArch's "max swapchain images = 3" makes the same trade. */
#define QUEUE_SLOTS 3
static uint32_t *slots[QUEUE_SLOTS];static unsigned slot_w[QUEUE_SLOTS],slot_h[QUEUE_SLOTS],slot_layout[QUEUE_SLOTS];
static unsigned q_read,q_count,shown=QUEUE_SLOTS;
static CRITICAL_SECTION frame_lock;static HANDLE frame_taken,presenter;static volatile LONG presenter_stop;
static double last_fresh_ms;static unsigned presented;
static unsigned queued_frames(void){EnterCriticalSection(&frame_lock);unsigned c=q_count;LeaveCriticalSection(&frame_lock);return c;}
/* Copies the newest frame into a free slot; never touches the slot on screen. Returns 0 when the queue is full. */
static int queue_frame(void){
    EnterCriticalSection(&frame_lock);
    if(q_count>=(unsigned)max_pending){LeaveCriticalSection(&frame_lock);return 0;}
    unsigned i=(q_read+q_count)%QUEUE_SLOTS;LeaveCriticalSection(&frame_lock);
    memcpy(slots[i],framebuffer,(size_t)frame_width*frame_height*4);presentation_filter(slots[i],frame_width,frame_height);slot_w[i]=frame_width;slot_h[i]=frame_height;slot_layout[i]=nds_layout_index;
    EnterCriticalSection(&frame_lock);q_count++;LeaveCriticalSection(&frame_lock);
    return 1;
}
/* With GPU presentation the achievements, notices and FPS overlay are drawn here with GDI into a bitmap the size of the
   picture, magenta where nothing is drawn, and handed to the graphics card with alpha. Only an actual change is uploaded. */
#define OVERLAY_KEY 0xff00ffu
static CRITICAL_SECTION overlay_lock;static uint32_t *overlay_pixels;static unsigned overlay_w,overlay_h;static int overlay_has,overlay_changed;
static void render_overlay(void){
    static HDC dc;static HBITMAP bitmap;static HGDIOBJ original;static uint32_t *bits;static unsigned bw,bh;
    unsigned w=(unsigned)view_w,h=(unsigned)view_h;if(w<160||h<120||w>4096||h>4096)return;
    if(!dc){HDC screen=GetDC(NULL);dc=CreateCompatibleDC(screen);ReleaseDC(NULL,screen);if(!dc)return;}
    if(bw!=w||bh!=h){
        if(bitmap){SelectObject(dc,original);DeleteObject(bitmap);bitmap=NULL;bits=NULL;}
        BITMAPINFO bi;memset(&bi,0,sizeof(bi));bi.bmiHeader.biSize=sizeof(bi.bmiHeader);bi.bmiHeader.biWidth=(LONG)w;bi.bmiHeader.biHeight=-(LONG)h;
        bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
        void *pixels=NULL;bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&pixels,NULL,0);if(!bitmap)return;
        original=SelectObject(dc,bitmap);bits=pixels;bw=w;bh=h;
    }
    size_t n=(size_t)w*h;for(size_t i=0;i<n;i++)bits[i]=OVERLAY_KEY;
    EnterCriticalSection(&paint_lock);overlay_achievements(dc,0,0,(int)w,(int)h);LeaveCriticalSection(&paint_lock);GdiFlush();
    int any=0;for(size_t i=0;i<n;i++){uint32_t v=bits[i]&0xffffff;if(v==OVERLAY_KEY)bits[i]=0;else{bits[i]=v|0xff000000u;any=1;}}
    EnterCriticalSection(&overlay_lock);
    if(!any){overlay_has=0;}
    else{
        if(!overlay_pixels||overlay_w!=w||overlay_h!=h){free(overlay_pixels);overlay_pixels=malloc(n*4);overlay_w=w;overlay_h=h;overlay_changed=1;}
        if(overlay_pixels&&(overlay_changed||!overlay_has||memcmp(overlay_pixels,bits,n*4))){memcpy(overlay_pixels,bits,n*4);overlay_changed=1;}
        overlay_has=overlay_pixels!=NULL;
    }
    LeaveCriticalSection(&overlay_lock);
}
/* Shows one frame per display refresh: the oldest waiting frame, or keeps the current one if none is ready. */
static DWORD WINAPI present_loop(LPVOID unused){
    (void)unused;SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_TIME_CRITICAL);
    while(!presenter_stop){
        if(gpu_present){Sleep(100);render_overlay();continue;}
        if(!wait_vblank||wait_vblank(&vblank)!=0){if(DwmFlush()!=S_OK)Sleep(8);}
        int fresh=0;unsigned i=0;
        EnterCriticalSection(&frame_lock);if(q_count){i=q_read;q_read=(q_read+1)%QUEUE_SLOTS;q_count--;shown=i;fresh=1;}LeaveCriticalSection(&frame_lock);
        if(!fresh){
            /* A refresh with no new frame is a visible stutter, unless the game is paused or between modes. */
            double now=now_ms();
            if(!paused&&!fast_forward&&!repeat_reset&&last_fresh_ms>0&&now-last_fresh_ms>1.5*refresh_ms&&presented>30){
                double gap=now-last_fresh_ms;if(gap>worst_frame_ms)worst_frame_ms=gap;
                if(hitches<16)snprintf(hitch_log[hitches],sizeof(hitch_log[0]),"repeated frame after %u: %.1f ms without a new frame (emulate %.1f, audio wait %.1f)",presented,gap,ms_run,ms_audio_wait);
                hitches++;last_fresh_ms=now; /* Count each stall once. */
            }
            continue;
        }
        SetEvent(frame_taken);
        HDC dc=GetDC(window);if(dc){present(window,dc,slots[i],slot_w[i],slot_h[i],slot_layout[i]);ReleaseDC(window,dc);}
        double now=now_ms();if(repeat_reset)repeat_reset=0;else if(presented>30&&now-last_fresh_ms>worst_frame_ms&&!paused&&!fast_forward)worst_frame_ms=now-last_fresh_ms;
        last_fresh_ms=now;presented++;
    }
    return 0;
}
static int gpu_frame(unsigned width,unsigned height){
    int vx=0,vy=0,vw=0,vh=0;
    EnterCriticalSection(&overlay_lock);
    /* Never wait for the display here: this runs inside the engine's frame, and the PS2 CPU waits with it. The flip
       model shows the newest frame at each refresh without tearing; the sound card paces emulation. */
    /* Achievements and notices are drawn over the picture on both GPU paths. */
    int shown=hw_vk?eh_vk_present(width,height,smooth_scaling,integer_scaling,overlay_has?overlay_pixels:NULL,overlay_w,overlay_h,overlay_changed,&vx,&vy,&vw,&vh)
        :eh_d3d_present(width,height,atomic_load(&display_aspect),0,smooth_scaling,integer_scaling,overlay_has?overlay_pixels:NULL,overlay_w,overlay_h,overlay_changed,&vx,&vy,&vw,&vh);
    overlay_changed=0;
    LeaveCriticalSection(&overlay_lock);
    if(shown<0)return -1;
    if(vw&&vh){view_x=vx;view_y=vy;view_w=vw;view_h=vh;}
    frame_width=width;frame_height=height;
    if(speculating)return 1;
    video_frames++;
    double now=now_ms();
    if(!paused&&!fast_forward&&!repeat_reset&&last_fresh_ms>0&&presented>30&&now-last_fresh_ms>1.5*refresh_ms){
        double gap=now-last_fresh_ms;if(gap>worst_frame_ms)worst_frame_ms=gap;hitches++;
    }
    if(repeat_reset)repeat_reset=0;
    last_fresh_ms=now;presented++;
    return 1;
}
/* Waits until it is time to emulate the next frame.
   vsync (display refresh within 1% of the game's rate): emulation runs as far ahead as the frame queue allows and the
   presenter releases one frame per refresh, so the game runs at the display's pace with no judder; the audio output
   rate follows the sound queue (by at most 0.5%) so sound never drifts. This is RetroArch's approach.
   Otherwise the sound card's clock paces emulation and the presenter shows the newest frame at each refresh.
   Either way the sound queue is capped, so a stalled display cannot let audio run away. */
static void pace_frame(void){
    if(!wave)return;
    /* WSOLA can need 55 ms of input before its first stretched block. Smooth
       playback reserves enough supply for that startup plus a guest frame. */
    double target=max_pending==1?64.0:AUDIO_TARGET_MS;
    double t0=now_ms();
    if(vsync_mode){
        while(!quit&&queued_frames()>=(unsigned)max_pending){
            MsgWaitForMultipleObjects(1,&frame_taken,FALSE,20,QS_ALLINPUT);
            MSG msg;while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        }
    }
    double t1=now_ms();EnterCriticalSection(&audio_lock);double queued=audio_queued_ms();LeaveCriticalSection(&audio_lock);
    if(queued<queue_min){queue_min=queued;}
    if(queued>queue_max){queue_max=queued;}
    const double produced_queue=queued;
    while(queued>(vsync_mode?target*2:target)&&!quit){Sleep(1);EnterCriticalSection(&audio_lock);queued=audio_queued_ms();LeaveCriticalSection(&audio_lock);}
    ms_vblank=t1-t0;ms_audio_wait=now_ms()-t1;
    if(queued<4.0)underruns++;
    if(vsync_mode){double error=(queued-target)/target;if(error>1)error=1;if(error<-1)error=-1;rate_ratio=1.0-0.005*error;}
    /* Stretch tempo, as PCSX2's TimeStretch sync does: a queue falling below its target means the game makes less sound
       than the card plays, so slow the sound until both match. Proportional, smoothed so changes are not heard, and back
       to exactly 1 (stretcher idle) once the queue recovers. */
    audio_tempo=eh_audio_sync_tempo(audio_tempo,produced_queue,target,audio_work_ratio);
}
static LRESULT CALLBACK wnd_proc(HWND hwnd,UINT msg,WPARAM w,LPARAM l){
    if(msg==WM_CLOSE){quit=1;return 0;}
    if(msg==WM_ENTERSIZEMOVE)in_size_move=1;
    if(msg==WM_EXITSIZEMOVE)in_size_move=0;
    if(msg==WM_DESTROY&&embedded){quit=1;return 0;} /* The launcher window closed; owned windows go with it. */
    if(msg==WM_KEYDOWN&&(w==VK_ESCAPE||w==VK_SPACE)){
        if(!(l&((LPARAM)1<<30))){if(controlled)menu_requested=1;else if(w==VK_ESCAPE)quit=1;}
        return 0;
    }
    if(msg==WM_LBUTTONDBLCLK){
        int x=(short)LOWORD(l),y=(short)HIWORD(l);
        if(!paused&&x>=view_x&&y>=view_y&&x<view_x+view_w&&y<view_y+view_h&&!nds_touch_at(x,y)){
            if(embedded)eventf("{\"event\":\"fullscreen-toggle\"}");
            else{
                static WINDOWPLACEMENT placement;static LONG_PTR style;static int fullscreen;
                if(!fullscreen){
                    MONITORINFO monitor;memset(&monitor,0,sizeof(monitor));monitor.cbSize=sizeof(monitor);placement.length=sizeof(placement);style=GetWindowLongPtrW(hwnd,GWL_STYLE);
                    if(GetWindowPlacement(hwnd,&placement)&&GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor)){
                        SetWindowLongPtrW(hwnd,GWL_STYLE,style&~WS_OVERLAPPEDWINDOW);
                        SetWindowPos(hwnd,HWND_TOP,monitor.rcMonitor.left,monitor.rcMonitor.top,monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top,SWP_NOOWNERZORDER|SWP_FRAMECHANGED);fullscreen=1;
                    }
                }else{
                    SetWindowLongPtrW(hwnd,GWL_STYLE,style);SetWindowPlacement(hwnd,&placement);
                    SetWindowPos(hwnd,NULL,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_FRAMECHANGED);fullscreen=0;
                }
            }
        }
        return 0;
    }
    if(msg==WM_MOUSEACTIVATE&&embedded)return MA_NOACTIVATE;
    if(msg==WM_ERASEBKGND)return 1; /* present() paints every pixel; erasing first would flicker. */
    if(msg==WM_PAINT){PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);
        if(gpu_present){EndPaint(hwnd,&ps);return 0;}
        if(shown<QUEUE_SLOTS)present(hwnd,dc,slots[shown],slot_w[shown],slot_h[shown],slot_layout[shown]);else present(hwnd,dc,framebuffer,frame_width,frame_height,nds_layout_index);
        EndPaint(hwnd,&ps);return 0;}
    return DefWindowProcW(hwnd,msg,w,l);
}
#endif

/* Writes a complete file next to the target, flushes it, then swaps it in; optionally keeps the old file as .bak. */
static int write_atomic(const char *path,const void *data,size_t size,int keep_backup){return eh_atomic_write(path,data,size,keep_backup);}
static void put32(uint8_t *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
/* The current frame as a 24-bit BMP, at the core's native resolution. */
static int write_bmp(const char *path){
    if(gpu_present){unsigned w=0,h=0;if(hw_vk?eh_vk_capture(framebuffer,&w,&h):eh_d3d_capture(framebuffer,&w,&h)){frame_width=w;frame_height=h;}}
    if(!frame_width||!frame_height)return 0;
    size_t row=((size_t)frame_width*3+3)&~(size_t)3,size=54+row*frame_height;uint8_t *bmp=calloc(1,size);if(!bmp)return 0;
    bmp[0]='B';bmp[1]='M';put32(bmp+2,(uint32_t)size);put32(bmp+10,54);put32(bmp+14,40);put32(bmp+18,frame_width);put32(bmp+22,frame_height);bmp[26]=1;bmp[28]=24;put32(bmp+34,(uint32_t)(row*frame_height));
    for(unsigned y=0;y<frame_height;y++)for(unsigned x=0;x<frame_width;x++){uint32_t rgb=framebuffer[(frame_height-1-y)*frame_width+x];uint8_t *p=bmp+54+y*row+x*3;p[0]=rgb;p[1]=rgb>>8;p[2]=rgb>>16;}
    int ok=write_atomic(path,bmp,size,0);free(bmp);return ok;
}

/* Every dynamically resolved function uses the canonical prototype's signature. */
#define FUNCTIONS(X) \
 X(unsigned,retro_api_version,(void)) X(void,retro_init,(void)) X(void,retro_deinit,(void)) \
 X(void,retro_set_environment,(retro_environment_t)) X(void,retro_set_video_refresh,(retro_video_refresh_t)) \
 X(void,retro_set_audio_sample,(retro_audio_sample_t)) X(void,retro_set_audio_sample_batch,(retro_audio_sample_batch_t)) \
 X(void,retro_set_input_poll,(retro_input_poll_t)) X(void,retro_set_input_state,(retro_input_state_t)) \
 X(void,retro_get_system_info,(struct retro_system_info*)) X(void,retro_get_system_av_info,(struct retro_system_av_info*)) \
 X(bool,retro_load_game,(const struct retro_game_info*)) X(void,retro_unload_game,(void)) X(void,retro_run,(void)) \
 X(void,retro_reset,(void)) X(size_t,retro_serialize_size,(void)) X(bool,retro_serialize,(void*,size_t)) X(bool,retro_unserialize,(const void*,size_t)) \
 X(void,retro_set_controller_port_device,(unsigned,unsigned)) X(void*,retro_get_memory_data,(unsigned)) X(size_t,retro_get_memory_size,(unsigned))
#define DECLARE(ret,name,args) typedef ret (RETRO_CALLCONV *name##_fn) args; name##_fn p_##name;

/* Where an engine's frame spends time inside the player's callbacks (reported per frame at the end of a session). */
static double cb_video_ms,cb_audio_ms,cb_input_ms;
static double cb_now(void){
#ifdef _WIN32
    return now_ms();
#else
    return 0;
#endif
}
static void timed_video(const void *data,unsigned width,unsigned height,size_t pitch){double t=cb_now();video(data,width,height,pitch);cb_video_ms+=cb_now()-t;}
static size_t timed_audio(const int16_t *data,size_t frames){double t=cb_now();size_t n=audio_batch(data,frames);cb_audio_ms+=cb_now()-t;return n;}
static void timed_poll(void){double t=cb_now();input_poll();cb_input_ms+=cb_now()-t;}
static int16_t timed_state(unsigned port,unsigned device,unsigned index,unsigned id){double t=cb_now();int16_t v=input_state(port,device,index,id);cb_input_ms+=cb_now()-t;return v;}

int eh_run(const char *core,const char *content,const char *save,unsigned limit,int mode,uintptr_t parent){
    const char *diagnostic=getenv("EMUHALL_DIAGNOSTICS");diagnostic_mode=diagnostic&&!strcmp(diagnostic,"1");
#ifndef _WIN32
    uint32_t *display_frame=NULL;
#endif

    int result=1,initialized=0,loaded=0;void *rom=NULL;FILE *file=NULL;size_t rom_size=0;unsigned frames=0,paced=0;
    variable_states=getenv("EMUHALL_VARIABLE_STATES")&&!strcmp(getenv("EMUHALL_VARIABLE_STATES"),"1");
    show_window=mode==MODE_WINDOW||mode==MODE_EMBED;embedded=mode==MODE_EMBED;controlled=mode==MODE_EMBED||mode==MODE_CONTROL||getenv("EMUHALL_WAIT_FOR_LAUNCHER");
    FUNCTIONS(DECLARE)
#ifdef _WIN32
    InitializeCriticalSection(&queue_lock);InitializeCriticalSection(&paint_lock);InitializeCriticalSection(&event_lock);event_lock_ready=1;
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    wchar_t *core_w=wide(core);HMODULE library=core_w?LoadLibraryExW(core_w,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS):NULL;free(core_w);
#define LOAD(ret,name,args) { FARPROC symbol=GetProcAddress(library,#name); if(sizeof(symbol)!=sizeof(p_##name))goto cleanup; memcpy(&p_##name,&symbol,sizeof(p_##name)); if(!p_##name){fprintf(stderr,"Missing required core symbol: %s\n",#name);goto cleanup;} }
#else
    (void)parent;
    void *library=dlopen(core,RTLD_NOW|RTLD_LOCAL);
    if(!library){const char *why=dlerror();fprintf(stderr,"Engine library: %s\n",why?why:"unknown error");}
#define LOAD(ret,name,args) p_##name=(name##_fn)dlsym(library,#name);if(!p_##name){fprintf(stderr,"Missing required core symbol: %s\n",#name);goto cleanup;}

#endif
    if(!library){fprintf(stderr,"Cannot load this emulator build\n");goto cleanup;}
    /* Engines get the managed per-game profile folder, never the launcher's working directory. */
    if(snprintf(profile_dir,sizeof(profile_dir),"%s",save)>=(int)sizeof(profile_dir))goto cleanup;
    {char *slash=strrchr(profile_dir,'/'),*back=strrchr(profile_dir,'\\');if(back>slash)slash=back;if(slash){*slash=0;system_dir=profile_dir;}}
    load_option_overrides();
    FUNCTIONS(LOAD)
#ifdef _WIN32
    FARPROC cheat_symbol=GetProcAddress(library,"retro_cheat_set"),reset_symbol=GetProcAddress(library,"retro_cheat_reset");memcpy(&set_cheat,&cheat_symbol,sizeof(set_cheat));memcpy(&reset_cheats,&reset_symbol,sizeof(reset_cheats));FARPROC typed_symbol=GetProcAddress(library,"emuhall_cheat_set_v1");memcpy(&typed_cheat,&typed_symbol,sizeof(typed_cheat));
#else
    set_cheat=(cheat_set_fn)dlsym(library,"retro_cheat_set");reset_cheats=(cheat_reset_fn)dlsym(library,"retro_cheat_reset");typed_cheat=(typed_cheat_fn)dlsym(library,"emuhall_cheat_set_v1");
#endif
    if(p_retro_api_version()!=RETRO_API_VERSION){fprintf(stderr,"Unsupported Libretro API version\n");goto cleanup;}
    framebuffer=calloc(2048*2048,sizeof(uint32_t));if(!framebuffer)goto cleanup;
    p_retro_set_environment(environment);p_retro_set_video_refresh(timed_video);p_retro_set_audio_sample(audio_sample);p_retro_set_audio_sample_batch(timed_audio);p_retro_set_input_poll(timed_poll);p_retro_set_input_state(timed_state);
    p_retro_init();initialized=1;
    struct retro_system_info info;memset(&info,0,sizeof(info));p_retro_get_system_info(&info);
    fprintf(stderr,"Engine: %s %s\n",info.library_name?info.library_name:"?",info.library_version?info.library_version:"?");
    eh_ra_set_core(info.library_name,info.library_version);
    struct retro_game_info game;memset(&game,0,sizeof(game));game.path=content;
    if(mode==MODE_INSPECT){
        /* No user content: a short synthetic probe lets engines which register options during
           load_game advertise their definitions. Never run frames or access a user's saves. */
        static const unsigned char probe[65536]="EMUHALL-OPTION-PROBE";
        game.data=probe;game.size=32;
        emit_properties();
        if(p_retro_load_game(&game)){loaded=1;}
        emit_properties();result=0;goto cleanup;
    }
    if(!info.need_fullpath){file=open_file(content,"rb");if(!file){fprintf(stderr,"Game file unavailable\n");goto cleanup;}if(fseek(file,0,SEEK_END))goto cleanup;long length=ftell(file);if(length<=0||length>1024L*1024*1024){fprintf(stderr,"Game is larger than 1 GB; this engine would need to stream it\n");goto cleanup;}rewind(file);rom_size=(size_t)length;/* Snes9x checks both LoROM and HiROM header offsets before testing the length.
       Keep a zeroed 64K backing allocation for small homebrew without changing the declared bytes or identity. */
    rom=calloc(rom_size<65536?65536:rom_size,1);if(!rom||fread(rom,1,rom_size,file)!=rom_size)goto cleanup;fclose(file);file=NULL;game.data=rom;game.size=rom_size;}
    if(!p_retro_load_game(&game)){fprintf(stderr,"%s\n",hw_renderer_error?hw_renderer_error:"This engine could not load the game");goto cleanup;}loaded=1;
#ifdef _WIN32
    /* melonDS registers its layout options in load_game, not set_environment. */
    nds_pointer_layout=strcmp(core_option("melonds_screen_layout1",""),"")!=0;
#endif
    for(unsigned port=0;port<controller_ports;port++)p_retro_set_controller_port_device(port,controller_devices[port]);
    struct retro_system_av_info av;memset(&av,0,sizeof(av));p_retro_get_system_av_info(&av);
    /* Some cores publish conservative placeholder maxima, then set their actual layout during load_game. */
    if(geometry_reported)av.geometry=current_geometry;
    current_geometry=av.geometry;atomic_store(&display_aspect,av.geometry.aspect_ratio);geometry_reported=1;
    if(!av.geometry.base_width||!av.geometry.base_height||av.geometry.max_width>FRAME_LIMIT||av.geometry.max_height>FRAME_LIMIT||!isfinite(av.timing.fps)||av.timing.fps<1||av.timing.fps>1000||!isfinite(av.timing.sample_rate)||av.timing.sample_rate<8000||av.timing.sample_rate>192000){fprintf(stderr,"Unsupported video or audio geometry\n");goto cleanup;}
    {unsigned w=av.geometry.max_width>av.geometry.base_width?av.geometry.max_width:av.geometry.base_width,h=av.geometry.max_height>av.geometry.base_height?av.geometry.max_height:av.geometry.base_height;
     if(w>frame_cap_w||h>frame_cap_h){
        unsigned cw=w>frame_cap_w?w:frame_cap_w,ch=h>frame_cap_h?h:frame_cap_h;uint32_t *grown=calloc((size_t)cw*ch,sizeof(uint32_t));
        if(!grown){fprintf(stderr,"Not enough memory for this engine's picture\n");goto cleanup;}
        free(framebuffer);framebuffer=grown;frame_cap_w=cw;frame_cap_h=ch;
     }}
    if(hw_active){
        unsigned w=av.geometry.max_width>av.geometry.base_width?av.geometry.max_width:av.geometry.base_width;
        unsigned h=av.geometry.max_height>av.geometry.base_height?av.geometry.max_height:av.geometry.base_height;
        if(hw_vk&&!eh_vk_create()){fprintf(stderr,"Vulkan is not available on this graphics card. Choose another renderer in the game's console options.\n");goto cleanup;}
        if(!hw_d3d&&!hw_vk&&!eh_gl_framebuffer(w,h,hw.depth,hw.stencil)){fprintf(stderr,"The graphics card could not set up this engine's picture\n");goto cleanup;}
        if(hw.context_reset)hw.context_reset();
    }
    size_t save_size=p_retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);void *save_memory=p_retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if(save_size>16*1024*1024||(save_size&&!save_memory)){fprintf(stderr,"Invalid save memory\n");goto cleanup;}
    if(save_size){file=open_file(save,"rb");if(file){
        /* Battery saves are the same bytes in every engine, but engines pad them differently (SkyEmu keeps a fixed
           128 KB Game Boy buffer, mGBA the cartridge's real size). Load the overlapping part; the original file is
           kept as .bak when this session writes its save. */
        if(fseek(file,0,SEEK_END)){goto cleanup;}
        long length=ftell(file);if(length<0){goto cleanup;}
        rewind(file);
        size_t usable=(size_t)length<save_size?(size_t)length:save_size;
        if((size_t)length!=save_size)fprintf(stderr,"Save file is %ld bytes and this engine uses %zu; loading the first %zu\n",length,save_size,usable);
        if(fread(save_memory,1,usable,file)!=usable){goto cleanup;}
        fclose(file);file=NULL;}}
    const char *resume_slot=getenv("EMUHALL_RESUME_SLOT");
    int continue_session=getenv("EMUHALL_RESUME_CONTINUE")&&!strcmp(getenv("EMUHALL_RESUME_CONTINUE"),"1");
    if(continue_session)resume_slot="0";
    if(resume_slot&&*resume_slot){
        char *end;unsigned long slot=strtoul(resume_slot,&end,10);char path[4300];
        if(*end||(!continue_session&&(slot<1||slot>10))||netplay_active||eh_ra_hardcore())goto cleanup;
        if(continue_session)snprintf(path,sizeof(path),"%s" SEP "resume.state",system_dir);
        else snprintf(path,sizeof(path),"%s" SEP "state%lu.state",system_dir,slot);
        const char *legacy=getenv("EMUHALL_LEGACY_STATE_SLOT");int older=legacy&&!strcmp(legacy,resume_slot);
        size_t size=older?eh_snapshot_legacy_size(path):eh_snapshot_size(path);void *state=size?malloc(size):NULL;
        int ok=state&&(older?eh_snapshot_legacy_load(path,state,size):eh_snapshot_load(path,state,size))==size&&(variable_states||size==p_retro_serialize_size());
        /* Hardware-rendered engines (PS2) finish creating their graphics device in their first frames; a state restored before
           that loses its picture (a flat colour). The verified state is applied once the first picture arrives. */
        if(ok&&hw_active){resume_state=state;resume_size=size;snprintf(resume_path,sizeof(resume_path),"%s",path);state=NULL;}
        else ok=ok&&p_retro_unserialize(state,size);
        free(state);
        if(!ok){fprintf(stderr,"%s Game not started.\n",!older&&eh_snapshot_settings_differ(path)?SETTINGS_STATE_MESSAGE:"The requested state could not be verified and restored.");goto cleanup;}
        if(!resume_state)eh_ra_load_progress(path);
    }
    if(controlled){
#ifdef _WIN32
        HANDLE reader=CreateThread(NULL,0,command_reader,NULL,0,NULL);if(!reader)goto cleanup;CloseHandle(reader);
#else
        pthread_t reader;if(pthread_create(&reader,NULL,command_reader,NULL))goto cleanup;pthread_detach(reader);
#endif
    }
    netplay_active=eh_netplay_start();if(netplay_active<0){if(quit)result=0;goto cleanup;}
    if(netplay_active)p_retro_set_controller_port_device(1,RETRO_DEVICE_JOYPAD);
#ifdef _WIN32
    LARGE_INTEGER frequency,start;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    if(show_window){
        /* Scale frames ourselves instead of Windows blurring the whole window; per-monitor so sizes match the launcher's. */
        typedef BOOL (WINAPI *SetDpiContextFn)(HANDLE);FARPROC dpi_symbol=GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetProcessDpiAwarenessContext");SetDpiContextFn set_dpi_context;memcpy(&set_dpi_context,&dpi_symbol,sizeof(set_dpi_context));
        if(!set_dpi_context||!set_dpi_context((HANDLE)-4))SetProcessDPIAware();
        WNDCLASSW wc;memset(&wc,0,sizeof(wc));wc.style=CS_DBLCLKS;wc.lpfnWndProc=wnd_proc;wc.hInstance=GetModuleHandleW(NULL);wc.lpszClassName=L"EmuHallPlay";wc.hCursor=LoadCursor(NULL,IDC_ARROW);RegisterClassW(&wc);
        if(embedded){
            /* A borderless popup owned by the launcher window and kept over its client area. Owned windows always
               sit above their owner, so the launcher's GPU-composited web view can never cover the game (a child
               window would share the owner's surface and be painted over). It never activates: the launcher stays
               the foreground window and keyboard focus stays in the page. */
            parent_window=(HWND)parent;RECT r;POINT origin={0,0};
            if(!IsWindow(parent_window)||!GetClientRect(parent_window,&r)||!ClientToScreen(parent_window,&origin)){fprintf(stderr,"Launcher window unavailable\n");goto cleanup;}
            window=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,wc.lpszClassName,L"EmuHall game",WS_POPUP|WS_VISIBLE,origin.x,origin.y,r.right,r.bottom,parent_window,NULL,wc.hInstance,NULL);if(!window)goto cleanup;
        }else{
            HDC screen=GetDC(NULL);int dpi=screen?GetDeviceCaps(screen,LOGPIXELSX):96;if(screen)ReleaseDC(NULL,screen);if(dpi<96)dpi=96;
            window=CreateWindowW(wc.lpszClassName,L"EmuHall | Arrows, Z/X, Enter | Esc to return",WS_OVERLAPPEDWINDOW|WS_VISIBLE,CW_USEDEFAULT,CW_USEDEFAULT,960*dpi/96,720*dpi/96,NULL,NULL,wc.hInstance,NULL);if(!window)goto cleanup;
            ShowWindow(window,SW_SHOWNORMAL);
        }
        UpdateWindow(window);
        timeBeginPeriod(1);prefer_realtime_scheduling();core_rate=av.timing.sample_rate;
        DWM_TIMING_INFO timing;memset(&timing,0,sizeof(timing));timing.cbSize=sizeof(timing);
        if(DwmGetCompositionTimingInfo(NULL,&timing)==S_OK&&timing.rateRefresh.uiDenominator){double hz=(double)timing.rateRefresh.uiNumerator/timing.rateRefresh.uiDenominator;vsync_mode=fabs(hz/av.timing.fps-1.0)<0.01;refresh_ms=1000.0/hz;}
        open_vblank(embedded?parent_window:GetDesktopWindow());
        for(int i=0;i<QUEUE_SLOTS;i++){slots[i]=calloc((size_t)frame_cap_w*frame_cap_h,4);if(!slots[i])goto cleanup;}
        InitializeCriticalSection(&frame_lock);frame_taken=CreateEventW(NULL,FALSE,FALSE,NULL);
        InitializeCriticalSection(&overlay_lock);
        if(hw_d3d==RETRO_HW_CONTEXT_D3D11&&window)gpu_present=eh_d3d_present_attach(window);
        else if(hw_vk&&window)gpu_present=eh_vk_present_attach(window);
        /* The swap chain shows each frame as it comes; the presenter's queue (and the display-paced mode that waits on it)
           is not used, so the sound card paces the game. */
        if(gpu_present)vsync_mode=0;
        presenter=CreateThread(NULL,0,present_loop,NULL,0,NULL);if(!presenter||!frame_taken)goto cleanup;
        WAVEFORMATEX format;memset(&format,0,sizeof(format));format.wFormatTag=WAVE_FORMAT_PCM;format.nChannels=2;format.nSamplesPerSec=OUT_RATE;format.wBitsPerSample=16;format.nBlockAlign=4;format.nAvgBytesPerSec=format.nSamplesPerSec*4;
        if(waveOutOpen(&wave,WAVE_MAPPER,&format,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR){fprintf(stderr,"Cannot open audio output\n");goto cleanup;}
        InitializeCriticalSection(&audio_lock);
        for(unsigned i=0;i<BLOCKS;i++){
            WAVEHDR *header=&audio_headers[i];memset(header,0,sizeof(*header));header->lpData=(char*)audio_blocks[i];header->dwBufferLength=BLOCK_FRAMES*4;
            if(waveOutPrepareHeader(wave,header,sizeof(*header))!=MMSYSERR_NOERROR){fprintf(stderr,"Cannot prepare audio output buffers\n");goto cleanup;}
        }
        audio_wake=CreateEventW(NULL,FALSE,FALSE,NULL);
        audio_prime_enabled=getenv("EMUHALL_SYSTEM")&&!strcmp(getenv("EMUHALL_SYSTEM"),"ps2");
        if(audio_prime_enabled)audio_priming=waveOutPause(wave)==MMSYSERR_NOERROR;
        audio_thread=audio_wake?CreateThread(NULL,0,audio_loop,NULL,0,NULL):NULL;if(!audio_thread)goto cleanup;
        if(!eh_pad_init())fprintf(stderr,"SDL3.dll unavailable: only Xbox-style (XInput) controllers will work\n");
    }
#endif
#ifndef _WIN32
    if(show_window){if(!eh_desktop_open(av.timing.sample_rate,0))goto cleanup;eh_pad_init();display_frame=malloc((size_t)frame_cap_w*frame_cap_h*4u);if(!display_frame)goto cleanup;}
    double deadline=show_window?now_ms():0;pace_debug=getenv("EMUHALL_PACE_DEBUG")!=NULL;
#endif
    temporal_supported=getenv("EMUHALL_TEMPORAL_SUPPORTED")&&!strcmp(getenv("EMUHALL_TEMPORAL_SUPPORTED"),"1")&&!hw_active&&!netplay_active;
    runahead_supported=temporal_supported&&(!getenv("EMUHALL_RUNAHEAD_SUPPORTED")||!strcmp(getenv("EMUHALL_RUNAHEAD_SUPPORTED"),"1"));
    if(controlled)eventf("{\"event\":\"started\"}");
    int launch_wait=getenv("EMUHALL_WAIT_FOR_LAUNCHER")&&!strcmp(getenv("EMUHALL_WAIT_FOR_LAUNCHER"),"1");
    double fps_start=0;unsigned fps_frames=0;
    {
#ifdef _WIN32
        HANDLE dog=CreateThread(NULL,0,watchdog_thread,NULL,0,NULL);if(dog)CloseHandle(dog);
#else
        pthread_t dog;if(!pthread_create(&dog,NULL,watchdog_thread,NULL))pthread_detach(dog);
#endif
        /* Netplay waits for the other player inside a frame, with its own timeouts. */
        watchdog_armed=!netplay_active;
    }
    while(!quit&&(!limit||frames<limit)){
        heartbeat++;
        if(paused||launch_wait){fps_start=0;fps_frames=0;}
#ifdef _WIN32
        if(show_window){MSG msg;while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
        if(embedded&&!quit){
            /* Follow the launcher window's client area as it moves and resizes; stop if it is gone. */
            RECT pr,wr;POINT origin={0,0};if(!IsWindow(parent_window)){quit=1;break;}
            GetClientRect(parent_window,&pr);ClientToScreen(parent_window,&origin);GetWindowRect(window,&wr);
            if(wr.left!=origin.x||wr.top!=origin.y||wr.right-wr.left!=pr.right||wr.bottom-wr.top!=pr.bottom)SetWindowPos(window,NULL,origin.x,origin.y,pr.right,pr.bottom,SWP_NOZORDER|SWP_NOACTIVATE);
            /* The popup never has focus. Read pause keys on their rising edges, so a held Space cannot reopen
               the menu when the page resumes the session. */
            static int esc_down=1,space_down=1;int esc=(GetAsyncKeyState(VK_ESCAPE)&0x8000)!=0,space=(GetAsyncKeyState(VK_SPACE)&0x8000)!=0;
            if(((esc&&!esc_down)||(space&&!space_down))&&has_input_focus())menu_requested=1;
            esc_down=esc;space_down=space;
        }
#endif
#ifndef _WIN32
        if(show_window){int events=eh_desktop_poll();if(events&2)quit=1;if(events&1){if(controlled)menu_requested=1;else quit=1;}}
#endif
        if(quit)break;
        if(!paused&&!launch_wait){
            double t0=now_ms();
#ifdef _WIN32
            size_t audio_before_run=audio_frames;
#endif
            if(netplay_active){
                uint16_t local=0;
                eh_pad_read(&pad);if(has_input_focus()){local=mapped_pad();for(unsigned i=0;i<16;i++)if(key_down(i))local|=1u<<i;}
                int64_t inputs=eh_netplay_frame(frames,local,frame_hash);if(inputs<0){if(quit)result=0;goto cleanup;}
                netplay_inputs[0]=(uint16_t)inputs;netplay_inputs[1]=(uint16_t)(inputs>>16);
            }
            if(eh_memory_cheat_active()&&!eh_ra_hardcore()&&!netplay_active)eh_memory_cheat_apply(p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM),p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM));
            p_retro_run();frames++;paced++;eh_ra_frame();
            if(early_cheats_pending){if(reset_cheats&&set_cheat&&!eh_ra_hardcore()&&!netplay_active){reset_cheats();for(unsigned i=0;i<32;i++)if(early_cheat[i])set_cheat(i,early_enabled[i],early_cheat[i]);}early_cheats_clear();}
#ifdef _WIN32
            if(wave)audio_window(av.timing.fps);
#endif
            if(resume_state&&(video_frames||frames>=600)){ /* once a picture has been delivered: the engine is fully up */
                int restored=p_retro_unserialize(resume_state,resume_size);free(resume_state);resume_state=NULL;
                if(!restored){fprintf(stderr,"%s Game not started.\n",eh_snapshot_settings_differ(resume_path)?SETTINGS_STATE_MESSAGE:"The requested state could not be restored.");goto cleanup;}
#ifdef _WIN32
                audio_reprime();
#endif
                eh_ra_load_progress(resume_path);
            }
            {double stamp=(double)hint_now();if(!fps_start)fps_start=stamp;fps_frames++;if(stamp-fps_start>=1000){int measured=(int)(fps_frames*1000000.0/(stamp-fps_start));atomic_store(&fps_milli,measured);eventf("{\"event\":\"metrics\",\"fps\":%.2f}",measured/1000.0);fps_start=stamp;fps_frames=0;}}
            if(temporal_supported&&!eh_ra_temporal_safe()){temporal_supported=rewind_enabled=runahead_enabled=0;eh_history_clear();}
            if(temporal_supported&&!eh_ra_hardcore()&&!fault){
                size_t state_size=(runahead_enabled||(rewind_enabled&&frames%6==0))?p_retro_serialize_size():0;
                if(state_size&&state_size<=32u*1024*1024){
                    void *state=malloc(state_size);
                    if(state&&p_retro_serialize(state,state_size)){
                        if(rewind_enabled&&frames%6==0)eh_history_push(state,state_size,framebuffer,frame_width,frame_height);
                        if(runahead_enabled){
                            for(unsigned i=0;i<16;i++)cached_keys[i]=key_down(i);
                            speculating=1;p_retro_run();speculating=0;
                            if(!p_retro_unserialize(state,state_size)){free(state);fault=1;fprintf(stderr,"Runahead restore failed; previous saves preserved\n");goto cleanup;}
                        }
                    }else{rewind_enabled=runahead_enabled=0;eventf("{\"event\":\"runtime\",\"message\":\"This engine could not create a rewind snapshot.\"}");}
                    free(state);
                }else if(runahead_enabled||(rewind_enabled&&frames%6==0)){
                    temporal_supported=rewind_enabled=runahead_enabled=0;
                    eventf("{\"event\":\"runtime\",\"temporalSupported\":false,\"rewind\":false,\"runahead\":false,\"message\":\"This game cannot provide snapshots within the history memory limit.\"}");
                }
            }
#ifndef _WIN32
            /* Emulation time per frame (including a GPU engine's read-back), for the session summary. */
            {double run_ms=now_ms()-t0;emulate_total_ms+=run_ms;if(run_ms>emulate_worst_ms)emulate_worst_ms=run_ms;}
            {double p0=now_ms();
            if(show_window&&frame_ready){memcpy(display_frame,framebuffer,(size_t)frame_width*frame_height*4);presentation_filter(display_frame,frame_width,frame_height);if(!eh_desktop_video(display_frame,frame_width,frame_height,integer_scaling,smooth_scaling)){fault=1;quit=1;}frame_ready=0;}
            pace_present_ms+=now_ms()-p0;}
#endif
#ifdef _WIN32
            double t1=now_ms();
            /* Engines drawing on the graphics card can still send a few ordinary frames (PCEE2 around a state load); with a
               swap chain nothing drains the queue, so they are not queued. */
            if(presenter&&frame_ready){if(!gpu_present)queue_frame();frame_ready=0;}
            ms_run=t1-t0;ms_present=now_ms()-t1;emulate_total_ms+=ms_run;if(ms_run>emulate_worst_ms)emulate_worst_ms=ms_run;
            if(wave&&core_rate>0&&audio_frames>audio_before_run){
                /* Audio gives guest duration even when a core advances two
                   console frames per call. Exclude the player's pacing wait. */
                double guest_ms=(double)(audio_frames-audio_before_run)*1000.0/core_rate;
                double ratio=ms_run/guest_ms;if(ratio>3.0)ratio=3.0;
                audio_work_ratio+=(ratio-audio_work_ratio)*0.1;
            }
            /* Development aid: EMUHALL_FRAME_DUMP=<folder> saves the picture and the average emulation time of
               every 500 frames, to check engines without a window. */
            static const char *dump_dir;static int dump_checked;static double dump_total;
            if(!dump_checked){dump_dir=getenv("EMUHALL_FRAME_DUMP");dump_checked=1;}
            if(dump_dir){dump_total+=ms_run;if(frames%500==0){char path[4300];snprintf(path,sizeof(path),"%s" SEP "frame-%05u.bmp",dump_dir,frames);
                write_bmp(path);fprintf(stderr,"frames %u-%u: %.2f ms per frame\n",frames-499,frames,dump_total/500);dump_total=0;}}
#endif
        }
        if(paused||launch_wait||!has_input_focus())eh_pad_stop_rumble();
        int resync=0;char command[8192];
        while(controlled&&queue_pop(command)){
            if(netplay_active&&strcmp(command,"quit")&&strcmp(command,"start")&&strcmp(command,"properties")&&strcmp(command,"controllers")&&strncmp(command,"set ",4)&&strncmp(command,"input-",6))continue;
            if(!strcmp(command,"start")){launch_wait=0;resync=1;continue;}
            /* "core-option KEY VALUE": an engine option changed in the launcher while playing (never in netplay, above). */
            if(!strncmp(command,"core-option ",12)){
                char key[128]={0};const char *rest=command+12;size_t n=strcspn(rest," ");
                if(n&&n<sizeof(key)&&rest[n]==' '){memcpy(key,rest,n);set_option_override(key,rest+n+1);}
                continue;
            }
            if(!strncmp(command,"set ",4)){
                char name[32]={0},value[32]={0};
                if(sscanf(command+4,"%31s %31s",name,value)==2){
                    if(!strcmp(name,"fps"))atomic_store(&fps_visible,!strcmp(value,"on"));
                    else if(!strcmp(name,"rumble"))eh_pad_rumble_enabled(!strcmp(value,"on"));
                    else if(!strcmp(name,"filter")){screen_filter=!strcmp(value,"crt")?1:!strcmp(value,"lcd")?2:!strcmp(value,"crt-soft")?3:!strcmp(value,"handheld")?4:!strcmp(value,"warm")?5:0;shader_failed=0;}
                    else if(!strcmp(name,"bezel"))atomic_store(&bezel_style,!strcmp(value,"console")?1:!strcmp(value,"arcade")?2:0);
                    else if(!strcmp(name,"rewind")){rewind_enabled=temporal_supported&&eh_ra_temporal_safe()&&!netplay_active&&!eh_ra_hardcore()&&!strcmp(value,"on");if(!rewind_enabled)eh_history_clear();}
                    else if(!strcmp(name,"runahead"))runahead_enabled=runahead_supported&&eh_ra_temporal_safe()&&!netplay_active&&!eh_ra_hardcore()&&!strcmp(value,"1");
                    else if(!strcmp(name,"scaling"))integer_scaling=!strcmp(value,"integer");
                    else if(!strcmp(name,"smoothing"))smooth_scaling=!strcmp(value,"on");
                    else if(!strcmp(name,"volume")){int v=atoi(value);if(v>=0&&v<=100)audio_volume=v;}
                    else if(!strcmp(name,"mute"))audio_muted=!strcmp(value,"on");
                    else if(!strcmp(name,"fast-mute"))fast_forward_mute=!strcmp(value,"on");
                    else if(!strcmp(name,"fast-speed")){unsigned speed=(unsigned)atoi(value);if(speed==150||speed==200||speed==300)fast_forward_speed=speed;}
                    else if(!strcmp(name,"latency"))max_pending=!strcmp(value,"low")?1:2;
                    else if(!strncmp(name,"sys.",4)){int code=atoi(value);for(unsigned i=0;i<21;i++)if(!strcmp(name+4,system_names[i])&&code>0&&code<256)system_keys[i]=code;}
                    else if(!strncmp(name,"pad.",4)){int code=atoi(value);for(int i=0;i<16;i++)if(!strcmp(name+4,key_names[i])&&code>=-1&&code<16)pad_map[i]=code<0?i:code;}
                    else if(!strncmp(name,"key.",4)){int code=atoi(value);for(int i=0;i<16;i++)if(!strcmp(name+4,key_names[i])&&code>0&&code<256)key_map[i]=code;}
                }
#ifdef _WIN32
                if(window)InvalidateRect(window,NULL,FALSE);
#endif
                if(!strcmp(name,"rewind")||!strcmp(name,"runahead"))emit_properties();
                continue;
            }
            if(!strncmp(command,"ra-login ",9)){
                eh_ra_start(command+9,content,rom,rom_size,(void*(*)(unsigned))p_retro_get_memory_data,(size_t(*)(unsigned))p_retro_get_memory_size);
                memset(command,0,sizeof(command));continue; /* The token does not linger in this buffer. */
            }
            if(!strcmp(command,"cheat-reset")){if(!eh_ra_hardcore()){eh_memory_cheat_reset();if(reset_cheats)reset_cheats();if(!frames)early_cheats_clear();}continue;}
            if(!strncmp(command,"cheat-memory ",13)){unsigned index,kind;int offset=0;const char *system=getenv("EMUHALL_SYSTEM");if(!eh_ra_hardcore()&&!netplay_active&&system&&sscanf(command+13,"%u %u %n",&index,&kind,&offset)==2&&offset>0&&((kind==10&&!strcmp(system,"n64"))||(kind==11&&!strcmp(system,"ps2")))&&!eh_memory_cheat_set(index,kind,command+13+offset,p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM)))eventf("{\"event\":\"error\",\"message\":\"Unsupported data-memory cheat. No code was applied.\"}");continue;}
            if(!strncmp(command,"cheat-typed ",12)){unsigned index,kind;int offset=0;if(!eh_ra_hardcore()&&typed_cheat&&sscanf(command+12,"%u %u %n",&index,&kind,&offset)==2&&index<32&&kind>=1&&kind<=4&&offset>0&&!typed_cheat(index,true,kind,command+12+offset))eventf("{\"event\":\"error\",\"message\":\"The engine rejected this cheat. This code was not applied.\"}");continue;}
            if(!strncmp(command,"cheat ",6)){
                unsigned index,enabled;int offset=0;
                if(!eh_ra_hardcore()&&set_cheat&&sscanf(command+6,"%u %u %n",&index,&enabled,&offset)==2&&index<32&&enabled<=1&&offset>0){
                    set_cheat(index,enabled,command+6+offset);
                    if(!frames){free(early_cheat[index]);early_cheat[index]=copy_text(command+6+offset);early_enabled[index]=(unsigned char)enabled;early_cheats_pending=early_cheat[index]!=NULL;}
                }
                continue;
            }
            if(!strncmp(command,"input-forget ",13)){eh_controller_forget(command+13);eh_pad_list();continue;}
            if(!strncmp(command,"input-profile ",14)){if(!eh_controller_profile(command+14))eventf("{\"event\":\"error\",\"message\":\"Invalid controller profile.\"}");else eh_pad_list();continue;}
            if(!strcmp(command,"controllers")){eh_pad_list();continue;}
            if(!strcmp(command,"properties")){emit_properties();continue;}
            if(!strcmp(command,"rewind")){
                if(!temporal_supported||!rewind_enabled||eh_ra_hardcore()||!eh_ra_temporal_safe()||netplay_active){eventf("{\"event\":\"error\",\"message\":\"Rewind is unavailable for this session.\"}");continue;}
                size_t size=0;for(unsigned step=0;step<10&&eh_history_size();step++)size=eh_history_back();
                void *state=size?malloc(size):NULL;unsigned w=0,h=0;
                int copied=state&&eh_history_copy(state,size,framebuffer,&w,&h);
                int ok=copied&&p_retro_unserialize(state,size);free(state);
                if(copied&&!ok){fault=1;fprintf(stderr,"Rewind restore failed; previous saves preserved\n");goto cleanup;}
                if(!ok){eventf("{\"event\":\"error\",\"message\":\"No rewind snapshot is available yet.\"}");continue;}
                frame_width=w;frame_height=h;resync=1;
#ifdef _WIN32
                if(wave){EnterCriticalSection(&audio_lock);waveOutReset(wave);ring_count=0;ring_read=0;frames_submitted=position_base=last_position=0;resample_pos=0;eh_stretch_reset();audio_tempo=1.0;LeaveCriticalSection(&audio_lock);}
#else
                if(show_window)eh_desktop_pause(paused);
#endif
                if(paused){char path[4300];snprintf(path,sizeof(path),"%s" SEP "menu-frame.bmp",system_dir);write_bmp(path);}else frame_ready=1;
                eventf("{\"event\":\"rewound\"}");continue;
            }
            if(!strncmp(command,"change-disc ",12)){
                char *end;unsigned long index=strtoul(command+12,&end,10);
                if(!command[12]||*end||!disks.get_num_images||!disks.set_image_index||!disks.set_eject_state||!disks.get_image_index||!disks.get_eject_state||index>=disks.get_num_images()){
                    eventf("{\"event\":\"error\",\"message\":\"Disc switching is unavailable.\"}");continue;}
                unsigned previous=disks.get_image_index();int was_ejected=disks.get_eject_state();
                int ok=disks.set_eject_state(true)&&disks.set_image_index((unsigned)index)&&disks.set_eject_state(false);
                if(!ok){disks.set_eject_state(true);disks.set_image_index(previous);disks.set_eject_state(was_ejected);eventf("{\"event\":\"error\",\"message\":\"The engine could not change discs. Try again.\"}");}
                emit_properties();continue;
            }
            if(!strcmp(command,"achievements")){eh_ra_list();continue;}
            if(!strncmp(command,"track-achievement ",18)){
                char *end;unsigned long id=strtoul(command+18,&end,10);
                if(command[18]&&!*end&&id<=0xffffffffUL)eh_ra_track((unsigned)id);
                else eventf("{\"event\":\"error\",\"message\":\"Choose an achievement to track.\"}");
                continue;
            }
            unsigned slot=1;char *arg=strchr(command,' ');if(arg)*arg++=0;
            if(arg&&(!strcmp(command,"save-state")||!strcmp(command,"load-state"))){
                char *end;unsigned long parsed=strtoul(arg,&end,10);
                if(!*arg||*end||parsed<1||parsed>10){eventf("{\"event\":\"error\",\"message\":\"Choose a save slot from 1 to 10.\"}");continue;}
                slot=(unsigned)parsed;
            }
            char path[4300];snprintf(path,sizeof(path),"%s" SEP "state%u.state",system_dir,slot);
            if(!strcmp(command,"quit"))quit=1;
            else if(!strcmp(command,"diagnostic-input")&&diagnostic_mode){
                char *end=NULL;unsigned long mask=arg?strtoul(arg,&end,10):65536;
                if(!arg||!*arg||!end||*end||mask>65535)eventf("{\"event\":\"error\",\"message\":\"Invalid diagnostic input\"}");
                else{diagnostic_mask=(int)mask;eventf("{\"event\":\"diagnostic-input\",\"buttons\":%u}",(unsigned)mask);}
            }
            else if(!strcmp(command,"menu"))menu_requested=1;
            else if(!strcmp(command,"resume")){
                if(paused){paused=0;resync=1;
#ifdef _WIN32
                    audio_reset_window();
                    audio_reprime();
                    if(wave&&!audio_priming)waveOutRestart(wave);
#endif
#ifdef _WIN32
                    if(embedded)ShowWindow(window,SW_SHOWNOACTIVATE);
#else
                    if(show_window)eh_desktop_pause(0);
#endif
                }
                eventf("{\"event\":\"resumed\"}");
            }
            else if(!strcmp(command,"reset")){p_retro_reset();eh_ra_reset();eh_history_clear();eventf("{\"event\":\"reset\"}");}
            else if(!strcmp(command,"fast-forward")){fast_forward=!fast_forward;resync=1;system_message(fast_forward?"Fast forward · %u%%":"Normal speed",fast_forward_speed);eventf("{\"event\":\"fast-forward\",\"on\":%s}",fast_forward?"true":"false");}
            else if(!strcmp(command,"save-state")||!strcmp(command,"save-close")){
                int closing=!strcmp(command,"save-close");
                if(closing&&eh_ra_hardcore()){eventf("{\"event\":\"error\",\"message\":\"Save states are unavailable in Hardcore mode.\"}");continue;}
                if(closing)snprintf(path,sizeof(path),"%s" SEP "resume.state",system_dir);
                size_t size=p_retro_serialize_size();void *state=size&&size<=256u*1024*1024?malloc(size):NULL;
                if(state&&p_retro_serialize(state,size)&&eh_snapshot_save(path,state,size)&&eh_ra_save_progress(path)){
                    char preview[4300];
                    if(closing)snprintf(preview,sizeof(preview),"%s" SEP "resume.bmp",system_dir);
                    else snprintf(preview,sizeof(preview),"%s" SEP "state%u.bmp",system_dir,slot);
                    if(!write_bmp(preview)){
                        /* Never show an older picture beside a newly saved state. The state remains usable even
                           when its optional preview cannot be written. */
#ifdef _WIN32
                        wchar_t *preview_w=wide(preview);if(preview_w){DeleteFileW(preview_w);free(preview_w);}
#else
                        remove(preview);
#endif
                        fprintf(stderr,"State %u saved, but its preview could not be written\n",slot);
                    }
                    if(closing){eventf("{\"event\":\"resume-saved\"}");quit_asked=1;quit=1;}
                    else {system_message("Saved to slot %u",slot);eventf("{\"event\":\"state-saved\",\"slot\":%u}",slot);}
                }
                else eventf("{\"event\":\"error\",\"message\":\"This game could not save a state right now.\"}");
                free(state);
            }
            else if(!strcmp(command,"load-state")&&eh_ra_hardcore())eventf("{\"event\":\"error\",\"message\":\"Loading states is turned off in RetroAchievements hardcore mode.\"}");
            else if(!strcmp(command,"load-state")){
                size_t size=eh_snapshot_size(path);void *state=size?malloc(size):NULL;
                if(!size){FILE *check=open_file(path,"rb");if(check){fclose(check);eventf("{\"event\":\"error\",\"message\":\"This state was saved with a different emulator version or game file.\"}");}else eventf("{\"event\":\"error\",\"message\":\"Slot %u is empty.\"}",slot);}
                else if(!variable_states&&size!=p_retro_serialize_size())eventf("{\"event\":\"error\",\"message\":\"%s\"}",eh_snapshot_settings_differ(path)?SETTINGS_STATE_MESSAGE:"This engine cannot restore that state size.");
                else if(state&&eh_snapshot_load(path,state,size)==size&&p_retro_unserialize(state,size)){
#ifdef _WIN32
                    audio_reprime();
#endif
                    eh_ra_load_progress(path);eh_history_clear();system_message("Loaded slot %u",slot);eventf("{\"event\":\"state-loaded\",\"slot\":%u}",slot);}
                else eventf("{\"event\":\"error\",\"message\":\"%s\"}",eh_snapshot_settings_differ(path)?SETTINGS_STATE_MESSAGE:"This state could not be verified or restored.");
                free(state);
            }
            else if(!strcmp(command,"screenshot")){
                snprintf(path,sizeof(path),"%s" SEP "screenshot-%u.bmp",system_dir,frames);
                if(write_bmp(path))eventf("{\"event\":\"screenshot\"}");else eventf("{\"event\":\"error\",\"message\":\"The screenshot could not be saved.\"}");
            }
            else eventf("{\"event\":\"error\",\"message\":\"Unknown command.\"}");
        }
        if(netplay_active&&menu_requested){quit=1;menu_requested=0;}
        if(menu_requested&&!quit){
            menu_requested=0;
            if(!paused){
                /* Freeze on this frame; the launcher draws its menu over a copy of it. */
                paused=1;char path[4300];snprintf(path,sizeof(path),"%s" SEP "menu-frame.bmp",system_dir);write_bmp(path);
#ifdef _WIN32
                if(wave)waveOutPause(wave);
#endif
#ifdef _WIN32
                if(embedded)ShowWindow(window,SW_HIDE);
#else
                if(show_window)eh_desktop_pause(1);
#endif
                eventf("{\"event\":\"menu\"}");
            }
        }
        if(paused||launch_wait){repeat_reset=1;eh_ra_idle();nap(10);continue;}
#ifdef _WIN32
        if(resync){paced=0;QueryPerformanceCounter(&start);}
        if(show_window&&fast_forward){
            static double deadline;double now=now_ms();
            if(resync||deadline<now-250)deadline=now;
            deadline+=100000.0/(av.timing.fps*fast_forward_speed);
            double ahead=deadline-now;if(ahead>=1)Sleep((DWORD)ahead);
        }
        if(show_window&&!fast_forward&&wave)pace_frame();
        if(resync||fast_forward)repeat_reset=1;
        if(!(show_window&&wave)&&show_window&&!fast_forward){LARGE_INTEGER now;QueryPerformanceCounter(&now);double ahead=paced/av.timing.fps-(double)(now.QuadPart-start.QuadPart)/frequency.QuadPart;
            if(ahead>0.001)Sleep((DWORD)(ahead*1000));else if(ahead<-0.25){paced=0;start=now;} /* Fell far behind: don't sprint to catch up. */}
#else
        (void)paced; /* the Windows timer pacing counts frames; this pacing keeps a deadline */
        if(show_window){double now=now_ms();if(resync||deadline<now-250){deadline=now;pace_resets++;}deadline+=1000.0/av.timing.fps/(fast_forward?fast_forward_speed/100.0:1.0);if(deadline<now)pace_late++;sleep_until_ms(deadline);pace_sleep_ms+=now_ms()-now;pace_frames++;
            if(pace_debug&&now-pace_since>=1000){fprintf(stderr,"pace: %u frames, present %.2f ms, sleep %.2f ms, late %u, resets %u\n",pace_frames,pace_present_ms/pace_frames,pace_sleep_ms/pace_frames,pace_late,pace_resets);pace_since=now;pace_frames=pace_late=pace_resets=0;pace_present_ms=pace_sleep_ms=0;}}
#endif
    }
#ifdef _WIN32
    if(presenter){presenter_stop=1;WaitForSingleObject(presenter,1000);CloseHandle(presenter);presenter=NULL;}
    for(unsigned i=0;i<hitches&&i<16;i++)fprintf(stderr,"%s\n",hitch_log[i]);
#endif
    if(fault){if(!eh_vk_lost())fprintf(stderr,"%s\n",callback_error?callback_error:"Engine returned invalid callback data");goto cleanup;}
    /* Some cores advertise provisional SRAM at load, then report no battery RAM
       after their first frame. Zero is a valid final capability, not a crash.
       Preserve any old save rather than replacing it with an empty file. */
    if(!p_retro_get_memory_size(RETRO_MEMORY_SAVE_RAM))save_size=0;
    if(save_size){
        if(p_retro_get_memory_size(RETRO_MEMORY_SAVE_RAM)!=save_size || !(save_memory=p_retro_get_memory_data(RETRO_MEMORY_SAVE_RAM))){fprintf(stderr,"Save memory changed unexpectedly; previous save preserved\n");goto cleanup;}
        /* Flush a new file first; preserve the previous save before replacing it. */
        if(!write_atomic(save,save_memory,save_size,1)){fprintf(stderr,"Save could not be committed\n");goto cleanup;}
    }
    bool diagnostic_marker=false;const uint8_t *ram=p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);size_t ram_size=p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if(ram&&ram_size>=4&&ram_size<=16*1024*1024)for(size_t i=0;i<=ram_size-4;i++)if(!memcmp(ram+i,"EHUM",4)){diagnostic_marker=true;break;}
    printf("{\"frames\":%u,\"video_frames\":%u,\"audio_frames\":%zu,\"input_polls\":%u,\"frame_hash\":%u,\"save_bytes\":%zu,\"diagnostic_marker\":%s,\"pacing\":\"%s\",\"hitches\":%u,\"worst_frame_ms\":%.1f,\"audio_underruns\":%u,\"audio_gaps\":%u,\"emulate_ms_avg\":%.2f,\"emulate_ms_worst\":%.1f,\"audio_api_ms_avg\":%.2f,\"cb_video_ms\":%.2f,\"cb_audio_ms\":%.2f,\"cb_input_ms\":%.2f}\n",
        frames,video_frames,audio_frames,polls,frame_hash,save_size,diagnostic_marker?"true":"false",
#ifdef _WIN32
        !show_window?"none":!wave?"timer":vsync_mode?"vsync":"audio",
#else
        "none",
#endif
        hitches,worst_frame_ms,underruns,audio_gaps,frames?emulate_total_ms/frames:0.0,emulate_worst_ms,frames?audio_api_total_ms/frames:0.0,frames?cb_video_ms/frames:0.0,frames?cb_audio_ms/frames:0.0,frames?cb_input_ms/frames:0.0);fflush(stdout);result=0;
cleanup:
    quit_asked=1; /* the watchdog bounds the teardown too */
    free(resume_state);resume_state=NULL;
    eh_pad_stop_rumble();
    eh_shader_close();
    eh_netplay_finish();
    if(file)fclose(file);
#ifdef _WIN32
    if(audio_thread){audio_stop=1;SetEvent(audio_wake);WaitForSingleObject(audio_thread,1000);CloseHandle(audio_thread);audio_thread=NULL;}
    if(audio_wake){CloseHandle(audio_wake);audio_wake=NULL;}
    if(wave){waveOutReset(wave);for(int i=0;i<BLOCKS;i++)if(audio_headers[i].dwFlags&WHDR_PREPARED)waveOutUnprepareHeader(wave,&audio_headers[i],sizeof(WAVEHDR));waveOutClose(wave);}
    if(presenter){presenter_stop=1;WaitForSingleObject(presenter,1000);CloseHandle(presenter);}
    if(frame_taken)CloseHandle(frame_taken);
    for(int i=0;i<QUEUE_SLOTS;i++)free(slots[i]);
    if(show_window)timeEndPeriod(1);
    if(window&&IsWindow(window))DestroyWindow(window);
    eh_pad_shutdown();
#else
    eh_pad_shutdown();
    free(display_frame);
#endif
    eh_ra_shutdown();
    /* The engine's teardown waits on its graphics device; a lost one would hold the process forever. */
    if(eh_vk_lost())watchdog_end(1,"Vulkan: the engine was not shut down because the graphics device was lost");
    if(hw_active&&hw.context_destroy&&loaded)hw.context_destroy();
    if(loaded)p_retro_unload_game();
    if(initialized)p_retro_deinit();
    eh_gl_destroy();
    eh_d3d_destroy();
    eh_vk_destroy();
#ifndef _WIN32
    eh_desktop_close();
#endif
    free(rom);free(framebuffer);
#ifdef _WIN32
    if(library)FreeLibrary(library);
#else
    if(library)dlclose(library);
#endif
    return result;
}
