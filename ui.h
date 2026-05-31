#ifndef UI_H
#define UI_H

#include <stdint.h>

typedef enum {
    UI_STATE_IDLE = 0,
    UI_STATE_GET_READY,
    UI_STATE_RECORDING,
    UI_STATE_PROCESSING,
    UI_STATE_UPLOADING,
    UI_STATE_ENROLLING,
    UI_STATE_PASS,
    UI_STATE_FAIL,
    UI_STATE_ENROLLED,
    UI_STATE_CLEARED
} UIState_t;

void UI_Init(void);
void UI_ShowState(UIState_t state, int user_id, int score);

#endif // UI_H


