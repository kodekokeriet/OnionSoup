/* Button map. Miyoo Mini delivers its buttons as SDL key presses. */
#ifndef INPUT_H
#define INPUT_H
#include <SDL.h>

#if SDL_MAJOR_VERSION >= 2
typedef SDL_Keycode SDLKey;   /* PC build uses SDL2, Miyoo uses SDL 1.2 */
#endif

#ifdef MIYOO
#define BTN_A      SDLK_SPACE
#define BTN_B      SDLK_LCTRL
#define BTN_X      SDLK_LSHIFT
#define BTN_Y      SDLK_LALT
#define BTN_L1     SDLK_e
#define BTN_R1     SDLK_t
#define BTN_L2     SDLK_TAB
#define BTN_R2     SDLK_BACKSPACE
#define BTN_SELECT SDLK_RCTRL
#define BTN_START  SDLK_RETURN
#define BTN_MENU   SDLK_ESCAPE
#else /* PC: keyboard */
/* The letter keys match the button names: press A for A, X for X ... */
#define BTN_A      SDLK_a
#define BTN_B      SDLK_b
#define BTN_X      SDLK_x
#define BTN_Y      SDLK_y
#define BTN_L1     SDLK_q
#define BTN_R1     SDLK_w
#define BTN_L2     SDLK_1
#define BTN_R2     SDLK_2
#define BTN_SELECT SDLK_TAB
#define BTN_START  SDLK_RETURN
#define BTN_MENU   SDLK_ESCAPE
#define KEY_HELP   SDLK_F1
#endif
#define BTN_UP     SDLK_UP
#define BTN_DOWN   SDLK_DOWN
#define BTN_LEFT   SDLK_LEFT
#define BTN_RIGHT  SDLK_RIGHT

#endif
