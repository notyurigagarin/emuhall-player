/* Controllers for the player (gamepad.c). */
#ifndef EMUHALL_GAMEPAD_H
#define EMUHALL_GAMEPAD_H
#include <stdint.h>

/* RetroPad button numbers, the bits of eh_pad.buttons. */
enum{EH_B,EH_Y,EH_SELECT,EH_START,EH_UP,EH_DOWN,EH_LEFT,EH_RIGHT,EH_A,EH_X,EH_L,EH_R,EH_L2,EH_R2,EH_L3,EH_R3};

/* One isolated local player port. Sticks run -32768..32767 with up and left negative;
   triggers run 0..32767. */
typedef struct{uint32_t buttons;int16_t lx,ly,rx,ry,lt,rt;int guide;} eh_pad;

int eh_pad_init(void); /* 1 when SDL3 is available, 0 when only XInput (or nothing) is. */
void eh_pad_read(eh_pad *out); /* Poll all devices; return player one. */
void eh_pad_read_port(unsigned port,eh_pad *out);
void eh_pad_list(void);
void eh_pad_probe(void);
unsigned eh_pad_gestures(void); /* Physical Guide and Back+Start, before remapping. */
int eh_pad_rumble(unsigned port,unsigned effect,uint16_t strength);
void eh_pad_rumble_enabled(int enabled);
void eh_pad_stop_rumble(void);
void eh_pad_shutdown(void);
#endif
