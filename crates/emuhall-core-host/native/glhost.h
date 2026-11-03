/* Hardware (OpenGL) rendering for engines (glhost.c). */
#ifndef EMUHALL_GLHOST_H
#define EMUHALL_GLHOST_H
#include <stdint.h>
#include "libretro.h"

int eh_gl_create(int core_profile,unsigned major,unsigned minor); /* 1 when a context of at least that version exists. */
int eh_gl_framebuffer(unsigned width,unsigned height,int with_depth,int with_stencil);
uintptr_t eh_gl_current_framebuffer(void);
retro_proc_address_t eh_gl_proc(const char *name);
/* Starts reading the frame just drawn and delivers the previous one into out (1), nothing yet (0) or fails (-1). */
int eh_gl_read(unsigned width,unsigned height,int bottom_left,uint32_t *out,unsigned *out_w,unsigned *out_h);
void eh_gl_destroy(void);
#endif
