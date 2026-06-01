#include <stdio.h>
#include "ui.h"
#include "Adafruit_SSD1351.h"
#include "oled_test.h"
#include "Adafruit_GFX.h"

#define UI_SCREEN_WIDTH 128
#define UI_CHAR_WIDTH   6
#define UI_LINE_HEIGHT  16
#define UI_BLOCK_TOP    40

static int UI_TextX(const char *text)
{
    int len = 0;
    int width;

    if(text == 0) {
        return 0;
    }

    while(text[len] != '\0') {
        len++;
    }

    width = len * UI_CHAR_WIDTH;
    if(width >= UI_SCREEN_WIDTH) {
        return 0;
    }

    return (UI_SCREEN_WIDTH - width) / 2;
}

static void UI_DisplayLines(const char *line1,
                            const char *line2,
                            const char *line3)
{
    fillScreen(BLACK);
    setTextColor(WHITE, BLACK);
    setTextSize(1);

    setCursor(UI_TextX(line1), UI_BLOCK_TOP);
    Outstr((char *)line1);

    setCursor(UI_TextX(line2), UI_BLOCK_TOP + UI_LINE_HEIGHT);
    Outstr((char *)line2);

    setCursor(UI_TextX(line3), UI_BLOCK_TOP + (2 * UI_LINE_HEIGHT));
    Outstr((char *)line3);
}

void UI_Init(void)
{
    Adafruit_Init();
    fillScreen(BLACK);
    setTextColor(WHITE, BLACK);
    setTextSize(1);
    setCursor(UI_TextX("Voice Auth ready"), UI_BLOCK_TOP + UI_LINE_HEIGHT);
    Outstr("Voice Auth ready");
}

void UI_ShowState(UIState_t state, int user_id, int score)
{
    char line1[32] = {0};

    switch (state)
    {
        case UI_STATE_IDLE:
            UI_DisplayLines("IDLE", "Waiting for motion", "Ready");
            break;
        case UI_STATE_GET_READY:
            UI_DisplayLines("Get Ready...", "Speak in 1 second", "Preparing");
            break;
        case UI_STATE_RECORDING:
            UI_DisplayLines("Speak now", "Recording 3 sec", "Hold still");
            break;
        case UI_STATE_PROCESSING:
            UI_DisplayLines("Processing", "Extracting features", "Please wait");
            break;
        case UI_STATE_UPLOADING:
            UI_DisplayLines("Uploading", "Sending audio to cloud", "Please wait");
            break;
        case UI_STATE_ENROLLING:
            UI_DisplayLines("Enroll Mode", "Get ready", "Say your phrase");
            break;
        case UI_STATE_PASS:
            snprintf(line1, sizeof(line1), "PASS (%d%%)", score);
            UI_DisplayLines(line1, "Authenticated", "Welcome");
            break;
        case UI_STATE_FAIL:
            if(score > 0) {
                snprintf(line1, sizeof(line1), "FAIL (%d%%)", score);
                UI_DisplayLines(line1, "No matching profile", "Try again");
            } else {
                UI_DisplayLines("FAIL", "No matching profile", "Try again");
            }
            break;
        case UI_STATE_ENROLLED:
            snprintf(line1, sizeof(line1), "ENROLLED: User %d", user_id);
            UI_DisplayLines(line1, "Profile saved", "Ready");
            break;
        case UI_STATE_CLEARED:
            UI_DisplayLines("CLEARED", "All profiles deleted", "Ready");
            break;
        case UI_STATE_TOO_NOISY:
            UI_DisplayLines("TOO NOISY", "Try again", "Returning idle");
            break;
        case UI_STATE_TOO_QUIET:
            UI_DisplayLines("TOO QUIET", "No clear voice", "Returning idle");
            break;
        case UI_STATE_AWS_ERROR:
            UI_DisplayLines("AWS ERROR", "No cloud result", "Try again");
            break;
        default:
            UI_DisplayLines("Unknown state", "", "");
            break;
    }
}

void UI_ShowInitializing(void)
{
    UI_DisplayLines("Initializing...", "Starting device", "Please wait");
}

void UI_ShowConnectingWifi(void)
{
    UI_DisplayLines("Connecting Wi-Fi", "Waiting for IP", "Please wait");
}

void UI_ShowWait(void)
{
    UI_DisplayLines("Please wait", "Checking cloud", "Almost ready");
}

void UI_ShowWord(const char *word)
{
    char line2[32] = {0};

    if((word == 0) || (word[0] == '\0')) {
        UI_DisplayLines("Word", "none detected", "Processing");
        return;
    }

    snprintf(line2, sizeof(line2), "%s", word);
    UI_DisplayLines("Word detected", line2, "Processing");
}

void UI_ShowWords(const char *words)
{
    char line2[32] = {0};

    if((words == 0) || (words[0] == '\0')) {
        UI_DisplayLines("Words detected", "none", "Processing");
        return;
    }

    snprintf(line2, sizeof(line2), "%s", words);
    UI_DisplayLines("Words detected", line2, "Processing");
}

void UI_ShowWelcome(const char *name)
{
    char line2[32] = {0};

    if((name == 0) || (name[0] == '\0')) {
        UI_DisplayLines("Welcome", "user!", "Ready");
        return;
    }

    snprintf(line2, sizeof(line2), "%s!", name);
    UI_DisplayLines("Welcome", line2, "Ready");
}

void UI_ShowNoUser(void)
{
    UI_DisplayLines("No existing profile", "Say apple", "to enroll");
}
