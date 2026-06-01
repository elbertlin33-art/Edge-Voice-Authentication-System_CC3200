#include <string.h>

#include "hw_types.h"
#include "hw_memmap.h"
#include "rom.h"
#include "rom_map.h"
#include "prcm.h"
#include "i2s.h"
#include "udma.h"
#include "utils.h"

#include "common.h"
#include "uart_if.h"
#include "udma_if.h"
#include "i2s_if.h"

#include "circ_buff.h"
#include "dsp_features.h"
#include "mic_capture.h"

/*
 * Big picture of this file:
 *
 * 1. Mic_Init() sets up the INMP441 I2S microphone.
 * 2. uDMA copies I2S samples into a circular buffer.
 * 3. Mic_RecordSeconds() reads that buffer and extracts mono PCM samples.
 * 4. Mic_RunMonitor() is an optional debug loop that prints audio stats.
 *
 * Why "polled DMA" instead of interrupt DMA?
 * - The DMA engine still moves samples from I2S to RAM.
 * - The CPU only polls the DMA control table to re-arm completed buffers.
 * - This avoided the CC3200 I2S interrupt registration hang seen in testing.
 */

/* INMP441 requires 64 bit clocks per stereo frame: 16 kHz * 2 * 32 = 1.024 MHz. */
#define MIC_CHANNELS              2
#define MIC_BITS_PER_SLOT         32
#define MIC_BYTES_PER_WORD        4
#define MIC_I2S_CLK_HZ            (MIC_SAMPLE_RATE_HZ * MIC_CHANNELS * MIC_BITS_PER_SLOT)

/* Each stereo frame has two 32-bit words. L/R=GND on INMP441 selects left slot. */
#define MIC_FRAME_BYTES           (MIC_CHANNELS * MIC_BYTES_PER_WORD)
#define MIC_ACTIVE_WORD_INDEX     0

/* DMA copies blocks of 256 32-bit words at a time. */
#define MIC_DMA_BUF_BYTES         (16 * 1024)
#define MIC_DMA_XFER_ITEMS        256
#define MIC_DMA_XFER_BYTES        (MIC_DMA_XFER_ITEMS * MIC_BYTES_PER_WORD)

/* Timeout prevents an infinite loop if I2S/DMA stops. */
#define MIC_RECORD_TIMEOUT_TICKS  0x00080000

#ifndef MIC_ENABLE_RECORD_TEST
#define MIC_ENABLE_RECORD_TEST    0
#endif

#if MIC_ENABLE_RECORD_TEST
#define MIC_RECORD_TEST_SECONDS   3
#define MIC_RECORD_TEST_SAMPLES   (MIC_SAMPLE_RATE_HZ * MIC_RECORD_TEST_SECONDS)
#define MIC_RECORD_TEST_CHUNK     MIC_SAMPLE_RATE_HZ
#endif

/* CC3200 SDK has no official 32-bit slot macro, so define the register value. */
#ifndef I2S_SLOT_SIZE_32
#define I2S_SLOT_SIZE_32          0x00F000F8
#endif

/* One circular buffer receives mic data from DMA. */
static tCircularBuffer *gRecordBuffer = 0;

/* Dummy TX data. CC3200 starts clean I2S clocks in RX+TX mode. */
static unsigned char gZeroTx[MIC_DMA_XFER_BYTES];

/* Simple counters for debugging and validation. */
static unsigned long gRxDmaCount = 0;
static unsigned long gTxDmaCount = 0;
static unsigned long gDroppedSamples = 0;
static int gMicReady = 0;

static unsigned char *Mic_AdvanceRecordPtr(unsigned char *ptr,
                                           unsigned long bytes)
{
    unsigned long offset;

    if(gRecordBuffer == 0) {
        return ptr;
    }

    ptr += bytes;
    if(ptr < gRecordBuffer->pucBufferEndPtr) {
        return ptr;
    }

    offset = (unsigned long)(ptr - gRecordBuffer->pucBufferEndPtr);
    return gRecordBuffer->pucBufferStartPtr + offset;
}

static void Mic_ResetRecordBuffer(void)
{
    if(gRecordBuffer != 0) {
        gRecordBuffer->pucReadPtr = gRecordBuffer->pucBufferStartPtr;
        gRecordBuffer->pucWritePtr = gRecordBuffer->pucBufferStartPtr;
    }
}

