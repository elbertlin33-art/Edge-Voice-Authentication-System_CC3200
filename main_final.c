//*****************************************************************************
//
// Application Name     -   Edge Voice Authentication System
// Application Overview -   Final simple main for the PIR + OLED app flow.
//
//*****************************************************************************

//*****************************************************************************
//
//! \addtogroup voice_auth
//! @{
//
//*****************************************************************************

#include <string.h>

// Driverlib includes
#include "hw_types.h"
#include "hw_ints.h"
#include "hw_memmap.h"
#include "rom.h"
#include "rom_map.h"
#include "interrupt.h"
#include "prcm.h"
#include "utils.h"
#include "spi.h"

// Common interface includes
#include "common.h"
#include "uart_if.h"

// App includes
#include "pin_mux_config.h"
#include "mic_capture.h"
#include "dsp_features.h"
#include "cloud_client.h"
#include "pir_sensor.h"
#include "ui.h"

#define APPLICATION_NAME        "Voice Auth"
#define APPLICATION_VERSION     "Final"
#define SPI_IF_BIT_RATE         1000000
#define RECORD_SECONDS          3
#define RECORD_SAMPLES          (MIC_SAMPLE_RATE_HZ * RECORD_SECONDS)
#define RECORD_CHUNK_SAMPLES    DSP_FFT_INPUT_SAMPLES
#define RUN_BOOT_UPLOAD_TEST    1
#define AUDIO_NOISY_AVG_ABS     14000
#define AUDIO_NOISY_PEAK        62000
#define AUDIO_QUIET_AVG_ABS     20
#define AUDIO_QUIET_PEAK        1000

//*****************************************************************************
//                 GLOBAL VARIABLES -- Start
//*****************************************************************************

#if defined(ccs) || defined(gcc)
extern void (* const g_pfnVectors[])(void);
#endif

#if defined(ewarm)
extern uVectorEntry __vector_table;
#endif

static short gPcmBuffer[RECORD_SAMPLES];
static short gPcmChunk[RECORD_CHUNK_SAMPLES];

//*****************************************************************************
//                 GLOBAL VARIABLES -- End
//*****************************************************************************

//*****************************************************************************
//                      LOCAL FUNCTION PROTOTYPES
//*****************************************************************************

static void BoardInit(void);
static void OLED_SPIInit(void);
static void DelayMs(unsigned long ms);
static void App_ShowState(UIState_t state, int userId, int score);
static int Audio_CheckSignal(const short *pcm, unsigned long samples);
static int RecordThreeSecondClip(void);
static void HandleMotionEvent(void);

//*****************************************************************************
//
//! Board Initialization & Configuration
//!
//! \param  None
//!
//! \return None
//
//*****************************************************************************
static void BoardInit(void)
{
/* In case of TI-RTOS vector table is initialize by OS itself */
#ifndef USE_TIRTOS
#if defined(ccs)
    MAP_IntVTableBaseSet((unsigned long)&g_pfnVectors[0]);
#endif
#if defined(ewarm)
    MAP_IntVTableBaseSet((unsigned long)&__vector_table);
#endif
#endif

    MAP_IntMasterEnable();
    MAP_IntEnable(FAULT_SYSTICK);

    PRCMCC3200MCUInit();
}

//*****************************************************************************
//
//! OLED SPI Initialization
//!
//! \param  None
//!
//! \return None
//
//*****************************************************************************
static void OLED_SPIInit(void)
{
    MAP_SPIReset(GSPI_BASE);
    MAP_SPIConfigSetExpClk(GSPI_BASE,
                           MAP_PRCMPeripheralClockGet(PRCM_GSPI),
                           SPI_IF_BIT_RATE,
                           SPI_MODE_MASTER,
                           SPI_SUB_MODE_0,
                           (SPI_SW_CTRL_CS |
                            SPI_4PIN_MODE |
                            SPI_TURBO_OFF |
                            SPI_CS_ACTIVELOW |
                            SPI_WL_8));
    MAP_SPIEnable(GSPI_BASE);
}

//*****************************************************************************
//
//! Simple delay in milliseconds
//!
//! \param  ms delay time
//!
//! \return None
//
//*****************************************************************************
static void DelayMs(unsigned long ms)
{
    while(ms--) {
        MAP_UtilsDelay(80000);
    }
}

