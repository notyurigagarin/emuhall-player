/* RetroAchievements inside the playback worker, through the official rcheevos client (third-party/rcheevos).
   The launcher signs in (it holds the token) and passes the session here with an "ra-login" command; this file
   identifies the game with RetroAchievements' own hashing, reads emulated memory through the core's libretro
   memory maps, evaluates achievements each frame and reports unlocks as events. */
#include "libretro.h"
#include "rc_client.h"
#include "rc_libretro.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#include "achievements.h"
#include "ra_system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <winhttp.h>
#include <wincodec.h>
#endif

/* Provided by host.c. */
void eh_event(const char *json);

static rc_client_t *client;
static rc_libretro_memory_regions_t regions;
static struct retro_memory_map *memory_map;
static int regions_ready,game_loaded;
static int regions_dirty=1;
static void *memory_pointers[3];
static size_t memory_sizes[3];
static unsigned read_failures,reported_read_failures;
static int tracking_dirty=1;
static unsigned long long tracking_reported_ms;
static void *(*core_memory_data)(unsigned);
static size_t (*core_memory_size)(unsigned);
static char user_agent[256],core_clause[96];
static unsigned console_id;
static char *content_path;
static void *content_data;
static size_t content_size;
/* A state can be loaded while authentication/identification is still in flight. */
static char pending_progress_path[4400];
void eh_ra_load_progress(const char *state_path);

/* Toast drawn by the host over the game: the newest unlock, shown for a few seconds. */
#ifdef _WIN32
static CRITICAL_SECTION toast_lock;
#endif
static char toast_text[256];
static unsigned long long toast_until_ms;
static eh_ra_notification_t notification;
static char notification_badge[1024];
/* A challenge exists only after rcheevos reports a primed Trigger. */
typedef struct { unsigned id; char title[256],badge[1024]; } active_challenge;
static active_challenge challenges[EH_RA_MAX_CHALLENGES];
static size_t num_challenges;
typedef struct { eh_ra_progress_t snapshot; char badge[1024]; } progress_display;
static progress_display automatic_progress,tracked_progress;
static unsigned tracked_id;
static unsigned long long tracked_checked_ms;
static char tracked_json[7200];

static void overlay_lock(void){
#ifdef _WIN32
    EnterCriticalSection(&toast_lock);
#endif
}
static void overlay_unlock(void){
#ifdef _WIN32
    LeaveCriticalSection(&toast_lock);
#endif
}

/* Image networking and WIC decoding happen on separate threads, never during an emulated frame.
   The bounded memory cache contains only official, public RA badge images (no tokens or game data). */
#define BADGE_CACHE_SIZE 32
typedef struct { char url[1024]; int loading,ready; uint32_t pixels[EH_RA_BADGE_SIZE*EH_RA_BADGE_SIZE]; } badge_image;
static badge_image badge_cache[BADGE_CACHE_SIZE];
static unsigned badge_next;

#ifdef _WIN32
typedef struct { unsigned slot; char url[1024]; } badge_job;

