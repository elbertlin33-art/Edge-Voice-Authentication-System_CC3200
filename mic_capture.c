#include <string.h>

#include "hw_types.h"
#include "hw_memmap.h"
#include "rom.h"
#include "rom_map.h"
#include "prcm.h"
#include "i2s.h"
#include "udma.h"

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

/* Each stereo frame has two 32-bit words: one empty/dummy word and one mic word. */
#define MIC_FRAME_BYTES           (MIC_CHANNELS * MIC_BYTES_PER_WORD)
#define MIC_ACTIVE_WORD_INDEX     1

/* DMA copies blocks of 256 32-bit words at a time. */
#define MIC_DMA_BUF_BYTES         (16 * 1024)
#define MIC_DMA_XFER_ITEMS        256
#define MIC_DMA_XFER_BYTES        (MIC_DMA_XFER_ITEMS * MIC_BYTES_PER_WORD)

/* Timeout prevents an infinite loop if I2S/DMA stops. */
#define MIC_RECORD_TIMEOUT_TICKS  0x00FFFFFF

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
    /* Primary RX DMA: I2S RX register -> circular buffer write pointer. */
    UDMASetupTransfer(UDMA_CH4_I2S_RX,
                      UDMA_MODE_PINGPONG,
                      MIC_DMA_XFER_ITEMS,
                      UDMA_SIZE_32,
                      UDMA_ARB_8,
                      (void *)I2S_RX_DMA_PORT,
                      UDMA_CHCTL_SRCINC_NONE,
                      (void *)GetWritePtr(gRecordBuffer),
                      UDMA_CHCTL_DSTINC_32);

    /* Alternate RX DMA: starts at the next block in the same buffer. */
    UDMASetupTransfer(UDMA_CH4_I2S_RX | UDMA_ALT_SELECT,
                      UDMA_MODE_PINGPONG,
                      MIC_DMA_XFER_ITEMS,
                      UDMA_SIZE_32,
                      UDMA_ARB_8,
                      (void *)I2S_RX_DMA_PORT,
                      UDMA_CHCTL_SRCINC_NONE,
                      (void *)(GetWritePtr(gRecordBuffer) + MIC_DMA_XFER_BYTES),
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

static void Mic_ClearOldSamples(void)
{
    /* Drop old samples by moving read pointer to current write pointer. */
    if (gRecordBuffer != 0) {
        gRecordBuffer->pucReadPtr = gRecordBuffer->pucWritePtr;
    }
}

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
        Mic_ClearOldSamples();
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

            /* The INMP441 sample is in the second 32-bit word for our wiring/config. */
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