static void Mic_ConfigI2S(void)
{
    /* Set the I2S module clock to 1.024 MHz. */
    MAP_PRCMI2SClockFreqSet(MIC_I2S_CLK_HZ);

    /* Configure 32-bit slots and DMA access for the I2S port. */
    MAP_I2SConfigSetExpClk(I2S_BASE,
                           MIC_I2S_CLK_HZ,
                           MIC_I2S_CLK_HZ,
                           I2S_SLOT_SIZE_32 | I2S_PORT_DMA);

    /* DATA_LINE_1 is RX on PIN_50, where the INMP441 SD wire goes. */
    MAP_I2SSerializerConfig(I2S_BASE,
                            I2S_DATA_LINE_1,
                            I2S_SER_MODE_RX,
                            I2S_INACT_LOW_LEVEL);

    /* DATA_LINE_0 is dummy TX on PIN_64, left unconnected. */
    MAP_I2SSerializerConfig(I2S_BASE,
                            I2S_DATA_LINE_0,
                            I2S_SER_MODE_TX,
                            I2S_INACT_LOW_LEVEL);
}

static void Mic_SetupRxDma(void)
{
    unsigned char *primaryPtr = GetWritePtr(gRecordBuffer);
    unsigned char *alternatePtr = Mic_AdvanceRecordPtr(primaryPtr,
                                                       MIC_DMA_XFER_BYTES);

    /* Primary RX DMA: I2S RX register -> circular buffer write pointer. */
    UDMASetupTransfer(UDMA_CH4_I2S_RX,
                      UDMA_MODE_PINGPONG,
                      MIC_DMA_XFER_ITEMS,
                      UDMA_SIZE_32,
                      UDMA_ARB_8,
                      (void *)I2S_RX_DMA_PORT,
                      UDMA_CHCTL_SRCINC_NONE,
                      (void *)primaryPtr,
                      UDMA_CHCTL_DSTINC_32);

    /* Alternate RX DMA: starts at the next block in the same buffer. */
    UDMASetupTransfer(UDMA_CH4_I2S_RX | UDMA_ALT_SELECT,
                      UDMA_MODE_PINGPONG,
                      MIC_DMA_XFER_ITEMS,
                      UDMA_SIZE_32,
                      UDMA_ARB_8,
                      (void *)I2S_RX_DMA_PORT,
                      UDMA_CHCTL_SRCINC_NONE,
                      (void *)alternatePtr,
                      UDMA_CHCTL_DSTINC_32);

    /* Allow normal DMA requests. This was required during bring-up. */
    MAP_uDMAChannelAttributeDisable(UDMA_CH4_I2S_RX,
                                    UDMA_ATTR_USEBURST | UDMA_ATTR_REQMASK);
}

static void Mic_SetupTxDma(void)
{
    /* Primary TX DMA sends zeros to keep the TX side alive. */
    UDMASetupTransfer(UDMA_CH5_I2S_TX,
                      UDMA_MODE_PINGPONG,
                      MIC_DMA_XFER_ITEMS,
                      UDMA_SIZE_32,
                      UDMA_ARB_8,
                      (void *)&gZeroTx[0],
                      UDMA_CHCTL_SRCINC_32,
                      (void *)I2S_TX_DMA_PORT,
                      UDMA_DST_INC_NONE);

    /* Alternate TX DMA also sends zeros. */
    UDMASetupTransfer(UDMA_CH5_I2S_TX | UDMA_ALT_SELECT,
                      UDMA_MODE_PINGPONG,
                      MIC_DMA_XFER_ITEMS,
                      UDMA_SIZE_32,
                      UDMA_ARB_8,
                      (void *)&gZeroTx[0],
                      UDMA_CHCTL_SRCINC_32,
                      (void *)I2S_TX_DMA_PORT,
                      UDMA_DST_INC_NONE);

    /* Allow normal DMA requests for dummy TX. */
    MAP_uDMAChannelAttributeDisable(UDMA_CH5_I2S_TX,
                                    UDMA_ATTR_USEBURST | UDMA_ATTR_REQMASK);
}