static int decode_badge(const void *data,size_t size,uint32_t *pixels){
    /* Resolve COM at runtime: no new native dependency or DLL is bundled. */
    typedef HRESULT (WINAPI *init_fn)(LPVOID,DWORD);
    typedef HRESULT (WINAPI *create_fn)(REFCLSID,LPUNKNOWN,DWORD,REFIID,LPVOID*);
    typedef void (WINAPI *uninit_fn)(void);
    HMODULE ole=LoadLibraryExW(L"ole32.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!ole)return 0;
    init_fn initialize=NULL;create_fn create=NULL;uninit_fn uninitialize=NULL;
    FARPROC symbol=GetProcAddress(ole,"CoInitializeEx");memcpy(&initialize,&symbol,sizeof(initialize));
    symbol=GetProcAddress(ole,"CoCreateInstance");memcpy(&create,&symbol,sizeof(create));
    symbol=GetProcAddress(ole,"CoUninitialize");memcpy(&uninitialize,&symbol,sizeof(uninitialize));
    int ok=0;HRESULT initialized=E_FAIL;
    IWICImagingFactory *factory=NULL;IWICStream *stream=NULL;IWICBitmapDecoder *decoder=NULL;
    IWICBitmapFrameDecode *frame=NULL;IWICBitmapScaler *scaler=NULL;IWICFormatConverter *converter=NULL;
    const GUID clsid={0xcacaf262,0x9370,0x4615,{0xa1,0x3b,0x9f,0x55,0x39,0xda,0x4c,0x0a}};
    const GUID iid={0xec5ec8a9,0xc395,0x4314,{0x9c,0x77,0x54,0xd7,0xa9,0x35,0xff,0x70}};
    const GUID bgra={0x6fddc324,0x4e03,0x4bfe,{0xb1,0x85,0x3d,0x77,0x76,0x8d,0xc9,0x0f}};
    if(!initialize||!create||!uninitialize)goto done;
    initialized=initialize(NULL,COINIT_MULTITHREADED);if(FAILED(initialized))goto done;
    if(FAILED(create(&clsid,NULL,CLSCTX_INPROC_SERVER,&iid,(void**)&factory)))goto done;
    if(FAILED(IWICImagingFactory_CreateStream(factory,&stream))||FAILED(IWICStream_InitializeFromMemory(stream,(BYTE*)data,(DWORD)size)))goto done;
    if(FAILED(IWICImagingFactory_CreateDecoderFromStream(factory,(IStream*)stream,NULL,WICDecodeMetadataCacheOnLoad,&decoder)))goto done;
    if(FAILED(IWICBitmapDecoder_GetFrame(decoder,0,&frame)))goto done;
    UINT width=0,height=0;
    if(FAILED(IWICBitmapFrameDecode_GetSize(frame,&width,&height))||!width||!height||width>2048||height>2048)goto done;
    if(FAILED(IWICImagingFactory_CreateBitmapScaler(factory,&scaler))||FAILED(IWICBitmapScaler_Initialize(scaler,(IWICBitmapSource*)frame,EH_RA_BADGE_SIZE,EH_RA_BADGE_SIZE,WICBitmapInterpolationModeFant)))goto done;
    if(FAILED(IWICImagingFactory_CreateFormatConverter(factory,&converter))||FAILED(IWICFormatConverter_Initialize(converter,(IWICBitmapSource*)scaler,&bgra,WICBitmapDitherTypeNone,NULL,0,WICBitmapPaletteTypeCustom)))goto done;
    ok=SUCCEEDED(IWICFormatConverter_CopyPixels(converter,NULL,EH_RA_BADGE_SIZE*4,EH_RA_BADGE_SIZE*EH_RA_BADGE_SIZE*4,(BYTE*)pixels));
    if(ok)for(unsigned i=0;i<EH_RA_BADGE_SIZE*EH_RA_BADGE_SIZE;i++){
        uint32_t p=pixels[i],a=p>>24;
        unsigned b=((p&255)*a+18*(255-a))/255,g=(((p>>8)&255)*a+27*(255-a))/255,r=(((p>>16)&255)*a+42*(255-a))/255;
        pixels[i]=0xff000000|(r<<16)|(g<<8)|b;
    }
done:
    if(converter)IWICFormatConverter_Release(converter);
    if(scaler)IWICBitmapScaler_Release(scaler);
    if(frame)IWICBitmapFrameDecode_Release(frame);
    if(decoder)IWICBitmapDecoder_Release(decoder);
    if(stream)IWICStream_Release(stream);
    if(factory)IWICImagingFactory_Release(factory);
    if(SUCCEEDED(initialized))uninitialize();
    FreeLibrary(ole);return ok;
}

static DWORD WINAPI badge_thread(LPVOID param){
    badge_job *job=param;uint32_t pixels[EH_RA_BADGE_SIZE*EH_RA_BADGE_SIZE];int decoded=0;
    wchar_t url[1200],host[256],path[1024];URL_COMPONENTS parts;memset(&parts,0,sizeof(parts));parts.dwStructSize=sizeof(parts);
    parts.lpszHostName=host;parts.dwHostNameLength=255;parts.lpszUrlPath=path;parts.dwUrlPathLength=1023;
    HINTERNET session=NULL,connection=NULL,request=NULL;unsigned char *body=NULL;size_t length=0;
    if(MultiByteToWideChar(CP_UTF8,0,job->url,-1,url,1200)&&WinHttpCrackUrl(url,0,0,&parts)&&parts.nScheme==INTERNET_SCHEME_HTTPS&&parts.nPort==443){
        host[parts.dwHostNameLength]=0;path[parts.dwUrlPathLength]=0;
        if(_wcsicmp(host,L"media.retroachievements.org")&&_wcsicmp(host,L"retroachievements.org"))goto done;
        session=WinHttpOpen(L"EmuHall/1.0.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
        if(session){WinHttpSetTimeouts(session,3000,3000,3000,3000);connection=WinHttpConnect(session,host,443,0);}
        if(connection)request=WinHttpOpenRequest(connection,L"GET",path,NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
        if(request){
            DWORD redirect=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect));
            if(WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,NULL,0,0,0)&&WinHttpReceiveResponse(request,NULL)){
                DWORD status=0,n=sizeof(status);WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&n,WINHTTP_NO_HEADER_INDEX);
                if(status!=200)goto done;
                for(;;){DWORD available=0,read=0;if(!WinHttpQueryDataAvailable(request,&available)||!available)break;
                    if(length+available>1024*1024)goto done;
                    unsigned char *grown=realloc(body,length+available);if(!grown)goto done;body=grown;
                    if(!WinHttpReadData(request,body+length,available,&read)||!read)break;
                    length+=read;
                }
                if(length)decoded=decode_badge(body,length,pixels);
            }
        }
    }
done:
    if(request)WinHttpCloseHandle(request);
    if(connection)WinHttpCloseHandle(connection);
    if(session)WinHttpCloseHandle(session);
    free(body);
    overlay_lock();badge_image *image=&badge_cache[job->slot];
    if(!strcmp(image->url,job->url)){image->loading=0;image->ready=decoded;if(decoded)memcpy(image->pixels,pixels,sizeof(pixels));}
    overlay_unlock();free(job);return 0;
}
#endif

/* Called with the overlay lock. Failed downloads remain decorative placeholders. */
static void request_badge(const char *url){
    if(!url||!*url||strlen(url)>=sizeof(badge_cache[0].url))return;
    for(unsigned i=0;i<BADGE_CACHE_SIZE;i++)if(!strcmp(badge_cache[i].url,url))return;
#ifdef _WIN32
    unsigned slot=badge_next++%BADGE_CACHE_SIZE;
    for(unsigned i=0;i<BADGE_CACHE_SIZE&&badge_cache[slot].loading;i++)slot=(slot+1)%BADGE_CACHE_SIZE;
    if(badge_cache[slot].loading)return;
    badge_job *job=calloc(1,sizeof(*job));if(!job)return;
    memset(&badge_cache[slot],0,sizeof(badge_cache[slot]));snprintf(badge_cache[slot].url,sizeof(badge_cache[slot].url),"%s",url);badge_cache[slot].loading=1;
    job->slot=slot;snprintf(job->url,sizeof(job->url),"%s",url);
    HANDLE thread=CreateThread(NULL,0,badge_thread,job,0,NULL);
    if(thread)CloseHandle(thread);else{badge_cache[slot].loading=0;free(job);}
#endif
}
static int copy_badge(const char *url,uint32_t *pixels){
    if(!url||!*url)return 0;
    for(unsigned i=0;i<BADGE_CACHE_SIZE;i++)if(badge_cache[i].ready&&!strcmp(badge_cache[i].url,url)){
        memcpy(pixels,badge_cache[i].pixels,sizeof(badge_cache[i].pixels));return 1;
    }
    return 0;
}

