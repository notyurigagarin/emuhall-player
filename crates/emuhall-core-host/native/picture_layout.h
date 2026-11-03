/* Copyright 2026 Braza IO Capital Holding. SPDX-License-Identifier: MIT */
#ifndef EMUHALL_PICTURE_LAYOUT_H
#define EMUHALL_PICTURE_LAYOUT_H
#include <math.h>
/* Display aspect may differ from stored pixels (PS2 commonly has non-square pixels). */
static inline void eh_picture_fit(unsigned cw,unsigned ch,unsigned fw,unsigned fh,float aspect,int integer_scale,int *width,int *height){
    if(!cw||!ch||!fw||!fh){*width=*height=0;return;}
    double ratio=isfinite(aspect)&&aspect>0.01f&&aspect<100.0f?aspect:(double)fw/fh;
    int w=(int)cw,h=(int)(w/ratio+0.5);
    if(h>(int)ch){h=(int)ch;w=(int)(h*ratio+0.5);}
    if(integer_scale){unsigned sx=cw/fw,sy=ch/fh,scale=sx<sy?sx:sy;if(scale){w=(int)(fw*scale);h=(int)(fh*scale);}}
    *width=w;*height=h;
}
#endif
