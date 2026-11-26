/* SELECT: settings page (tempo, tap tempo, swing, sequence, quantize, ...). */
#ifndef SETTINGS_H
#define SETTINGS_H
#include "input.h"

void settings_open(void);
void settings_draw(void);
void settings_key(SDLKey k, int rep);   /* closes itself on SELECT / B */
const char *settings_help(void);

#endif