static void json_string(char *out,size_t cap,const char *text){
    size_t n=0;if(cap<3)return;out[n++]='"';
    for(const unsigned char *p=(const unsigned char*)(text?text:"");*p&&n+7<cap;p++){
        if(*p=='"'||*p=='\\'){out[n++]='\\';out[n++]=*p;}
        else if(*p<0x20){n+=snprintf(out+n,cap-n,"\\u%04x",*p);}
        else out[n++]=*p;
    }
    out[n++]='"';out[n]=0;
}

/* Only the core's memory is read, and only while the emulation thread is stopped between frames. */
static uint32_t RC_CCONV read_memory(uint32_t address,uint8_t *buffer,uint32_t num_bytes,rc_client_t *c){
    (void)c;uint32_t count=regions_ready?rc_libretro_memory_read(&regions,address,buffer,num_bytes):0;
    if(count!=num_bytes&&read_failures<0xffffffffU)read_failures++;
    return count;
}
static void RC_CCONV core_memory_info(uint32_t id,rc_libretro_core_memory_info_t *info){
    info->data=core_memory_data?core_memory_data(id):NULL;info->size=core_memory_size?core_memory_size(id):0;
}

/* Cores may replace RAM at reset or after loading a state. Descriptors are not the
   initialized RA regions: both must be refreshed before the next memory read. */
static void refresh_memory(void){
    static const unsigned ids[]={RETRO_MEMORY_SYSTEM_RAM,RETRO_MEMORY_SAVE_RAM,RETRO_MEMORY_VIDEO_RAM};
    if(!core_memory_data||!core_memory_size)return;
    for(unsigned i=0;i<3;i++){
        void *pointer=core_memory_data(ids[i]);size_t size=core_memory_size(ids[i]);
        if(pointer!=memory_pointers[i]||size!=memory_sizes[i])regions_dirty=1;
        memory_pointers[i]=pointer;memory_sizes[i]=size;
    }
    if(!regions_dirty)return;
    rc_libretro_memory_destroy(&regions);
    regions_ready=rc_libretro_memory_init(&regions,memory_map,core_memory_info,console_id);regions_dirty=0;tracking_dirty=1;
    size_t mapped=0;for(unsigned i=0;i<regions.count;i++)if(regions.data[i])mapped+=regions.size[i];
    fprintf(stderr,"RetroAchievements memory: %u regions, %lu mapped bytes, %s\n",regions.count,(unsigned long)mapped,regions_ready?"ready":"unavailable");
}

#ifdef _WIN32
typedef struct{char *url,*post,*content_type;rc_client_server_callback_t callback;void *callback_data;}http_job;
/* One HTTPS request on its own thread, so a slow server never stalls a frame. */
static DWORD WINAPI http_thread(LPVOID param){
    http_job *job=param;rc_api_server_response_t response;memset(&response,0,sizeof(response));
    char *body=NULL;size_t length=0;int status=0;
    wchar_t url[2048],host[256],path[1792];
    URL_COMPONENTS parts;memset(&parts,0,sizeof(parts));parts.dwStructSize=sizeof(parts);
    parts.lpszHostName=host;parts.dwHostNameLength=256;parts.lpszUrlPath=path;parts.dwUrlPathLength=1792;
    HINTERNET session=NULL,connection=NULL,request=NULL;
    if(MultiByteToWideChar(CP_UTF8,0,job->url,-1,url,2048)&&WinHttpCrackUrl(url,0,0,&parts)){
        wchar_t agent[160];MultiByteToWideChar(CP_UTF8,0,user_agent,-1,agent,160);
        session=WinHttpOpen(agent,WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
        if(session)connection=WinHttpConnect(session,host,parts.nPort,0);
        if(connection)request=WinHttpOpenRequest(connection,job->post?L"POST":L"GET",path,NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0);
        if(request){
            wchar_t header[256]=L"";if(job->content_type){wchar_t type[200];MultiByteToWideChar(CP_UTF8,0,job->content_type,-1,type,200);swprintf(header,256,L"Content-Type: %ls",type);}
            DWORD post_length=job->post?(DWORD)strlen(job->post):0;
            if(WinHttpSendRequest(request,header[0]?header:WINHTTP_NO_ADDITIONAL_HEADERS,header[0]?(DWORD)-1:0,job->post,post_length,post_length,0)&&WinHttpReceiveResponse(request,NULL)){
                DWORD code=0,size=sizeof(code);
                if(WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&code,&size,WINHTTP_NO_HEADER_INDEX))status=(int)code;
                for(;;){DWORD available=0;if(!WinHttpQueryDataAvailable(request,&available)||!available)break;
                    if(length+available>8*1024*1024)break;
                    char *grown=realloc(body,length+available+1);if(!grown)break;body=grown;
                    DWORD got=0;if(!WinHttpReadData(request,body+length,available,&got)||!got)break;length+=got;}
                if(body)body[length]=0;
            }
        }
    }
    if(request){WinHttpCloseHandle(request);}
    if(connection){WinHttpCloseHandle(connection);}
    if(session){WinHttpCloseHandle(session);}
    /* No status means the request never completed: rcheevos treats that as retryable. */
    response.body=body?body:"";response.body_length=length;response.http_status_code=status?status:RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    job->callback(&response,job->callback_data);
    free(body);free(job->url);free(job->post);free(job->content_type);free(job);
    return 0;
}
#endif

static char *copy_text(const char *text){if(!text)return NULL;size_t n=strlen(text)+1;char *c=malloc(n);if(c)memcpy(c,text,n);return c;}

