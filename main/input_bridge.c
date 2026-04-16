#include "input_bridge.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>

static raw_input_state_t g;
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

void bridge_init(void) { memset(&g, 0, sizeof(g)); }

void bridge_push_mouse(uint16_t buttons, int16_t dx, int16_t dy, int16_t wheel) {
  portENTER_CRITICAL(&g_mux);
  g.mouse_buttons = buttons;
  g.mouse_dx += dx;
  g.mouse_dy += dy;
  g.mouse_wheel += wheel << 6;
  portEXIT_CRITICAL(&g_mux);
}

void bridge_push_keyboard(uint8_t const keycode[6], uint8_t modifier) {
  portENTER_CRITICAL(&g_mux);
  memcpy(g.kbd_keycode, keycode, 6);
  g.kbd_modifier = modifier;
  portEXIT_CRITICAL(&g_mux);
}

void bridge_get_snapshot(raw_input_state_t *out) {
  portENTER_CRITICAL(&g_mux);
  *out = g;
  g.mouse_dx = 0;
  g.mouse_dy = 0;
  if(g.mouse_wheel > 0) g.mouse_wheel--;
  if(g.mouse_wheel < 0) g.mouse_wheel++;
  portEXIT_CRITICAL(&g_mux);
}