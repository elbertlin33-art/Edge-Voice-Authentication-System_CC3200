#include "hw_types.h"
#include "hw_ints.h"
#include "rom.h"
#include "rom_map.h"
#include "interrupt.h"
#include "prcm.h"
#include "hw_memmap.h"
#include "hw_common_reg.h"

#include "common.h"
#include "uart_if.h"
#include "pin_mux_config.h"

#include "mic_capture.h"
#include "dsp_features.h"

/*
 * Change this value to test any recording length that fits in RAM.
 * Example: 1 records one second, 2 records two seconds, 5 records five seconds.
 */
#define TEST_RECORD_SECONDS      1
#define TEST_RECORD_SAMPLES      (MIC_SAMPLE_RATE_HZ * TEST_RECORD_SECONDS)
#define RECORD_CHUNK_SAMPLES     DSP_FFT_INPUT_SAMPLES
#define WAVE_PREVIEW_POINTS      128
#define FFT_NORMALIZE_PEAK       12000

static short gChunk[RECORD_CHUNK_SAMPLES];
static short gWavePreview[WAVE_PREVIEW_POINTS];
static long gFftMag[DSP_FFT_BIN_COUNT];
static long gFftSum[DSP_FFT_BIN_COUNT];

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

static void PrintFftBins(const long *mag, unsigned long binCount)
{
    unsigned long i;

    if (mag == 0) {
        return;
    }

    UART_PRINT("\n\rFFT_BEGIN input_samples=%lu\n\r",
               (unsigned long)DSP_FFT_INPUT_SAMPLES);

    for (i = 0; i < binCount; i++) {
        UART_PRINT("FFT[%4lu Hz]=%ld\n\r",
                   DSP_GetFftBinHz(i),
                   mag[i]);
    }

    UART_PRINT("FFT_END\n\r");
}

int main(void)
{
    unsigned long totalTarget = TEST_RECORD_SAMPLES;
    unsigned long totalCaptured = 0;
    unsigned long previewTarget = 0;
    unsigned long previewCount = 0;
    unsigned long previewStep = TEST_RECORD_SAMPLES / WAVE_PREVIEW_POINTS;
    unsigned long fftBlocks = 0;
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

    if (previewStep == 0) {
        previewStep = 1;
    }

    while (totalCaptured < totalTarget) {
        unsigned long i;
        unsigned long want = totalTarget - totalCaptured;

        if (want > RECORD_CHUNK_SAMPLES) {
            want = RECORD_CHUNK_SAMPLES;
        }

        captured = Mic_RecordSamples(gChunk, RECORD_CHUNK_SAMPLES, want);
        if (captured <= 0) {
            UART_PRINT("Recording stopped early. captured=%lu\n\r", totalCaptured);
            break;
        }

        for (i = 0; i < (unsigned long)captured; i++) {
            short sample = gChunk[i];
            unsigned long sampleIndex = totalCaptured + i;

            if ((previewCount < WAVE_PREVIEW_POINTS) &&
                (sampleIndex >= previewTarget)) {
                gWavePreview[previewCount] = sample;
                previewCount++;
                previewTarget += previewStep;
            }
        }

        /*
         * Each full chunk contributes one frequency analysis block. The final
         * short chunk is used for waveform/stats, but skipped for FFT.
         */
        if ((unsigned long)captured == RECORD_CHUNK_SAMPLES) {
            unsigned long bin;

            DSP_RemoveDC(gChunk, RECORD_CHUNK_SAMPLES);
            DSP_NormalizePeak(gChunk, RECORD_CHUNK_SAMPLES, FFT_NORMALIZE_PEAK);

            if (DSP_ComputeFFT(gChunk,
                               RECORD_CHUNK_SAMPLES,
                               gFftMag,
                               DSP_FFT_BIN_COUNT) == 0) {
                for (bin = 0; bin < DSP_FFT_BIN_COUNT; bin++) {
                    gFftSum[bin] += gFftMag[bin];
                }
                fftBlocks++;
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
    PrintWavePreview(gWavePreview, previewCount, previewStep, totalCaptured);

    if (fftBlocks > 0) {
        unsigned long bin;

        for (bin = 0; bin < DSP_FFT_BIN_COUNT; bin++) {
            gFftMag[bin] = gFftSum[bin] / (long)fftBlocks;
        }

        PrintFftBins(gFftMag, DSP_FFT_BIN_COUNT);
    } else {
        UART_PRINT("FFT failed. Need at least %lu samples.\n\r",
                   (unsigned long)DSP_FFT_INPUT_SAMPLES);
    }

    UART_PRINT("\n\rTest done. Change TEST_RECORD_SECONDS to try another x-second capture.\n\r");

    while (1) {
    }
}