static void RC_CCONV server_call(const rc_api_request_t *request,rc_client_server_callback_t callback,void *callback_data,rc_client_t *c){
    (void)c;
    const char *diagnostic=getenv("EMUHALL_DIAGNOSTICS");
    if(diagnostic&&!strcmp(diagnostic,"1")&&request->post_data&&
       (strstr(request->post_data,"r=awardachievement")||strstr(request->post_data,"r=submitlbentry"))){
        rc_api_server_response_t blocked;memset(&blocked,0,sizeof(blocked));blocked.body="";blocked.http_status_code=RC_API_SERVER_RESPONSE_CLIENT_ERROR;
        fprintf(stderr,"RetroAchievements diagnostics: blocked a result submission\n");callback(&blocked,callback_data);return;
    }
#ifdef _WIN32
    http_job *job=calloc(1,sizeof(*job));
    if(job){job->url=copy_text(request->url);job->post=copy_text(request->post_data);job->content_type=copy_text(request->content_type);job->callback=callback;job->callback_data=callback_data;
        HANDLE thread=CreateThread(NULL,0,http_thread,job,0,NULL);if(thread){CloseHandle(thread);return;}
        free(job->url);free(job->post);free(job->content_type);free(job);}
#endif
    rc_api_server_response_t failed;memset(&failed,0,sizeof(failed));failed.body="";failed.http_status_code=RC_API_SERVER_RESPONSE_CLIENT_ERROR;
    callback(&failed,callback_data);
}

static unsigned long long clock_ms(void){
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now;if(timespec_get(&now,TIME_UTC)==TIME_UTC)return (unsigned long long)now.tv_sec*1000+(unsigned long long)now.tv_nsec/1000000;
    return 0;
#endif
}

static void show_notification(int kind,const char *heading,const char *detail,const char *badge,const rc_client_user_game_summary_t *summary){
    overlay_lock();memset(&notification,0,sizeof(notification));
    notification.kind=kind;notification.hardcore=rc_client_get_hardcore_enabled(client);
    snprintf(notification.heading,sizeof(notification.heading),"%s",heading?heading:"");
    snprintf(notification.detail,sizeof(notification.detail),"%s",detail?detail:"");
    snprintf(notification_badge,sizeof(notification_badge),"%s",badge?badge:"");
    if(summary){notification.unlocked=summary->num_unlocked_achievements;notification.total=summary->num_core_achievements;notification.points=summary->points_unlocked;notification.points_total=summary->points_core;}
    request_badge(notification_badge);
    snprintf(toast_text,sizeof(toast_text),"%s",heading?heading:"");toast_until_ms=clock_ms()+5000;
    overlay_unlock();
}

int eh_ra_notification(eh_ra_notification_t *out){
    if(!client||!out)return 0;
    overlay_lock();int showing=notification.heading[0]&&clock_ms()<toast_until_ms;
    if(showing){*out=notification;out->badge_ready=copy_badge(notification_badge,out->badge_pixels);}
    overlay_unlock();return showing;
}

size_t eh_ra_challenges(eh_ra_challenge_t *out,size_t max){
    if(!client||!out||!max)return 0;
    overlay_lock();size_t count=num_challenges<max?num_challenges:max;
    for(size_t i=0;i<count;i++){
        out[i].id=challenges[i].id;snprintf(out[i].title,sizeof(out[i].title),"%s",challenges[i].title);
        out[i].badge_ready=copy_badge(challenges[i].badge,out[i].badge_pixels);
    }
    overlay_unlock();return count;
}

int eh_ra_progress(eh_ra_progress_t *out){
    if(!client||!out)return 0;
    overlay_lock();progress_display *display=automatic_progress.snapshot.id&&automatic_progress.snapshot.id!=tracked_progress.snapshot.id?&automatic_progress:&tracked_progress;
    int showing=display->snapshot.id!=0;
    if(showing){*out=display->snapshot;out->badge_ready=copy_badge(display->badge,out->badge_pixels);}
    overlay_unlock();return showing;
}

/* Copies the current toast if one is showing; returns 0 when there is nothing to draw. */
int eh_ra_toast(char *out,size_t cap){
    int showing=0;
#ifdef _WIN32
    if(!client)return 0;
    EnterCriticalSection(&toast_lock);
#endif
    if(toast_text[0]&&clock_ms()<toast_until_ms){snprintf(out,cap,"%s",toast_text);showing=1;}
#ifdef _WIN32
    LeaveCriticalSection(&toast_lock);
#endif
    return showing;
}

static void report_game(void){
    const rc_client_game_t *game=rc_client_get_game_info(client);
    rc_client_user_game_summary_t summary;memset(&summary,0,sizeof(summary));rc_client_get_user_game_summary(client,&summary);
    char title[600],badge_url[1024]="",badge[2100];json_string(title,sizeof(title),game&&game->title?game->title:"");
    if(game)rc_client_game_get_image_url(game,badge_url,sizeof(badge_url));
    json_string(badge,sizeof(badge),badge_url);
    char json[3500];snprintf(json,sizeof(json),"{\"event\":\"ra-game\",\"id\":%u,\"available\":true,\"title\":%s,\"badge\":%s,\"unlocked\":%u,\"total\":%u,\"points\":%u,\"pointsTotal\":%u,\"hardcore\":%s}",
        game?game->id:0,title,badge,summary.num_unlocked_achievements,summary.num_core_achievements,summary.points_unlocked,summary.points_core,rc_client_get_hardcore_enabled(client)?"true":"false");
    eh_event(json);
}

void eh_ra_list(void);

