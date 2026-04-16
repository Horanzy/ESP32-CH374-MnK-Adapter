#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
  uint16_t mouse_buttons; 
  int32_t mouse_dx;       
  int32_t mouse_dy;       
  int16_t mouse_wheel;

  uint8_t kbd_keycode[6];
  uint8_t kbd_modifier;
} raw_input_state_t;

void bridge_init(void);
void bridge_push_mouse(uint16_t buttons, int16_t dx, int16_t dy, int16_t wheel);
void bridge_push_keyboard(uint8_t const keycode[6], uint8_t modifier);
void bridge_get_snapshot(raw_input_state_t *out);
