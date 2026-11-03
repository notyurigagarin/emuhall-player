/* Copyright 2026 Braza IO Capital Holding. SPDX-License-Identifier: MIT */
/* An original test window hosts a worker, repeatedly changing its own bounds.
   Only scratch profiles should be supplied. It never controls another app. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <stdint.h>
#include <stdint.h>
static int append_arg(wchar_t *buffer,size_t capacity,const wchar_t *arg){
    size_t used=wcslen(buffer);if(used+3>=capacity)return 0;buffer[used++]=L'"';
    unsigned slashes=0;
    for(;;arg++){
        if(*arg==L'\\'){slashes++;continue;}
        unsigned count=slashes*(!*arg||*arg==L'"'?2:1)+(*arg==L'"');
        if(used+count+3>=capacity)return 0;
        while(count--)buffer[used++]=L'\\';slashes=0;
        if(!*arg)break;buffer[used++]=*arg;
    }
    buffer[used++]=L'"';buffer[used++]=L' ';buffer[used]=0;return 1;
}
int wmain(int argc,wchar_t **argv){
    if(argc!=7)return 2; /* helper WORKER CORE GAME SAVE FRAMES MODE */
    HWND parent=CreateWindowExW(0,L"STATIC",L"EmuHall embedded resize test",WS_OVERLAPPEDWINDOW|WS_VISIBLE,40,40,800,600,NULL,NULL,GetModuleHandleW(NULL),NULL);
    if(!parent)return 3;
    if(GetEnvironmentVariableW(L"EMUHALL_TEST_PARENT_FULLSCREEN",NULL,0)){
        MONITORINFO info={0};info.cbSize=sizeof(info);GetMonitorInfoW(MonitorFromWindow(parent,MONITOR_DEFAULTTOPRIMARY),&info);
        SetWindowLongPtrW(parent,GWL_STYLE,WS_POPUP|WS_VISIBLE);
        SetWindowPos(parent,NULL,info.rcMonitor.left,info.rcMonitor.top,info.rcMonitor.right-info.rcMonitor.left,info.rcMonitor.bottom-info.rcMonitor.top,SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    }
    if(GetEnvironmentVariableW(L"EMUHALL_TEST_PARENT_FULLSCREEN",NULL,0)){
        MONITORINFO info={0};info.cbSize=sizeof(info);GetMonitorInfoW(MonitorFromWindow(parent,MONITOR_DEFAULTTOPRIMARY),&info);
        SetWindowLongPtrW(parent,GWL_STYLE,WS_POPUP|WS_VISIBLE);
        SetWindowPos(parent,NULL,info.rcMonitor.left,info.rcMonitor.top,info.rcMonitor.right-info.rcMonitor.left,info.rcMonitor.bottom-info.rcMonitor.top,SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    }
    wchar_t command[32768]={0},embed[80];swprintf(embed,80,L"embed:%llu",(unsigned long long)(uintptr_t)parent);
    for(int i=1;i<6;i++)if(!append_arg(command,32768,argv[i]))return 4;
    if(!append_arg(command,32768,embed))return 4;
    STARTUPINFOW startup={0};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process={0};
    if(!CreateProcessW(argv[1],command,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process))return 5;
    CloseHandle(process.hThread);ULONGLONG start=GetTickCount64(),next=start+4000;unsigned changes=0;
    for(;;){
        DWORD wait=MsgWaitForMultipleObjects(1,&process.hProcess,FALSE,100,QS_ALLINPUT);if(wait==WAIT_OBJECT_0)break;
        MSG message;while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        ULONGLONG now=GetTickCount64();
        if(now>=next){
            MONITORINFO info={0};info.cbSize=sizeof(info);GetMonitorInfoW(MonitorFromWindow(parent,MONITOR_DEFAULTTOPRIMARY),&info);
            int full=!(changes%2);SetWindowLongPtrW(parent,GWL_STYLE,full?WS_POPUP|WS_VISIBLE:WS_OVERLAPPEDWINDOW|WS_VISIBLE);
            SetWindowPos(parent,NULL,full?info.rcMonitor.left:40,full?info.rcMonitor.top:40,full?info.rcMonitor.right-info.rcMonitor.left:900,full?info.rcMonitor.bottom-info.rcMonitor.top:650,SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
            fprintf(stderr,"Synthetic parent: %s transition %u\n",full?"fullscreen":"windowed",++changes);next=now+4000;
        }
        if(now-start>180000){TerminateProcess(process.hProcess,99);break;}
    }
    DWORD result=99;GetExitCodeProcess(process.hProcess,&result);CloseHandle(process.hProcess);DestroyWindow(parent);return (int)result;
}
