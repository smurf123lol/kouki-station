#ifndef SETUP_STUBS_H
#define SETUP_STUBS_H

/*
 * Счётчики заглушек соседних модулей стенда Setup (T121 П11), см. setup_stubs.c.
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void setup_stubs_reset(void);

int setup_ui_setup_calls(void);
const char *setup_ui_setup_ssid(void);
const char *setup_ui_setup_url(void);

int setup_voice_print_state_calls(void);
int setup_voice_start_calls(void);
int setup_voice_stop_calls(void);
int setup_voice_mock_done_calls(void);
int setup_voice_last_session(void);

int setup_history_clear_calls(void);
int setup_history_push_calls(void);

#ifdef __cplusplus
}
#endif

#endif  // SETUP_STUBS_H
