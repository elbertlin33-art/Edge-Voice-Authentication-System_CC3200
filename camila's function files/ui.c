#include <stdio.h>
#include "ui.h"
#include "Adafruit_SSD1351.h"
#include "oled_test.h"
#include "Adafruit_GFX.h"

static void UI_DisplayLines(const char *line1,
                            const char *line2,
                            const char *line3)
{
    fillScreen(BLACK);
    setTextColor(WHITE, BLACK);
    setTextSize(1);

    setCursor(0, 0);
    Outstr((char *)line1);

    setCursor(0, 16);
    Outstr((char *)line2);

    setCursor(0, 32);
    Outstr((char *)line3);
}

void UI_Init(void)
{
    Adafruit_Init();
    fillScreen(BLACK);
    setTextColor(WHITE, BLACK);
    setTextSize(1);
    setCursor(0, 0);
    Outstr("Voice Auth ready");
}

void UI_ShowState(UIState_t state, int user_id, int score)
{
    char line1[32] = {0};
    char line2[32] = {0};
    char line3[32] = {0};

    switch (state)
    {
        case UI_STATE_IDLE:
            UI_DisplayLines("IDLE", "Waiting for motion", "Ready");
            break;
        case UI_STATE_GET_READY:
            UI_DisplayLines("Get Ready...", "Please stay still", "Preparing");
            break;
        case UI_STATE_RECORDING:
            UI_DisplayLines("Recording", "3 seconds", "Hold still");
            break;
        case UI_STATE_PROCESSING:
            UI_DisplayLines("Processing", "Extracting features", "Please wait");
            break;
        case UI_STATE_UPLOADING:
            UI_DisplayLines("Uploading", "Sending audio to cloud", "Please wait");
            break;
        case UI_STATE_PASS:
            snprintf(line1, sizeof(line1), "PASS: User %d", user_id);
            snprintf(line2, sizeof(line2), "%d%% similarity", score);
            UI_DisplayLines(line1, line2, "Authenticated");
            break;
        case UI_STATE_FAIL:
            UI_DisplayLines("FAIL", "No matching profile", "Try again");
            break;
        case UI_STATE_ENROLLED:
            snprintf(line1, sizeof(line1), "ENROLLED: User %d", user_id);
            UI_DisplayLines(line1, "Profile saved", "Ready");
            break;
        case UI_STATE_CLEARED:
            UI_DisplayLines("CLEARED", "All profiles deleted", "Ready");
            break;
        default:
            UI_DisplayLines("Unknown state", "", "");
            break;
    }
}
