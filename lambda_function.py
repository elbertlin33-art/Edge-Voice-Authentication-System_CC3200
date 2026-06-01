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

PROFILE_PREFIX = "profiles/"
PENDING_KEY = "profiles/pending_enroll.json"

NOISY_AVG_ABS = 14000
NOISY_PEAK_TO_PEAK = 62000
CLIPPED_AVG_ABS = 4000
QUIET_AVG_ABS = 200
QUIET_PEAK_TO_PEAK = 1000
AUTH_PASS_SCORE = 91
MAX_FEATURE_VALUE = 0.85

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

    if avg_abs > NOISY_AVG_ABS or (
        peak_to_peak > NOISY_PEAK_TO_PEAK and avg_abs > CLIPPED_AVG_ABS
    ):
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


def features_are_usable(features):
    return bool(features) and max(features) <= MAX_FEATURE_VALUE


def profile_is_complete(profile):
    return (
        isinstance(profile.get("user_id"), int)
        and profile.get("user_id", 0) > 0
        and bool(profile.get("user_name"))
        and bool(profile.get("password"))
        and bool(profile.get("password_words"))
        and features_are_usable(profile.get("features", []))
    )


def extract_words(transcript):
    return re.findall(r"[a-z']+", transcript.lower())


def clean_user_name(words):
    if not words:
        return ""

    return words[0][:24]


def profile_key(name):
    safe_name = re.sub(r"[^a-z0-9_-]", "", name.lower())
    return f"{PROFILE_PREFIX}{safe_name}.json"


def put_json(key, data):
    s3.put_object(
        Bucket=BUCKET,
        Key=key,
        Body=json.dumps(data),
        ContentType="application/json",
    )


def get_json(key):
    obj = s3.get_object(Bucket=BUCKET, Key=key)
    return json.loads(obj["Body"].read().decode("utf-8"))


def delete_key(key):
    try:
        s3.delete_object(Bucket=BUCKET, Key=key)
    except Exception:
        pass


def list_profiles():
    profiles = []
    response_data = s3.list_objects_v2(Bucket=BUCKET, Prefix=PROFILE_PREFIX)

    for item in response_data.get("Contents", []):
        key = item["Key"]
        if key == PENDING_KEY or not key.endswith(".json"):
            continue

        try:
            profile = get_json(key)
        except Exception:
            continue

        if profile_is_complete(profile):
            profiles.append(profile)

    return profiles


def clear_profiles():
    response_data = s3.list_objects_v2(Bucket=BUCKET, Prefix=PROFILE_PREFIX)
    for item in response_data.get("Contents", []):
        delete_key(item["Key"])


def status_response():
    profiles = list_profiles()
    return {
        "ok": True,
        "mode": "status",
        "has_users": len(profiles) > 0,
        "user_count": len(profiles),
    }


def save_audio(audio_bytes):
    timestamp = int(time.time())
    wav_key = f"uploads/{timestamp}_{uuid.uuid4().hex[:8]}.wav"
    wav_bytes = make_wav_bytes(audio_bytes)

    s3.put_object(
        Bucket=BUCKET,
        Key=wav_key,
        Body=wav_bytes,
        ContentType="audio/wav",
    )

    return wav_key


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


def transcribe_audio(audio_bytes):
    audio_level, audio_info = check_audio_level(audio_bytes)
    if audio_level != "ok":
        return None, [], "", "", {
            "ok": False,
            "result": audio_level,
            "word": "",
            "words": "",
            "audio": audio_info,
        }

    wav_key = save_audio(audio_bytes)
    transcript = transcribe_english(wav_key)
    words = extract_words(transcript)
    words_text = " ".join(words)
    word = words[0] if words else ""

    return wav_key, words, words_text, word, None


def fail_response(mode, wav_key, transcript, words_text, word, reason, score=0, command="auth"):
    return {
        "ok": True,
        "mode": mode,
        "wav_key": wav_key,
        "transcript": transcript,
        "words": words_text,
        "word": word,
        "command": command,
        "result": "fail",
        "reason": reason,
        "user_id": 0,
        "score": score,
        "user_name": "",
    }


def audio_error_response(mode, audio_error, command="auth"):
    reason = audio_error.get("result", "bad_audio")
    return {
        "ok": True,
        "mode": mode,
        "wav_key": "",
        "transcript": "",
        "words": "",
        "word": "",
        "command": command,
        "result": "fail",
        "reason": reason,
        "user_id": 0,
        "score": 0,
        "user_name": "",
        "audio": audio_error.get("audio", {}),
    }