static size_t achievement_json(char *json,size_t cap,const rc_client_achievement_t *a,int challenge){
    char title[600],description[1600],progress[64],badge_url[1024]="",locked_url[1024]="",badge[2100],locked[2100];
    json_string(title,sizeof(title),a->title);json_string(description,sizeof(description),a->description);json_string(progress,sizeof(progress),a->measured_progress);
    rc_client_achievement_get_image_url(a,RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED,badge_url,sizeof(badge_url));
    rc_client_achievement_get_image_url(a,RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE,locked_url,sizeof(locked_url));
    json_string(badge,sizeof(badge),badge_url);json_string(locked,sizeof(locked),locked_url);
    /* `unlocked` flags include historical softcore awards; state reflects the currently selected mode. */
    int unlocked=a->state==RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED;
    char value[32]="null",target[32]="null";unsigned current=0,total=0;char plain[32];size_t j=0;
    /* rcheevos owns the measurement. Never infer a count/target from description text. */
    for(size_t i=0;a->measured_progress[i]&&j+1<sizeof(plain);i++)if(a->measured_progress[i]!=',')plain[j++]=a->measured_progress[i];
    plain[j]=0;
    if(sscanf(plain,"%u/%u",&current,&total)==2&&total){snprintf(value,sizeof(value),"%u",current);snprintf(target,sizeof(target),"%u",total);}
    float percent=a->measured_progress[0]?a->measured_percent:0;if(!(percent>=0))percent=0;if(percent>100)percent=100;
    int n=snprintf(json,cap,"{\"id\":%u,\"title\":%s,\"description\":%s,\"points\":%u,\"unlocked\":%s,\"progress\":%s,\"progressPercent\":%.3f,\"measuredValue\":%s,\"measuredTarget\":%s,\"badge\":%s,\"badgeLocked\":%s,\"missable\":%s,\"challengeActive\":%s,\"unlockTime\":%lld,\"supported\":%s}",
        a->id,title,description,a->points,unlocked?"true":"false",progress,percent,value,target,badge,locked,a->type==RC_CLIENT_ACHIEVEMENT_TYPE_MISSABLE?"true":"false",challenge&&!unlocked?"true":"false",unlocked?(long long)a->unlock_time:0,a->state==RC_CLIENT_ACHIEVEMENT_STATE_DISABLED?"false":"true");
    return n>0&&(size_t)n<cap?(size_t)n:0;
}

static void copy_progress(progress_display *display,const rc_client_achievement_t *a,int pinned){
    memset(display,0,sizeof(*display));if(!a)return;
    display->snapshot.id=a->id;display->snapshot.pinned=pinned;
    snprintf(display->snapshot.title,sizeof(display->snapshot.title),"%s",a->title?a->title:"");
    snprintf(display->snapshot.progress,sizeof(display->snapshot.progress),"%s",a->state==RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED?"Unlocked":a->measured_progress);
    display->snapshot.percent=a->state==RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED?100:a->measured_progress[0]?a->measured_percent:0;
    if(!(display->snapshot.percent>=0))display->snapshot.percent=0;
    if(display->snapshot.percent>100)display->snapshot.percent=100;
    char plain[32];size_t n=0;
    for(size_t i=0;a->measured_progress[i]&&n+1<sizeof(plain);i++)if(a->measured_progress[i]!=',')plain[n++]=a->measured_progress[i];
    plain[n]=0;
    display->snapshot.has_counts=sscanf(plain,"%u/%u",&display->snapshot.current,&display->snapshot.target)==2&&display->snapshot.target;
    rc_client_achievement_get_image_url(a,RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED,display->badge,sizeof(display->badge));request_badge(display->badge);
}

static void progress_changed(const rc_client_achievement_t *a,const char *action){
    overlay_lock();copy_progress(&automatic_progress,a,0);overlay_unlock();
    char detail[7000],json[7200];
    if(a&&achievement_json(detail,sizeof(detail),a,a->bucket==RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE)){
        snprintf(json,sizeof(json),"{\"event\":\"ra-progress-%s\",\"achievement\":%s}",action,detail);eh_event(json);
    }else eh_event("{\"event\":\"ra-progress-hide\"}");
}

static void update_tracked(int force){
    if(!client||!game_loaded)return;
    unsigned long long now=clock_ms();if(!force&&now-tracked_checked_ms<250)return;tracked_checked_ms=now;
    const rc_client_achievement_t *a=tracked_id&&regions_ready?rc_client_get_achievement_info(client,tracked_id):NULL;
    if(a&&a->state==RC_CLIENT_ACHIEVEMENT_STATE_DISABLED)a=NULL;
    char detail[7000]="null",json[7200];
    if(a)achievement_json(detail,sizeof(detail),a,a->bucket==RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE);
    snprintf(json,sizeof(json),"{\"event\":\"ra-tracked\",\"achievement\":%s}",detail);
    if(force||strcmp(tracked_json,json)){
        snprintf(tracked_json,sizeof(tracked_json),"%s",json);
        overlay_lock();copy_progress(&tracked_progress,a,1);overlay_unlock();eh_event(json);
    }
}

void eh_ra_track(unsigned id){
    tracked_id=id;tracked_json[0]=0;tracked_checked_ms=0;update_tracked(1);
}

static void challenge_changed(const rc_client_achievement_t *a,int active){
    char badge[1024]="";rc_client_achievement_get_image_url(a,RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED,badge,sizeof(badge));
    overlay_lock();size_t i=0;while(i<num_challenges&&challenges[i].id!=a->id)i++;
    if(active){
        if(i<num_challenges||num_challenges<EH_RA_MAX_CHALLENGES){
            if(i==num_challenges)num_challenges++;
            challenges[i].id=a->id;snprintf(challenges[i].title,sizeof(challenges[i].title),"%s",a->title?a->title:"");snprintf(challenges[i].badge,sizeof(challenges[i].badge),"%s",badge);request_badge(badge);
        }
    }else if(i<num_challenges){memmove(&challenges[i],&challenges[i+1],(num_challenges-i-1)*sizeof(challenges[0]));num_challenges--;}
    overlay_unlock();
    char detail[7000],json[7100];if(achievement_json(detail,sizeof(detail),a,active)){
        snprintf(json,sizeof(json),"{\"event\":\"ra-challenge-%s\",\"achievement\":%s}",active?"show":"hide",detail);eh_event(json);
    }
}

