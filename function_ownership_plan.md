# Function Ownership Plan

**Project:** Edge Voice Authentication System  
**Goal:** Keep the function list small and easy to integrate.

This plan is based on what already exists in the repo. We should avoid creating too many tiny functions. The main idea is to wrap the working code into a few clean modules.

---

# Existing Code We Can Reuse

## Already Available

```c
PinMuxConfig()
```
Configures current pins, including OLED/SPI, UART, PIR GPIO, and I2S pins.

```c
CreateCircularBuffer()
GetBufferSize()
GetWritePtr()
GetReadPtr()
UpdateWritePtr()
UpdateReadPtr()
```
Already implemented in `circ_buff.c/.h`. Use these for mic DMA buffers.

```c
UDMAInit()
UDMAChannelSelect()
UDMASetupTransfer()
```
Already provided by TI SDK common files. Use for mic DMA.

```c
Adafruit_Init()
fillScreen()
setCursor()
Outstr()
drawPixel()
drawLine()
```
Already available for OLED drawing/text.

```c
connectToAccessPoint()
tls_connect()
```
Already available in `utils/network_utils.c/.h`. Use for Wi-Fi/TLS setup.

```c
http_post()
```
Already exists in `main.c` as a static function. We can either reuse its logic or move it into a cloud module later.

```c
MicSetupRxDMA32()
MicSetupTxDMA32()
MicPollDMA32()
```
Currently exist in `main_mic_test.c`. These are the working microphone DMA pieces that should be moved into a mic module.

---

# Elbert

## Focus

- Microphone capture
- Signal processing / voice features
- AWS POST / cloud path
- Final signal-processing validation

## Functions To Implement

```c
def Mic_Init(void) {
    Move the working microphone setup from main_mic_test.c into a reusable module.

    Responsibilities:
    - Configure I2S for INMP441: 16 kHz WS, 32-bit slots, ~1.024 MHz SCK.
    - Initialize uDMA.
    - Create/prepare circular buffers.
    - Configure RX DMA and dummy TX DMA.
    - Enable I2S RX/TX FIFOs.

    Reuse:
    - UDMAInit()
    - UDMAChannelSelect()
    - UDMASetupTransfer()
    - CreateCircularBuffer()
    - MicSetupRxDMA32() logic from main_mic_test.c
    - MicSetupTxDMA32() logic from main_mic_test.c
}
```

```c
def Mic_Record3Sec(short *pcm_out, unsigned long max_samples) {
    Record one fixed 3-second mono audio clip.

    Responsibilities:
    - Start I2S audio capture.
    - Use polled DMA to move microphone samples into pcm_out.
    - Extract the active MIC word from each stereo frame.
    - Store 16-bit signed PCM samples.
    - Return the number of samples recorded.

    Expected output:
    - 16 kHz * 3 seconds = 48000 mono samples.

    Reuse:
    - MicPollDMA32() logic from main_mic_test.c
    - GetBufferSize()
    - UpdateReadPtr()
}
```

```c
def Mic_GetStats(const short *pcm, unsigned long n, long *avg_abs, int *peak_to_peak) {
    Compute basic audio sanity stats.

    Responsibilities:
    - Find min and max sample values.
    - Compute peak-to-peak amplitude.
    - Compute average absolute amplitude.
    - Use this for debugging, silence rejection, and final report measurements.
}
```

```c
def DSP_ExtractFeatures(const short *pcm, unsigned long n, long *features_out) {
    Convert recorded audio into a compact voice feature vector.

    Responsibilities:
    - Remove DC offset.
    - Normalize volume.
    - Compute FFT or simpler spectral features.
    - Keep voice-relevant frequency information.

    Resume value:
    - This is the main signal-processing/ML part of the project.
}
```

```c
def DSP_CompareFeatures(const long *a, const long *b, unsigned long len) {
    Compare two voice feature vectors.

    Responsibilities:
    - Compute cosine similarity or another simple similarity score.
    - Return a scaled score.
    - Used for local authentication or validation experiments.
}
```

```c
def Cloud_Init(void) {
    Prepare Wi-Fi/TLS for AWS communication.

    Responsibilities:
    - Connect to access point.
    - Set time if TLS needs it.
    - Open TLS socket.

    Reuse:
    - connectToAccessPoint()
    - tls_connect()
    - existing AWS/TLS notes from LAB4_AWS_TLS_NOTES.md
}
```

