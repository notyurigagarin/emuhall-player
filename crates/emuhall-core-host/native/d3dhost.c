/* Direct3D 11 and 12 rendering for engines that offer it (PS2 with LRPS2). On Intel and other integrated graphics
   these are much faster than OpenGL. The engine draws with a device created here; each finished frame is copied
   back into the player's ordinary frame buffer exactly like the OpenGL path (glhost.c), so presentation, pacing,
   the pause-menu picture and screenshots stay the same for every engine. Copies run one frame behind, so the CPU
   never waits for the graphics card.

   Only version 1 of libretro's Direct3D interfaces is offered: the frame is handed over on the thread that runs
   the engine and copied before video_refresh returns, which needs no locking. The DLLs are loaded at run time;
   all of them ship with Windows 10 and 11. */
#include "d3dhost.h"
#include "picture_layout.h"
#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include "libretro.h"

/* Version 1 of libretro_d3d11.h / libretro_d3d12.h (libretro-common), the layout engines read. */
struct eh_d3d11_interface {
    enum retro_hw_render_interface_type interface_type;unsigned interface_version;void *handle;
    ID3D11Device *device;ID3D11DeviceContext *context;D3D_FEATURE_LEVEL featureLevel;pD3DCompile D3DCompile;
};
struct eh_d3d12_interface {
    enum retro_hw_render_interface_type interface_type;unsigned interface_version;void *handle;
    ID3D12Device *device;ID3D12CommandQueue *queue;pD3DCompile D3DCompile;D3D12_RESOURCE_STATES required_state;
    void (*set_texture)(void *handle,ID3D12Resource *texture,DXGI_FORMAT format);
};

static unsigned api; /* RETRO_HW_CONTEXT_D3D11, _D3D12, or 0 */
static HMODULE compiler_dll,d3d_dll,dxgi_dll;
static int warned_format;
#define READ_SLOTS 3