static void Mic_RearmRxIfDone(unsigned long tableIndex, unsigned long channel)
{
    /* Get the uDMA control table created by UDMAInit(). */
    tDMAControlTable *table = MAP_uDMAControlBaseGet();

    /* If transfer mode is nonzero, this DMA half is still active. */
    if ((table[tableIndex].ulControl & UDMA_CHCTL_XFERMODE_M) != 0) {
        return;
    }

    /* Reuse the completed half with the current circular-buffer write pointer. */
    MAP_uDMAChannelTransferSet(channel,
                               UDMA_MODE_PINGPONG,
                               (void *)I2S_RX_DMA_PORT,
                               (void *)GetWritePtr(gRecordBuffer),
                               MIC_DMA_XFER_ITEMS);

    /* Enable the DMA half again. */
    MAP_uDMAChannelEnable(channel);

    /* Tell the circular buffer that DMA filled another block. */
    UpdateWritePtr(gRecordBuffer, MIC_DMA_XFER_BYTES);

    /* Count this for debug prints. */
    gRxDmaCount++;
}

static void Mic_RearmTxIfDone(unsigned long tableIndex, unsigned long channel)
{
    /* Get the uDMA control table created by UDMAInit(). */
    tDMAControlTable *table = MAP_uDMAControlBaseGet();

    /* If transfer mode is nonzero, this DMA half is still active. */
    if ((table[tableIndex].ulControl & UDMA_CHCTL_XFERMODE_M) != 0) {
        return;
    }

    /* Reuse the completed TX half and send zeros again. */
    MAP_uDMAChannelTransferSet(channel,
                               UDMA_MODE_PINGPONG,
                               (void *)&gZeroTx[0],
                               (void *)I2S_TX_DMA_PORT,
                               MIC_DMA_XFER_ITEMS);

    /* Enable the DMA half again. */
    MAP_uDMAChannelEnable(channel);

    /* Count this for debug prints. */
    gTxDmaCount++;
}

static void Mic_PollDma(void)
{
    /* Primary RX control table index is 0x4. */
    Mic_RearmRxIfDone(0x4, UDMA_CH4_I2S_RX);

    /* Alternate RX control table index is 0x24. */
    Mic_RearmRxIfDone(0x24, UDMA_CH4_I2S_RX | UDMA_ALT_SELECT);

    /* Primary TX control table index is 0x5. */
    Mic_RearmTxIfDone(0x5, UDMA_CH5_I2S_TX);

    /* Alternate TX control table index is 0x25. */
    Mic_RearmTxIfDone(0x25, UDMA_CH5_I2S_TX | UDMA_ALT_SELECT);
}

#if MIC_ENABLE_RECORD_TEST
static void Mic_DelayMs(unsigned long ms)
{
    while(ms--) {
        MAP_UtilsDelay(80000);
    }
}

static void Mic_PrintHexByte(unsigned char value, unsigned long *lineCount)
{
    UART_PRINT("%02x", (unsigned int)value);

    (*lineCount)++;
    if((*lineCount % 32) == 0) {
        UART_PRINT("\n\r");
    }
}

static void Mic_PrintHexU16(unsigned short value, unsigned long *lineCount)
{
    Mic_PrintHexByte((unsigned char)(value & 0xFF), lineCount);
    Mic_PrintHexByte((unsigned char)((value >> 8) & 0xFF), lineCount);
}

static void Mic_PrintHexU32(unsigned long value, unsigned long *lineCount)
{
    Mic_PrintHexByte((unsigned char)(value & 0xFF), lineCount);
    Mic_PrintHexByte((unsigned char)((value >> 8) & 0xFF), lineCount);
    Mic_PrintHexByte((unsigned char)((value >> 16) & 0xFF), lineCount);
    Mic_PrintHexByte((unsigned char)((value >> 24) & 0xFF), lineCount);
}

static void Mic_PrintHexText(const char *text, unsigned long *lineCount)
{
    while(*text != '\0') {
        Mic_PrintHexByte((unsigned char)*text, lineCount);
        text++;
    }
}

static void Mic_PrintZeroStats(const short *pcm, unsigned long sampleCount)
{
    unsigned long i;
    unsigned long zeroCount = 0;
    long maxAbs = 0;

    for(i = 0; i < sampleCount; i++) {
        long sample = pcm[i];
        long absSample = (sample < 0) ? -sample : sample;

        if(sample == 0) {
            zeroCount++;
        }

        if(absSample > maxAbs) {
            maxAbs = absSample;
        }
    }

    UART_PRINT("zero_samples=%lu nonzero_samples=%lu max_abs=%ld\n\r",
               zeroCount,
               sampleCount - zeroCount,
               maxAbs);
}

