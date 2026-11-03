/* Original EmuHall synthetic ABI fixture. Contains no game or firmware content. */
#include "libretro.h"
#ifdef EH_TEMPORAL_FAIL
#define EH_TEMPORAL
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
static retro_environment_t env;
static retro_video_refresh_t video;
static retro_audio_sample_batch_t audio;
static retro_input_poll_t poll;
static retro_input_state_t input;
#ifdef EH_NDS
static uint32_t pixels[512*510];
static unsigned picture_width=256,picture_height=384;
#else
static uint32_t pixels[160*144];
#endif
static uint8_t sram[32];
static uint32_t frame;
#ifdef EH_GL
/* Draws with the graphics card: clears the frontend's frame buffer to a colour that changes every frame. */
typedef void (*gl_bind_fb_t)(unsigned,unsigned);typedef void (*gl_clear_color_t)(float,float,float,float);
typedef void (*gl_clear_t)(unsigned);typedef void (*gl_viewport_t)(int,int,int,int);
static struct retro_hw_render_callback hw;static gl_bind_fb_t gl_bind_fb;static gl_clear_color_t gl_clear_color;
static gl_clear_t gl_clear;static gl_viewport_t gl_viewport;static int gl_ready;static uintptr_t cached_fbo;
typedef void(*gl_pixel_store_t)(unsigned,int);typedef void(*gl_read_buffer_t)(unsigned);
static gl_pixel_store_t gl_pixel_store;static gl_read_buffer_t gl_read_buffer;
static void gl_reset(void){
    retro_proc_address_t f;
    f=hw.get_proc_address("glBindFramebuffer");memcpy(&gl_bind_fb,&f,sizeof(f));
    f=hw.get_proc_address("glClearColor");memcpy(&gl_clear_color,&f,sizeof(f));
    f=hw.get_proc_address("glClear");memcpy(&gl_clear,&f,sizeof(f));
    f=hw.get_proc_address("glViewport");memcpy(&gl_viewport,&f,sizeof(f));
    f=hw.get_proc_address("glPixelStorei");memcpy(&gl_pixel_store,&f,sizeof(f));
    f=hw.get_proc_address("glReadBuffer");memcpy(&gl_read_buffer,&f,sizeof(f));
    cached_fbo=hw.get_current_framebuffer();
    /* A core can copy depth images through the attachment name (GLSM does).
       Exercise that real GL operation rather than assuming a renderbuffer works. */
    typedef void(*copy_image_t)(unsigned,unsigned,int,int,int,int,unsigned,unsigned,int,int,int,int,int,int,int);
    typedef void(*get_attachment_t)(unsigned,unsigned,unsigned,int*);
    typedef void(*gen_tex_t)(int,unsigned*);typedef void(*bind_tex_t)(unsigned,unsigned);
    typedef void(*tex_image_t)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
    typedef unsigned(*get_error_t)(void);typedef void(*tex_parameter_t)(unsigned,unsigned,int);
    tex_parameter_t parameter=NULL;copy_image_t copy_image=NULL;get_attachment_t attachment=NULL;gen_tex_t gen=NULL,del=NULL;bind_tex_t bind=NULL;tex_image_t image=NULL;get_error_t error=NULL;
    f=hw.get_proc_address("glCopyImageSubData");memcpy(&copy_image,&f,sizeof(f));
    if(copy_image){
        f=hw.get_proc_address("glGetFramebufferAttachmentParameteriv");memcpy(&attachment,&f,sizeof(f));
        f=hw.get_proc_address("glGenTextures");memcpy(&gen,&f,sizeof(f));f=hw.get_proc_address("glDeleteTextures");memcpy(&del,&f,sizeof(f));
        f=hw.get_proc_address("glBindTexture");memcpy(&bind,&f,sizeof(f));f=hw.get_proc_address("glTexImage2D");memcpy(&image,&f,sizeof(f));
        f=hw.get_proc_address("glGetError");memcpy(&error,&f,sizeof(f));f=hw.get_proc_address("glTexParameteri");memcpy(&parameter,&f,sizeof(f));
        gl_bind_fb(0x8D40,(unsigned)cached_fbo);int target=0;attachment(0x8D40,0x8D00,0x8CD1,&target);
        unsigned texture=0;gen(1,&texture);bind(0x0DE1,texture);image(0x0DE1,0,0x81A6,160,144,0,0x1902,0x1405,NULL);parameter(0x0DE1,0x2801,0x2600);parameter(0x0DE1,0x2800,0x2600);
        while(error()){};
        copy_image(texture,0x0DE1,0,0,0,0,(unsigned)target,0x0DE1,0,0,0,0,160,144,1);
        if(error())abort();del(1,&texture);bind(0x0DE1,0);
    }
    gl_ready=gl_bind_fb&&gl_clear_color&&gl_clear&&gl_viewport&&gl_pixel_store&&gl_read_buffer;
}
static void gl_destroy(void){gl_ready=0;}
#endif
RETRO_API unsigned retro_api_version(void){
#ifdef EH_BAD_API
return 99;
#else
return RETRO_API_VERSION;
#endif
}
#ifdef EH_PROPERTIES
static unsigned disc_index;static bool disc_ejected;
static bool disc_eject(bool open){disc_ejected=open;return true;}
static bool disc_is_ejected(void){return disc_ejected;}
static unsigned disc_current(void){return disc_index;}
static unsigned disc_count(void){return 2;}
static bool disc_select(unsigned index){if(!disc_ejected||index>=2)return false;disc_index=index;return true;}
#endif
static void register_options(void){
    static struct retro_core_option_v2_definition definitions[]={
        {.key="fixture_mode",.desc="Fixture mode",.values={{"alternate",NULL},{"standard",NULL},{NULL,NULL}},.default_value="standard"},
#ifdef EH_NDS
        {.key="melonds_screen_layout1",.desc="Synthetic screen layout",.values={{"top-bottom",NULL},{"left-right",NULL},{NULL,NULL}},.default_value="top-bottom"},
        {.key="melonds_number_of_screen_layouts",.desc="Synthetic layout count",.values={{"1",NULL},{NULL,NULL}},.default_value="1"},
        {.key="melonds_screen_gap",.desc="Synthetic screen gap",.values={{"0",NULL},{"32",NULL},{NULL,NULL}},.default_value="0"},
        {.key="melonds_secondary_screen_scale",.desc="Synthetic secondary size",.values={{"100",NULL},{"50",NULL},{NULL,NULL}},.default_value="100"},
#endif
        {0}
    };
    static struct retro_core_options_v2 options={NULL,definitions};
    env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2,&options);
}
RETRO_API void retro_set_environment(retro_environment_t cb){
    env=cb;
#ifdef EH_VARIABLE_STATES
    uint64_t quirks=RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE;
    if(!env(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS,&quirks)||!(quirks&RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE))abort();
#endif
#ifdef EH_PROPERTIES
    static struct retro_disk_control_callback disk={disc_eject,disc_is_ejected,disc_current,disc_select,disc_count,NULL,NULL};
    env(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE,&disk);
#endif
#ifndef EH_NDS
    register_options();
#endif
#ifdef EH_DUALSHOCK
    static const struct retro_controller_description pads[]={{"standard",RETRO_DEVICE_JOYPAD},{"analog",RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG,0)},{"DualShock",RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG,1)}};
    static const struct retro_controller_info ports[]={{pads,3},{NULL,0}};
    env(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO,(void*)ports);
