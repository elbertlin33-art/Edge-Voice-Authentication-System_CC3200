#include "app.h"
#include "pin_mux_config.h"
#include "ui.h"
#include "pir_sensor.h"
#include "common.h"
#include "rom_map.h"
#include "utils.h"
#include "uart_if.h"
#include "mic_capture.h"
#include "cloud_client.h"
#include "dsp_features.h"
#include <string.h>

#define RECORD_SECONDS       3
#define RECORD_SAMPLES       (MIC_SAMPLE_RATE_HZ * RECORD_SECONDS)
#define RECORD_CHUNK_SAMPLES DSP_FFT_INPUT_SAMPLES
#define PIR_COOLDOWN_MS      2000

static short gPcmBuffer[RECORD_SAMPLES];
static short gPcmChunk[RECORD_CHUNK_SAMPLES];

static void App_DelayMs(unsigned long ms)
{
    while (ms--)
    {
        MAP_UtilsDelay(80000); // ~1 ms at 80 MHz
    }
}

static int App_RecordClip(void)
{
    unsigned long totalCaptured = 0;

    UI_ShowState(UI_STATE_RECORDING, 0, 0);
    UART_PRINT("Mic: recording %d seconds...\n\r", RECORD_SECONDS);

    while (totalCaptured < RECORD_SAMPLES)
    {
        unsigned long want = RECORD_SAMPLES - totalCaptured;
        int captured;

        if (want > RECORD_CHUNK_SAMPLES)
            want = RECORD_CHUNK_SAMPLES;

        captured = Mic_RecordSamples(gPcmChunk, RECORD_CHUNK_SAMPLES, want);
        if (captured <= 0)
        {
            UART_PRINT("Mic: chunk failed, captured=%d\n\r", captured);
            break;
        }

        memcpy(&gPcmBuffer[totalCaptured], gPcmChunk, captured * sizeof(short));
        totalCaptured += (unsigned long)captured;

        UART_PRINT("Mic: captured %lu / %lu samples\n\r",
                   totalCaptured, (unsigned long)RECORD_SAMPLES);

        if ((unsigned long)captured < want)
            break;
    }

    return (int)totalCaptured;
}

static void App_HandleEnrollEvent(void)
{
    int samplesRecorded;
    int userId = 0;

    UI_ShowState(UI_STATE_ENROLLING, 0, 0);
    App_DelayMs(1000);

    samplesRecorded = App_RecordClip();

    UI_ShowState(UI_STATE_UPLOADING, 0, 0);
    App_DelayMs(1000);

    if ((samplesRecorded == RECORD_SAMPLES) &&
        (Cloud_EnrollVoice(gPcmBuffer, RECORD_SAMPLES, 0, 0, &userId) == 0))
    {
        UI_ShowState(UI_STATE_ENROLLED, userId, 0);
    }
    else
    {
        UI_ShowState(UI_STATE_FAIL, 0, 0);
    }

    App_DelayMs(2000);
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
            UART_PRINT("PIR: signal after idle = %d\n\r", PIR_MotionDetected());
            App_DelayMs(PIR_COOLDOWN_MS);
        }
        else
        {
            App_DelayMs(100);
        }
    }
}

void App_HandleMotionEvent(void)
{
    int samplesRecorded;
    int userId = 0;
    int score  = 0;
    CloudCommand_t command;

    UI_ShowState(UI_STATE_GET_READY, 0, 0);
    App_DelayMs(1000);

    samplesRecorded = App_RecordClip();

    UI_ShowState(UI_STATE_UPLOADING, 0, 0);
    App_DelayMs(1000);

    if (samplesRecorded != RECORD_SAMPLES)
    {
        UI_ShowState(UI_STATE_FAIL, 0, 0);
        App_DelayMs(2000);
        return;
    }

    command = Cloud_DetectCommand(gPcmBuffer, RECORD_SAMPLES, 0, 0);

    if (command == CLOUD_COMMAND_ENROLL)
    {
        App_HandleEnrollEvent();
    }
    else if (command == CLOUD_COMMAND_CLEAR)
    {
        Cloud_ClearProfiles();
        UI_ShowState(UI_STATE_CLEARED, 0, 0);
        App_DelayMs(2000);
    }
    else
    {
        UI_ShowState(UI_STATE_PROCESSING, 0, 0);
        App_DelayMs(1000);

        if (Cloud_AuthenticateVoice(gPcmBuffer, RECORD_SAMPLES, 0, 0,
                                    &userId, &score) == 0)
        {
            UI_ShowState(UI_STATE_PASS, userId, score);
        }
        else
        {
            UI_ShowState(UI_STATE_FAIL, 0, 0);
        }

        App_DelayMs(2000);
    }
}