static void Mic_PrintWavFileHex(const short *pcm, unsigned long sampleCount)
{
    unsigned long i;
    unsigned long lineCount = 0;
    unsigned long dataBytes = sampleCount * 2;
    unsigned long wavBytes = dataBytes + 44;

    UART_PRINT("\n\rWAV_HEX_BEGIN filename=mic_capture_test.wav bytes=%lu samples=%lu\n\r",
               wavBytes,
               sampleCount);

    Mic_PrintHexText("RIFF", &lineCount);
    Mic_PrintHexU32(wavBytes - 8, &lineCount);
    Mic_PrintHexText("WAVE", &lineCount);

    Mic_PrintHexText("fmt ", &lineCount);
    Mic_PrintHexU32(16, &lineCount);
    Mic_PrintHexU16(1, &lineCount);
    Mic_PrintHexU16(1, &lineCount);
    Mic_PrintHexU32(MIC_SAMPLE_RATE_HZ, &lineCount);
    Mic_PrintHexU32(MIC_SAMPLE_RATE_HZ * 2, &lineCount);
    Mic_PrintHexU16(2, &lineCount);
    Mic_PrintHexU16(16, &lineCount);

    Mic_PrintHexText("data", &lineCount);
    Mic_PrintHexU32(dataBytes, &lineCount);

    for(i = 0; i < sampleCount; i++) {
        Mic_PrintHexU16((unsigned short)pcm[i], &lineCount);
    }

    if((lineCount % 32) != 0) {
        UART_PRINT("\n\r");
    }

    UART_PRINT("WAV_HEX_END\n\r");
}
#endif

int Mic_Init(void)
{
    /* If already initialized, do nothing. */
    if (gMicReady) {
        return 0;
    }

    /* Fill dummy TX memory with zeros. */
    memset(gZeroTx, 0, sizeof(gZeroTx));

    /* Allocate the RX circular buffer. */
    gRecordBuffer = CreateCircularBuffer(MIC_DMA_BUF_BYTES);

    /* Fail if heap allocation did not work. */
    if (gRecordBuffer == 0) {
        return -1;
    }

    /* Enable CC3200 I2S clock. */
    AudioInit();

    /* Enable and configure the uDMA controller. */
    UDMAInit();

    /* Assign and configure dummy TX DMA first, matching TI's audio example style. */
    UDMAChannelSelect(UDMA_CH5_I2S_TX, 0);
    Mic_SetupTxDma();

    /* Assign and configure RX DMA for the microphone. */
    UDMAChannelSelect(UDMA_CH4_I2S_RX, 0);
    Mic_SetupRxDma();

    /* Configure INMP441-compatible I2S timing and serializers. */
    Mic_ConfigI2S();

    /* Clear stale I2S error/status bits before starting. */
    MAP_I2SIntClear(I2S_BASE, I2S_INT_RDMA | I2S_INT_XDMA |
                              I2S_INT_ROVRN | I2S_INT_RSYNCERR |
                              I2S_INT_XUNDRN | I2S_INT_XSYNCERR);

    /* Enable RX and TX FIFOs. */
    MAP_I2SRxFIFOEnable(I2S_BASE, 8, 1);
    MAP_I2STxFIFOEnable(I2S_BASE, 8, 1);

    /* Start I2S in RX+TX mode so BCLK and WS are generated. */
    Audio_Start(I2S_MODE_RX_TX);

    /* Kick both DMA channels once. After this, Mic_PollDma() re-arms them. */
    MAP_uDMAChannelRequest(UDMA_CH4_I2S_RX);
    MAP_uDMAChannelRequest(UDMA_CH5_I2S_TX);

    /* Mark module ready. */
    gMicReady = 1;

    return 0;
}

