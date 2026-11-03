/* SDL-normalized controllers and isolated local player ports. SDL is loaded at runtime on every desktop OS. */
#include "gamepad.h"
#include <SDL3/SDL.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
typedef DWORD (WINAPI *XInputGetStateFn)(DWORD,XINPUT_STATE*);
static XInputGetStateFn xinput;
typedef DWORD (WINAPI *XInputSetStateFn)(DWORD,XINPUT_VIBRATION*);
static XInputSetStateFn xinput_set;static int xinput_ports[4]={-1,-1,-1,-1};
static void *library_open(void){return (void*)LoadLibraryExW(L"SDL3.dll",NULL,LOAD_LIBRARY_SEARCH_APPLICATION_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);}
static void *library_symbol(void *library,const char *name){FARPROC symbol=GetProcAddress((HMODULE)library,name);void *out=NULL;memcpy(&out,&symbol,sizeof(out));return out;}
static void library_close(void *library){FreeLibrary((HMODULE)library);}
#else
#include <dlfcn.h>
static void *library_open(void){
#ifdef __APPLE__
    void *library=dlopen("@executable_path/libSDL3.dylib",RTLD_NOW|RTLD_LOCAL);if(!library)library=dlopen("libSDL3.dylib",RTLD_NOW|RTLD_LOCAL);
#else
    void *library=dlopen("libSDL3.so.0",RTLD_NOW|RTLD_LOCAL);if(!library)library=dlopen("libSDL3.so",RTLD_NOW|RTLD_LOCAL);
#endif
    return library;
}
static void *library_symbol(void *library,const char *name){return dlsym(library,name);}
static void library_close(void *library){dlclose(library);}
#endif
#define SDL_FUNCTIONS(X) \
    X(bool,SDL_SetHint,(const char*,const char*)) X(void,SDL_Delay,(Uint32)) X(void,SDL_SetMainReady,(void)) X(bool,SDL_SetAppMetadata,(const char*,const char*,const char*)) X(bool,SDL_InitSubSystem,(SDL_InitFlags)) X(void,SDL_QuitSubSystem,(SDL_InitFlags)) \
    X(void,SDL_PumpEvents,(void)) X(void,SDL_free,(void*)) X(SDL_JoystickID*,SDL_GetJoysticks,(int*)) \
    X(bool,SDL_IsGamepad,(SDL_JoystickID)) X(SDL_Gamepad*,SDL_OpenGamepad,(SDL_JoystickID)) \
    X(void,SDL_CloseGamepad,(SDL_Gamepad*)) X(bool,SDL_GamepadConnected,(SDL_Gamepad*)) \
    X(SDL_GamepadType,SDL_GetGamepadType,(SDL_Gamepad*)) X(bool,SDL_GetGamepadButton,(SDL_Gamepad*,SDL_GamepadButton)) X(Sint16,SDL_GetGamepadAxis,(SDL_Gamepad*,SDL_GamepadAxis)) \
    X(SDL_Joystick*,SDL_OpenJoystick,(SDL_JoystickID)) X(void,SDL_CloseJoystick,(SDL_Joystick*)) \
    X(bool,SDL_JoystickConnected,(SDL_Joystick*)) X(int,SDL_GetNumJoystickButtons,(SDL_Joystick*)) \
    X(int,SDL_GetNumJoystickAxes,(SDL_Joystick*)) X(int,SDL_GetNumJoystickHats,(SDL_Joystick*)) \
    X(bool,SDL_GetJoystickButton,(SDL_Joystick*,int)) X(Sint16,SDL_GetJoystickAxis,(SDL_Joystick*,int)) \
    X(Uint8,SDL_GetJoystickHat,(SDL_Joystick*,int)) X(SDL_JoystickID,SDL_GetGamepadID,(SDL_Gamepad*)) \
    X(SDL_JoystickID,SDL_GetJoystickID,(SDL_Joystick*))     X(const char*,SDL_GetGamepadName,(SDL_Gamepad*)) X(const char*,SDL_GetJoystickName,(SDL_Joystick*)) \
    X(SDL_GUID,SDL_GetJoystickGUIDForID,(SDL_JoystickID)) X(void,SDL_GUIDToString,(SDL_GUID,char*,int)) \
    X(const char*,SDL_GetGamepadSerial,(SDL_Gamepad*)) X(const char*,SDL_GetJoystickSerial,(SDL_Joystick*))