/* --- shared ---------------------------------------------------------------------------------------------- */
/* The default adapter, the one Windows would use for this program, and its name for the session log. */
static IDXGIAdapter1 *default_adapter(char *name,size_t cap){
    typedef HRESULT (WINAPI *Factory)(REFIID,void**);
    name[0]=0;if(!dxgi_dll)dxgi_dll=LoadLibraryExW(L"dxgi.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!dxgi_dll)return NULL;
    FARPROC symbol=GetProcAddress(dxgi_dll,"CreateDXGIFactory1");if(!symbol)return NULL;
    Factory create;memcpy(&create,&symbol,sizeof(create));
    IDXGIFactory1 *factory=NULL;IDXGIAdapter1 *adapter=NULL;
    if(FAILED(create(&IID_IDXGIFactory1,(void**)&factory)))return NULL;
    if(FAILED(IDXGIFactory1_EnumAdapters1(factory,0,&adapter)))adapter=NULL;
    IDXGIFactory1_Release(factory);
    if(adapter){DXGI_ADAPTER_DESC1 desc;if(SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter,&desc)))WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name,(int)cap,NULL,NULL);}
    return adapter;
}
static pD3DCompile shader_compiler(void){
    if(!compiler_dll){compiler_dll=LoadLibraryExW(L"d3dcompiler_47.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);}if(!compiler_dll)return NULL;
    FARPROC symbol=GetProcAddress(compiler_dll,"D3DCompile");pD3DCompile compile=NULL;
    if(symbol){memcpy(&compile,&symbol,sizeof(compile));}return compile;
}
/* One row of a finished frame into the player's 0x00RRGGBB pixels. */
static int convert_row(DXGI_FORMAT format,const uint8_t *src,uint32_t *dst,unsigned w){
    switch(format){
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        for(unsigned x=0;x<w;x++,src+=4){dst[x]=((uint32_t)src[0]<<16)|((uint32_t)src[1]<<8)|src[2];}return 1;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:case DXGI_FORMAT_B8G8R8X8_UNORM:case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        for(unsigned x=0;x<w;x++,src+=4){dst[x]=((uint32_t)src[2]<<16)|((uint32_t)src[1]<<8)|src[0];}return 1;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:case DXGI_FORMAT_R10G10B10A2_UNORM:
        for(unsigned x=0;x<w;x++,src+=4){uint32_t v;memcpy(&v,src,4);dst[x]=(((v>>2)&255)<<16)|(((v>>12)&255)<<8)|((v>>22)&255);}return 1;
    default:
        if(!warned_format){warned_format=1;fprintf(stderr,"Direct3D: frame format %d is not supported\n",(int)format);}
        return 0;
    }
}

/* --- Direct3D 11 ----------------------------------------------------------------------------------------- */
static ID3D11Device *d11;static ID3D11DeviceContext *d11_ctx;static struct eh_d3d11_interface d11_iface;
/* Three staging textures in a ring: frames are copied as they finish and read back only once the graphics card is done
   with them. Waiting instead made the CPU and GPU take turns: 5-7 ms a frame on an integrated GPU. */
static ID3D11Texture2D *d11_staging[READ_SLOTS];static unsigned d11_w[READ_SLOTS],d11_h[READ_SLOTS];static DXGI_FORMAT d11_format[READ_SLOTS];
static int d11_pending[READ_SLOTS],d11_write,d11_read_slot;

static int d11_create(void){
    typedef HRESULT (WINAPI *Create)(IDXGIAdapter*,D3D_DRIVER_TYPE,HMODULE,UINT,const D3D_FEATURE_LEVEL*,UINT,UINT,ID3D11Device**,D3D_FEATURE_LEVEL*,ID3D11DeviceContext**);
    d3d_dll=LoadLibraryExW(L"d3d11.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!d3d_dll){fprintf(stderr,"Direct3D 11 is not installed\n");return 0;}
    FARPROC symbol=GetProcAddress(d3d_dll,"D3D11CreateDevice");if(!symbol)return 0;
    Create create;memcpy(&create,&symbol,sizeof(create));
    char name[256];IDXGIAdapter1 *adapter=default_adapter(name,sizeof(name));
    /* At most 11.0, as PCSX2 asks for: at 11.1 its renderer compiles shader model 5.1, which only Direct3D 12 runs,
       and Direct3D 11 then silently refuses every shader. */
    static const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level=0;D3D_DRIVER_TYPE type=adapter?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr=create((IDXGIAdapter*)adapter,type,NULL,0,levels,3,D3D11_SDK_VERSION,&d11,&level,&d11_ctx);
    if(adapter)IDXGIAdapter1_Release(adapter);
    if(FAILED(hr)){fprintf(stderr,"Direct3D 11 is not available on this graphics card (0x%08lx)\n",(unsigned long)hr);return 0;}
    pD3DCompile compile=shader_compiler();if(!compile){fprintf(stderr,"Direct3D 11: the shader compiler (d3dcompiler_47.dll) is missing\n");return 0;}
    d11_iface.interface_type=RETRO_HW_RENDER_INTERFACE_D3D11;d11_iface.interface_version=1;d11_iface.handle=&d11_iface;
    d11_iface.device=d11;d11_iface.context=d11_ctx;d11_iface.featureLevel=level;d11_iface.D3DCompile=compile;
    fprintf(stderr,"Direct3D 11: %s (feature level %x)\n",name[0]?name:"default adapter",(unsigned)level);
    return 1;
}
/* Maps one finished copy into out: 1 delivered, 0 not ready (only when not waiting), -1 failed. */
static int d11_take(int slot,int wait,uint32_t *out,unsigned *out_w,unsigned *out_h){
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr=ID3D11DeviceContext_Map(d11_ctx,(ID3D11Resource*)d11_staging[slot],0,D3D11_MAP_READ,wait?0:D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
    if(hr==DXGI_ERROR_WAS_STILL_DRAWING)return 0;
    d11_pending[slot]=0;d11_read_slot=(slot+1)%READ_SLOTS;
    if(FAILED(hr))return -1;
    int ok=1;for(unsigned y=0;y<d11_h[slot]&&ok;y++){ok=convert_row(d11_format[slot],(const uint8_t*)mapped.pData+(size_t)y*mapped.RowPitch,out+(size_t)y*d11_w[slot],d11_w[slot]);}
    ID3D11DeviceContext_Unmap(d11_ctx,(ID3D11Resource*)d11_staging[slot],0);
    if(!ok)return -1;
    *out_w=d11_w[slot];*out_h=d11_h[slot];return 1;
}
static int d11_read(unsigned width,unsigned height,uint32_t *out,unsigned *out_w,unsigned *out_h){
    int got=0;
    /* Version 1: the frame is the view the engine left at pixel-shader slot 0. */
    ID3D11ShaderResourceView *view=NULL;ID3D11DeviceContext_PSGetShaderResources(d11_ctx,0,1,&view);
    if(view){
        ID3D11Resource *resource=NULL;ID3D11Texture2D *texture=NULL;ID3D11ShaderResourceView_GetResource(view,&resource);
        if(resource&&SUCCEEDED(ID3D11Resource_QueryInterface(resource,&IID_ID3D11Texture2D,(void**)&texture))){
            D3D11_TEXTURE2D_DESC desc;ID3D11Texture2D_GetDesc(texture,&desc);
            unsigned w=width<desc.Width?width:desc.Width,h=height<desc.Height?height:desc.Height;int slot=d11_write;
            if(desc.SampleDesc.Count==1&&w&&h){
                /* All copies still in flight: wait for the oldest and show it, so no frame is ever dropped. This only
                   waits when the graphics card is two frames behind, and then it is the bottleneck anyway. */
                if(d11_pending[slot]){got=d11_take(slot,1,out,out_w,out_h);if(got<0)return -1;}
                if(!d11_staging[slot]||d11_w[slot]!=w||d11_h[slot]!=h||d11_format[slot]!=desc.Format){
                    if(d11_staging[slot]){ID3D11Texture2D_Release(d11_staging[slot]);}d11_staging[slot]=NULL;
                    D3D11_TEXTURE2D_DESC staging={0};staging.Width=w;staging.Height=h;staging.MipLevels=1;staging.ArraySize=1;
                    staging.Format=desc.Format;staging.SampleDesc.Count=1;staging.Usage=D3D11_USAGE_STAGING;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                    if(FAILED(ID3D11Device_CreateTexture2D(d11,&staging,NULL,&d11_staging[slot]))){d11_staging[slot]=NULL;}
                    d11_w[slot]=w;d11_h[slot]=h;d11_format[slot]=desc.Format;
                }
                if(d11_staging[slot]){
                    D3D11_BOX box={0,0,0,w,h,1};
                    ID3D11DeviceContext_CopySubresourceRegion(d11_ctx,(ID3D11Resource*)d11_staging[slot],0,0,0,0,resource,0,&box);
                    ID3D11DeviceContext_Flush(d11_ctx); /* Submit now: a buffered copy would never be ready when checked. */
                    d11_pending[slot]=1;d11_write=(slot+1)%READ_SLOTS;
                }
            }
            ID3D11Texture2D_Release(texture);
        }
        if(resource){ID3D11Resource_Release(resource);}
        ID3D11ShaderResourceView_Release(view);
    }
    /* Otherwise the oldest copy the graphics card has finished, one per frame for an even cadence; never wait for it. */
    if(!got&&d11_pending[d11_read_slot])got=d11_take(d11_read_slot,0,out,out_w,out_h);
    return got;
}
static void d11_destroy(void){
    for(int i=0;i<READ_SLOTS;i++){if(d11_staging[i])ID3D11Texture2D_Release(d11_staging[i]);d11_staging[i]=NULL;d11_pending[i]=0;}
    d11_write=d11_read_slot=0;
    if(d11_ctx){ID3D11DeviceContext_ClearState(d11_ctx);ID3D11DeviceContext_Release(d11_ctx);}d11_ctx=NULL;
    if(d11){ID3D11Device_Release(d11);}d11=NULL;
}

/* --- Direct3D 12 ----------------------------------------------------------------------------------------- */
static ID3D12Device *d12;static ID3D12CommandQueue *d12_queue;static struct eh_d3d12_interface d12_iface;
static ID3D12CommandAllocator *d12_alloc[READ_SLOTS];static ID3D12GraphicsCommandList *d12_list;
static ID3D12Fence *d12_fence;static HANDLE d12_event;static UINT64 d12_fence_value,d12_slot_fence[READ_SLOTS];
/* A ring of readback buffers, as for Direct3D 11. */
static ID3D12Resource *d12_buffer[READ_SLOTS];static UINT64 d12_buffer_size[READ_SLOTS];static unsigned d12_w[READ_SLOTS],d12_h[READ_SLOTS],d12_pitch[READ_SLOTS];
static DXGI_FORMAT d12_format[READ_SLOTS];static int d12_pending[READ_SLOTS],d12_write,d12_read_slot;
/* Set by the engine just before video_refresh; valid until it returns. */
static ID3D12Resource *d12_frame;static DXGI_FORMAT d12_frame_format;

static void d12_set_texture(void *handle,ID3D12Resource *texture,DXGI_FORMAT format){(void)handle;d12_frame=texture;d12_frame_format=format;}
static int d12_wait(UINT64 value){
    if(!value||ID3D12Fence_GetCompletedValue(d12_fence)>=value)return 1;
    if(FAILED(ID3D12Fence_SetEventOnCompletion(d12_fence,value,d12_event)))return 0;
    return WaitForSingleObject(d12_event,5000)==WAIT_OBJECT_0;
}
static int d12_create(void){
    typedef HRESULT (WINAPI *Create)(IUnknown*,D3D_FEATURE_LEVEL,REFIID,void**);
    d3d_dll=LoadLibraryExW(L"d3d12.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!d3d_dll){fprintf(stderr,"Direct3D 12 needs Windows 10 or later\n");return 0;}
    FARPROC symbol=GetProcAddress(d3d_dll,"D3D12CreateDevice");if(!symbol)return 0;
    Create create;memcpy(&create,&symbol,sizeof(create));
    char name[256];IDXGIAdapter1 *adapter=default_adapter(name,sizeof(name));
    HRESULT hr=create((IUnknown*)adapter,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&d12);
    if(adapter)IDXGIAdapter1_Release(adapter);
    if(FAILED(hr)){fprintf(stderr,"Direct3D 12 is not available on this graphics card (0x%08lx)\n",(unsigned long)hr);return 0;}
    D3D12_COMMAND_QUEUE_DESC queue={0};queue.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    if(FAILED(ID3D12Device_CreateCommandQueue(d12,&queue,&IID_ID3D12CommandQueue,(void**)&d12_queue)))return 0;
    for(int i=0;i<READ_SLOTS;i++)if(FAILED(ID3D12Device_CreateCommandAllocator(d12,D3D12_COMMAND_LIST_TYPE_DIRECT,&IID_ID3D12CommandAllocator,(void**)&d12_alloc[i])))return 0;
    if(FAILED(ID3D12Device_CreateCommandList(d12,0,D3D12_COMMAND_LIST_TYPE_DIRECT,d12_alloc[0],NULL,&IID_ID3D12GraphicsCommandList,(void**)&d12_list)))return 0;
    ID3D12GraphicsCommandList_Close(d12_list);
    if(FAILED(ID3D12Device_CreateFence(d12,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void**)&d12_fence)))return 0;
    d12_event=CreateEventW(NULL,FALSE,FALSE,NULL);if(!d12_event)return 0;
    pD3DCompile compile=shader_compiler();if(!compile){fprintf(stderr,"Direct3D 12: the shader compiler (d3dcompiler_47.dll) is missing\n");return 0;}
    d12_iface.interface_type=RETRO_HW_RENDER_INTERFACE_D3D12;d12_iface.interface_version=1;d12_iface.handle=&d12_iface;
    d12_iface.device=d12;d12_iface.queue=d12_queue;d12_iface.D3DCompile=compile;
    /* The engine leaves each frame ready to copy, so no state changes are needed here. */
    d12_iface.required_state=D3D12_RESOURCE_STATE_COPY_SOURCE;d12_iface.set_texture=d12_set_texture;
    fprintf(stderr,"Direct3D 12: %s\n",name[0]?name:"default adapter");
    return 1;
}
static int d12_take(int slot,uint32_t *out,unsigned *out_w,unsigned *out_h){
    if(!d12_wait(d12_slot_fence[slot]))return -1;
    d12_pending[slot]=0;d12_read_slot=(slot+1)%READ_SLOTS;
    D3D12_RANGE range={0,(SIZE_T)d12_pitch[slot]*d12_h[slot]};void *data=NULL;
    if(FAILED(ID3D12Resource_Map(d12_buffer[slot],0,&range,&data)))return -1;
    int ok=1;for(unsigned y=0;y<d12_h[slot]&&ok;y++){ok=convert_row(d12_format[slot],(const uint8_t*)data+(size_t)y*d12_pitch[slot],out+(size_t)y*d12_w[slot],d12_w[slot]);}
    D3D12_RANGE none={0,0};ID3D12Resource_Unmap(d12_buffer[slot],0,&none);
    if(!ok)return -1;
    *out_w=d12_w[slot];*out_h=d12_h[slot];return 1;
}
static int d12_read(unsigned width,unsigned height,uint32_t *out,unsigned *out_w,unsigned *out_h){
    int got=0;
    ID3D12Resource *frame=d12_frame;d12_frame=NULL;
    if(frame&&width&&height){
        int slot=d12_write;
        /* The buffer and allocator this slot used READ_SLOTS frames ago must be free; it almost always is. An unread
           copy there is shown now, never dropped. */
        if(!d12_wait(d12_slot_fence[slot]))return -1;
        if(d12_pending[slot]){got=d12_take(slot,out,out_w,out_h);if(got<0)return -1;}
        unsigned pitch=(width*4+D3D12_TEXTURE_DATA_PITCH_ALIGNMENT-1)&~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT-1);
        UINT64 size=(UINT64)pitch*height;
        if(!d12_buffer[slot]||d12_buffer_size[slot]<size){
            if(d12_buffer[slot]){ID3D12Resource_Release(d12_buffer[slot]);}d12_buffer[slot]=NULL;d12_buffer_size[slot]=0;
            D3D12_HEAP_PROPERTIES heap={0};heap.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC desc={0};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=size;desc.Height=1;desc.DepthOrArraySize=1;
            desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if(FAILED(ID3D12Device_CreateCommittedResource(d12,&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&d12_buffer[slot])))return -1;
            d12_buffer_size[slot]=size;
        }
        if(FAILED(ID3D12CommandAllocator_Reset(d12_alloc[slot]))||FAILED(ID3D12GraphicsCommandList_Reset(d12_list,d12_alloc[slot],NULL)))return -1;
        D3D12_TEXTURE_COPY_LOCATION dst={0},src={0};
        dst.pResource=d12_buffer[slot];dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format=d12_frame_format;dst.PlacedFootprint.Footprint.Width=width;
        dst.PlacedFootprint.Footprint.Height=height;dst.PlacedFootprint.Footprint.Depth=1;dst.PlacedFootprint.Footprint.RowPitch=pitch;
        src.pResource=frame;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=0;
        D3D12_BOX box={0,0,0,width,height,1};
        ID3D12GraphicsCommandList_CopyTextureRegion(d12_list,&dst,0,0,0,&src,&box);
        if(FAILED(ID3D12GraphicsCommandList_Close(d12_list)))return -1;
        ID3D12CommandList *lists[]={(ID3D12CommandList*)d12_list};
        /* Same queue as the engine's: the copy runs after the frame is drawn and before anything drawn next. */
        ID3D12CommandQueue_ExecuteCommandLists(d12_queue,1,lists);
        if(FAILED(ID3D12CommandQueue_Signal(d12_queue,d12_fence,++d12_fence_value)))return -1;
        d12_slot_fence[slot]=d12_fence_value;d12_w[slot]=width;d12_h[slot]=height;d12_pitch[slot]=pitch;d12_format[slot]=d12_frame_format;
        d12_pending[slot]=1;d12_write=(slot+1)%READ_SLOTS;
    }
    /* Otherwise the oldest finished copy, one per frame, as for Direct3D 11; never wait for it. */
    if(!got&&d12_pending[d12_read_slot]&&ID3D12Fence_GetCompletedValue(d12_fence)>=d12_slot_fence[d12_read_slot])got=d12_take(d12_read_slot,out,out_w,out_h);
    return got;
}
static void d12_destroy(void){
    if(d12_fence&&d12_event)d12_wait(d12_fence_value);
    d12_write=d12_read_slot=0;
    for(int i=0;i<READ_SLOTS;i++){
        if(d12_buffer[i]){ID3D12Resource_Release(d12_buffer[i]);}d12_buffer[i]=NULL;d12_buffer_size[i]=0;d12_pending[i]=0;d12_slot_fence[i]=0;
        if(d12_alloc[i]){ID3D12CommandAllocator_Release(d12_alloc[i]);}d12_alloc[i]=NULL;
    }
    if(d12_list){ID3D12GraphicsCommandList_Release(d12_list);}d12_list=NULL;
    if(d12_fence){ID3D12Fence_Release(d12_fence);}d12_fence=NULL;d12_fence_value=0;
    if(d12_event){CloseHandle(d12_event);}d12_event=NULL;
    if(d12_queue){ID3D12CommandQueue_Release(d12_queue);}d12_queue=NULL;
    if(d12){ID3D12Device_Release(d12);}d12=NULL;d12_frame=NULL;
}

/* --- Direct3D 11 presentation ---------------------------------------------------------------------------- */
/* The frame never leaves the graphics card: it is copied into a texture of ours (for pauses, saves and screenshots)
   and drawn, scaled, into a swap chain on the game window. Copying every frame back to the CPU instead cost an
   integrated GPU 5-7 ms a frame and made the CPU and GPU take turns. Drawing happens in a device context state of our
   own, swapped in and out, so the engine's cached bindings are untouched. */
static HWND p_window;static IDXGISwapChain1 *p_chain;static ID3D11RenderTargetView *p_rtv;static UINT p_w,p_h;
static ID3D11Device1 *p_dev1;static ID3D11DeviceContext1 *p_ctx1;static ID3DDeviceContextState *p_state;
static ID3D11VertexShader *p_vs;static ID3D11PixelShader *p_ps,*p_ps_overlay;
static ID3D11SamplerState *p_point,*p_linear;static ID3D11BlendState *p_blend;
static ID3D11Texture2D *p_last;static ID3D11ShaderResourceView *p_last_srv;static unsigned p_last_w,p_last_h;static DXGI_FORMAT p_last_format;
static ID3D11Texture2D *p_overlay;static ID3D11ShaderResourceView *p_overlay_srv;static unsigned p_overlay_w,p_overlay_h;static int p_overlay_valid;
static const char present_hlsl[]=
    "Texture2D t:register(t0);SamplerState s:register(s0);"
    "struct V{float4 p:SV_Position;float2 u:TEXCOORD0;};"
    "V vs(uint i:SV_VertexID){V o;float2 q=float2((i<<1)&2,i&2);o.p=float4(q*float2(2,-2)+float2(-1,1),0,1);o.u=q;return o;}"
    "float4 ps(V i):SV_Target{return float4(t.Sample(s,i.u).rgb,1);}"
    "float4 ps_overlay(V i):SV_Target{return t.Sample(s,i.u);}";

static ID3DBlob *compile_shader(const char *entry,const char *target){
    pD3DCompile compile=shader_compiler();ID3DBlob *code=NULL,*errors=NULL;
    if(!compile||FAILED(compile(present_hlsl,sizeof(present_hlsl)-1,"present",NULL,NULL,entry,target,0,0,&code,&errors))){
        if(errors){fprintf(stderr,"Direct3D 11 presentation shader: %.*s\n",(int)ID3D10Blob_GetBufferSize(errors),(const char*)ID3D10Blob_GetBufferPointer(errors));ID3D10Blob_Release(errors);}
        return NULL;
    }
    if(errors){ID3D10Blob_Release(errors);}
    return code;
}
static DXGI_FORMAT typed(DXGI_FORMAT format){
    switch(format){
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:return format;
    }
}
void eh_d3d_present_detach(void){
    if(p_overlay_srv){ID3D11ShaderResourceView_Release(p_overlay_srv);}p_overlay_srv=NULL;
    if(p_overlay){ID3D11Texture2D_Release(p_overlay);}p_overlay=NULL;p_overlay_w=p_overlay_h=0;p_overlay_valid=0;
    if(p_last_srv){ID3D11ShaderResourceView_Release(p_last_srv);}p_last_srv=NULL;
    if(p_last){ID3D11Texture2D_Release(p_last);}p_last=NULL;p_last_w=p_last_h=0;
    if(p_rtv){ID3D11RenderTargetView_Release(p_rtv);}p_rtv=NULL;
    if(p_chain){IDXGISwapChain1_Release(p_chain);}p_chain=NULL;p_w=p_h=0;
    if(p_state){ID3DDeviceContextState_Release(p_state);}p_state=NULL;
    if(p_blend){ID3D11BlendState_Release(p_blend);}p_blend=NULL;
    if(p_point){ID3D11SamplerState_Release(p_point);}p_point=NULL;
    if(p_linear){ID3D11SamplerState_Release(p_linear);}p_linear=NULL;
    if(p_ps_overlay){ID3D11PixelShader_Release(p_ps_overlay);}p_ps_overlay=NULL;
    if(p_ps){ID3D11PixelShader_Release(p_ps);}p_ps=NULL;
    if(p_vs){ID3D11VertexShader_Release(p_vs);}p_vs=NULL;
    if(p_ctx1){ID3D11DeviceContext1_Release(p_ctx1);}p_ctx1=NULL;
    if(p_dev1){ID3D11Device1_Release(p_dev1);}p_dev1=NULL;
    p_window=NULL;
}
int eh_d3d_present_attach(void *hwnd){
    if(api!=RETRO_HW_CONTEXT_D3D11||!hwnd||p_chain)return p_chain!=NULL;
    if(FAILED(ID3D11Device_QueryInterface(d11,&IID_ID3D11Device1,(void**)&p_dev1))||
       FAILED(ID3D11DeviceContext_QueryInterface(d11_ctx,&IID_ID3D11DeviceContext1,(void**)&p_ctx1))){eh_d3d_present_detach();return 0;}
    IDXGIDevice1 *device=NULL;IDXGIAdapter *adapter=NULL;IDXGIFactory2 *factory=NULL;
    if(SUCCEEDED(ID3D11Device_QueryInterface(d11,&IID_IDXGIDevice1,(void**)&device))){
        /* Up to three frames queued (the default): with one, even Present(0) waits for the previous frame to finish, and
           the CPU and GPU take turns again (6 ms a frame on an integrated GPU). */
        IDXGIDevice1_SetMaximumFrameLatency(device,3);
        if(SUCCEEDED(IDXGIDevice1_GetAdapter(device,&adapter))){IDXGIAdapter_GetParent(adapter,&IID_IDXGIFactory2,(void**)&factory);IDXGIAdapter_Release(adapter);}
        IDXGIDevice1_Release(device);
    }
    if(!factory){eh_d3d_present_detach();return 0;}
    DXGI_SWAP_CHAIN_DESC1 desc;memset(&desc,0,sizeof(desc));
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;desc.Scaling=DXGI_SCALING_NONE;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    HRESULT hr=IDXGIFactory2_CreateSwapChainForHwnd(factory,(IUnknown*)d11,(HWND)hwnd,&desc,NULL,NULL,&p_chain);
    if(FAILED(hr)){desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;hr=IDXGIFactory2_CreateSwapChainForHwnd(factory,(IUnknown*)d11,(HWND)hwnd,&desc,NULL,NULL,&p_chain);}
    if(SUCCEEDED(hr)){IDXGIFactory2_MakeWindowAssociation(factory,(HWND)hwnd,DXGI_MWA_NO_ALT_ENTER|DXGI_MWA_NO_WINDOW_CHANGES);}
    IDXGIFactory2_Release(factory);
    if(FAILED(hr)){fprintf(stderr,"Direct3D 11: no swap chain for the game window (0x%08lx); frames are copied instead\n",(unsigned long)hr);eh_d3d_present_detach();return 0;}
    ID3DBlob *vs=compile_shader("vs","vs_4_0"),*ps=compile_shader("ps","ps_4_0"),*po=compile_shader("ps_overlay","ps_4_0");
    int ok=vs&&ps&&po
        &&SUCCEEDED(ID3D11Device_CreateVertexShader(d11,ID3D10Blob_GetBufferPointer(vs),ID3D10Blob_GetBufferSize(vs),NULL,&p_vs))
        &&SUCCEEDED(ID3D11Device_CreatePixelShader(d11,ID3D10Blob_GetBufferPointer(ps),ID3D10Blob_GetBufferSize(ps),NULL,&p_ps))
        &&SUCCEEDED(ID3D11Device_CreatePixelShader(d11,ID3D10Blob_GetBufferPointer(po),ID3D10Blob_GetBufferSize(po),NULL,&p_ps_overlay));
    if(vs){ID3D10Blob_Release(vs);}if(ps){ID3D10Blob_Release(ps);}if(po){ID3D10Blob_Release(po);}
    D3D11_SAMPLER_DESC sampler;memset(&sampler,0,sizeof(sampler));
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;sampler.ComparisonFunc=D3D11_COMPARISON_NEVER;
    sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;ok=ok&&SUCCEEDED(ID3D11Device_CreateSamplerState(d11,&sampler,&p_point));
    sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;ok=ok&&SUCCEEDED(ID3D11Device_CreateSamplerState(d11,&sampler,&p_linear));
    D3D11_BLEND_DESC blend;memset(&blend,0,sizeof(blend));
    blend.RenderTarget[0].BlendEnable=TRUE;blend.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA;blend.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;blend.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;blend.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    ok=ok&&SUCCEEDED(ID3D11Device_CreateBlendState(d11,&blend,&p_blend));
    D3D_FEATURE_LEVEL level=d11_iface.featureLevel;
    ok=ok&&SUCCEEDED(ID3D11Device1_CreateDeviceContextState(p_dev1,0,&level,1,D3D11_SDK_VERSION,&IID_ID3D11Device,NULL,&p_state));
    if(!ok){fprintf(stderr,"Direct3D 11: presentation could not be prepared; frames are copied instead\n");eh_d3d_present_detach();return 0;}
    p_window=(HWND)hwnd;
    fprintf(stderr,"Direct3D 11: frames are shown on the graphics card\n");
    return 1;
}
int eh_d3d_present(unsigned frame_w,unsigned frame_h,float aspect,int sync,int smooth,int integer_scale,
                   const uint32_t *overlay,unsigned overlay_w,unsigned overlay_h,int overlay_changed,
                   int *view_x,int *view_y,int *view_w,int *view_h){
    if(!p_chain)return 0;
    /* Version 1: the frame is the view the engine left at pixel-shader slot 0. Keep a copy of the visible part. */
    ID3D11ShaderResourceView *view=NULL;ID3D11DeviceContext_PSGetShaderResources(d11_ctx,0,1,&view);
    if(view){
        ID3D11Resource *resource=NULL;ID3D11Texture2D *texture=NULL;ID3D11ShaderResourceView_GetResource(view,&resource);
        if(resource&&SUCCEEDED(ID3D11Resource_QueryInterface(resource,&IID_ID3D11Texture2D,(void**)&texture))){
            D3D11_TEXTURE2D_DESC desc;ID3D11Texture2D_GetDesc(texture,&desc);
            unsigned w=frame_w<desc.Width?frame_w:desc.Width,h=frame_h<desc.Height?frame_h:desc.Height;
            if(desc.SampleDesc.Count==1&&w&&h){
                if(!p_last||p_last_w!=w||p_last_h!=h||p_last_format!=desc.Format){
                    if(p_last_srv){ID3D11ShaderResourceView_Release(p_last_srv);}p_last_srv=NULL;
                    if(p_last){ID3D11Texture2D_Release(p_last);}p_last=NULL;
                    D3D11_TEXTURE2D_DESC copy;memset(&copy,0,sizeof(copy));copy.Width=w;copy.Height=h;copy.MipLevels=1;copy.ArraySize=1;
                    copy.Format=desc.Format;copy.SampleDesc.Count=1;copy.Usage=D3D11_USAGE_DEFAULT;copy.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                    D3D11_SHADER_RESOURCE_VIEW_DESC srv;memset(&srv,0,sizeof(srv));srv.Format=typed(desc.Format);
                    srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
                    if(FAILED(ID3D11Device_CreateTexture2D(d11,&copy,NULL,&p_last))||
                       FAILED(ID3D11Device_CreateShaderResourceView(d11,(ID3D11Resource*)p_last,&srv,&p_last_srv))){
                        if(p_last){ID3D11Texture2D_Release(p_last);}p_last=NULL;p_last_srv=NULL;
                    }
                    p_last_w=w;p_last_h=h;p_last_format=desc.Format;
                }
                if(p_last){D3D11_BOX box={0,0,0,w,h,1};ID3D11DeviceContext_CopySubresourceRegion(d11_ctx,(ID3D11Resource*)p_last,0,0,0,0,resource,0,&box);}
            }
            ID3D11Texture2D_Release(texture);
        }
        if(resource){ID3D11Resource_Release(resource);}
        ID3D11ShaderResourceView_Release(view);
    }
    if(!p_last_srv)return 1;
    RECT client;GetClientRect(p_window,&client);UINT cw=(UINT)client.right,ch=(UINT)client.bottom;
    if(!cw||!ch)return 1;
    if(cw!=p_w||ch!=p_h||!p_rtv){
        /* The inactive presentation context state retains its render target.
           Release that binding before ResizeBuffers, as well as our COM view. */
        ID3DDeviceContextState *saved=NULL;
        ID3D11DeviceContext1_SwapDeviceContextState(p_ctx1,p_state,&saved);
        ID3D11DeviceContext_OMSetRenderTargets(d11_ctx,0,NULL,NULL);
        ID3D11DeviceContext_Flush(d11_ctx);
        ID3D11DeviceContext1_SwapDeviceContextState(p_ctx1,saved,NULL);
        if(saved)ID3DDeviceContextState_Release(saved);
        if(p_rtv){ID3D11RenderTargetView_Release(p_rtv);}p_rtv=NULL;
        HRESULT resize=IDXGISwapChain1_ResizeBuffers(p_chain,0,cw,ch,DXGI_FORMAT_UNKNOWN,0);
        if(FAILED(resize)){fprintf(stderr,"Direct3D 11: resize to %ux%u failed (0x%08lx)\n",cw,ch,(unsigned long)resize);return -1;}
        ID3D11Texture2D *back=NULL;
        if(FAILED(IDXGISwapChain1_GetBuffer(p_chain,0,&IID_ID3D11Texture2D,(void**)&back)))return -1;
        HRESULT hr=ID3D11Device_CreateRenderTargetView(d11,(ID3D11Resource*)back,NULL,&p_rtv);ID3D11Texture2D_Release(back);
        if(FAILED(hr))return -1;
        p_w=cw;p_h=ch;
    }
    /* Letterboxed exactly like the CPU path (present_frame in host.c). */
    int dw,dh;eh_picture_fit(cw,ch,p_last_w,p_last_h,aspect,integer_scale,&dw,&dh);
    int x=((int)cw-dw)/2,y=((int)ch-dh)/2;*view_x=x;*view_y=y;*view_w=dw;*view_h=dh;
    ID3DDeviceContextState *previous=NULL;ID3D11DeviceContext1_SwapDeviceContextState(p_ctx1,p_state,&previous);
    ID3D11DeviceContext *ctx=d11_ctx;static const float black[4]={0,0,0,1};
    ID3D11DeviceContext_OMSetRenderTargets(ctx,1,&p_rtv,NULL);ID3D11DeviceContext_ClearRenderTargetView(ctx,p_rtv,black);
    ID3D11DeviceContext_IASetInputLayout(ctx,NULL);ID3D11DeviceContext_IASetPrimitiveTopology(ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx,p_vs,NULL,0);ID3D11DeviceContext_PSSetShader(ctx,p_ps,NULL,0);
    ID3D11DeviceContext_PSSetSamplers(ctx,0,1,smooth?&p_linear:&p_point);ID3D11DeviceContext_PSSetShaderResources(ctx,0,1,&p_last_srv);
    D3D11_VIEWPORT viewport={(FLOAT)x,(FLOAT)y,(FLOAT)dw,(FLOAT)dh,0.0f,1.0f};ID3D11DeviceContext_RSSetViewports(ctx,1,&viewport);
    ID3D11DeviceContext_OMSetBlendState(ctx,NULL,NULL,0xffffffff);ID3D11DeviceContext_Draw(ctx,3,0);
    if(!overlay)p_overlay_valid=0;
    else if(overlay_changed||!p_overlay_valid){
        if(!p_overlay||p_overlay_w!=overlay_w||p_overlay_h!=overlay_h){
            if(p_overlay_srv){ID3D11ShaderResourceView_Release(p_overlay_srv);}p_overlay_srv=NULL;
            if(p_overlay){ID3D11Texture2D_Release(p_overlay);}p_overlay=NULL;
            D3D11_TEXTURE2D_DESC desc;memset(&desc,0,sizeof(desc));desc.Width=overlay_w;desc.Height=overlay_h;desc.MipLevels=1;desc.ArraySize=1;
            desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DYNAMIC;
            desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            if(FAILED(ID3D11Device_CreateTexture2D(d11,&desc,NULL,&p_overlay))||
               FAILED(ID3D11Device_CreateShaderResourceView(d11,(ID3D11Resource*)p_overlay,NULL,&p_overlay_srv))){
                if(p_overlay){ID3D11Texture2D_Release(p_overlay);}p_overlay=NULL;p_overlay_srv=NULL;
            }
            p_overlay_w=overlay_w;p_overlay_h=overlay_h;
        }
        D3D11_MAPPED_SUBRESOURCE mapped;p_overlay_valid=0;
        if(p_overlay&&SUCCEEDED(ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)p_overlay,0,D3D11_MAP_WRITE_DISCARD,0,&mapped))){
            for(unsigned row=0;row<overlay_h;row++){memcpy((uint8_t*)mapped.pData+(size_t)row*mapped.RowPitch,overlay+(size_t)row*overlay_w,(size_t)overlay_w*4);}
            ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)p_overlay,0);p_overlay_valid=1;
        }
    }
    if(p_overlay_valid&&p_overlay_w==(unsigned)dw&&p_overlay_h==(unsigned)dh){
        ID3D11DeviceContext_PSSetShader(ctx,p_ps_overlay,NULL,0);ID3D11DeviceContext_PSSetSamplers(ctx,0,1,&p_point);
        ID3D11DeviceContext_PSSetShaderResources(ctx,0,1,&p_overlay_srv);ID3D11DeviceContext_OMSetBlendState(ctx,p_blend,NULL,0xffffffff);
        ID3D11DeviceContext_Draw(ctx,3,0);
    }
    ID3D11ShaderResourceView *none=NULL;ID3D11DeviceContext_PSSetShaderResources(ctx,0,1,&none);
    ID3D11DeviceContext1_SwapDeviceContextState(p_ctx1,previous,NULL);
    if(previous){ID3DDeviceContextState_Release(previous);}
    HRESULT hr=IDXGISwapChain1_Present(p_chain,sync?1:0,0);
    return SUCCEEDED(hr)||hr==DXGI_STATUS_OCCLUDED?1:-1;
}
int eh_d3d_capture(uint32_t *out,unsigned *out_w,unsigned *out_h){
    if(!p_last)return 0;
    D3D11_TEXTURE2D_DESC desc;memset(&desc,0,sizeof(desc));desc.Width=p_last_w;desc.Height=p_last_h;desc.MipLevels=1;desc.ArraySize=1;
    desc.Format=p_last_format;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging=NULL;if(FAILED(ID3D11Device_CreateTexture2D(d11,&desc,NULL,&staging)))return 0;
    ID3D11DeviceContext_CopyResource(d11_ctx,(ID3D11Resource*)staging,(ID3D11Resource*)p_last);
    D3D11_MAPPED_SUBRESOURCE mapped;int ok=0;
    if(SUCCEEDED(ID3D11DeviceContext_Map(d11_ctx,(ID3D11Resource*)staging,0,D3D11_MAP_READ,0,&mapped))){
        ok=1;for(unsigned y=0;y<p_last_h&&ok;y++){ok=convert_row(p_last_format,(const uint8_t*)mapped.pData+(size_t)y*mapped.RowPitch,out+(size_t)y*p_last_w,p_last_w);}
        ID3D11DeviceContext_Unmap(d11_ctx,(ID3D11Resource*)staging,0);
    }
    ID3D11Texture2D_Release(staging);
    if(ok){*out_w=p_last_w;*out_h=p_last_h;}
    return ok;
}

