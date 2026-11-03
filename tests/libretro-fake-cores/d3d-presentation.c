/* Copyright 2026 Braza IO Capital Holding. SPDX-License-Identifier: MIT */
/* Original solid-colour fixture: no game, firmware, or emulator source. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <math.h>
#include "libretro.h"
#include "../../crates/emuhall-core-host/native/d3dhost.h"
#include "../../crates/emuhall-core-host/native/picture_layout.h"
struct interface11 { unsigned type,version;void *handle;ID3D11Device *device;ID3D11DeviceContext *context;D3D_FEATURE_LEVEL level;pD3DCompile compile; };
int main(void){
    int w,h;eh_picture_fit(1920,1080,512,448,16.0f/9.0f,0,&w,&h);
    if(w!=1920||h!=1080)return 1;
    eh_picture_fit(1920,1080,512,448,4.0f/3.0f,0,&w,&h);
    if(w!=1440||h!=1080)return 2;
    eh_picture_fit(800,600,160,144,NAN,0,&w,&h);
    if(w!=667||h!=600)return 3;
    HWND window=CreateWindowExW(0,L"STATIC",L"EmuHall synthetic presentation",WS_OVERLAPPEDWINDOW,0,0,640,480,NULL,NULL,GetModuleHandleW(NULL),NULL);
    if(!window||!eh_d3d_create(RETRO_HW_CONTEXT_D3D11)||!eh_d3d_present_attach(window))return 4;
    const struct interface11 *iface=eh_d3d_interface();
    uint32_t pixels[64*64];for(unsigned i=0;i<64*64;i++)pixels[i]=0xff0000ff;
    D3D11_TEXTURE2D_DESC desc={0};desc.Width=desc.Height=64;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data={pixels,64*4,0};ID3D11Texture2D *texture=NULL;ID3D11ShaderResourceView *view=NULL;
    if(FAILED(ID3D11Device_CreateTexture2D(iface->device,&desc,&data,&texture))||FAILED(ID3D11Device_CreateShaderResourceView(iface->device,(ID3D11Resource*)texture,NULL,&view)))return 5;
    for(unsigned i=0;i<12;i++){
        SetWindowPos(window,NULL,0,0,i%2?1920:640,i%2?1080:480,SWP_NOZORDER|SWP_NOACTIVATE);
        ID3D11DeviceContext_PSSetShaderResources(iface->context,0,1,&view);
        int x,y,vw,vh;if(eh_d3d_present(64,64,16.0f/9.0f,0,0,0,NULL,0,0,0,&x,&y,&vw,&vh)<1)return 6;
        if(vw<1||vh<1||fabs((double)vw/vh-16.0/9.0)>0.01)return 7;
    }
    ID3D11ShaderResourceView_Release(view);ID3D11Texture2D_Release(texture);eh_d3d_destroy();DestroyWindow(window);
    puts("PASS: display aspect and twelve Direct3D screen-buffer resizes");return 0;
}
