import base64
import io
import json
import re
import time
import urllib.request
import uuid
import wave

import boto3
import numpy as np


BUCKET = "audio-auth-cc3200"

SAMPLE_RATE = 16000
CHANNELS = 1
SAMPLE_WIDTH_BYTES = 2

FFT_SIZE = 1024
VOICE_MIN_HZ = 100
VOICE_MAX_HZ = 4000
FEATURE_LEN = 32

USER_ID = 1
PROFILE_KEY = f"profiles/user_{USER_ID}.json"

NOISY_AVG_ABS = 14000
NOISY_PEAK_TO_PEAK = 62000
QUIET_AVG_ABS = 20
QUIET_PEAK_TO_PEAK = 1000
AUTH_PASS_SCORE = 75

TRANSCRIBE_WAIT_SECONDS = 26
TRANSCRIBE_POLL_SECONDS = 2

s3 = boto3.client("s3")
transcribe = boto3.client("transcribe")


def response(status_code, body):
    return {
        "statusCode": status_code,
        "headers": {"Content-Type": "application/json"},
        "body": json.dumps(body),
    }


def get_audio_bytes(event):
    body = event.get("body") or ""

    if event.get("isBase64Encoded", False):
        return base64.b64decode(body)

    if isinstance(body, bytes):
        return body

    return body.encode("latin1")


def make_wav_bytes(pcm_bytes):
    wav_buffer = io.BytesIO()

    with wave.open(wav_buffer, "wb") as wav_file:
        wav_file.setnchannels(CHANNELS)
        wav_file.setsampwidth(SAMPLE_WIDTH_BYTES)
        wav_file.setframerate(SAMPLE_RATE)
        wav_file.writeframes(pcm_bytes)

    return wav_buffer.getvalue()


def pcm_samples(audio_bytes):
    if len(audio_bytes) < 2:
        return np.array([], dtype=np.float32)

    even_length = len(audio_bytes) - (len(audio_bytes) % 2)
    return np.frombuffer(audio_bytes[:even_length], dtype="<i2").astype(np.float32)


def check_audio_level(audio_bytes):
    samples = pcm_samples(audio_bytes)

    if len(samples) == 0:
        return "too_quiet", {"avg_abs": 0, "peak_to_peak": 0}

    avg_abs = float(np.mean(np.abs(samples)))
    peak_to_peak = float(np.max(samples) - np.min(samples))
    info = {"avg_abs": int(avg_abs), "peak_to_peak": int(peak_to_peak)}

    if avg_abs < QUIET_AVG_ABS or peak_to_peak < QUIET_PEAK_TO_PEAK:
        return "too_quiet", info

    if avg_abs > NOISY_AVG_ABS or peak_to_peak > NOISY_PEAK_TO_PEAK:
        return "too_noisy", info

    return "ok", info


def extract_fft_features(audio_bytes):
    samples = pcm_samples(audio_bytes)

    if len(samples) < FFT_SIZE:
        return [0.0] * FEATURE_LEN

    samples = samples - np.mean(samples)

    peak = np.max(np.abs(samples))
    if peak > 0:
        samples = samples / peak

    freqs = np.fft.rfftfreq(FFT_SIZE, d=1.0 / SAMPLE_RATE)
    voice_mask = (freqs >= VOICE_MIN_HZ) & (freqs <= VOICE_MAX_HZ)

    spectra = []
    for start in range(0, len(samples) - FFT_SIZE + 1, FFT_SIZE):
        frame = samples[start:start + FFT_SIZE] * np.hanning(FFT_SIZE)
        magnitude = np.abs(np.fft.rfft(frame))
        spectra.append(magnitude[voice_mask])

    if not spectra:
        return [0.0] * FEATURE_LEN

    avg_spectrum = np.mean(np.array(spectra), axis=0)
    bands = np.array_split(avg_spectrum, FEATURE_LEN)
    features = np.array([np.mean(band) for band in bands], dtype=np.float32)

    norm = np.linalg.norm(features)
    if norm > 0:
        features = features / norm

    return features.tolist()


def cosine_score(features_a, features_b):
    a = np.array(features_a, dtype=np.float32)
    b = np.array(features_b, dtype=np.float32)

    denom = np.linalg.norm(a) * np.linalg.norm(b)
    if denom == 0:
        return 0

    return int(100 * np.dot(a, b) / denom)


def load_profile():
    try:
        obj = s3.get_object(Bucket=BUCKET, Key=PROFILE_KEY)
        return json.loads(obj["Body"].read().decode("utf-8"))
    except Exception:
        return None


