#include <stdio.h>
#include <string.h>

#include "hw_types.h"
#include "hw_ints.h"
#include "hw_memmap.h"
#include "hw_common_reg.h"
#include "rom.h"
#include "rom_map.h"
#include "interrupt.h"
#include "prcm.h"
#include "utils.h"
#include "i2s.h"
#include "udma.h"

#include "common.h"
#include "uart_if.h"
#include "udma_if.h"
#include "i2s_if.h"

#include "pin_mux_config.h"
#include "circ_buff.h"

#define SAMPLE_RATE_HZ      16000
#define CHANNELS            2
#define BITS_PER_SLOT       32
#define BYTES_PER_SAMPLE    4
#define I2S_CLK_HZ          (SAMPLE_RATE_HZ * CHANNELS * BITS_PER_SLOT)
#define WINDOW_FRAMES       4096
#define WINDOW_BYTES        (WINDOW_FRAMES * CHANNELS * BYTES_PER_SAMPLE)
#define DMA_WINDOW_BYTES    WINDOW_BYTES

#define DMA_BUF_BYTES       (16 * 1024)
#define DMA_XFER_ITEMS      256
#define DMA_XFER_BYTES      (DMA_XFER_ITEMS * BYTES_PER_SAMPLE)
#define READ_CHUNK_BYTES    512
#define DMA_MIC_WORD_INDEX  1

#define MIC_CAPTURE_CPU_POLL          1
#define MIC_CAPTURE_DMA_TI_EXPERIMENT 2

/*
 * Keep CPU_POLL as the known-good INMP441 path. DMA_TI_EXPERIMENT follows the
 * setup order from C:\ti\CC3200SDK_1.5.0\cc3200-sdk\example\wifi_audio_app,
 * but keeps 32-bit I2S slots for the INMP441.
 */
#define MIC_CAPTURE_MODE    MIC_CAPTURE_DMA_TI_EXPERIMENT

#define I2S_DMA_RX_DONE     0x00000010
#define I2S_DMA_TX_DONE     0x00000020

#ifndef I2S_SLOT_SIZE_32
#define I2S_SLOT_SIZE_32    0x00F000F8
#endif

long g_recording[WINDOW_FRAMES * CHANNELS];
static unsigned char g_zeroTx[DMA_XFER_BYTES];
static volatile unsigned long g_i2sIrqCount = 0;
static volatile unsigned long g_rxDmaCount = 0;
static volatile unsigned long g_txDmaCount = 0;
static volatile unsigned long g_lastDmaStatus = 0;
static volatile unsigned long g_lastI2SStatus = 0;

tCircularBuffer *pRecordBuffer = NULL;
tCircularBuffer *pPlayBuffer   = NULL;

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

static void MicSetupRxDMA32(tCircularBuffer *pRecord)
{
    UDMASetupTransfer(UDMA_CH4_I2S_RX,
                      UDMA_MODE_PINGPONG,
                      DMA_XFER_ITEMS,
                      UDMA_SIZE_32,
                      UDMA_ARB_8,
                      (void *)I2S_RX_DMA_PORT,
                      UDMA_CHCTL_SRCINC_NONE,
                      (void *)GetWritePtr(pRecord),
                      UDMA_CHCTL_DSTINC_32);

    UDMASetupTransfer(UDMA_CH4_I2S_RX | UDMA_ALT_SELECT,
                      UDMA_MODE_PINGPONG,
                      DMA_XFER_ITEMS,
                      UDMA_SIZE_32,
                      UDMA_ARB_8,
                      (void *)I2S_RX_DMA_PORT,
                      UDMA_CHCTL_SRCINC_NONE,
                      (void *)(GetWritePtr(pRecord) + DMA_XFER_BYTES),
                      UDMA_CHCTL_DSTINC_32);

    MAP_uDMAChannelAttributeDisable(UDMA_CH4_I2S_RX,
                                    UDMA_ATTR_USEBURST | UDMA_ATTR_REQMASK);
}

