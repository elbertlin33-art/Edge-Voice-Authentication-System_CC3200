#include "hw_types.h"
#include "hw_ints.h"
#include "rom.h"
#include "rom_map.h"
#include "interrupt.h"
#include "prcm.h"
#include "hw_memmap.h"
#include "hw_common_reg.h"
#include "utils.h"

#include "common.h"
#include "uart_if.h"
#include "pin_mux_config.h"

#include "mic_capture.h"
#include "dsp_features.h"

/*
 * Change this value to test any recording length that fits in RAM.
 * Example: 1 records one second, 2 records two seconds, 5 records five seconds.
 */
#define TEST_RECORD_SECONDS      3
#define TEST_RECORD_SAMPLES      (MIC_SAMPLE_RATE_HZ * TEST_RECORD_SECONDS)
#define RECORD_CHUNK_SAMPLES     DSP_FFT_INPUT_SAMPLES
#define WAVE_PREVIEW_POINTS      128
#define MIC_SETTLE_MS            100

static short gChunk[RECORD_CHUNK_SAMPLES];
static short gRecording[TEST_RECORD_SAMPLES];
static short gWavePreview[WAVE_PREVIEW_POINTS];

#if defined(ccs)
extern void (* const g_pfnVectors[])(void);
#endif

#if defined(ewarm)
extern uVectorEntry __vector_table;
#endif

