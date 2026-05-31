#include "app.h"
#include "../pin_mux_config.h"
#include "ui.h"
#include "pir_sensor.h"
#include "rom_map.h"
#include "utils.h"

static void App_DelayMs(unsigned long ms)
{
    while (ms--)
    {
        MAP_UtilsDelay(80000); // ~1 ms at 80 MHz
    }
}

void App_Init(void)
{
    PinMuxConfig();
    UI_Init();
    PIR_Init();
    UI_ShowState(UI_STATE_IDLE, 0, 0);
}

void App_Run(void)
{
    while (1)
    {
        if (PIR_MotionDetected())
        {
            App_HandleMotionEvent();
            UI_ShowState(UI_STATE_IDLE, 0, 0);
        }
        else
        {
            MAP_UtilsDelay(80000);
        }
    }
}

void App_HandleMotionEvent(void)
{
    UI_ShowState(UI_STATE_GET_READY, 0, 0);
    App_DelayMs(1000);

    UI_ShowState(UI_STATE_RECORDING, 0, 0);
    App_DelayMs(3000);

    UI_ShowState(UI_STATE_PROCESSING, 0, 0);
    App_DelayMs(1000);

    UI_ShowState(UI_STATE_UPLOADING, 0, 0);
    App_DelayMs(1000);

    // Placeholder result state until DSP/cloud modules are connected.
    UI_ShowState(UI_STATE_FAIL, 0, 0);
    App_DelayMs(2000);
}