static void MicSetupTxDMA32(void)
{
    UDMASetupTransfer(UDMA_CH5_I2S_TX,
                      UDMA_MODE_PINGPONG,
                      DMA_XFER_ITEMS,
                      UDMA_SIZE_32,
                      UDMA_ARB_8,
                      (void *)&g_zeroTx[0],
                      UDMA_CHCTL_SRCINC_32,
                      (void *)I2S_TX_DMA_PORT,
                      UDMA_DST_INC_NONE);

    UDMASetupTransfer(UDMA_CH5_I2S_TX | UDMA_ALT_SELECT,
                      UDMA_MODE_PINGPONG,
                      DMA_XFER_ITEMS,
                      UDMA_SIZE_32,
                      UDMA_ARB_8,
                      (void *)&g_zeroTx[0],
                      UDMA_CHCTL_SRCINC_32,
                      (void *)I2S_TX_DMA_PORT,
                      UDMA_DST_INC_NONE);

    MAP_uDMAChannelAttributeDisable(UDMA_CH5_I2S_TX,
                                    UDMA_ATTR_USEBURST | UDMA_ATTR_REQMASK);
}

static void MicDMA32Callback(void)
{
    tDMAControlTable *pControlTable;
    unsigned long dmaStatus;

    g_i2sIrqCount++;
    dmaStatus = MAP_uDMAIntStatus();
    g_lastDmaStatus = dmaStatus;
    g_lastI2SStatus = I2SIntStatus(I2S_BASE);
    pControlTable = MAP_uDMAControlBaseGet();

    if (dmaStatus & I2S_DMA_RX_DONE) {
        I2SIntClear(I2S_BASE, I2S_INT_RDMA);

        if ((pControlTable[0x4].ulControl & UDMA_CHCTL_XFERMODE_M) == 0) {
            MAP_uDMAChannelTransferSet(UDMA_CH4_I2S_RX,
                                       UDMA_MODE_PINGPONG,
                                       (void *)I2S_RX_DMA_PORT,
                                       (void *)GetWritePtr(pRecordBuffer),
                                       DMA_XFER_ITEMS);
            MAP_uDMAChannelEnable(UDMA_CH4_I2S_RX);
            UpdateWritePtr(pRecordBuffer, DMA_XFER_BYTES);
            g_rxDmaCount++;
        } else if ((pControlTable[0x24].ulControl & UDMA_CHCTL_XFERMODE_M) == 0) {
            MAP_uDMAChannelTransferSet(UDMA_CH4_I2S_RX | UDMA_ALT_SELECT,
                                       UDMA_MODE_PINGPONG,
                                       (void *)I2S_RX_DMA_PORT,
                                       (void *)GetWritePtr(pRecordBuffer),
                                       DMA_XFER_ITEMS);
            MAP_uDMAChannelEnable(UDMA_CH4_I2S_RX | UDMA_ALT_SELECT);
            UpdateWritePtr(pRecordBuffer, DMA_XFER_BYTES);
            g_rxDmaCount++;
        }
    }

    if (dmaStatus & I2S_DMA_TX_DONE) {
        I2SIntClear(I2S_BASE, I2S_INT_XDMA);

        if ((pControlTable[0x5].ulControl & UDMA_CHCTL_XFERMODE_M) == 0) {
            MAP_uDMAChannelTransferSet(UDMA_CH5_I2S_TX,
                                       UDMA_MODE_PINGPONG,
                                       (void *)&g_zeroTx[0],
                                       (void *)I2S_TX_DMA_PORT,
                                       DMA_XFER_ITEMS);
            MAP_uDMAChannelEnable(UDMA_CH5_I2S_TX);
            g_txDmaCount++;
        } else if ((pControlTable[0x25].ulControl & UDMA_CHCTL_XFERMODE_M) == 0) {
            MAP_uDMAChannelTransferSet(UDMA_CH5_I2S_TX | UDMA_ALT_SELECT,
                                       UDMA_MODE_PINGPONG,
                                       (void *)&g_zeroTx[0],
                                       (void *)I2S_TX_DMA_PORT,
                                       DMA_XFER_ITEMS);
            MAP_uDMAChannelEnable(UDMA_CH5_I2S_TX | UDMA_ALT_SELECT);
            g_txDmaCount++;
        }
    }
}

