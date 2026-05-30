#include "dsp_features.h"

/*
 * Big picture of this file:
 *
 * 1. DSP_GetAudioStats() checks whether the recording looks alive.
 * 2. DSP_RemoveDC() recenters audio around zero.
 * 3. DSP_NormalizePeak() makes quiet/loud recordings easier to compare.
 * 4. DSP_ComputeFFT() estimates selected voice-band frequency bins.
 * 5. DSP_ExtractFeatures() creates a simple starter feature vector.
 * 6. DSP_CompareFeatures() compares two feature vectors with cosine similarity.
 *
 * DSP_ComputeFFT() uses Goertzel math instead of a full FFT array. Goertzel is
 * equivalent to evaluating selected DFT/FFT bins, which keeps this embedded
 * version small and easy to explain.
 */

static const unsigned long gFftBinHz[DSP_FFT_BIN_COUNT] = {
    125, 250, 375, 500, 625, 750, 875, 1000,
    1125, 1250, 1375, 1500, 1625, 1750, 1875, 2000,
    2125, 2250, 2375, 2500, 2625, 2750, 2875, 3000,
    3125, 3250, 3375, 3500, 3625, 3750, 3875, 4000
};

/*
 * Q14 coefficients for 2*cos(2*pi*k/1024), where k matches the bins above
 * at 16 kHz sample rate.
 */
static const int gGoertzelCoeffQ14[DSP_FFT_BIN_COUNT] = {
    32729, 32610, 32413, 32138, 31786, 31357, 30853, 30274,
    29622, 28899, 28106, 27246, 26320, 25330, 24279, 23170,
    22006, 20788, 19520, 18205, 16846, 15447, 14010, 12540,
    11039, 9512, 7962, 6393, 4808, 3212, 1608, 0
};

static long AbsLong(long value)
{
    /* Return positive magnitude of a signed long. */
    return (value < 0) ? -value : value;
}

static short ClampShort(long value)
{
    /* Keep value inside signed 16-bit range. */
    if (value > 32767) {
        return 32767;
    }

    /* Keep value inside signed 16-bit range. */
    if (value < -32768) {
        return -32768;
    }

    /* Safe to cast after clamping. */
    return (short)value;
}

static long IntegerSqrt(long value)
{
    /* Integer square root, avoids floating point on CC3200. */
    long bit = 1L << 30;
    long result = 0;

    /* sqrt(0) and sqrt(negative) are treated as zero. */
    if (value <= 0) {
        return 0;
    }

    /* Find the highest power-of-four bit that fits in value. */
    while (bit > value) {
        bit >>= 2;
    }

    /* Build square root one bit-pair at a time. */
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }

    return result;
}

static long GoertzelMagnitude(const short *pcm, unsigned long n, int coeffQ14)
{
    unsigned long i;
    long q0 = 0;
    long q1 = 0;
    long q2 = 0;

    /*
     * Use only DSP_FFT_INPUT_SAMPLES samples so each printed bin is stable and
     * easy to compare. Shift input down strongly enough to keep the 32-bit
     * coefficient multiply safe on CC3200.
     */
    for (i = 0; i < DSP_FFT_INPUT_SAMPLES && i < n; i++) {
        q0 = ((long)pcm[i] >> 8) + (((long)coeffQ14 * q1) >> 14) - q2;
        q2 = q1;
        q1 = q0;
    }

    /*
     * Approximate magnitude without squaring. This is intentionally simple for
     * the CC3200 and for UART debug output.
     */
    return AbsLong(q1) + AbsLong(q2);
}

void DSP_GetAudioStats(const short *pcm, unsigned long n, tAudioStats *stats)
{
    unsigned long i;
    short minValue = 32767;
    short maxValue = -32768;
    long sumAbs = 0;

    /* Require valid input. */
    if ((pcm == 0) || (stats == 0) || (n == 0)) {
        return;
    }

    /* Scan every sample once. */
    for (i = 0; i < n; i++) {
        short sample = pcm[i];

        /* Track smallest sample. */
        if (sample < minValue) {
            minValue = sample;
        }

        /* Track largest sample. */
        if (sample > maxValue) {
            maxValue = sample;
        }

        /* Sum magnitudes for average absolute amplitude. */
        sumAbs += AbsLong(sample);
    }

    /* Store results for caller. */
    stats->min = minValue;
    stats->max = maxValue;
    stats->peakToPeak = maxValue - minValue;
    stats->avgAbs = sumAbs / (long)n;
}