```c
def Cloud_PostAudioOrFeatures(const short *pcm,
                              unsigned long samples,
                              const long *features,
                              unsigned long feature_len,
                              char *response,
                              unsigned long response_len) {
    Send project data to AWS.

    Responsibilities:
    - POST raw PCM or extracted features.
    - Receive response from AWS.
    - Store response text for mode/result parsing.

    Reuse:
    - http_post() logic from main.c
}
```

---

# Camila

## Focus

- PIR sensor
- OLED UI
- Main state machine
- Integration flow

## Functions To Implement

```c
def PIR_Init(void) {
    Configure the PIR motion sensor input.

    Responsibilities:
    - Confirm PIR pin is configured as GPIO input.
    - Initialize any local PIR state variables.
    - Keep this simple for MVP: polling is enough.

    Reuse:
    - PinMuxConfig() already configures PIN_45 as GPIO input.
}
```

```c
def PIR_MotionDetected(void) {
    Return whether PIR currently detects motion.

    Responsibilities:
    - Read the PIR GPIO pin.
    - Return 1 for motion, 0 for no motion.
    - Optional: add simple debounce later.
}
```

```c
def UI_Init(void) {
    Initialize the OLED display.

    Responsibilities:
    - Initialize SPI/OLED.
    - Clear screen.
    - Set default text color/size.

    Reuse:
    - Adafruit_Init()
    - fillScreen()
    - setTextColor()
    - setTextSize()
}
```

```c
def UI_ShowState(int state, int user_id, int score) {
    One display function for all major UI states.

    Responsibilities:
    - Show IDLE.
    - Show GET READY.
    - Show RECORDING.
    - Show PROCESSING.
    - Show UPLOADING.
    - Show PASS/FAIL with score.
    - Show ENROLLED or CLEARED.

    Why one function:
    - Keeps the API small.
    - Main app only needs one UI call.

    Reuse:
    - fillScreen()
    - setCursor()
    - Outstr()
}
```

```c
def App_Init(void) {
    Initialize all project modules in the correct order.

    Responsibilities:
    - Board init.
    - PinMuxConfig().
    - UART terminal.
    - UI_Init().
    - PIR_Init().
    - Mic_Init().
    - Cloud_Init() if using cloud in that build.
}
```

```c
def App_Run(void) {
    Main application loop.

    Responsibilities:
    - Show idle state.
    - Wait for PIR motion.
    - Call App_HandleMotionEvent() when motion is detected.
    - Return to idle after each attempt.
}
```

```c
def App_HandleMotionEvent(void) {
    Run one full demo cycle.

    Responsibilities:
    - UI: get ready.
    - Record 3 seconds using Mic_Record3Sec().
    - Get audio stats using Mic_GetStats().
    - Run DSP_ExtractFeatures().
    - Call Cloud_PostAudioOrFeatures() or local compare.
    - Show result using UI_ShowState().
}
```

---

# Minimal Function Count

## Elbert Owns 7 Main Functions

1. `Mic_Init`
2. `Mic_Record3Sec`
3. `Mic_GetStats`
4. `DSP_ExtractFeatures`
5. `DSP_CompareFeatures`
6. `Cloud_Init`
7. `Cloud_PostAudioOrFeatures`

## Camila Owns 6 Main Functions

1. `PIR_Init`
2. `PIR_MotionDetected`
3. `UI_Init`
4. `UI_ShowState`
5. `App_Init`
6. `App_Run`

## Shared Function

1. `App_HandleMotionEvent`

This is the only function where everyone's modules meet.

---

# Integration Plan

## Step 1: Agree On Headers

Create only these headers:

```text
mic_capture.h
dsp_features.h
cloud_client.h
pir_sensor.h
ui.h
app.h
```

## Step 2: Use Stubs First

Camila can build the UI/state machine before the real mic/cloud are fully integrated by using temporary stub behavior:

```c
Mic_Record3Sec() returns fake samples.
Cloud_PostAudioOrFeatures() returns fake PASS/FAIL.
DSP_ExtractFeatures() returns fake features.
```

## Step 3: Replace Stubs One At A Time

Recommended replacement order:

1. Real PIR.
2. Real OLED states.
3. Real microphone capture.
4. Real DSP stats/features.
5. Real AWS POST.
6. Full final flow.

## Step 4: Keep main.c Simple

Final `main.c` should look roughly like:

```c
int main(void)
{
    App_Init();
    App_Run();
}
```

That is the easiest way to integrate without everyone editing the same file constantly.

