/* SPDX-License-Identifier: MIT
 * Optional dynamic preparation ABI. No Citra implementation is linked into this worker. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
int eh_citra_content(const char *core,const char *user,const char *input,int mode,
    unsigned long long *id,char *title,size_t title_cap,char *launch,size_t launch_cap,char *folder,size_t folder_cap){
    typedef int (*prepare_fn)(const char*,const char*,int,unsigned long long*,char*,size_t,char*,size_t,char*,size_t);
    prepare_fn prepare=NULL;
#ifdef _WIN32
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,core,-1,NULL,0);if(!n)return -1;
    wchar_t *wide=calloc((size_t)n,sizeof(wchar_t));if(!wide)return -1;
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,core,-1,wide,n);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    HMODULE library=LoadLibraryExW(wide,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);free(wide);
    if(!library)return -1;
    FARPROC symbol=GetProcAddress(library,"emuhall_citra_content_v1");memcpy(&prepare,&symbol,sizeof(prepare));
#else
    void *library=dlopen(core,RTLD_NOW|RTLD_LOCAL);if(!library)return -1;
    void *symbol=dlsym(library,"emuhall_citra_content_v1");memcpy(&prepare,&symbol,sizeof(prepare));
#endif
    int result=prepare?prepare(user,input,mode,id,title,title_cap,launch,launch_cap,folder,folder_cap):-2;
#ifdef _WIN32
    FreeLibrary(library);
#else
    dlclose(library);
#endif
    return result;
}
