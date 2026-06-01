#ifndef __MIC_CAPTURE_H__
#define __MIC_CAPTURE_H__

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * Big picture:
 * - The INMP441 sends I2S audio.
 * - CC3200 I2S receives 32-bit stereo frames.
 * - uDMA copies those frames into a circular RAM buffer.
 * - Mic_RecordSeconds() pulls the active mic word out as signed 16-bit PCM.
 */

#define MIC_SAMPLE_RATE_HZ      16000

typedef struct
{
    unsigned long rxDmaCount;      /* Number of RX DMA blocks completed. */
    unsigned long txDmaCount;      /* Number of dummy TX DMA blocks completed. */
    unsigned long droppedSamples;  /* Samples not captured before timeout. */
} tMicCaptureStats;

int Mic_Init(void);
int Mic_RecordSamples(short *pcmOut, unsigned long maxSamples, unsigned long samples);
int Mic_RecordMoreSamples(short *pcmOut, unsigned long maxSamples, unsigned long samples);
int Mic_RecordSeconds(short *pcmOut, unsigned long maxSamples, unsigned long seconds);
void Mic_GetDmaStats(tMicCaptureStats *stats);
void Mic_RunMonitor(void);

#if MIC_ENABLE_RECORD_TEST
void Mic_RunRecordTest(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
