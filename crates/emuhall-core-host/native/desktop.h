#ifndef EMUHALL_DESKTOP_H
#define EMUHALL_DESKTOP_H
#include <stdint.h>
#include <stddef.h>
int eh_desktop_open(double rate,int hidden);
int eh_desktop_poll(void); /* bit 1: menu; bit 2: close */
int eh_desktop_focus(void);
int eh_desktop_key(int key);
int16_t eh_desktop_pointer(unsigned id);
int eh_desktop_video(const uint32_t*,unsigned,unsigned,int,int);
void eh_desktop_audio(const int16_t*,size_t,unsigned,int,unsigned);
void eh_desktop_pause(int);
void eh_desktop_close(void);
double eh_desktop_now(void);
int eh_desktop_gl_create(int,unsigned,unsigned);
void *eh_desktop_gl_proc(const char*);
void eh_desktop_gl_close(void);
int eh_desktop_shader_begin(void);
void eh_desktop_shader_end(void);
void eh_desktop_shader_close(void);
int eh_desktop_smoke(void);
#endif