def start_or_auth(audio_bytes, mode):
    wav_key, words, words_text, word, audio_error = transcribe_audio(audio_bytes)
    if audio_error:
        return audio_error_response(mode, audio_error)

    transcript = words_text

    if not words:
        return fail_response(mode, wav_key, transcript, words_text, word, "no_word")

    if words == ["apple"]:
        delete_key(PENDING_KEY)
        return {
            "ok": True,
            "mode": mode,
            "wav_key": wav_key,
            "transcript": transcript,
            "words": words_text,
            "word": word,
            "command": "enroll",
            "result": "ready",
            "reason": "say_password",
            "user_id": 0,
            "score": 0,
            "user_name": "",
        }

    if words == ["clear"]:
        clear_profiles()
        return {
            "ok": True,
            "mode": mode,
            "wav_key": wav_key,
            "transcript": transcript,
            "words": words_text,
            "word": word,
            "command": "clear",
            "result": "cleared",
            "user_id": 0,
            "score": 0,
            "user_name": "",
        }

    profiles = list_profiles()
    if not profiles:
        return fail_response(
            mode,
            wav_key,
            transcript,
            words_text,
            word,
            "no_enrolled_user",
        )

    features = extract_fft_features(audio_bytes)
    if not features_are_usable(features):
        return fail_response(mode, wav_key, transcript, words_text, word, "bad_audio")

    best_score = 0
    best_user = ""
    password_matched = False

    for profile in profiles:
        profile_words = profile.get("password_words", [])
        profile_features = profile.get("features", [])

        if not features_are_usable(profile_features):
            continue

        score = cosine_score(features, profile_features)
        if score > best_score:
            best_score = score
            best_user = profile.get("user_name", "")

        if words != profile_words:
            continue

        password_matched = True
        if score >= AUTH_PASS_SCORE:
            return {
                "ok": True,
                "mode": mode,
                "wav_key": wav_key,
                "transcript": transcript,
                "words": words_text,
                "word": word,
                "command": "auth",
                "result": "pass",
                "user_id": profile.get("user_id", 0),
                "score": score,
                "user_name": profile.get("user_name", ""),
            }

    reason = "voice_mismatch" if password_matched else "password_mismatch"
    return fail_response(mode, wav_key, transcript, words_text, word, reason, best_score)


def enroll_password(audio_bytes, mode):
    wav_key, words, words_text, word, audio_error = transcribe_audio(audio_bytes)
    if audio_error:
        return audio_error_response(mode, audio_error, command="enroll")

    if not words:
        return fail_response(
            mode,
            wav_key,
            words_text,
            words_text,
            word,
            "no_password",
            command="enroll",
        )

    if words == ["apple"]:
        return fail_response(
            mode,
            wav_key,
            words_text,
            words_text,
            word,
            "password_cannot_be_apple",
            command="enroll",
        )

    features = extract_fft_features(audio_bytes)
    if not features_are_usable(features):
        return fail_response(
            mode,
            wav_key,
            words_text,
            words_text,
            word,
            "bad_profile_audio",
            command="enroll",
        )

    put_json(
        PENDING_KEY,
        {
            "password": words_text,
            "password_words": words,
            "features": features,
            "created_at": int(time.time()),
        },
    )

    return {
        "ok": True,
        "mode": mode,
        "wav_key": wav_key,
        "transcript": words_text,
        "words": words_text,
        "word": word,
        "command": "enroll",
        "result": "password_saved",
        "reason": "say_user_name",
        "user_id": 0,
        "score": 0,
        "user_name": "",
    }


def enroll_name(audio_bytes, mode):
    wav_key, words, words_text, word, audio_error = transcribe_audio(audio_bytes)
    if audio_error:
        return audio_error_response(mode, audio_error, command="enroll")

    user_name = clean_user_name(words)
    if user_name == "":
        return fail_response(
            mode,
            wav_key,
            words_text,
            words_text,
            word,
            "no_user_name",
            command="enroll",
        )

    try:
        pending = get_json(PENDING_KEY)
    except Exception:
        return fail_response(mode, wav_key, words_text, words_text, word, "missing_password")

    pending_password = pending.get("password", "")
    pending_password_words = pending.get("password_words", [])
    pending_features = pending.get("features", [])
    if not pending_password or not pending_password_words or not features_are_usable(pending_features):
        delete_key(PENDING_KEY)
        return fail_response(
            mode,
            wav_key,
            words_text,
            words_text,
            word,
            "missing_password",
            command="enroll",
        )

    user_id = max([profile.get("user_id", 0) for profile in list_profiles()] or [0]) + 1
    profile = {
        "user_id": user_id,
        "user_name": user_name,
        "password": pending_password,
        "password_words": pending_password_words,
        "features": pending_features,
    }

    if not profile_is_complete(profile):
        return fail_response(
            mode,
            wav_key,
            words_text,
            words_text,
            word,
            "incomplete_profile",
            command="enroll",
        )

    put_json(profile_key(user_name), profile)
    delete_key(PENDING_KEY)

    return {
        "ok": True,
        "mode": mode,
        "wav_key": wav_key,
        "transcript": words_text,
        "words": words_text,
        "word": word,
        "command": "enroll",
        "result": "enrolled",
        "reason": "",
        "user_id": user_id,
        "score": 0,
        "user_name": user_name,
        "password": pending_password,
    }


def process_audio(audio_bytes, mode):
    if mode == "enroll_password":
        return enroll_password(audio_bytes, mode)

    if mode == "enroll_name":
        return enroll_name(audio_bytes, mode)

    return start_or_auth(audio_bytes, mode)


def lambda_handler(event, context):
    mode = (event.get("queryStringParameters") or {}).get("mode", "process")
    audio_bytes = get_audio_bytes(event)

    if mode == "status":
        return response(200, status_response())

    if mode == "clear":
        clear_profiles()
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
