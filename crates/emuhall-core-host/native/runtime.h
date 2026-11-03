#ifndef EMUHALL_RUNTIME_H
#define EMUHALL_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
int eh_snapshot_save(const char*,const void*,size_t);
size_t eh_snapshot_size(const char*);
size_t eh_snapshot_load(const char*,void*,size_t);
int eh_snapshot_settings_differ(const char*);
int eh_atomic_write(const char*,const void*,size_t,int);
int eh_controller_profile(const char*);
void eh_controller_forget(const char*);
int eh_history_push(const void*,size_t,const uint32_t*,unsigned,unsigned);
void eh_history_clear(void);
size_t eh_history_size(void),eh_history_back(void);
int eh_history_copy(void*,size_t,uint32_t*,unsigned*,unsigned*);
int eh_video_filter(uint32_t*,unsigned,unsigned,unsigned);
size_t eh_snapshot_legacy_size(const char*);
size_t eh_snapshot_legacy_load(const char*,void*,size_t);
int eh_memory_cheat_active(void);
int eh_memory_cheat_set(unsigned,unsigned,const char*,size_t);
void eh_memory_cheat_reset(void);
void eh_memory_cheat_apply(void*,size_t);
#endif