static int Mic_RecordSamplesInternal(short *pcmOut,
                                     unsigned long maxSamples,
                                     unsigned long samples,
                                     int clearOldSamples)
{
    unsigned long captured = 0;
    unsigned long timeout = 0;

    /* Validate input and requested sample count. */
    if (!gMicReady || (pcmOut == 0) || (samples == 0) || (maxSamples < samples)) {
        return -1;
    }

    if(clearOldSamples) {
        UART_PRINT("Mic: restarting DMA pipeline\n\r");

        /* Force both DMA channels idle so re-setup doesn't corrupt in-flight transfers. */
        MAP_uDMAChannelDisable(UDMA_CH4_I2S_RX);
        MAP_uDMAChannelDisable(UDMA_CH5_I2S_TX);

        Mic_ResetRecordBuffer();

        /* Clear any I2S error flags (ROVRN, XUNDRN) that fired during idle.
         * Without this, the I2S hardware stops issuing DMA requests. */
        MAP_I2SIntClear(I2S_BASE, I2S_INT_RDMA | I2S_INT_XDMA |
                                  I2S_INT_ROVRN | I2S_INT_RSYNCERR |
                                  I2S_INT_XUNDRN | I2S_INT_XSYNCERR);

        /* Re-arm DMA descriptors from the current buffer write position. */
        Mic_SetupRxDma();
        Mic_SetupTxDma();

        /* Software-kick both channels to drain the stale FIFO and prime the pipeline. */
        MAP_uDMAChannelRequest(UDMA_CH4_I2S_RX);
        MAP_uDMAChannelRequest(UDMA_CH5_I2S_TX);

        UART_PRINT("Mic: DMA pipeline restarted\n\r");
    }

    /* Keep reading until the requested number of mono samples is captured. */
    while (captured < samples) {
        /* Check whether any DMA ping/pong half finished and re-arm it. */
        Mic_PollDma();

        /* One stereo frame is two 32-bit words: dummy word + active mic word. */
        if (GetBufferSize(gRecordBuffer) >= MIC_FRAME_BYTES) {
            unsigned long frame[MIC_CHANNELS];
            unsigned long rawMicWord;

            /* Copy one stereo frame out of the circular DMA buffer. */
            ReadBuffer(gRecordBuffer, (unsigned char *)frame, MIC_FRAME_BYTES);

            /* The INMP441 sample is in the selected I2S word for our L/R wiring. */
            rawMicWord = frame[MIC_ACTIVE_WORD_INDEX];

            /* Convert from 32-bit I2S slot to signed 16-bit PCM. */
            pcmOut[captured] = (short)(rawMicWord >> 16);

            /* Move to next output sample. */
            captured++;

            /* Reset timeout because progress happened. */
            timeout = 0;
        } else {
            /* No full frame available yet. Keep waiting, but not forever. */
            timeout++;

            /* Return partial count if DMA/I2S stops. */
            if (timeout > MIC_RECORD_TIMEOUT_TICKS) {
                UART_PRINT("Mic: timeout waiting for samples, captured=%lu / %lu\n\r",
                           captured,
                           samples);
                gDroppedSamples += (samples - captured);
                return (int)captured;
            }
        }
    }

    return (int)captured;
}

int Mic_RecordSamples(short *pcmOut, unsigned long maxSamples, unsigned long samples)
{
    return Mic_RecordSamplesInternal(pcmOut, maxSamples, samples, 1);
}

int Mic_RecordMoreSamples(short *pcmOut, unsigned long maxSamples, unsigned long samples)
{
    return Mic_RecordSamplesInternal(pcmOut, maxSamples, samples, 0);
}

int Mic_RecordSeconds(short *pcmOut, unsigned long maxSamples, unsigned long seconds)
{
    unsigned long needSamples = MIC_SAMPLE_RATE_HZ * seconds;

    /* Convert x seconds into x * sample_rate samples. */
    if (seconds == 0) {
        return -1;
    }

    return Mic_RecordSamples(pcmOut, maxSamples, needSamples);
}

void Mic_GetDmaStats(tMicCaptureStats *stats)
{
    /* Do nothing if caller passed no output struct. */
    if (stats == 0) {
        return;
    }

    /* Copy internal counters to caller-visible struct. */
    stats->rxDmaCount = gRxDmaCount;
    stats->txDmaCount = gTxDmaCount;
    stats->droppedSamples = gDroppedSamples;
}

void Mic_RunMonitor(void)
{
    /* Small chunk keeps RAM usage low. */
    static short pcm[DSP_FFT_INPUT_SAMPLES];
    unsigned long iter = 0;

    /* Initialize mic before monitoring. */
    if (Mic_Init() < 0) {
        UART_PRINT("MIC: init failed\n\r");
        return;
    }

    UART_PRINT("\n\r=== INMP441 DMA Mic Monitor ===\n\r");
    UART_PRINT("Tap or talk into the mic. Watch p-p / |avg| jump.\n\r");

    while (1) {
        tAudioStats audio;
        tMicCaptureStats dma;
        int n;

        /* Record a short audio chunk. */
        n = Mic_RecordSamples(pcm, DSP_FFT_INPUT_SAMPLES, DSP_FFT_INPUT_SAMPLES);

        /* Print an error if no samples arrived. */
        if (n <= 0) {
            UART_PRINT("MIC: record failed\n\r");
            continue;
        }

        /* Compute simple audio stats. */
        DSP_GetAudioStats(pcm, (unsigned long)n, &audio);

        /* Copy DMA counters. */
        Mic_GetDmaStats(&dma);

        /* Print a readable monitor line. */
        UART_PRINT("[%4lu] MIC: min=%6d max=%6d p-p=%6d |avg|=%5ld rx=%lu tx=%lu drop=%lu\n\r",
                   iter++,
                   audio.min,
                   audio.max,
                   audio.peakToPeak,
                   audio.avgAbs,
                   dma.rxDmaCount,
                   dma.txDmaCount,
                   dma.droppedSamples);
    }
}

