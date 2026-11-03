/* SPDX-License-Identifier: MIT
   Original RAM fixture: N64 achievement address space, word order and expansion RAM. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../../crates/emuhall-core-host/native/ra_system.h"
#include "rc_libretro.h"
#include "rc_runtime.h"
#include "rc_runtime_types.h"
static unsigned char ram[8*1024*1024];
static rc_libretro_memory_regions_t regions;
static void memory(unsigned id, rc_libretro_core_memory_info_t *info){
    info->data=id==RETRO_MEMORY_SYSTEM_RAM?ram:NULL;
    info->size=id==RETRO_MEMORY_SYSTEM_RAM?sizeof(ram):0;
}
static uint32_t RC_CCONV peek(uint32_t address,uint32_t bytes,void *unused){
    unsigned char value[4]={0}; (void)unused;
    assert(bytes<=4);assert(rc_libretro_memory_read(&regions,address,value,bytes)==bytes);
    uint32_t result=0;for(unsigned i=0;i<bytes;i++)result|=(uint32_t)value[i]<<(8*i);
    return result;
}
static int unlocked;
static void RC_CCONV event(const rc_runtime_event_t *e){
    if(e->type==RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED&&e->id==1)unlocked++;
}
int main(void){
    struct retro_memory_map map={0};
    assert(console_for("n64")==RC_CONSOLE_NINTENDO_64);
    assert(rc_libretro_memory_init(&regions,&map,memory,console_for("n64")));
    rc_runtime_t runtime;rc_runtime_init(&runtime);
    assert(rc_runtime_activate_achievement(&runtime,1,"0xX000100=305419896_0xH400000=90_0xH7fffff=165",NULL,0)==0);
    rc_runtime_do_frame(&runtime,event,peek,NULL,NULL);assert(!unlocked);
    /* Mupen stores the guest word 0x12345678 as 78 56 34 12. */
    memcpy(ram+0x100,"\x78\x56\x34\x12",4);ram[0x400000]=90;ram[0x7fffff]=165;
    rc_runtime_do_frame(&runtime,event,peek,NULL,NULL);assert(unlocked==1);
    unsigned char output[4]={0};
    assert(rc_libretro_memory_read(&regions,0x7fffff,output,4)==1);
    assert(output[0]==165);
    assert(rc_libretro_memory_read(&regions,0x800000,output,1)==0);
    rc_runtime_destroy(&runtime);rc_libretro_memory_destroy(&regions);
    return 0;
}