static void BoardInit(void)
{
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

static void PrintRecordingInfo(unsigned long sampleCount)
{
    UART_PRINT("\n\rRECORDING_INFO\n\r");
    UART_PRINT("sample_rate=%lu seconds=%lu total_samples=%lu\n\r",
               (unsigned long)MIC_SAMPLE_RATE_HZ,
               (unsigned long)TEST_RECORD_SECONDS,
               sampleCount);
}

static void DelayMs(unsigned long ms)
{
    while(ms--) {
        MAP_UtilsDelay(80000);
    }
}

static void PrintWavePreview(const short *preview,
                             unsigned long previewCount,
                             unsigned long previewStep,
                             unsigned long sampleCount)
{
    unsigned long i;

    if ((preview == 0) || (previewCount == 0)) {
        return;
    }

    UART_PRINT("\n\rWAVE_BEGIN total_samples=%lu printed_points=%lu\n\r",
               sampleCount,
               previewCount);

    for (i = 0; i < previewCount; i++) {
        UART_PRINT("WAVE[%5lu]=%6d\n\r", i * previewStep, preview[i]);
    }

    UART_PRINT("WAVE_END\n\r");
}

static void PrintHexByte(unsigned char value, unsigned long *lineCount)
{
    UART_PRINT("%02x", (unsigned int)value);

    (*lineCount)++;
    if((*lineCount % 32) == 0) {
        UART_PRINT("\n\r");
    }
}

static void PrintHexU16(unsigned short value, unsigned long *lineCount)
{
    PrintHexByte((unsigned char)(value & 0xFF), lineCount);
    PrintHexByte((unsigned char)((value >> 8) & 0xFF), lineCount);
}

static void PrintHexU32(unsigned long value, unsigned long *lineCount)
{
    PrintHexByte((unsigned char)(value & 0xFF), lineCount);
    PrintHexByte((unsigned char)((value >> 8) & 0xFF), lineCount);
    PrintHexByte((unsigned char)((value >> 16) & 0xFF), lineCount);
    PrintHexByte((unsigned char)((value >> 24) & 0xFF), lineCount);
}

static void PrintHexText(const char *text, unsigned long *lineCount)
{
    while(*text != '\0') {
        PrintHexByte((unsigned char)*text, lineCount);
        text++;
    }
}

static void PrintWavFileHex(const short *pcm, unsigned long sampleCount)
{
    unsigned long i;
    unsigned long lineCount = 0;
    unsigned long dataBytes = sampleCount * 2;
    unsigned long wavBytes = dataBytes + 44;

    UART_PRINT("\n\rWAV_HEX_BEGIN filename=mic_test.wav bytes=%lu samples=%lu\n\r",
               wavBytes,
               sampleCount);

    PrintHexText("RIFF", &lineCount);
    PrintHexU32(wavBytes - 8, &lineCount);
    PrintHexText("WAVE", &lineCount);

    PrintHexText("fmt ", &lineCount);
    PrintHexU32(16, &lineCount);
    PrintHexU16(1, &lineCount);
    PrintHexU16(1, &lineCount);
    PrintHexU32(MIC_SAMPLE_RATE_HZ, &lineCount);
    PrintHexU32(MIC_SAMPLE_RATE_HZ * 2, &lineCount);
    PrintHexU16(2, &lineCount);
    PrintHexU16(16, &lineCount);

    PrintHexText("data", &lineCount);
    PrintHexU32(dataBytes, &lineCount);

    for(i = 0; i < sampleCount; i++) {
        PrintHexU16((unsigned short)pcm[i], &lineCount);
    }

    if((lineCount % 32) != 0) {
        UART_PRINT("\n\r");
    }

    UART_PRINT("WAV_HEX_END\n\r");
}

int main(void)
{
    unsigned long totalTarget = TEST_RECORD_SAMPLES;
    unsigned long totalCaptured = 0;
    unsigned long previewTarget = 0;
    unsigned long previewCount = 0;
    unsigned long previewStep = TEST_RECORD_SAMPLES / WAVE_PREVIEW_POINTS;
    tAudioStats stats;
    int captured;

    BoardInit();
    PinMuxConfig();
    InitTerm();
    ClearTerm();

    UART_PRINT("\n\r=== INMP441 Record + DSP Test ===\n\r");
    UART_PRINT("Recording %lu second(s) at %lu Hz.\n\r",
               (unsigned long)TEST_RECORD_SECONDS,
               (unsigned long)MIC_SAMPLE_RATE_HZ);

    if (Mic_Init() < 0) {
        UART_PRINT("MIC init failed.\n\r");
        while (1) {
        }
    }

    UART_PRINT("Mic initialized. Recording now...\n\r");
    DelayMs(MIC_SETTLE_MS);

    if (previewStep == 0) {
        previewStep = 1;
    }

    while (totalCaptured < totalTarget) {
        unsigned long i;
        unsigned long want = totalTarget - totalCaptured;

        if (want > RECORD_CHUNK_SAMPLES) {
            want = RECORD_CHUNK_SAMPLES;
        }

        UART_PRINT("Requesting chunk: want=%lu total=%lu\n\r", want, totalCaptured);
        captured = (totalCaptured == 0)
                 ? Mic_RecordSamples(gChunk, RECORD_CHUNK_SAMPLES, want)
                 : Mic_RecordMoreSamples(gChunk, RECORD_CHUNK_SAMPLES, want);
        UART_PRINT("Chunk returned: captured=%d\n\r", captured);

        if (captured <= 0) {
            UART_PRINT("Recording stopped early. captured=%lu\n\r", totalCaptured);
            break;
        }

        for (i = 0; i < (unsigned long)captured; i++) {
            short sample = gChunk[i];
            unsigned long sampleIndex = totalCaptured + i;

            if (sampleIndex < TEST_RECORD_SAMPLES) {
                gRecording[sampleIndex] = sample;
            }

            if ((previewCount < WAVE_PREVIEW_POINTS) &&
                (sampleIndex >= previewTarget)) {
                gWavePreview[previewCount] = sample;
                previewCount++;
                previewTarget += previewStep;
            }
        }

        totalCaptured += (unsigned long)captured;

        if ((unsigned long)captured < want) {
            break;
        }
    }

    if (totalCaptured == 0) {
        UART_PRINT("Recording failed.\n\r");
        while (1) {
        }
    }

    UART_PRINT("Recording complete. captured=%lu\n\r", totalCaptured);

    PrintRecordingInfo(totalCaptured);

    DSP_GetAudioStats(gRecording, totalCaptured, &stats);
    UART_PRINT("\n\rAUDIO_STATS min=%d max=%d peak_to_peak=%d avg_abs=%ld\n\r",
               stats.min,
               stats.max,
               stats.peakToPeak,
               stats.avgAbs);

    PrintWavePreview(gWavePreview, previewCount, previewStep, totalCaptured);
    PrintWavFileHex(gRecording, totalCaptured);

    UART_PRINT("\n\rTest done. Change TEST_RECORD_SECONDS to try another x-second capture.\n\r");

    while (1) {
    }
}
