/* Project Oogie: the controller layer's interface for the settings menu. */
#ifndef PC_INPUT_H
#define PC_INPUT_H

#include <stdint.h>
#include <stddef.h>

/* Physical pad buttons, in Xbox (positional) names. */
enum {
    PB_A, PB_B, PB_X, PB_Y, PB_LB, PB_RB, PB_LT, PB_RT, PB_BACK, PB_START,
    PB_LS, PB_RS, PB_DUP, PB_DDOWN, PB_DLEFT, PB_DRIGHT, PB_TOUCH, PB_MISC,
    PB_GUIDE, PB_COUNT
};

/* Buttons held on any connected pad (1 << PB_x), sticks folded into the
 * d-pad bits. For the settings menu, which reads pads while the game cannot. */
uint32_t pc_input_raw_buttons(void);

/* Name of a PB_x button as settings.ini writes it ("A", "LB", "DUp" ...). */
const char *pc_input_button_name(int pb);

/* Re-read [ControllerButtons] from settings.ini (after the menu changed it). */
void pc_input_binds_reload(void);

/* One line per connected pad ("Xbox pad (XInput 1)", "DualSense ..."). */
int pc_input_pad_list(char lines[][96], int max);

#endif