static const char *StateName(UIState_t state)
{
    switch(state) {
        case UI_STATE_IDLE:
            return "IDLE";
        case UI_STATE_GET_READY:
            return "GET_READY";
        case UI_STATE_RECORDING:
            return "RECORDING";
        case UI_STATE_PROCESSING:
            return "PROCESSING";
        case UI_STATE_UPLOADING:
            return "UPLOADING";
        case UI_STATE_ENROLLING:
            return "ENROLLING";
        case UI_STATE_PASS:
            return "PASS";
        case UI_STATE_FAIL:
            return "FAIL";
        case UI_STATE_ENROLLED:
            return "ENROLLED";
        case UI_STATE_CLEARED:
            return "CLEARED";
        case UI_STATE_TOO_NOISY:
            return "TOO_NOISY";
        case UI_STATE_TOO_QUIET:
            return "TOO_QUIET";
        case UI_STATE_AWS_ERROR:
            return "AWS_ERROR";
        default:
            return "UNKNOWN";
    }
}

static void App_ShowState(UIState_t state, int userId, int score)
{
    UART_PRINT("STATE: %s", StateName(state));

    if(userId > 0) {
        UART_PRINT(" user=%d", userId);
    }

    if(score > 0) {
        UART_PRINT(" score=%d", score);
    }

    UART_PRINT("\n\r");
    UI_ShowState(state, userId, score);
}

static int Audio_CheckSignal(const short *pcm, unsigned long samples)
{
    tAudioStats stats;

    DSP_GetAudioStats(pcm, samples, &stats);

    UART_PRINT("Mic: min=%d max=%d peak_to_peak=%d avg_abs=%ld\n\r",
               stats.min,
               stats.max,
               stats.peakToPeak,
               stats.avgAbs);

    if((stats.avgAbs < AUDIO_QUIET_AVG_ABS) ||
       (stats.peakToPeak < AUDIO_QUIET_PEAK)) {
        UART_PRINT("Audio: too quiet\n\r");
        return UI_STATE_TOO_QUIET;
    }

    if((stats.avgAbs > AUDIO_NOISY_AVG_ABS) ||
       (stats.peakToPeak > AUDIO_NOISY_PEAK)) {
        UART_PRINT("Audio: too noisy\n\r");
        return UI_STATE_TOO_NOISY;
    }

    return 0;
}

//*****************************************************************************
//
//! Record one 3-second voice clip
//!
//! \param  None
//!
//! \return Number of samples recorded
//
//*****************************************************************************
static int RecordThreeSecondClip(void)
{
    unsigned long totalCaptured = 0;
    tMicCaptureStats stats;

    App_ShowState(UI_STATE_RECORDING, 0, 0);
    UART_PRINT("Mic: recording %d seconds...\n\r", RECORD_SECONDS);

    while(totalCaptured < RECORD_SAMPLES) {
        unsigned long want = RECORD_SAMPLES - totalCaptured;
        int captured;

        if(want > RECORD_CHUNK_SAMPLES) {
            want = RECORD_CHUNK_SAMPLES;
        }

        captured = (totalCaptured == 0)
                 ? Mic_RecordSamples(gPcmChunk, RECORD_CHUNK_SAMPLES, want)
                 : Mic_RecordMoreSamples(gPcmChunk, RECORD_CHUNK_SAMPLES, want);
        UART_PRINT("captured=%d\n\r", captured);
        if(captured <= 0) {
            UART_PRINT("Mic: chunk failed, captured=%d\n\r", captured);
            break;
        }

        memcpy(&gPcmBuffer[totalCaptured], gPcmChunk, captured * sizeof(short));
        totalCaptured += (unsigned long)captured;

        UART_PRINT("Mic: captured %lu / %lu samples\n\r",
                   totalCaptured,
                   (unsigned long)RECORD_SAMPLES);

        if((unsigned long)captured < want) {
            UART_PRINT("Mic: short chunk, wanted=%lu got=%d\n\r", want, captured);
            break;
        }
    }

    Mic_GetDmaStats(&stats);
    UART_PRINT("Mic: dma rx=%lu tx=%lu dropped=%lu\n\r",
               stats.rxDmaCount,
               stats.txDmaCount,
               stats.droppedSamples);
    UART_PRINT("Mic: audio bytes ready = %lu\n\r", totalCaptured * sizeof(short));

    return (int)totalCaptured;
}