def save_profile(features):
    s3.put_object(
        Bucket=BUCKET,
        Key=PROFILE_KEY,
        Body=json.dumps({"user_id": USER_ID, "features": features}),
        ContentType="application/json",
    )


def clear_profile():
    try:
        s3.delete_object(Bucket=BUCKET, Key=PROFILE_KEY)
    except Exception:
        pass


def transcribe_english(wav_key):
    job_name = f"cc3200-{int(time.time())}-{uuid.uuid4().hex[:8]}"
    media_uri = f"s3://{BUCKET}/{wav_key}"

    transcribe.start_transcription_job(
        TranscriptionJobName=job_name,
        Media={"MediaFileUri": media_uri},
        MediaFormat="wav",
        MediaSampleRateHertz=SAMPLE_RATE,
        LanguageCode="en-US",
    )

    deadline = time.time() + TRANSCRIBE_WAIT_SECONDS

    while time.time() < deadline:
        job = transcribe.get_transcription_job(TranscriptionJobName=job_name)
        job_info = job["TranscriptionJob"]
        status = job_info["TranscriptionJobStatus"]

        if status == "COMPLETED":
            uri = job_info["Transcript"]["TranscriptFileUri"]
            with urllib.request.urlopen(uri, timeout=10) as transcript_file:
                data = json.loads(transcript_file.read().decode("utf-8"))

            try:
                transcribe.delete_transcription_job(TranscriptionJobName=job_name)
            except Exception:
                pass

            return data["results"]["transcripts"][0]["transcript"].strip()

        if status == "FAILED":
            reason = job_info.get("FailureReason", "unknown")
            raise RuntimeError(reason)

        time.sleep(TRANSCRIBE_POLL_SECONDS)

    raise TimeoutError("transcription timed out")


def choose_command(transcript):
    words = re.findall(r"[a-z']+", transcript.lower())

    if "apple" in words:
        return "enroll", "apple"

    if "clear" in words:
        return "clear", "clear"

    if words:
        return "auth", words[0]

    return "auth", ""


def process_audio(audio_bytes, mode):
    audio_level, audio_info = check_audio_level(audio_bytes)
    if audio_level != "ok":
        return {
            "ok": False,
            "mode": mode,
            "result": audio_level,
            "word": "",
            "audio": audio_info,
        }

    timestamp = int(time.time())
    wav_key = f"uploads/{timestamp}_{uuid.uuid4().hex[:8]}.wav"
    wav_bytes = make_wav_bytes(audio_bytes)

    s3.put_object(
        Bucket=BUCKET,
        Key=wav_key,
        Body=wav_bytes,
        ContentType="audio/wav",
    )

    transcript = transcribe_english(wav_key)
    command, word = choose_command(transcript)
    features = extract_fft_features(audio_bytes)

    if command == "enroll":
        save_profile(features)
        return {
            "ok": True,
            "mode": mode,
            "wav_key": wav_key,
            "transcript": transcript,
            "word": word,
            "command": "enroll",
            "result": "enrolled",
            "user_id": USER_ID,
        }

    if command == "clear":
        clear_profile()
        return {
            "ok": True,
            "mode": mode,
            "wav_key": wav_key,
            "transcript": transcript,
            "word": word,
            "command": "clear",
            "result": "cleared",
        }

    profile = load_profile()
    if profile is None:
        return {
            "ok": True,
            "mode": mode,
            "wav_key": wav_key,
            "transcript": transcript,
            "word": word,
            "command": "auth",
            "result": "fail",
            "user_id": USER_ID,
            "score": 0,
        }

    score = cosine_score(features, profile["features"])
    passed = score >= AUTH_PASS_SCORE

    return {
        "ok": True,
        "mode": mode,
        "wav_key": wav_key,
        "transcript": transcript,
        "word": word,
        "command": "auth",
        "result": "pass" if passed else "fail",
        "user_id": USER_ID,
        "score": score,
    }


def lambda_handler(event, context):
    mode = (event.get("queryStringParameters") or {}).get("mode", "process")
    audio_bytes = get_audio_bytes(event)

    if mode == "clear":
        clear_profile()
        return response(200, {"ok": True, "mode": mode, "result": "cleared"})

    if len(audio_bytes) == 0:
        return response(400, {"ok": False, "mode": mode, "error": "empty_audio"})

    try:
        return response(200, process_audio(audio_bytes, mode))
    except Exception as error:
        return response(
            500,
            {
                "ok": False,
                "mode": mode,
                "error": str(error),
            },
        )
