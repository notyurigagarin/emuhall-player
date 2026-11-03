/* Hardware (Direct3D 11/12) rendering for engines (d3dhost.c). Windows only; elsewhere every call reports failure. */
#ifndef EMUHALL_D3DHOST_H
#define EMUHALL_D3DHOST_H
#include <stdint.h>

int eh_d3d_create(unsigned context_type); /* RETRO_HW_CONTEXT_D3D11 or _D3D12: 1 when the device exists. */
const void *eh_d3d_interface(void);        /* For RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE. */
/* Starts copying the frame just drawn and delivers the previous one into out (1), nothing yet (0) or fails (-1). */
int eh_d3d_read(unsigned width,unsigned height,uint32_t *out,unsigned *out_w,unsigned *out_h);
void eh_d3d_destroy(void);

/* Direct3D 11 only: show frames on the graphics card in the game window, with no copy back to the CPU. */
int eh_d3d_present_attach(void *hwnd); /* 1 when frames can be shown this way. */
/* Shows the frame the engine just drew, letterboxed like the CPU path, with an optional overlay (BGRA with alpha,
   the size of the picture area) drawn on top. Returns the picture area for mouse mapping. */
int eh_d3d_present(unsigned frame_w,unsigned frame_h,float aspect,int sync,int smooth,int integer_scale,
                   const uint32_t *overlay,unsigned overlay_w,unsigned overlay_h,int overlay_changed,
                   int *view_x,int *view_y,int *view_w,int *view_h);
/* Copies the last shown frame into out (0x00RRGGBB), waiting for the graphics card: for pauses, saves, screenshots. */
int eh_d3d_capture(uint32_t *out,unsigned *out_w,unsigned *out_h);
void eh_d3d_present_detach(void);
#endif