#endif
}
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb){video=cb;}
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb){(void)cb;}
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb){audio=cb;}
RETRO_API void retro_set_input_poll(retro_input_poll_t cb){poll=cb;}
RETRO_API void retro_set_input_state(retro_input_state_t cb){input=cb;}
RETRO_API void retro_init(void){enum retro_pixel_format fmt=RETRO_PIXEL_FORMAT_XRGB8888;env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT,&fmt);frame=0;}
RETRO_API void retro_deinit(void){}
RETRO_API void retro_get_system_info(struct retro_system_info *info){memset(info,0,sizeof(*info));info->library_name="EmuHall Signal Garden";info->library_version="1";info->valid_extensions="ehdemo";}
RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info){memset(info,0,sizeof(*info));
#ifdef EH_NDS
info->geometry.base_width=info->geometry.max_width=picture_width;info->geometry.base_height=info->geometry.max_height=picture_height;
#else
info->geometry.base_width=info->geometry.max_width=160;info->geometry.base_height=info->geometry.max_height=144;
#endif
info->timing.fps=60;info->timing.sample_rate=48000;
#ifdef EH_LAYOUT_GEOMETRY
info->geometry.max_width=4000;info->geometry.max_height=4800;
#endif
}
RETRO_API bool retro_load_game(const struct retro_game_info *game){
const char *expected_system=getenv("EMUHALL_FIXTURE_SYSTEM_DIRECTORY");
if(expected_system){const char *system=NULL,*save=NULL;
if(!env(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY,&system)||!env(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY,&save)||!system||!save||strcmp(system,expected_system)||!strcmp(system,save))return false;
fprintf(stderr,"Synthetic core: shared system directory is separate from game saves\n");}
#ifdef EH_LAYOUT_GEOMETRY
    struct retro_game_geometry layout={160,144,160,144,0};
    if(!env(RETRO_ENVIRONMENT_SET_GEOMETRY,&layout))return false;
#endif
#ifdef EH_LOAD_FAIL
return false;
#else
#ifdef EH_DIR_PROBE
/* Writes a marker wherever the host says saves belong, so tests can check that location. */
const char *dir=NULL;char path[4096];FILE *probe;
if(!env(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY,&dir)||!dir)return false;
snprintf(path,sizeof(path),"%s/probe.txt",dir);probe=fopen(path,"wb");if(!probe)return false;fputs("probe",probe);fclose(probe);
#endif
#ifdef EH_GL
memset(&hw,0,sizeof(hw));hw.context_type=RETRO_HW_CONTEXT_OPENGL_CORE;hw.version_major=3;hw.version_minor=3;
hw.context_reset=gl_reset;hw.context_destroy=gl_destroy;hw.bottom_left_origin=true;hw.depth=true;
if(!env(RETRO_ENVIRONMENT_SET_HW_RENDER,&hw))return false;
#endif
struct retro_variable option={"fixture_mode",NULL};
#ifdef EH_NDS
/* melonDS DS registers options during game loading, rather than set_environment.
   Keeping that lifecycle matters: early screen detection missed the real core. */
register_options();
struct retro_variable layout={"melonds_screen_layout1",NULL},gap={"melonds_screen_gap",NULL},scale={"melonds_secondary_screen_scale",NULL};
env(RETRO_ENVIRONMENT_GET_VARIABLE,&layout);env(RETRO_ENVIRONMENT_GET_VARIABLE,&gap);env(RETRO_ENVIRONMENT_GET_VARIABLE,&scale);
if(layout.value&&!strcmp(layout.value,"left-right")){picture_width=512;picture_height=192;}
else {picture_width=256;picture_height=192+(gap.value?atoi(gap.value):0)+192*(scale.value?atoi(scale.value):100)/100;}
#endif
return env(RETRO_ENVIRONMENT_GET_VARIABLE,&option)&&option.value&&!strcmp(option.value,"standard")&&game && game->size>=13 && !memcmp(game->data,"EMUHALL-DEMO\0",13);
#endif
}
RETRO_API void retro_unload_game(void){}
static int ran_frame; /* EH_LATE_CHEATS: like melonDS DS with a GPU renderer, cheats only stick after the first frame. */
RETRO_API void retro_run(void){
ran_frame=1;
#ifdef EH_CRASH
abort();
#endif
#ifdef EH_HANG
for(;;){}
#endif
poll();int held=input(0,RETRO_DEVICE_JOYPAD,0,RETRO_DEVICE_ID_JOYPAD_A);if(held)sram[0]++;
for(unsigned y=0;y<144;y++)for(unsigned x=0;x<160;x++){unsigned ring=((x-80)*(x-80)+(y-72)*(y-72)+frame*100)/200;pixels[y*160+x]=((ring%2)?0x103a35:0x75edbb)^(held?0x303060:0);}
int16_t sound[1600];for(int i=0;i<800;i++)sound[i*2]=sound[i*2+1]=held?((i%100<50)?1200:-1200):0;
/* Like real cores (SkyEmu), only present what the frontend reports as enabled. */
int av=0;env(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE,&av);
if(av&2)audio(sound,800);
#ifdef EH_GL
static unsigned gl_width=160,gl_height=144;
if(frame==8||frame==16){struct retro_game_geometry layout={frame==8?320:160,frame==8?288:144,320,288,0};if(env(RETRO_ENVIRONMENT_SET_GEOMETRY,&layout)){gl_width=layout.base_width;gl_height=layout.base_height;}}
if(gl_ready&&(av&1)){if(hw.get_current_framebuffer()!=cached_fbo)abort();gl_bind_fb(0x8D40 /* GL_FRAMEBUFFER */,(unsigned)cached_fbo);gl_viewport(0,0,gl_width,gl_height);
    gl_clear_color((frame%3)/2.0f,0.5f,1.0f,1.0f);gl_clear(0x4000 /* GL_COLOR_BUFFER_BIT */);
    /* Pause-screen buffer copies can leave these states behind. The host must isolate readback. */
    gl_pixel_store(0x0D03 /* PACK_SKIP_ROWS */,2);gl_pixel_store(0x0D04 /* PACK_SKIP_PIXELS */,3);
    gl_read_buffer(0 /* NONE */);video(RETRO_HW_FRAME_BUFFER_VALID,gl_width,gl_height,0);
    gl_pixel_store(0x0D03,0);gl_pixel_store(0x0D04,0);gl_read_buffer(0x8CE0 /* COLOR_ATTACHMENT0 */);}
#elif defined(EH_BAD_GEOMETRY)
if(av&1)video(pixels,65535,65535,160*4);
#elif defined(EH_NDS)
if(av&1)video(pixels,picture_width,picture_height,picture_width*4);
#else
if(av&1)video(pixels,160,144,160*4);
#endif
frame++;
#ifdef EH_TEMPORAL
memcpy(sram+4,&frame,sizeof(frame));
#endif
}
RETRO_API void retro_reset(void){frame=0;}
/* EH_DUALSHOCK: like PCSX ReARMed, a plain pad is listed first and the analog DualShock after it. */
RETRO_API void retro_set_controller_port_device(unsigned port,unsigned device){if(!port){sram[3]=(uint8_t)(device>>8);sram[5]=(uint8_t)device;}}
RETRO_API size_t retro_serialize_size(void){
#ifdef EH_TEMPORAL
return sizeof(frame)+sizeof(sram);
#elif defined(EH_VARIABLE_STATES)
static unsigned probe;return sizeof(frame)+(probe++%2)*4;
#else
return sizeof(frame);
#endif
}
RETRO_API bool retro_serialize(void *data,size_t size){
#ifdef EH_TEMPORAL
if(size!=sizeof(frame)+sizeof(sram))return false;memcpy(data,&frame,sizeof(frame));memcpy((char*)data+sizeof(frame),sram,sizeof(sram));return true;
#elif defined(EH_VARIABLE_STATES)
if(size!=4&&size!=8)return false;memset(data,0,size);memcpy(data,&frame,sizeof(frame));return true;
#else
if(size!=sizeof(frame))return false;memcpy(data,&frame,size);return true;
#endif
}
RETRO_API bool retro_unserialize(const void *data,size_t size){
#ifdef EH_TEMPORAL_FAIL
(void)data;(void)size;return false;
#elif defined(EH_TEMPORAL)
if(size!=sizeof(frame)+sizeof(sram))return false;memcpy(&frame,data,sizeof(frame));memcpy(sram,(const char*)data+sizeof(frame),sizeof(sram));return true;
#elif defined(EH_VARIABLE_STATES)
if(size!=4&&size!=8)return false;memcpy(&frame,data,sizeof(frame));return true;
#else
if(size!=sizeof(frame))return false;memcpy(&frame,data,size);return true;
#endif
}
RETRO_API void *retro_get_memory_data(unsigned id){
#ifdef EH_PROPERTIES
if(id==RETRO_MEMORY_SYSTEM_RAM)return sram;
#endif
return id==RETRO_MEMORY_SAVE_RAM?sram:NULL;}
RETRO_API size_t retro_get_memory_size(unsigned id){
#ifdef EH_PROPERTIES
if(id==RETRO_MEMORY_SYSTEM_RAM)return sizeof(sram);
#endif
#ifdef EH_DYNAMIC_SAVE
if(frame)return 0;
#endif
return id==RETRO_MEMORY_SAVE_RAM?sizeof(sram):0;}
RETRO_API unsigned retro_get_region(void){return RETRO_REGION_NTSC;}
RETRO_API void retro_cheat_reset(void){sram[2]=0;}
RETRO_API void retro_cheat_set(unsigned i,bool enabled,const char *code){(void)i;(void)code;
#ifdef EH_LATE_CHEATS
if(!ran_frame)return;
#endif
sram[2]=enabled?1:0;}
RETRO_API bool retro_load_game_special(unsigned type,const struct retro_game_info *g,size_t n){(void)type;(void)g;(void)n;return false;}