void DSP_RemoveDC(short *pcm, unsigned long n)
{
    unsigned long i;
    long sum = 0;
    long mean;

    /* Require valid input. */
    if ((pcm == 0) || (n == 0)) {
        return;
    }

    /* Add all samples to estimate DC offset. */
    for (i = 0; i < n; i++) {
        sum += pcm[i];
    }

    /* Average sample value is the DC offset. */
    mean = sum / (long)n;

    /* Subtract DC offset from every sample. */
    for (i = 0; i < n; i++) {
        pcm[i] = ClampShort((long)pcm[i] - mean);
    }
}

void DSP_NormalizePeak(short *pcm, unsigned long n, short targetPeak)
{
    unsigned long i;
    long peak = 0;

    /* Require valid input and a positive target. */
    if ((pcm == 0) || (n == 0) || (targetPeak <= 0)) {
        return;
    }

    /* Find current peak magnitude. */
    for (i = 0; i < n; i++) {
        long mag = AbsLong(pcm[i]);
        if (mag > peak) {
            peak = mag;
        }
    }

    /* Silent audio cannot be normalized. */
    if (peak == 0) {
        return;
    }

    /* Scale every sample so the peak reaches targetPeak. */
    for (i = 0; i < n; i++) {
        pcm[i] = ClampShort(((long)pcm[i] * targetPeak) / peak);
    }
}

int DSP_ComputeFFT(const short *pcm, unsigned long n, long *magOut, unsigned long magLen)
{
    unsigned long i;

    /* Require enough audio and output space for the fixed bin list. */
    if ((pcm == 0) || (magOut == 0) ||
        (n < DSP_FFT_INPUT_SAMPLES) ||
        (magLen < DSP_FFT_BIN_COUNT)) {
        return -1;
    }

    /* Evaluate each selected voice-band frequency bin. */
    for (i = 0; i < DSP_FFT_BIN_COUNT; i++) {
        magOut[i] = GoertzelMagnitude(pcm, n, gGoertzelCoeffQ14[i]);
    }

    return 0;
}

unsigned long DSP_GetFftBinHz(unsigned long index)
{
    /* Return zero for invalid bin index. */
    if (index >= DSP_FFT_BIN_COUNT) {
        return 0;
    }

    return gFftBinHz[index];
}

int DSP_ExtractFeatures(const short *pcm, unsigned long n, long *featuresOut, unsigned long featureLen)
{
    unsigned long i;
    unsigned long blockSize;

    /* Require valid buffers and nonzero sizes. */
    if ((pcm == 0) || (featuresOut == 0) || (featureLen == 0) || (n == 0)) {
        return -1;
    }

    /* Split audio into equal blocks. */
    blockSize = n / featureLen;
    if (blockSize == 0) {
        return -1;
    }

    /* Each feature is RMS energy from one time block. */
    for (i = 0; i < featureLen; i++) {
        unsigned long j;
        unsigned long start = i * blockSize;
        unsigned long end = start + blockSize;
        long sumSquares = 0;

        /* Sum sample energy in this block. */
        for (j = start; (j < end) && (j < n); j++) {
            long sample = ((long)pcm[j]) >> 4;
            sumSquares += sample * sample;
        }

        /* Scaled RMS = sqrt(mean(square)). The shift above avoids overflow. */
        featuresOut[i] = IntegerSqrt(sumSquares / (long)blockSize);
    }

    return 0;
}

long DSP_CompareFeatures(const long *a, const long *b, unsigned long len)
{
    unsigned long i;
    long dot = 0;
    long normA = 0;
    long normB = 0;
    long denom;

    /* Require valid feature vectors. */
    if ((a == 0) || (b == 0) || (len == 0)) {
        return 0;
    }

    /* Compute dot product and vector magnitudes. */
    for (i = 0; i < len; i++) {
        dot += a[i] * b[i];
        normA += a[i] * a[i];
        normB += b[i] * b[i];
    }

    /* Cosine denominator = |a| * |b|. */
    denom = IntegerSqrt(normA) * IntegerSqrt(normB);
    if (denom == 0) {
        return 0;
    }

    /* Return scaled cosine similarity. 10000 means approximately 1.0. */
    return (dot * 10000L) / denom;
}
