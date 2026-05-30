#ifndef __DSP_FEATURES_H__
#define __DSP_FEATURES_H__

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * Big picture:
 * These functions are small DSP helpers for the audio buffer produced by
 * Mic_RecordSeconds(). They are intentionally simple for integration.
 */

#define DSP_FEATURE_LEN          16
#define DSP_FFT_BIN_COUNT        32
#define DSP_FFT_INPUT_SAMPLES    1024

typedef struct
{
    short min;
    short max;
    int peakToPeak;
    long avgAbs;
} tAudioStats;

void DSP_GetAudioStats(const short *pcm, unsigned long n, tAudioStats *stats);
void DSP_RemoveDC(short *pcm, unsigned long n);
void DSP_NormalizePeak(short *pcm, unsigned long n, short targetPeak);
unsigned long DSP_GetFftBinHz(unsigned long index);
int DSP_ComputeFFT(const short *pcm, unsigned long n, long *magOut, unsigned long magLen);
int DSP_ExtractFeatures(const short *pcm, unsigned long n, long *featuresOut, unsigned long featureLen);
long DSP_CompareFeatures(const long *a, const long *b, unsigned long len);

#ifdef __cplusplus
}
#endif

#endif