static void MicPollDMA32(void)
{
    tDMAControlTable *pControlTable = MAP_uDMAControlBaseGet();

    if ((pControlTable[0x4].ulControl & UDMA_CHCTL_XFERMODE_M) == 0) {
        MAP_uDMAChannelTransferSet(UDMA_CH4_I2S_RX,
                                   UDMA_MODE_PINGPONG,
                                   (void *)I2S_RX_DMA_PORT,
                                   (void *)GetWritePtr(pRecordBuffer),
                                   DMA_XFER_ITEMS);
        MAP_uDMAChannelEnable(UDMA_CH4_I2S_RX);
        UpdateWritePtr(pRecordBuffer, DMA_XFER_BYTES);
        g_rxDmaCount++;
    }

    if ((pControlTable[0x24].ulControl & UDMA_CHCTL_XFERMODE_M) == 0) {
        MAP_uDMAChannelTransferSet(UDMA_CH4_I2S_RX | UDMA_ALT_SELECT,
                                   UDMA_MODE_PINGPONG,
                                   (void *)I2S_RX_DMA_PORT,
                                   (void *)GetWritePtr(pRecordBuffer),
                                   DMA_XFER_ITEMS);
        MAP_uDMAChannelEnable(UDMA_CH4_I2S_RX | UDMA_ALT_SELECT);
        UpdateWritePtr(pRecordBuffer, DMA_XFER_BYTES);
        g_rxDmaCount++;
    }

    if ((pControlTable[0x5].ulControl & UDMA_CHCTL_XFERMODE_M) == 0) {
        MAP_uDMAChannelTransferSet(UDMA_CH5_I2S_TX,
                                   UDMA_MODE_PINGPONG,
                                   (void *)&g_zeroTx[0],
                                   (void *)I2S_TX_DMA_PORT,
                                   DMA_XFER_ITEMS);
        MAP_uDMAChannelEnable(UDMA_CH5_I2S_TX);
        g_txDmaCount++;
    }

    if ((pControlTable[0x25].ulControl & UDMA_CHCTL_XFERMODE_M) == 0) {
        MAP_uDMAChannelTransferSet(UDMA_CH5_I2S_TX | UDMA_ALT_SELECT,
                                   UDMA_MODE_PINGPONG,
                                   (void *)&g_zeroTx[0],
                                   (void *)I2S_TX_DMA_PORT,
                                   DMA_XFER_ITEMS);
        MAP_uDMAChannelEnable(UDMA_CH5_I2S_TX | UDMA_ALT_SELECT);
        g_txDmaCount++;
    }
}