#if MIC_ENABLE_RECORD_TEST
void Mic_RunRecordTest(void)
{
    static short pcm[MIC_RECORD_TEST_SAMPLES];
    unsigned long i;
    tAudioStats audio;
    tMicCaptureStats dma;
    int n;

    if (Mic_Init() < 0) {
        UART_PRINT("MIC: init failed\n\r");
        return;
    }

    UART_PRINT("\n\r=== INMP441 Record Test From mic_capture.c ===\n\r");
    UART_PRINT("First, quick live check. Tap or talk into the mic.\n\r");

    for(i = 0; i < 8; i++) {
        n = Mic_RecordSamples(pcm, DSP_FFT_INPUT_SAMPLES, DSP_FFT_INPUT_SAMPLES);
        if(n <= 0) {
            UART_PRINT("[%lu] live check failed\n\r", i);
            continue;
        }

        DSP_GetAudioStats(pcm, (unsigned long)n, &audio);
        Mic_GetDmaStats(&dma);
        UART_PRINT("[%lu] live min=%6d max=%6d p-p=%6d |avg|=%5ld rx=%lu tx=%lu drop=%lu\n\r",
                   i,
                   audio.min,
                   audio.max,
                   audio.peakToPeak,
                   audio.avgAbs,
                   dma.rxDmaCount,
                   dma.txDmaCount,
                   dma.droppedSamples);
    }

    UART_PRINT("\n\rRecording %lu seconds in 3...\n\r",
               (unsigned long)MIC_RECORD_TEST_SECONDS);
    Mic_DelayMs(1000);
    UART_PRINT("Recording %lu seconds in 2...\n\r",
               (unsigned long)MIC_RECORD_TEST_SECONDS);
    Mic_DelayMs(1000);
    UART_PRINT("Recording %lu seconds in 1...\n\r",
               (unsigned long)MIC_RECORD_TEST_SECONDS);
    Mic_DelayMs(1000);
    UART_PRINT("START TALKING NOW for %lu seconds\n\r",
               (unsigned long)MIC_RECORD_TEST_SECONDS);

    n = 0;
    for(i = 0; i < MIC_RECORD_TEST_SECONDS; i++) {
        int chunk;

        UART_PRINT("record chunk %lu / %lu...\n\r",
                   i + 1,
                   (unsigned long)MIC_RECORD_TEST_SECONDS);

        chunk = Mic_RecordSamples(&pcm[n],
                                  MIC_RECORD_TEST_SAMPLES - (unsigned long)n,
                                  MIC_RECORD_TEST_CHUNK);
        UART_PRINT("chunk captured=%d\n\r", chunk);

        if(chunk <= 0) {
            break;
        }

        n += chunk;
    }

    if(n <= 0) {
        UART_PRINT("record failed, captured=%d\n\r", n);
        return;
    }

    UART_PRINT("record complete, captured=%d\n\r", n);
    UART_PRINT("RECORDING_INFO sample_rate=%lu seconds=%lu total_samples=%d\n\r",
               (unsigned long)MIC_SAMPLE_RATE_HZ,
               (unsigned long)MIC_RECORD_TEST_SECONDS,
               n);

    DSP_GetAudioStats(pcm, (unsigned long)n, &audio);
    Mic_GetDmaStats(&dma);
    UART_PRINT("AUDIO_STATS min=%d max=%d peak_to_peak=%d avg_abs=%ld\n\r",
               audio.min,
               audio.max,
               audio.peakToPeak,
               audio.avgAbs);
    UART_PRINT("DMA_STATS rx=%lu tx=%lu dropped=%lu\n\r",
               dma.rxDmaCount,
               dma.txDmaCount,
               dma.droppedSamples);

    Mic_PrintZeroStats(pcm, (unsigned long)n);
    Mic_PrintWavFileHex(pcm, (unsigned long)n);

    UART_PRINT("\n\rMic record test done.\n\r");
}
#endif