static void status(const char *message){
    fprintf(stderr,"RetroAchievements: %s\n",message);
    char text[600],json[700];json_string(text,sizeof(text),message);snprintf(json,sizeof(json),"{\"event\":\"ra-status\",\"message\":%s}",text);eh_event(json);
}

static void RC_CCONV on_event(const rc_client_event_t *event,rc_client_t *c){
    (void)c;
    if(event->type==RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED&&event->achievement){
        const rc_client_achievement_t *a=event->achievement;
        char title[600],description[1600],badge_url[1024]="",badge[2100],json[5000];json_string(title,sizeof(title),a->title);json_string(description,sizeof(description),a->description);
        rc_client_achievement_get_image_url(a,RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED,badge_url,sizeof(badge_url));json_string(badge,sizeof(badge),badge_url);
        snprintf(json,sizeof(json),"{\"event\":\"achievement\",\"id\":%u,\"title\":%s,\"description\":%s,\"points\":%u,\"badge\":%s,\"unlockTime\":%lld}",a->id,title,description,a->points,badge,(long long)a->unlock_time);
        eh_event(json);
        char detail[256];snprintf(detail,sizeof(detail),"%u points · %s",a->points,a->title?a->title:"");
        show_notification(2,"Achievement unlocked",detail,badge_url,NULL);report_game();eh_ra_list();
    }else if(event->type==RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW&&event->achievement){
        challenge_changed(event->achievement,1);
    }else if(event->type==RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE&&event->achievement){
        challenge_changed(event->achievement,0);
    }else if(event->type==RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW&&event->achievement){
        progress_changed(event->achievement,"show");
    }else if(event->type==RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE&&event->achievement){
        progress_changed(event->achievement,"update");
    }else if(event->type==RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE){
        progress_changed(NULL,"hide");
    }else if(event->type==RC_CLIENT_EVENT_GAME_COMPLETED){
        show_notification(3,"Game completed","Every achievement unlocked",NULL,NULL);report_game();
    }else if(event->type==RC_CLIENT_EVENT_SERVER_ERROR&&event->server_error){
        status(event->server_error->error_message?event->server_error->error_message:"RetroAchievements reported an error");
    }else if(event->type==RC_CLIENT_EVENT_DISCONNECTED){
        status("RetroAchievements is unreachable; unlocks will be sent when the connection returns.");
    }else if(event->type==RC_CLIENT_EVENT_RECONNECTED){
        status("Reconnected to RetroAchievements.");
    }
}

static void RC_CCONV on_game_loaded(int result,const char *error,rc_client_t *c,void *userdata){
    (void)c;(void)userdata;
    if(result==RC_OK){game_loaded=1;
        if(pending_progress_path[0]) { char path[4400];snprintf(path,sizeof(path),"%s",pending_progress_path);pending_progress_path[0]=0;eh_ra_load_progress(path); }
        report_game();eh_ra_list();update_tracked(1);
        const rc_client_game_t *game=rc_client_get_game_info(client);char detail[256],badge_url[1024]="";
        rc_client_user_game_summary_t summary;memset(&summary,0,sizeof(summary));rc_client_get_user_game_summary(client,&summary);
        snprintf(detail,sizeof(detail),"%u / %u unlocked · %s",summary.num_unlocked_achievements,summary.num_core_achievements,rc_client_get_hardcore_enabled(client)?"Hardcore":"Softcore");
        if(game)rc_client_game_get_image_url(game,badge_url,sizeof(badge_url));
        if(game&&summary.num_core_achievements)show_notification(1,game->title,detail,badge_url,&summary);
        char title[600],badge[2100],json[3500];json_string(title,sizeof(title),game?game->title:"");json_string(badge,sizeof(badge),badge_url);
        snprintf(json,sizeof(json),"{\"event\":\"ra-game-loaded\",\"title\":%s,\"badge\":%s,\"unlocked\":%u,\"total\":%u,\"points\":%u,\"pointsTotal\":%u,\"hardcore\":%s}",title,badge,summary.num_unlocked_achievements,summary.num_core_achievements,summary.points_unlocked,summary.points_core,rc_client_get_hardcore_enabled(client)?"true":"false");eh_event(json);
    }
    else if(result==RC_NO_GAME_LOADED){
        /* The fingerprint RetroAchievements identifies games by, so the person can ask for this version to be added. */
        const rc_client_game_t *unknown=rc_client_get_game_info(client);
        if(unknown&&unknown->hash&&strlen(unknown->hash)==32){char json[160];snprintf(json,sizeof(json),"{\"event\":\"ra-unknown\",\"hash\":\"%s\"}",unknown->hash);eh_event(json);}
        status("RetroAchievements does not recognize this exact game file.");}
    else status(error?error:"Could not load achievements for this game.");
    free(content_path);content_path=NULL;free(content_data);content_data=NULL;
}

static void RC_CCONV on_login(int result,const char *error,rc_client_t *c,void *userdata){
    (void)userdata;
    if(result!=RC_OK){status(error?error:"RetroAchievements sign-in failed; sign in again in Settings.");return;}
    rc_client_begin_identify_and_load_game(c,console_id,content_path,content_data,content_size,on_game_loaded,NULL);
}

/* RetroAchievements' fingerprint of a game file (discs are hashed from their boot executable), without signing in.
   Lets the library name a disc whose file name says nothing (e.g. "SLES_009.72.bin"). 0 when not hashable. */
int eh_ra_hash(const char *system,const char *path,char out[33]){
    unsigned console=console_for(system);
    /* Identification support does not advertise unverified achievement runtime integration. */
    if(!strcmp(system,"megadrive"))console=RC_CONSOLE_MEGA_DRIVE;
    if(!strcmp(system,"sms"))console=RC_CONSOLE_MASTER_SYSTEM;
    if(!strcmp(system,"gg"))console=RC_CONSOLE_GAME_GEAR;
    return console&&rc_hash_generate_from_file(out,console,path);
}

