#include <stdio.h>
#include "ui.h"
#include "Adafruit_SSD1351.h"
#include "oled_test.h"
#include "Adafruit_GFX.h"

#define UI_SCREEN_WIDTH 128
#define UI_CENTER_X     64
#define UI_CHAR_WIDTH   6

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

static void UI_DrawText(const char *text, int y, unsigned int color)
{
    if(text == 0) {
        return;
    }

    setTextSize(1);
    setTextColor(color, BLACK);
    setCursor(UI_TextX(text), y);
    Outstr((char *)text);
}

static void UI_DrawTitle(const char *title)
{
    UI_DrawText(title, 4, WHITE);
    drawFastHLine(12, 18, 104, 0x39E7);
}

static void UI_DrawThickLine(int x0, int y0, int x1, int y1,
                             unsigned char width,
                             unsigned int color)
{
    signed char offset;
    signed char half = (signed char)(width / 2);

    for(offset = -half; offset <= half; offset++) {
        drawLine(x0 + offset, y0, x1 + offset, y1, color);
        drawLine(x0, y0 + offset, x1, y1 + offset, color);
    }
}

static void UI_DrawHourglass(const char *title)
{
    fillScreen(BLACK);
    UI_DrawTitle(title);

    drawRect(36, 30, 56, 78, WHITE);
    drawRect(37, 31, 54, 76, WHITE);
    fillRect(42, 36, 44, 10, CYAN);
    fillTriangle(44, 48, 84, 48, 64, 68, YELLOW);
    fillTriangle(44, 100, 84, 100, 64, 76, YELLOW);
    fillRect(42, 92, 44, 10, CYAN);
}

static void UI_DrawWarning(const char *title)
{
    fillScreen(BLACK);
    UI_DrawTitle(title);

    fillTriangle(64, 26, 15, 112, 113, 112, RED);
    fillTriangle(64, 39, 29, 103, 99, 103, YELLOW);
    fillRoundRect(59, 55, 10, 30, 4, BLACK);
    fillCircle(64, 94, 6, BLACK);
}

static void UI_DrawMicrophone(const char *title)
{
    fillScreen(BLACK);
    UI_DrawTitle(title);

    drawCircle(UI_CENTER_X, 76, 36, WHITE);
    drawCircle(UI_CENTER_X, 76, 37, WHITE);
    fillRoundRect(54, 48, 20, 36, 10, RED);
    drawCircle(64, 72, 22, RED);
    fillRect(36, 42, 24, 34, BLACK);
    fillRect(68, 42, 24, 34, BLACK);
    fillRect(60, 92, 8, 12, RED);
    fillRoundRect(48, 103, 32, 7, 3, RED);
}

static void UI_DrawClosedLock(const char *title)
{
    fillScreen(BLACK);
    UI_DrawTitle(title);

    drawCircle(64, 58, 26, WHITE);
    drawCircle(64, 58, 25, WHITE);
    drawCircle(64, 58, 24, WHITE);
    fillRect(35, 58, 58, 24, BLACK);
    fillRoundRect(30, 66, 68, 46, 7, WHITE);
    fillCircle(64, 88, 5, BLACK);
    fillRect(61, 91, 6, 12, BLACK);
}

static void UI_DrawOpenLock(const char *title)
{
    fillScreen(BLACK);
    UI_DrawTitle(title);

    fillRoundRect(28, 72, 68, 42, 7, GREEN);
    fillCircle(64, 92, 5, BLACK);
    fillRect(61, 95, 6, 10, BLACK);

    UI_DrawThickLine(76, 72, 76, 46, 8, WHITE);
    drawCircle(98, 48, 22, WHITE);
    drawCircle(98, 48, 21, WHITE);
    fillRect(76, 46, 32, 35, BLACK);
    UI_DrawThickLine(119, 48, 119, 67, 8, BLACK);
}

static void UI_DrawWifi(const char *title)
{
    fillScreen(BLACK);
    UI_DrawTitle(title);

    drawCircle(UI_CENTER_X, 118, 72, WHITE);
    drawCircle(UI_CENTER_X, 118, 71, WHITE);
    drawCircle(UI_CENTER_X, 118, 50, WHITE);
    drawCircle(UI_CENTER_X, 118, 49, WHITE);
    drawCircle(UI_CENTER_X, 118, 28, WHITE);
    drawCircle(UI_CENTER_X, 118, 27, WHITE);

    fillRect(0, 86, 128, 42, BLACK);
    fillRect(0, 20, 11, 108, BLACK);
    fillRect(117, 20, 11, 108, BLACK);
    fillCircle(UI_CENTER_X, 102, 10, WHITE);
}

void UI_Init(void)
{
    Adafruit_Init();
    fillScreen(BLACK);
    UI_DrawTitle("VOICE AUTH READY");
}

void UI_ShowState(UIState_t state, int user_id, int score)
{
    char title[24] = {0};

    (void)user_id;

    switch (state)
    {
        case UI_STATE_IDLE:
            fillScreen(BLACK);
            break;
        case UI_STATE_GET_READY:
            UI_DrawHourglass("GET READY");
            break;
        case UI_STATE_RECORDING:
            UI_DrawMicrophone("RECORDING");
            break;
        case UI_STATE_PROCESSING:
            UI_DrawHourglass("PROCESSING");
            break;
        case UI_STATE_UPLOADING:
            UI_DrawWifi("UPLOADING");
            break;
        case UI_STATE_ENROLLING:
            UI_DrawOpenLock("ENROLLING");
            break;
        case UI_STATE_PASS:
            snprintf(title, sizeof(title), "PASS %d%%", score);
            UI_DrawOpenLock(title);
            break;
        case UI_STATE_FAIL:
            snprintf(title, sizeof(title), (score > 0) ? "FAIL %d%%" : "FAIL", score);
            UI_DrawClosedLock(title);
            break;
        case UI_STATE_ENROLLED:
            UI_DrawOpenLock("ENROLLED");
            break;
        case UI_STATE_CLEARED:
            UI_DrawClosedLock("CLEARED");
            break;
        case UI_STATE_TOO_NOISY:
            UI_DrawWarning("TOO NOISY");
            break;
        case UI_STATE_TOO_QUIET:
            UI_DrawWarning("TOO QUIET");
            break;
        case UI_STATE_AWS_ERROR:
            UI_DrawWarning("AWS ERROR");
            break;
        default:
            UI_DrawWarning("ERROR");
            break;
    }
}

void UI_ShowInitializing(void)
{
    UI_DrawHourglass("INITIALIZING");
}

void UI_ShowConnectingWifi(void)
{
    UI_DrawWifi("CONNECTING");
}

void UI_ShowWait(void)
{
    UI_DrawHourglass("PLEASE WAIT");
}

void UI_ShowWord(const char *word)
{
    (void)word;
    UI_DrawOpenLock("WORD HEARD");
}

void UI_ShowWords(const char *words)
{
    (void)words;
    UI_DrawOpenLock("WORDS HEARD");
}

void UI_ShowWelcome(const char *name)
{
    char title[24] = {0};

    if((name == 0) || (name[0] == '\0')) {
        UI_DrawOpenLock("WELCOME");
        return;
    }

    snprintf(title, sizeof(title), "WELCOME %s", name);
    UI_DrawOpenLock(title);
}

void UI_ShowNoUser(void)
{
    UI_DrawClosedLock("NO PROFILE");
}
