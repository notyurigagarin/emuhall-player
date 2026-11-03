#ifndef EMUHALL_SHADERS_H
#define EMUHALL_SHADERS_H
#include <stdint.h>
/* Original curated GPU presets. This is not a Slang/GLSLP file loader. */
int eh_shader_apply(uint32_t *pixels,unsigned width,unsigned height,unsigned preset);
void eh_shader_close(void);
int eh_shader_smoke(void);
#endif