//*****************************************************************************
//
//! Run one voice-auth attempt after PIR motion
//!
//! \param  None
//!
//! \return None
//
//*****************************************************************************
static void HandleMotionEvent(void)
{
    int samplesRecorded;
    int audioProblem;
    CloudResult_t cloudResult;

    App_ShowState(UI_STATE_GET_READY, 0, 0);
    DelayMs(1000);

    samplesRecorded = RecordThreeSecondClip();

    if(samplesRecorded != RECORD_SAMPLES) {
        App_ShowState(UI_STATE_FAIL, 0, 0);
        DelayMs(2000);
        return;
    }

    audioProblem = Audio_CheckSignal(gPcmBuffer, RECORD_SAMPLES);
    if(audioProblem != 0) {
        App_ShowState((UIState_t)audioProblem, 0, 0);
        DelayMs(2000);
        return;
    }

    App_ShowState(UI_STATE_UPLOADING, 0, 0);
    DelayMs(1000);

    if(Cloud_ProcessVoice(gPcmBuffer, RECORD_SAMPLES, &cloudResult) < 0) {
        App_ShowState(UI_STATE_AWS_ERROR, 0, 0);
        DelayMs(2000);
        return;
    }

    UART_PRINT("Word detected: %s\n\r", cloudResult.word);
    UI_ShowWord(cloudResult.word);
    DelayMs(1500);

    if(cloudResult.command == CLOUD_COMMAND_ENROLL) {
        App_ShowState(UI_STATE_ENROLLED, cloudResult.userId, 0);
        DelayMs(2000);
    } else if(cloudResult.command == CLOUD_COMMAND_CLEAR) {
        App_ShowState(UI_STATE_CLEARED, 0, 0);
        DelayMs(2000);
    } else if(cloudResult.command == CLOUD_COMMAND_AUTHENTICATE) {
        App_ShowState(UI_STATE_PROCESSING, 0, 0);
        DelayMs(1000);
        if(cloudResult.passed) {
            App_ShowState(UI_STATE_PASS, cloudResult.userId, cloudResult.score);
        } else {
            App_ShowState(UI_STATE_FAIL, 0, 0);
        }
        DelayMs(2000);
    } else {
        App_ShowState(UI_STATE_AWS_ERROR, 0, 0);
        DelayMs(2000);
    }
}

//*****************************************************************************
//
//! Main
//!
//! \param  none
//!
//! \return None
//
//*****************************************************************************
void main()
{
    long lRetVal = -1;

    BoardInit();
    PinMuxConfig();
    InitTerm();
    ClearTerm();

    OLED_SPIInit();
    UI_Init();
    App_ShowState(UI_STATE_PROCESSING, 0, 0);

    lRetVal = Cloud_Init();
    if(lRetVal < 0) {
        App_ShowState(UI_STATE_FAIL, 0, 0);
        UART_PRINT("Unable to initialize cloud client\n\r");
        LOOP_FOREVER();
    }

    PIR_Init();

    lRetVal = Mic_Init();
    if(lRetVal < 0) {
        App_ShowState(UI_STATE_FAIL, 0, 0);
        UART_PRINT("Unable to initialize microphone\n\r");
        LOOP_FOREVER();
    }

    App_ShowState(UI_STATE_IDLE, 0, 0);

#if RUN_BOOT_UPLOAD_TEST
    UART_PRINT("TEST: running one recording/upload without PIR\n\r");
    HandleMotionEvent();
    App_ShowState(UI_STATE_IDLE, 0, 0);
#endif

    while(1) {
        if(PIR_MotionDetected()) {
            UART_PRINT("PIR: motion detected\n\r");
            HandleMotionEvent();
            App_ShowState(UI_STATE_IDLE, 0, 0);
            UART_PRINT("PIR: signal after idle = %d\n\r", PIR_MotionDetected());
        } else {
            DelayMs(100);
        }
    }
}