/* The engine in use, for the user agent RetroAchievements asks for: "<core name>/<version>", no spaces. */
void eh_ra_set_core(const char *name,const char *version){
    char n[48],v[40];snprintf(n,sizeof(n),"%s",name&&*name?name:"unknown");snprintf(v,sizeof(v),"%s",version&&*version?version:"0");
    for(char *p=n;*p;p++)if(*p==' '||*p=='/'||*p=='('||*p==')')*p='_';
    v[strcspn(v," ()")]=0;if(v[0]=='v'||v[0]=='V')memmove(v,v+1,strlen(v));
    snprintf(core_clause,sizeof(core_clause),"%s/%s",n,v);
}

/* "Windows 10.0"-style name for the user agent. */
static void os_clause(char *out,size_t cap){
#ifdef _WIN32
    typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW*);
    FARPROC symbol=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion");OSVERSIONINFOW v;memset(&v,0,sizeof(v));v.dwOSVersionInfoSize=sizeof(v);
    if(symbol){RtlGetVersionFn get;memcpy(&get,&symbol,sizeof(get));if(get(&v)==0){snprintf(out,cap,"Windows %lu.%lu",v.dwMajorVersion,v.dwMinorVersion);return;}}
    snprintf(out,cap,"Windows");
#else
    snprintf(out,cap,"Unknown");
#endif
}

/* Keeps the core's memory map so achievements can find the emulated system's RAM. */
void eh_ra_memory_map(const struct retro_memory_map *map){
    if(!map||!map->num_descriptors){
        if(memory_map){free((void*)memory_map->descriptors);free(memory_map);memory_map=NULL;}
        regions_dirty=1;return;
    }
    if(!map->descriptors||map->num_descriptors>4096)return;
    struct retro_memory_map *copy=calloc(1,sizeof(*copy));
    struct retro_memory_descriptor *descriptors=calloc(map->num_descriptors,sizeof(*descriptors));
    if(!copy||!descriptors){free(copy);free(descriptors);return;}
    memcpy(descriptors,map->descriptors,map->num_descriptors*sizeof(*descriptors));
    copy->descriptors=descriptors;copy->num_descriptors=map->num_descriptors;
    if(memory_map){free((void*)memory_map->descriptors);free(memory_map);}
    memory_map=copy;
    regions_dirty=1;
}

/* "ra-login <system> <hardcore 0|1> <user> <token>": starts sign-in and game identification. */
int eh_ra_start(const char *args,const char *path,const void *data,size_t size,void *(*memory_data)(unsigned),size_t (*memory_size)(unsigned)){
    char system[16]={0},user[128]={0},token[128]={0};int hardcore=0;
    if(client||sscanf(args,"%15s %d %127s %127s",system,&hardcore,user,token)!=4)return 0;
    console_id=console_for(system);if(!console_id){status("RetroAchievements does not support this system.");return 0;}
#ifdef _WIN32
    InitializeCriticalSection(&toast_lock);
#endif
    core_memory_data=memory_data;core_memory_size=memory_size;
    refresh_memory();
    client=rc_client_create(read_memory,server_call);if(!client)return 0;
    char clause[64]="";rc_client_get_user_agent_clause(client,clause,sizeof(clause));
    char os[48];os_clause(os,sizeof(os));
    /* RetroAchievements' format: EmulatorName/version (OS version) core/version, then the rcheevos clause. */
    snprintf(user_agent,sizeof(user_agent),"EmuHall/1.0.0 (%s) %s %s",os,core_clause[0]?core_clause:"unknown/0",clause);
    rc_client_set_event_handler(client,on_event);
    rc_client_set_allow_background_memory_reads(client,0); /* Memory is read only between frames, on this thread. */
    rc_client_set_hardcore_enabled(client,hardcore);
    /* Local reproduction sessions may inspect official definitions/progress but
       must never submit an achievement or leaderboard result. */
    const char *diagnostic=getenv("EMUHALL_DIAGNOSTICS");
    if(diagnostic&&!strcmp(diagnostic,"1")){
        rc_client_set_hardcore_enabled(client,0);
        rc_client_set_spectator_mode_enabled(client,1);
        fprintf(stderr,"RetroAchievements diagnostics: spectator mode, submissions disabled\n");
    }
    /* Hashing needs the file (discs are hashed from their tracks) or, for cartridges, the bytes the core loaded. */
    content_path=copy_text(path);
    if(data&&size){content_data=malloc(size);if(content_data){memcpy(content_data,data,size);content_size=size;}}
    rc_client_begin_login_with_token(client,user,token,on_login,NULL);
    return 1;
}

void eh_ra_frame(void){
    if(!client)return;
    refresh_memory();
    if(game_loaded&&regions_ready)rc_client_do_frame(client);else rc_client_idle(client);
    update_tracked(0);
    if(game_loaded&&(tracking_dirty||(read_failures!=reported_read_failures&&clock_ms()-tracking_reported_ms>=1000)))eh_ra_list();
}
void eh_ra_idle(void){if(client){refresh_memory();rc_client_idle(client);update_tracked(0);}}
void eh_ra_reset(void){pending_progress_path[0]=0;if(client){refresh_memory();rc_client_reset(client);eh_ra_list();}}
int eh_ra_hardcore(void){return client&&rc_client_get_hardcore_enabled(client);}

