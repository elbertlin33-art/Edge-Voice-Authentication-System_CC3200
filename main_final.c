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
#include "cloud_client.h"
#include "camila's function files/pir_sensor.h"
#include "camila's function files/ui.h"

#define APPLICATION_NAME        "Voice Auth"
#define APPLICATION_VERSION     "Final"
#define SPI_IF_BIT_RATE         1000000
#define RECORD_SECONDS          3
#define RECORD_SAMPLES          (MIC_SAMPLE_RATE_HZ * RECORD_SECONDS)

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

//*****************************************************************************
//                 GLOBAL VARIABLES -- End
//*****************************************************************************

//*****************************************************************************
//                      LOCAL FUNCTION PROTOTYPES
//*****************************************************************************

static void BoardInit(void);
static void OLED_SPIInit(void);
static void DelayMs(unsigned long ms);
static int RecordThreeSecondClip(void);
static void HandleEnrollEvent(void);
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
    UI_ShowState(UI_STATE_RECORDING, 0, 0);
    return Mic_RecordSeconds(gPcmBuffer, RECORD_SAMPLES, RECORD_SECONDS);
}

//*****************************************************************************
//
//! Run the apple/enroll path
//!
//! \param  None
//!
//! \return None
//
//*****************************************************************************
static void HandleEnrollEvent(void)
{
    int samplesRecorded;
    int userId = 0;

    UI_ShowState(UI_STATE_ENROLLING, 0, 0);
    DelayMs(1000);

    samplesRecorded = RecordThreeSecondClip();

    UI_ShowState(UI_STATE_UPLOADING, 0, 0);
    DelayMs(1000);

    if((samplesRecorded == RECORD_SAMPLES) &&
       (Cloud_EnrollVoice(gPcmBuffer, RECORD_SAMPLES, &userId) == 0)) {
        UI_ShowState(UI_STATE_ENROLLED, userId, 0);
    } else {
        UI_ShowState(UI_STATE_FAIL, 0, 0);
    }

    DelayMs(2000);
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
    int userId = 0;
    int score = 0;
    CloudCommand_t command;

    UI_ShowState(UI_STATE_GET_READY, 0, 0);
    DelayMs(1000);

    samplesRecorded = RecordThreeSecondClip();

    UI_ShowState(UI_STATE_UPLOADING, 0, 0);
    DelayMs(1000);

    if(samplesRecorded != RECORD_SAMPLES) {
        UI_ShowState(UI_STATE_FAIL, 0, 0);
        DelayMs(2000);
        return;
    }

    command = Cloud_DetectCommand(gPcmBuffer, RECORD_SAMPLES);

    if(command == CLOUD_COMMAND_ENROLL) {
        HandleEnrollEvent();
    } else if(command == CLOUD_COMMAND_CLEAR) {
        Cloud_ClearProfiles();
        UI_ShowState(UI_STATE_CLEARED, 0, 0);
        DelayMs(2000);
    } else {
        UI_ShowState(UI_STATE_PROCESSING, 0, 0);
        DelayMs(1000);
        if(Cloud_AuthenticateVoice(gPcmBuffer,
                                   RECORD_SAMPLES,
                                   &userId,
                                   &score) == 0) {
            UI_ShowState(UI_STATE_PASS, userId, score);
        } else {
            UI_ShowState(UI_STATE_FAIL, 0, 0);
        }
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
    PIR_Init();
    lRetVal = Mic_Init();
    if(lRetVal < 0) {
        UART_PRINT("Unable to initialize microphone\n\r");
        LOOP_FOREVER();
    }
    lRetVal = Cloud_Init();
    if(lRetVal < 0) {
        UART_PRINT("Unable to initialize cloud client\n\r");
        LOOP_FOREVER();
    }

    UI_ShowState(UI_STATE_IDLE, 0, 0);

    while(1) {
        if(PIR_MotionDetected()) {
            HandleMotionEvent();
            UI_ShowState(UI_STATE_IDLE, 0, 0);
        } else {
            DelayMs(100);
        }
    }
}