#define DECLARE(ret,name,args) static ret (SDLCALL *p_##name)args;
SDL_FUNCTIONS(DECLARE)
enum{MAX_PADS=8};
static struct{SDL_JoystickID id;SDL_Gamepad *gamepad;SDL_Joystick *joystick;char key[81],name[97];} pads[MAX_PADS];
static eh_pad ports[4];
static int rumble_enabled=1;static uint16_t motors[4][2];
static bool (SDLCALL *rumble_gamepad)(SDL_Gamepad*,Uint16,Uint16,Uint32);
static bool (SDLCALL *rumble_joystick)(SDL_Joystick*,Uint16,Uint16,Uint32);
static unsigned gestures;
unsigned eh_pad_gestures(void){return gestures;}
static void *sdl_library;static int sdl_ready;static unsigned polls;
extern int eh_controller_route(const char*,unsigned,eh_pad*);
extern int eh_controller_assignment(const char*);
extern void eh_controller_key(const char*,const char*,unsigned,char*,size_t);
static void refresh_pads(void);
static void list_devices(void);
int eh_pad_init(void){
    if(sdl_ready)return 1;
    sdl_library=library_open();
    if(sdl_library){int ok=1;
#define LOAD(ret,name,args) {void *symbol=library_symbol(sdl_library,#name);if(!symbol)ok=0;else memcpy(&p_##name,&symbol,sizeof(p_##name));}
        SDL_FUNCTIONS(LOAD)
        if(ok){p_SDL_SetMainReady();p_SDL_SetAppMetadata("EmuHall","0.1.0-dev","dev.emuhall.player");p_SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");ok=p_SDL_InitSubSystem(SDL_INIT_GAMEPAD);}
        if(ok){void *rg=library_symbol(sdl_library,"SDL_RumbleGamepad"),*rj=library_symbol(sdl_library,"SDL_RumbleJoystick");memcpy(&rumble_gamepad,&rg,sizeof(rg));memcpy(&rumble_joystick,&rj,sizeof(rj));sdl_ready=1;refresh_pads();return 1;}
        library_close(sdl_library);sdl_library=NULL;
    }
#ifdef _WIN32
    HMODULE x=LoadLibraryExW(L"xinput1_4.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!x)x=LoadLibraryExW(L"xinput9_1_0.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(x){FARPROC symbol=GetProcAddress(x,"XInputGetState");memcpy(&xinput,&symbol,sizeof(xinput));symbol=GetProcAddress(x,"XInputSetState");memcpy(&xinput_set,&symbol,sizeof(xinput_set));}
#endif
    return 0;
}
static void keep_largest(int16_t *axis,int16_t value){if((value<0?-value:value)>(*axis<0?-*axis:*axis))*axis=value;}
static void press(eh_pad *out,int button,int down){if(down)out->buttons|=1u<<button;}

/* Pads SDL has no layout for (unknown generic USB pads): the usual DirectInput order. They can be remapped later. */
static void read_joystick(SDL_Joystick *j,eh_pad *out){
    static const int order[12]={EH_B,EH_A,EH_Y,EH_X,EH_L,EH_R,EH_L2,EH_R2,EH_SELECT,EH_START,EH_L3,EH_R3};
    int n=p_SDL_GetNumJoystickButtons(j);for(int i=0;i<n&&i<12;i++)press(out,order[i],p_SDL_GetJoystickButton(j,i));
    if(p_SDL_GetNumJoystickHats(j)>0){Uint8 h=p_SDL_GetJoystickHat(j,0);
        press(out,EH_UP,h&SDL_HAT_UP);press(out,EH_DOWN,h&SDL_HAT_DOWN);press(out,EH_LEFT,h&SDL_HAT_LEFT);press(out,EH_RIGHT,h&SDL_HAT_RIGHT);}
    int axes=p_SDL_GetNumJoystickAxes(j);
    if(axes>=2){keep_largest(&out->lx,p_SDL_GetJoystickAxis(j,0));keep_largest(&out->ly,p_SDL_GetJoystickAxis(j,1));}
    if(axes>=4){keep_largest(&out->rx,p_SDL_GetJoystickAxis(j,2));keep_largest(&out->ry,p_SDL_GetJoystickAxis(j,3));}
}

static void read_gamepad(SDL_Gamepad *g,eh_pad *out){
    /* SDL names buttons by position, so a PlayStation Cross, Xbox A and Switch B all land on the RetroPad's B. */
    static const struct{SDL_GamepadButton from;int to;} map[]={
        {SDL_GAMEPAD_BUTTON_SOUTH,EH_B},{SDL_GAMEPAD_BUTTON_EAST,EH_A},{SDL_GAMEPAD_BUTTON_WEST,EH_Y},{SDL_GAMEPAD_BUTTON_NORTH,EH_X},
        {SDL_GAMEPAD_BUTTON_BACK,EH_SELECT},{SDL_GAMEPAD_BUTTON_START,EH_START},{SDL_GAMEPAD_BUTTON_LEFT_STICK,EH_L3},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK,EH_R3},{SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,EH_L},{SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,EH_R},
        {SDL_GAMEPAD_BUTTON_DPAD_UP,EH_UP},{SDL_GAMEPAD_BUTTON_DPAD_DOWN,EH_DOWN},{SDL_GAMEPAD_BUTTON_DPAD_LEFT,EH_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,EH_RIGHT}};
    for(size_t i=0;i<sizeof(map)/sizeof(map[0]);i++)press(out,map[i].to,p_SDL_GetGamepadButton(g,map[i].from));
    if(p_SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_GUIDE))out->guide=1;
    keep_largest(&out->lx,p_SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX));keep_largest(&out->ly,p_SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
    keep_largest(&out->rx,p_SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX));keep_largest(&out->ry,p_SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
    Sint16 lt=p_SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER),rt=p_SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    if(lt>out->lt)out->lt=lt;
    if(rt>out->rt)out->rt=rt;
}