int main(void)
{
    unsigned long iter = 0;

    BoardInit();
    PinMuxConfig();
    InitTerm();
    ClearTerm();

    UART_PRINT("\n\r=== INMP441 Live Mic Monitor ===\n\r");
    UART_PRINT("Tap or talk into the mic. Watch L-ch p-p / |avg| jump.\n\r");
    UART_PRINT("Window = %d frames = %d ms\n\n\r",
               WINDOW_FRAMES, (WINDOW_FRAMES * 1000) / SAMPLE_RATE_HZ);

    UART_PRINT("step 1: CreateCircularBuffer...\n\r");
    pRecordBuffer = CreateCircularBuffer(DMA_BUF_BYTES);
    pPlayBuffer   = CreateCircularBuffer(DMA_BUF_BYTES);
    if (pRecordBuffer == NULL || pPlayBuffer == NULL) {
        UART_PRINT("ERR: CreateCircularBuffer\n\r");
        LOOP_FOREVER();
    }
    UART_PRINT("step 1 OK\n\r");

    UART_PRINT("step 2: AudioInit...\n\r");
    AudioInit();
    UART_PRINT("step 2 OK\n\r");

    UART_PRINT("step 3: UDMAInit...\n\r");
#if MIC_CAPTURE_MODE == MIC_CAPTURE_CPU_POLL
    UART_PRINT("step 3 skipped for CPU-poll diagnostic\n\r");
#else
    UDMAInit();
    UART_PRINT("step 3 OK\n\r");
#endif

    UART_PRINT("step 4: TX DMA setup...\n\r");
#if MIC_CAPTURE_MODE == MIC_CAPTURE_CPU_POLL
    UART_PRINT("step 4 skipped for CPU-poll diagnostic\n\r");
#else
    UDMAChannelSelect(UDMA_CH5_I2S_TX, NULL);
    MicSetupTxDMA32();
    UART_PRINT("step 4 OK\n\r");
#endif

    UART_PRINT("step 5: RX DMA setup...\n\r");
#if MIC_CAPTURE_MODE == MIC_CAPTURE_CPU_POLL
    UART_PRINT("step 5 skipped for CPU-poll diagnostic\n\r");
#else
    UDMAChannelSelect(UDMA_CH4_I2S_RX, NULL);
    MicSetupRxDMA32(pRecordBuffer);
    UART_PRINT("step 5 OK\n\r");
#endif

    UART_PRINT("step 6: I2S FIFO setup...\n\r");
#if MIC_CAPTURE_MODE == MIC_CAPTURE_CPU_POLL
    MAP_I2SIntClear(I2S_BASE, 0xFFFFFFFF);
    MAP_I2SRxFIFOEnable(I2S_BASE, 8, 1);
    MAP_I2STxFIFOEnable(I2S_BASE, 8, 1);
#else
    UART_PRINT("step 6 deferred until after I2S format config\n\r");
#endif
    UART_PRINT("step 6 OK\n\r");

    UART_PRINT("step 7: I2S 32-bit slot configure...\n\r");
    MAP_PRCMI2SClockFreqSet(I2S_CLK_HZ);
    MAP_I2SConfigSetExpClk(I2S_BASE,
                           I2S_CLK_HZ,
                           I2S_CLK_HZ,
                           I2S_SLOT_SIZE_32 | I2S_PORT_DMA);
    MAP_I2SSerializerConfig(I2S_BASE,
                            I2S_DATA_LINE_1,
                            I2S_SER_MODE_RX,
                            I2S_INACT_LOW_LEVEL);
    MAP_I2SSerializerConfig(I2S_BASE,
                            I2S_DATA_LINE_0,
                            I2S_SER_MODE_TX,
                            I2S_INACT_LOW_LEVEL);
#if MIC_CAPTURE_MODE == MIC_CAPTURE_DMA_TI_EXPERIMENT
    UART_PRINT("step 7a: clear I2S status...\n\r");
    MAP_I2SIntClear(I2S_BASE, I2S_INT_RDMA | I2S_INT_XDMA |
                              I2S_INT_ROVRN | I2S_INT_RSYNCERR |
                              I2S_INT_XUNDRN | I2S_INT_XSYNCERR);
    UART_PRINT("step 7b: enable RX/TX FIFOs...\n\r");
    MAP_I2SRxFIFOEnable(I2S_BASE, 8, 1);
    MAP_I2STxFIFOEnable(I2S_BASE, 8, 1);
#endif
    UART_PRINT("step 7 OK\n\r");

    UART_PRINT("step 8: Audio_Start...\n\r");
    Audio_Start(I2S_MODE_RX_TX);
    UART_PRINT("Audio_Start done. Waiting for samples...\n\r");

#if MIC_CAPTURE_MODE == MIC_CAPTURE_DMA_TI_EXPERIMENT
    MAP_uDMAChannelRequest(UDMA_CH4_I2S_RX);
    MAP_uDMAChannelRequest(UDMA_CH5_I2S_TX);
    UART_PRINT("DMA software kick issued for RX/TX channels.\n\r");
#endif

#if MIC_CAPTURE_MODE == MIC_CAPTURE_CPU_POLL
    while (1) {
        unsigned int frames = 0;
        unsigned long timeout = 0;
        long lsum_abs = 0;
        short lmin = 32767, lmax = -32768;
        short rmin = 32767, rmax = -32768;
        unsigned long firstRaw = 0;
        unsigned long lastRaw = 0;

        while (frames < WINDOW_FRAMES) {
            if (I2SRxFIFOStatusGet(I2S_BASE) > 0) {
                unsigned long rawL = HWREG(I2S_RX_DMA_PORT);
                unsigned long rawR = 0;
                short L;
                short R;

                if (I2SRxFIFOStatusGet(I2S_BASE) > 0) {
                    rawR = HWREG(I2S_RX_DMA_PORT);
                }

                if (frames == 0) {
                    firstRaw = rawL;
                }
                lastRaw = rawL;

                L = (short)(rawL >> 16);
                R = (short)(rawR >> 16);

                if (L < lmin) lmin = L;
                if (L > lmax) lmax = L;
                if (R < rmin) rmin = R;
                if (R > rmax) rmax = R;
                lsum_abs += (L < 0) ? -L : L;
                frames++;
                timeout = 0;
            } else {
                timeout++;
                if ((timeout & 0x7FFFFF) == 0) {
                    UART_PRINT("  poll stall: frames=%u i2s=%08lx rfifo=%lu first=%08lx last=%08lx\n\r",
                               frames,
                               I2SIntStatus(I2S_BASE),
                               I2SRxFIFOStatusGet(I2S_BASE),
                               firstRaw,
                               lastRaw);
                }
            }
        }

        UART_PRINT("[%4lu] CPU L: min=%6d max=%6d p-p=%6d |avg|=%5ld   R: p-p=%6d raw=%08lx/%08lx\n\r",
                   iter++,
                   lmin,
                   lmax,
                   lmax - lmin,
                   lsum_abs / WINDOW_FRAMES,
                   rmax - rmin,
                   firstRaw,
                   lastRaw);
    }
#else
    unsigned long stall = 0;
    while (1) {
        unsigned int bytes = 0;
        unsigned char *dest = (unsigned char *)g_recording;

        while (bytes < DMA_WINDOW_BYTES) {
            MicPollDMA32();
            unsigned int avail = GetBufferSize(pRecordBuffer);
            if (avail >= (2 * READ_CHUNK_BYTES)) {
                unsigned int n = READ_CHUNK_BYTES;
                if (bytes + n > DMA_WINDOW_BYTES) {
                    n = DMA_WINDOW_BYTES - bytes;
                }
                memcpy(dest + bytes, pRecordBuffer->pucReadPtr, n);
                UpdateReadPtr(pRecordBuffer, READ_CHUNK_BYTES);
                bytes += n;
                stall = 0;
            } else {
                stall++;
                if ((stall & 0x7FFFFF) == 0) {
                    tDMAControlTable *ctl = MAP_uDMAControlBaseGet();
                    UART_PRINT("  stall: avail=%u  W=%p  R=%p  irq=%lu rx=%lu tx=%lu\n\r",
                               avail,
                               (void*)pRecordBuffer->pucWritePtr,
                               (void*)pRecordBuffer->pucReadPtr,
                               g_i2sIrqCount,
                               g_rxDmaCount,
                               g_txDmaCount);
                    UART_PRINT("         dma=%08lx i2s=%08lx rfifo=%lu wfifo=%lu rxCtl=%08lx/%08lx txCtl=%08lx/%08lx en=%u/%u\n\r",
                               g_lastDmaStatus,
                               I2SIntStatus(I2S_BASE),
                               I2SRxFIFOStatusGet(I2S_BASE),
                               I2STxFIFOStatusGet(I2S_BASE),
                               ctl[0x4].ulControl,
                               ctl[0x24].ulControl,
                               ctl[0x5].ulControl,
                               ctl[0x25].ulControl,
                               MAP_uDMAChannelIsEnabled(UDMA_CH4_I2S_RX),
                               MAP_uDMAChannelIsEnabled(UDMA_CH5_I2S_TX));
                    MAP_uDMAChannelRequest(UDMA_CH4_I2S_RX);
                    MAP_uDMAChannelRequest(UDMA_CH5_I2S_TX);
                }
            }
        }

        short micMin = 32767, micMax = -32768;
        short otherMin = 32767, otherMax = -32768;
        long micSumAbs = 0;
        unsigned long firstRaw = 0;
        unsigned long lastRaw = 0;
        int i;
        for (i = 0; i < WINDOW_FRAMES; i++) {
            unsigned long rawOther = (unsigned long)g_recording[(2 * i) + (1 - DMA_MIC_WORD_INDEX)];
            unsigned long rawMic = (unsigned long)g_recording[(2 * i) + DMA_MIC_WORD_INDEX];
            short other = (short)(rawOther >> 16);
            short mic = (short)(rawMic >> 16);

            if (i == 0) {
                firstRaw = rawMic;
            }
            lastRaw = rawMic;

            if (mic < micMin) micMin = mic;
            if (mic > micMax) micMax = mic;
            if (other < otherMin) otherMin = other;
            if (other > otherMax) otherMax = other;
            micSumAbs += (mic < 0) ? -mic : mic;
        }

        UART_PRINT("[%4lu] DMA32 MIC: min=%6d max=%6d p-p=%6d |avg|=%5ld   other p-p=%6d raw=%08lx/%08lx rx=%lu tx=%lu\n\r",
                   iter++,
                   micMin,
                   micMax,
                   micMax - micMin,
                   micSumAbs / WINDOW_FRAMES,
                   otherMax - otherMin,
                   firstRaw,
                   lastRaw,
                   g_rxDmaCount,
                   g_txDmaCount);
    }
#endif
}