/* Achievement progress travels with save states, so loading an older state does not confuse the runtime. */
int eh_ra_save_progress(const char *state_path){
    if(!client||!game_loaded)return 1;
    size_t size=rc_client_progress_size(client);if(!size)return 1;
    uint8_t *buffer=malloc(size);if(!buffer)return 0;
    int ok=rc_client_serialize_progress_sized(client,buffer,size)==RC_OK;
    char path[4400];snprintf(path,sizeof(path),"%s.achievements",state_path);
    FILE *f=ok?fopen(path,"wb"):NULL;ok=f&&fwrite(buffer,1,size,f)==size;if(f)fclose(f);
    free(buffer);return ok;
}
void eh_ra_load_progress(const char *state_path){
    if(!client||!game_loaded){snprintf(pending_progress_path,sizeof(pending_progress_path),"%s",state_path);return;}
    refresh_memory();
    char path[4400];snprintf(path,sizeof(path),"%s.achievements",state_path);
    FILE *f=fopen(path,"rb");if(!f){rc_client_deserialize_progress_sized(client,NULL,0);eh_ra_list();return;}
    fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    uint8_t *buffer=size>0&&size<16*1024*1024?malloc((size_t)size):NULL;
    int result=RC_INVALID_STATE;
    if(buffer&&fread(buffer,1,(size_t)size,f)==(size_t)size)result=rc_client_deserialize_progress_sized(client,buffer,(size_t)size);
    if(result!=RC_OK)rc_client_deserialize_progress_sized(client,NULL,0);
    free(buffer);fclose(f);eh_ra_list();
}

/* The achievement list for the in-game menu, as one event. */
void eh_ra_list(void){
    if(!client||!game_loaded){eh_event("{\"event\":\"ra-list\",\"available\":false}");return;}
    rc_client_achievement_list_t *list=rc_client_create_achievement_list(client,RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
    if(!list){eh_event("{\"event\":\"ra-list\",\"available\":false}");return;}
    size_t cap=1<<20,n=0;char *json=malloc(cap);if(!json){rc_client_destroy_achievement_list(list);return;}
    unsigned unsupported=0,total=0;
    for(uint32_t b=0;b<list->num_buckets;b++)for(uint32_t i=0;i<list->buckets[b].num_achievements;i++){
        total++;if(list->buckets[b].achievements[i]->state==RC_CLIENT_ACHIEVEMENT_STATE_DISABLED)unsupported++;
    }
    const char *tracking=!regions_ready||(unsupported&&unsupported==total)?"unavailable":unsupported?"partial":"active";
    char diagnostic[300];snprintf(diagnostic,sizeof(diagnostic),"{\"event\":\"ra-tracking\",\"memoryReady\":%s,\"readFailures\":%u,\"unsupported\":%u,\"state\":\"%s\"}",regions_ready?"true":"false",read_failures,unsupported,tracking);eh_event(diagnostic);
    if(read_failures!=reported_read_failures){fprintf(stderr,"RetroAchievements memory: %u failed reads, %u/%u achievements unsupported\n",read_failures,unsupported,total);reported_read_failures=read_failures;}
    tracking_dirty=0;tracking_reported_ms=clock_ms();
    /* An unmapped/disabled achievement must also disappear from the native HUD;
       a failed memory reference need not produce a normal Trigger HIDE event. */
    overlay_lock();
    for(size_t c=0;c<num_challenges;){
        int active=0;
        if(regions_ready)for(uint32_t b=0;b<list->num_buckets;b++)for(uint32_t i=0;i<list->buckets[b].num_achievements;i++){
            const rc_client_achievement_t *a=list->buckets[b].achievements[i];
            if(a->id==challenges[c].id&&a->bucket==RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE)active=1;
        }
        if(active)c++;else{memmove(&challenges[c],&challenges[c+1],(num_challenges-c-1)*sizeof(challenges[0]));num_challenges--;}
    }
    if(!regions_ready)memset(&automatic_progress,0,sizeof(automatic_progress));
    overlay_unlock();
    n+=snprintf(json+n,cap-n,"{\"event\":\"ra-list\",\"available\":true,\"tracking\":{\"memoryReady\":%s,\"readFailures\":%u,\"unsupported\":%u,\"state\":\"%s\"},\"achievements\":[",regions_ready?"true":"false",read_failures,unsupported,tracking);
    int first=1;
    for(uint32_t b=0;b<list->num_buckets;b++)for(uint32_t i=0;i<list->buckets[b].num_achievements;i++){
        const rc_client_achievement_t *a=list->buckets[b].achievements[i];
        if(cap-n<7100)break;
        int active=regions_ready&&a->bucket==RC_CLIENT_ACHIEVEMENT_BUCKET_ACTIVE_CHALLENGE;
        if(!first)json[n++]=',';
        size_t written=achievement_json(json+n,cap-n,a,active);if(!written){if(!first)n--;break;}n+=written;
        first=0;
    }
    snprintf(json+n,cap-n,"]}");
    eh_event(json);free(json);rc_client_destroy_achievement_list(list);update_tracked(1);
}

void eh_ra_shutdown(void){
    if(client){overlay_lock();num_challenges=0;notification.heading[0]=0;toast_text[0]=0;memset(&automatic_progress,0,sizeof(automatic_progress));memset(&tracked_progress,0,sizeof(tracked_progress));overlay_unlock();eh_event("{\"event\":\"ra-list\",\"available\":false}");}
    if(client){rc_client_destroy(client);client=NULL;}
    if(regions_ready){rc_libretro_memory_destroy(&regions);regions_ready=0;}
    if(memory_map){free((void*)memory_map->descriptors);free(memory_map);memory_map=NULL;}
    game_loaded=0;regions_dirty=1;tracked_id=0;tracked_json[0]=0;read_failures=reported_read_failures=0;pending_progress_path[0]=0;
    tracking_dirty=1;tracking_reported_ms=0;
    memset(memory_pointers,0,sizeof(memory_pointers));memset(memory_sizes,0,sizeof(memory_sizes));
}

/* Temporal replay must not leave achievement progress ahead of the restored game. */
int eh_ra_temporal_safe(void){return client==NULL;}