/* --- entry points ---------------------------------------------------------------------------------------- */
int eh_d3d_create(unsigned context_type){
    if(api)return api==context_type;
    int ok=context_type==RETRO_HW_CONTEXT_D3D11?d11_create():context_type==RETRO_HW_CONTEXT_D3D12?d12_create():0;
    if(ok)api=context_type;else{d11_destroy();d12_destroy();}
    return ok;
}
const void *eh_d3d_interface(void){
    return api==RETRO_HW_CONTEXT_D3D11?(const void*)&d11_iface:api==RETRO_HW_CONTEXT_D3D12?(const void*)&d12_iface:NULL;
}
int eh_d3d_read(unsigned width,unsigned height,uint32_t *out,unsigned *out_w,unsigned *out_h){
    return api==RETRO_HW_CONTEXT_D3D11?d11_read(width,height,out,out_w,out_h):api==RETRO_HW_CONTEXT_D3D12?d12_read(width,height,out,out_w,out_h):-1;
}
void eh_d3d_destroy(void){
    eh_d3d_present_detach();
    d11_destroy();d12_destroy();api=0;
    /* The libraries stay loaded: engines may keep function pointers into the compiler until they are unloaded. */
}
#else
int eh_d3d_create(unsigned context_type){(void)context_type;return 0;}
const void *eh_d3d_interface(void){return 0;}
int eh_d3d_read(unsigned width,unsigned height,uint32_t *out,unsigned *out_w,unsigned *out_h){(void)width;(void)height;(void)out;(void)out_w;(void)out_h;return -1;}
void eh_d3d_destroy(void){}
int eh_d3d_present_attach(void *hwnd){(void)hwnd;return 0;}
int eh_d3d_present(unsigned frame_w,unsigned frame_h,float aspect,int sync,int smooth,int integer_scale,const uint32_t *overlay,unsigned overlay_w,unsigned overlay_h,int overlay_changed,int *view_x,int *view_y,int *view_w,int *view_h){(void)frame_w;(void)frame_h;(void)aspect;(void)sync;(void)smooth;(void)integer_scale;(void)overlay;(void)overlay_w;(void)overlay_h;(void)overlay_changed;(void)view_x;(void)view_y;(void)view_w;(void)view_h;return 0;}
int eh_d3d_capture(uint32_t *out,unsigned *out_w,unsigned *out_h){(void)out;(void)out_w;(void)out_h;return 0;}
void eh_d3d_present_detach(void){}
#endif
