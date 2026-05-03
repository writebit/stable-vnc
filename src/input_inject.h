#ifndef INPUT_INJECT_H
#define INPUT_INJECT_H

#include <stdint.h>
#include <stdbool.h>

void input_inject_mouse_move(uint16_t x, uint16_t y, uint32_t display_width, uint32_t display_height);
void input_inject_mouse_button(uint16_t x, uint16_t y, uint8_t button_mask,
                                uint32_t display_width, uint32_t display_height);
void input_inject_key_event(uint32_t keysym, bool down);

#endif
