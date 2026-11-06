/* Hardware (Vulkan) rendering for engines (vkhost.c): Windows, Linux and Android share it; only the window surface is
   platform-specific. */
#ifndef EMUHALL_VKHOST_H
#define EMUHALL_VKHOST_H
#include <stdint.h>

/* Environment calls the engine makes before and while its device is created. */
void eh_vk_set_negotiation(const void *negotiation);
int eh_vk_create(void);              /* Instance and device; 1 on success. Called after the game is loaded. */
const void *eh_vk_interface(void);   /* For RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE. */
/* Copies the frame just handed over into out one frame later, never waiting (headless): 1, 0 (nothing yet) or -1. */
int eh_vk_read(unsigned width,unsigned height,uint32_t *out,unsigned *out_w,unsigned *out_h);
/* Shows frames in a swap chain on the game window instead (window: HWND on Windows). */
int eh_vk_present_attach(void *window);
/* overlay (may be null): opaque or fully transparent BGRA pixels the size of the shown picture, drawn over it. */
int eh_vk_present(unsigned frame_w,unsigned frame_h,int smooth,int integer_scale,const uint32_t *overlay,unsigned overlay_w,unsigned overlay_h,int overlay_changed,int *view_x,int *view_y,int *view_w,int *view_h);
int eh_vk_capture(uint32_t *out,unsigned *out_w,unsigned *out_h);
int eh_vk_lost(void);                /* 1 once the driver lost the device or stopped finishing work */
void eh_vk_destroy(void);
/* Graphics cards (not CPU drivers) offering Vulkan 1.1, from a throwaway instance (all devices are listed on stderr);
   -1 without a Vulkan loader. */
int eh_vk_count_devices(void);
#endif