static void copy_name(char *out,const char *name){if(!name)name="Controller";size_t n=strlen(name);if(n>96){n=96;while(n&&((unsigned char)name[n]&0xc0)==0x80)n--;}memcpy(out,name,n);out[n]=0;for(size_t i=0;i<n;i++)if((unsigned char)out[i]<32||out[i]==127)out[i]=' ';}
static void refresh_pads(void){
    int changed=0;
    for(int i=0;i<MAX_PADS;i++){
        if((pads[i].gamepad&&!p_SDL_GamepadConnected(pads[i].gamepad))||(pads[i].joystick&&!p_SDL_JoystickConnected(pads[i].joystick))){
            if(pads[i].gamepad)p_SDL_CloseGamepad(pads[i].gamepad);else p_SDL_CloseJoystick(pads[i].joystick);
            memset(&pads[i],0,sizeof(pads[i]));changed=1;
        }
    }
    int count=0;SDL_JoystickID *ids=p_SDL_GetJoysticks(&count);if(!ids)return;
    for(int n=0;n<count;n++){
        int known=0,slot=-1;for(int i=0;i<MAX_PADS;i++){if(pads[i].id==ids[n]&&(pads[i].gamepad||pads[i].joystick))known=1;if(slot<0&&!pads[i].gamepad&&!pads[i].joystick)slot=i;}
        if(known||slot<0)continue;
        pads[slot].id=ids[n];
        if(p_SDL_IsGamepad(ids[n]))pads[slot].gamepad=p_SDL_OpenGamepad(ids[n]);else pads[slot].joystick=p_SDL_OpenJoystick(ids[n]);
        if(!pads[slot].gamepad&&!pads[slot].joystick){memset(&pads[slot],0,sizeof(pads[slot]));continue;}
        char guid[33];p_SDL_GUIDToString(p_SDL_GetJoystickGUIDForID(ids[n]),guid,sizeof(guid));
        const char *serial=pads[slot].gamepad?p_SDL_GetGamepadSerial(pads[slot].gamepad):p_SDL_GetJoystickSerial(pads[slot].joystick);
        for(unsigned ordinal=0;ordinal<MAX_PADS;ordinal++){
            eh_controller_key(guid,serial,ordinal,pads[slot].key,sizeof(pads[slot].key));int used=0;
            for(int i=0;i<MAX_PADS;i++)if(i!=slot&&!strcmp(pads[i].key,pads[slot].key))used=1;
            if(!used)break;
        }
        copy_name(pads[slot].name,pads[slot].gamepad?p_SDL_GetGamepadName(pads[slot].gamepad):p_SDL_GetJoystickName(pads[slot].joystick));changed=1;
        fprintf(stderr,"Controller connected: %s (%s)\n",pads[slot].name,pads[slot].gamepad?"known layout":"generic layout");
    }
    p_SDL_free(ids);if(changed)list_devices();
}
static void player_ports(int *plan){
    int used[4]={0};for(int i=0;i<MAX_PADS;i++)plan[i]=-1;
    for(int pass=0;pass<2;pass++)for(int i=0;i<MAX_PADS;i++)if(pads[i].gamepad||pads[i].joystick){
        int assignment=eh_controller_assignment(pads[i].key);if(assignment==-1||(pass==0)!=(assignment>=0))continue;
        int port=assignment>=0?assignment:i;
        if(assignment<0&&(port>=4||used[port])){for(port=0;port<4&&used[port];port++);}
        if(port>=0&&port<4&&!used[port]){plan[i]=port;used[port]=1;}
    }
}
static void json_text(const char *text){putchar('"');for(const unsigned char *p=(const unsigned char*)text;*p;p++){if(*p=='"'||*p=='\\'){putchar('\\');putchar(*p);}else if(*p<32)printf("\\u%04x",*p);else putchar(*p);}putchar('"');}
extern void eh_events_begin(void),eh_events_end(void);
static void list_devices(void){
    int plan[MAX_PADS];player_ports(plan);
    eh_events_begin();
    printf("{\"event\":\"controllers\",\"devices\":[");int comma=0;
    for(int i=0;i<MAX_PADS;i++)if(pads[i].gamepad||pads[i].joystick){
        if(comma++){putchar(',');}
        printf("{\"id\":");json_text(pads[i].key);printf(",\"name\":");json_text(pads[i].name);
        int assigned=plan[i];
        printf(",\"player\":");if(assigned<0)printf("null");else printf("%d",assigned+1);
        SDL_GamepadType type=pads[i].gamepad?p_SDL_GetGamepadType(pads[i].gamepad):SDL_GAMEPAD_TYPE_UNKNOWN;
        const char *style=(type==SDL_GAMEPAD_TYPE_PS3||type==SDL_GAMEPAD_TYPE_PS4||type==SDL_GAMEPAD_TYPE_PS5)?"playstation":(type>=SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO&&type<=SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR)?"switch":"xbox";
        printf(",\"known\":%s,\"style\":\"%s\"}",pads[i].gamepad?"true":"false",style);
    }
#ifdef _WIN32
    if(!sdl_ready&&xinput)for(DWORD n=0;n<4;n++){XINPUT_STATE state;memset(&state,0,sizeof(state));if(xinput(n,&state)!=ERROR_SUCCESS)continue;
        char key[32],name[48];snprintf(key,sizeof(key),"xinput-%u",(unsigned)n);snprintf(name,sizeof(name),"Xbox controller %u",(unsigned)n+1);
        if(comma++){putchar(',');}printf("{\"id\":");json_text(key);printf(",\"name\":");json_text(name);int port=eh_controller_assignment(key);if(port==-2)port=(int)n;
        printf(",\"player\":");if(port<0)printf("null");else printf("%d",port+1);printf(",\"known\":true}");
    }
#endif
    printf("]}\n");eh_events_end();
}
void eh_pad_probe(void){if(sdl_ready)for(int i=0;i<8;i++){p_SDL_PumpEvents();refresh_pads();p_SDL_Delay(25);}list_devices();}
void eh_pad_list(void){if(sdl_ready){p_SDL_PumpEvents();refresh_pads();}list_devices();}
static void route(const char *key,unsigned automatic,eh_pad value,int *used){gestures|=value.guide?1u:0u;if((value.buttons&((1u<<EH_SELECT)|(1u<<EH_START)))==((1u<<EH_SELECT)|(1u<<EH_START)))gestures|=2u;int port=eh_controller_route(key,automatic,&value);if(port>=0&&port<4&&!used[port]){ports[port]=value;used[port]=1;}}
void eh_pad_read_port(unsigned port,eh_pad *out){if(port<4)*out=ports[port];else memset(out,0,sizeof(*out));}
void eh_pad_read(eh_pad *out){
    memset(ports,0,sizeof(ports));gestures=0;int used[4]={0};
    if(sdl_ready){
        p_SDL_PumpEvents();if(polls++%30==0)refresh_pads();
        int plan[MAX_PADS];player_ports(plan);
        for(int i=0;i<MAX_PADS;i++)if(plan[i]>=0){
            unsigned automatic=(unsigned)plan[i];
            eh_pad value;memset(&value,0,sizeof(value));if(pads[i].gamepad)read_gamepad(pads[i].gamepad,&value);else read_joystick(pads[i].joystick,&value);
            press(&value,EH_L2,value.lt>10000);press(&value,EH_R2,value.rt>10000);route(pads[i].key,automatic,value,used);
        }
    }
#ifdef _WIN32
    else if(xinput){
        for(int i=0;i<4;i++)xinput_ports[i]=-1;
        for(int pass=0;pass<2;pass++)for(DWORD n=0;n<4;n++){
            char key[32];snprintf(key,sizeof(key),"xinput-%u",(unsigned)n);int assignment=eh_controller_assignment(key);
            if(assignment==-1||(pass==0)!=(assignment>=0))continue;
            unsigned automatic=n;if(assignment<0&&used[automatic]){for(automatic=0;automatic<4&&used[automatic];automatic++);}
            XINPUT_STATE s;memset(&s,0,sizeof(s));if(xinput(n,&s)!=ERROR_SUCCESS)continue;
            eh_pad value;memset(&value,0,sizeof(value));WORD b=s.Gamepad.wButtons;
            static const struct{WORD from;int to;} map[]={{XINPUT_GAMEPAD_A,EH_B},{XINPUT_GAMEPAD_B,EH_A},{XINPUT_GAMEPAD_X,EH_Y},{XINPUT_GAMEPAD_Y,EH_X},{XINPUT_GAMEPAD_BACK,EH_SELECT},{XINPUT_GAMEPAD_START,EH_START},{XINPUT_GAMEPAD_LEFT_THUMB,EH_L3},{XINPUT_GAMEPAD_RIGHT_THUMB,EH_R3},{XINPUT_GAMEPAD_LEFT_SHOULDER,EH_L},{XINPUT_GAMEPAD_RIGHT_SHOULDER,EH_R},{XINPUT_GAMEPAD_DPAD_UP,EH_UP},{XINPUT_GAMEPAD_DPAD_DOWN,EH_DOWN},{XINPUT_GAMEPAD_DPAD_LEFT,EH_LEFT},{XINPUT_GAMEPAD_DPAD_RIGHT,EH_RIGHT}};
            for(size_t i=0;i<sizeof(map)/sizeof(map[0]);i++)press(&value,map[i].to,b&map[i].from);
            value.lx=s.Gamepad.sThumbLX;value.ly=(int16_t)(s.Gamepad.sThumbLY==-32768?32767:-s.Gamepad.sThumbLY);value.rx=s.Gamepad.sThumbRX;value.ry=(int16_t)(s.Gamepad.sThumbRY==-32768?32767:-s.Gamepad.sThumbRY);
            value.lt=(int16_t)(s.Gamepad.bLeftTrigger*128);value.rt=(int16_t)(s.Gamepad.bRightTrigger*128);press(&value,EH_L2,value.lt>10000);press(&value,EH_R2,value.rt>10000);
            int assigned=eh_controller_assignment(key);int routed=assigned>=0?assigned:(int)automatic;if(routed>=0&&routed<4&&!used[routed])xinput_ports[routed]=(int)n;route(key,automatic,value,used);
        }
    }
#endif
    static unsigned rumble_tick;if(++rumble_tick%10==0)for(unsigned port=0;port<4;port++)if(motors[port][0]||motors[port][1])eh_pad_rumble(port,0,motors[port][0]);
    eh_pad_read_port(0,out);
}
int eh_pad_rumble(unsigned port,unsigned effect,uint16_t strength){
    if(port>=4||effect>1)return 0;
    if(!rumble_enabled)strength=0;
    motors[port][effect]=strength;int supported=0;
    if(sdl_ready){int plan[MAX_PADS];player_ports(plan);for(int i=0;i<MAX_PADS;i++)if(plan[i]==(int)port){
        if(pads[i].gamepad&&rumble_gamepad)supported|=rumble_gamepad(pads[i].gamepad,motors[port][0],motors[port][1],500);
        if(pads[i].joystick&&rumble_joystick)supported|=rumble_joystick(pads[i].joystick,motors[port][0],motors[port][1],500);
    }}
#ifdef _WIN32
    else if(xinput_set&&xinput_ports[port]>=0){XINPUT_VIBRATION vibration={motors[port][0],motors[port][1]};supported=xinput_set((DWORD)xinput_ports[port],&vibration)==ERROR_SUCCESS;}
#endif
    return supported;
}
void eh_pad_stop_rumble(void){for(unsigned port=0;port<4;port++)if(motors[port][0]||motors[port][1]){eh_pad_rumble(port,0,0);eh_pad_rumble(port,1,0);}}
void eh_pad_rumble_enabled(int enabled){rumble_enabled=!!enabled;if(!rumble_enabled)eh_pad_stop_rumble();}
void eh_pad_shutdown(void){
    eh_pad_stop_rumble();
    if(!sdl_ready)return;
    for(int i=0;i<MAX_PADS;i++){if(pads[i].gamepad)p_SDL_CloseGamepad(pads[i].gamepad);if(pads[i].joystick)p_SDL_CloseJoystick(pads[i].joystick);}
    memset(pads,0,sizeof(pads));p_SDL_QuitSubSystem(SDL_INIT_GAMEPAD);sdl_ready=0;if(sdl_library)library_close(sdl_library);sdl_library=NULL;
}
