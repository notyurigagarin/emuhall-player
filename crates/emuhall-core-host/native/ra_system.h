/* SPDX-License-Identifier: MIT */
#ifndef EMUHALL_RA_SYSTEM_H
#define EMUHALL_RA_SYSTEM_H
#include <string.h>
#include "rc_consoles.h"
static unsigned console_for(const char *system){
    if(!strcmp(system,"gb"))return RC_CONSOLE_GAMEBOY;
    if(!strcmp(system,"gbc"))return RC_CONSOLE_GAMEBOY_COLOR;
    if(!strcmp(system,"gba"))return RC_CONSOLE_GAMEBOY_ADVANCE;
    if(!strcmp(system,"nes"))return RC_CONSOLE_NINTENDO;
    if(!strcmp(system,"snes"))return RC_CONSOLE_SUPER_NINTENDO;
    if(!strcmp(system,"ps1"))return RC_CONSOLE_PLAYSTATION;
    if(!strcmp(system,"nds"))return RC_CONSOLE_NINTENDO_DS;
    /* RA's Mupen sets use the exposed little-endian RDRAM layout directly. */
    if(!strcmp(system,"n64"))return RC_CONSOLE_NINTENDO_64;
    if(!strcmp(system,"ps2"))return RC_CONSOLE_PLAYSTATION_2;
    return 0;
}
#endif
