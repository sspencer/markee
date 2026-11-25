#define _POSIX_C_SOURCE 200809L

#include "audio.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "raylib.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MARKEE_AUDIO_SAMPLE_RATE 44100U
#define MARKEE_AUDIO_VOICES      4
#define MARKEE_AUDIO_QUEUE_CAP   16

static const MarkeeAlertInfo ALERT_INFO[MARKEE_ALERT_COUNT] = {
    {1, "ding",  "Warm single desk-bell ding",          650},
    {2, "chime", "Gentle two-note ascending chime",     850},
    {3, "bell",  "Soft three-note sparkling bell triad", 1050},
};

typedef struct {
    int id;
    uint8_t volume;
} AlertRequest;

struct MarkeeAudio {
    pthread_mutex_t lock;
    bool device_owned;
    bool ready;
    Sound voices[MARKEE_ALERT_COUNT][MARKEE_AUDIO_VOICES];
    bool voices_loaded[MARKEE_ALERT_COUNT];
    int next_voice[MARKEE_ALERT_COUNT];

    AlertRequest queue[MARKEE_AUDIO_QUEUE_CAP];
    size_t q_head;
    size_t q_tail;
    size_t q_count;

    /* Pending custom melody PCM (played and unloaded on main thread in audio_tick) */
    int16_t *pending_melody_pcm;
    size_t pending_melody_frames;
    unsigned int pending_melody_sr;
    uint8_t pending_melody_vol;
    Sound active_melody_sound;
    bool has_active_melody;

    uint64_t trigger_count;
    int last_alert_id;
    uint8_t last_volume;
};

const MarkeeAlertInfo *audio_get_alert_info(int id) {
    if (id < 1 || id > MARKEE_ALERT_COUNT) return NULL;
    return &ALERT_INFO[id - 1];
}

int audio_parse_alert_id(const char *s) {
    if (!s) return 0;
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) len--;
    if (len == 0 || len >= 32) return 0;

    char tok[32];
    memcpy(tok, s, len);
    tok[len] = '\0';

    /* Match numeric ID "1".."3" */
    if (len == 1 && tok[0] >= '1' && tok[0] <= '0' + MARKEE_ALERT_COUNT) {
        return tok[0] - '0';
    }
    /* Match friendly names ("ding", "chime", "bell") */
    for (int i = 0; i < MARKEE_ALERT_COUNT; i++) {
        if (strcasecmp(tok, ALERT_INFO[i].name) == 0) {
            return ALERT_INFO[i].id;
        }
    }
    return 0;
}

/* --- Procedural synthesis of gentle, musical notification sounds --- */

static double raised_cosine_attack(double t, double attack_sec) {
    if (t <= 0.0) return 0.0;
    if (t >= attack_sec) return 1.0;
    return 0.5 * (1.0 - cos(M_PI * t / attack_sec));
}

/* Add a bell/celesta note into buf[0..n) starting at start_sec.
 * Combines a slightly detuned fundamental pair (natural acoustic shimmer)
 * with harmonic and subtle modal bell overtones whose decay times shorten
 * progressively with frequency. */
static void add_bell_note(double *buf, size_t n, double sr,
                          double start_sec, double freq_hz, double gain,
                          double decay_sec, double attack_sec,
                          double h2_amp, double bell_modal_amp,
                          double h3_amp, double h4_amp, double detune_hz) {
    size_t start_idx = (size_t)llround(start_sec * sr);
    if (start_idx >= n) return;

    const double two_pi = 2.0 * M_PI;
    for (size_t i = start_idx; i < n; i++) {
        double t = (double)(i - start_idx) / sr;
        double env_atk = raised_cosine_attack(t, attack_sec);

        double f_env = exp(-t / decay_sec);
        double fundamental = 0.5 * sin(two_pi * (freq_hz - detune_hz) * t) +
                             0.5 * sin(two_pi * (freq_hz + detune_hz) * t);

        double sample = fundamental * f_env;

        if (h2_amp > 0.0) {
            sample += h2_amp * sin(two_pi * (2.0 * freq_hz) * t) *
                      exp(-t / (decay_sec * 0.44));
        }
        if (bell_modal_amp > 0.0) {
            /* 2.756 is the second circular-plate / chime modal ratio: adds subtle bell warmth */
            sample += bell_modal_amp * sin(two_pi * (2.756 * freq_hz) * t) *
                      exp(-t / (decay_sec * 0.19));
        }
        if (h3_amp > 0.0) {
            sample += h3_amp * sin(two_pi * (3.0 * freq_hz) * t) *
                      exp(-t / (decay_sec * 0.22));
        }
        if (h4_amp > 0.0) {
            sample += h4_amp * sin(two_pi * (4.0 * freq_hz) * t) *
                      exp(-t / (decay_sec * 0.13));
        }

        buf[i] += gain * env_atk * sample;
    }
}

bool audio_synthesize_pcm(int id, int16_t **out_samples, size_t *out_frame_count,
                          unsigned int *out_sample_rate) {
    const MarkeeAlertInfo *info = audio_get_alert_info(id);
    if (!info || !out_samples || !out_frame_count || !out_sample_rate) return false;

    const unsigned int sr = MARKEE_AUDIO_SAMPLE_RATE;
    const size_t n = (size_t)((uint64_t)sr * (uint64_t)info->duration_ms / 1000U);
    if (n == 0) return false;

    double *mix = calloc(n, sizeof(double));
    if (!mix) return false;

    switch (id) {
        case 1:
            /* "ding": Single clear, warm desk-bell / glass celesta note (G5 = 783.99 Hz) */
            add_bell_note(mix, n, (double)sr,
                          0.000, 783.99, 1.00,
                          0.21, 0.0035,
                          0.28, 0.075, 0.09, 0.00, 0.60);
            break;

        case 2:
            /* "chime": Gentle two-note ascending perfect fifth (C5 -> G5), warm marimba/vibe timbre */
            add_bell_note(mix, n, (double)sr,
                          0.000, 523.25, 0.85,
                          0.24, 0.0050,
                          0.22, 0.00, 0.06, 0.00, 0.40);
            add_bell_note(mix, n, (double)sr,
                          0.145, 783.99, 1.00,
                          0.26, 0.0045,
                          0.25, 0.03, 0.07, 0.00, 0.55);
            break;

        case 3:
            /* "bell": Soft three-note ascending major triad (D5 -> F#5 -> A5), sparkling wind-chime */
            add_bell_note(mix, n, (double)sr,
                          0.000, 587.33, 0.78,
                          0.25, 0.0035,
                          0.28, 0.060, 0.05, 0.055, 0.55);
            add_bell_note(mix, n, (double)sr,
                          0.105, 739.99, 0.88,
                          0.26, 0.0035,
                          0.30, 0.065, 0.05, 0.060, 0.65);
            add_bell_note(mix, n, (double)sr,
                          0.210, 880.00, 1.00,
                          0.31, 0.0035,
                          0.30, 0.065, 0.05, 0.065, 0.75);
            break;

        default:
            free(mix);
            return false;
    }

    /* Smooth tail fade-out over the last 45 ms so the buffer ends cleanly at zero */
    size_t fade_out = (size_t)(sr * 45U / 1000U);
    if (fade_out > n / 4) fade_out = n / 4;
    for (size_t i = 0; i < fade_out; i++) {
        double frac = (double)(i + 1) / (double)fade_out;
        double w = 0.5 * (1.0 + cos(M_PI * frac));
        mix[n - fade_out + i] *= w;
    }

    /* Normalize to a comfortable, non-startling peak (~0.50 full scale, ~ -6 dBFS) */
    double peak = 0.0;
    for (size_t i = 0; i < n; i++) {
        double a = fabs(mix[i]);
        if (a > peak) peak = a;
    }
    const double target_peak = 0.50;
    double scale = (peak > 1e-9) ? (target_peak / peak) : 0.0;

    int16_t *pcm = malloc(n * sizeof(int16_t));
    if (!pcm) {
        free(mix);
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        double v = mix[i] * scale * 32767.0;
        if (v > 32767.0) v = 32767.0;
        if (v < -32767.0) v = -32767.0;
        pcm[i] = (int16_t)llround(v);
    }

    free(mix);
    *out_samples = pcm;
    *out_frame_count = n;
    *out_sample_rate = sr;
    return true;
}

static bool parse_note_freq(const char *tok, double *out_hz, bool *out_rest) {
    *out_hz = 0.0;
    *out_rest = false;
    if (!tok || !*tok) return false;
    if ((tok[0] == 'R' || tok[0] == 'r' || tok[0] == '-') && tok[1] == '\0') {
        *out_rest = true;
        return true;
    }
    int semi = -1;
    switch (toupper((unsigned char)tok[0])) {
        case 'C': semi = 0; break;
        case 'D': semi = 2; break;
        case 'E': semi = 4; break;
        case 'F': semi = 5; break;
        case 'G': semi = 7; break;
        case 'A': semi = 9; break;
        case 'B': semi = 11; break;
        default: return false;
    }
    const char *p = tok + 1;
    if (*p == '#') { semi++; p++; }
    else if (*p == 'b' || *p == 'B') { semi--; p++; }
    if (!isdigit((unsigned char)*p) || p[1] != '\0') return false;
    int oct = *p - '0';
    if (oct < 2 || oct > 7) return false;
    int midi = (oct + 1) * 12 + semi;
    *out_hz = 440.0 * pow(2.0, (double)(midi - 69) / 12.0);
    return true;
}

bool audio_synthesize_melody_pcm(const char *melody, int16_t **out_samples,
                                 size_t *out_frame_count, unsigned int *out_sample_rate) {
    if (!melody || !*melody || !out_samples || !out_frame_count || !out_sample_rate) return false;

    typedef struct {
        double hz;
        bool rest;
        int dur_ms;
    } MelodyNote;

    MelodyNote notes[32];
    size_t count = 0;
    int total_ms = 0;

    const char *p = melody;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;
        if (count >= 32) return false;

        const char *end = p;
        while (*end && *end != ',' && !isspace((unsigned char)*end)) end++;
        size_t len = (size_t)(end - p);
        if (len == 0 || len >= 24) return false;

        char tok[24];
        memcpy(tok, p, len);
        tok[len] = '\0';
        p = end;

        int dur_ms = 160;
        char *colon = strchr(tok, ':');
        if (colon) {
            *colon = '\0';
            char *dend = NULL;
            long d = strtol(colon + 1, &dend, 10);
            if (dend == colon + 1 || *dend != '\0' || d < 30 || d > 2000) return false;
            dur_ms = (int)d;
        }

        double hz = 0.0;
        bool rest = false;
        if (!parse_note_freq(tok, &hz, &rest)) return false;

        notes[count].hz = hz;
        notes[count].rest = rest;
        notes[count].dur_ms = dur_ms;
        count++;
        total_ms += dur_ms;
        if (total_ms > 5000) return false;
    }

    if (count == 0) return false;

    const unsigned int sr = MARKEE_AUDIO_SAMPLE_RATE;
    const int tail_ms = 260;
    const size_t n = (size_t)((uint64_t)sr * (uint64_t)(total_ms + tail_ms) / 1000U);
    double *mix = calloc(n, sizeof(double));
    if (!mix) return false;

    double cursor_sec = 0.0;
    for (size_t i = 0; i < count; i++) {
        double dur_sec = (double)notes[i].dur_ms / 1000.0;
        if (!notes[i].rest) {
            double decay = dur_sec * 1.1;
            if (decay < 0.14) decay = 0.14;
            if (decay > 0.35) decay = 0.35;
            add_bell_note(mix, n, (double)sr, cursor_sec, notes[i].hz, 0.90,
                          decay, 0.004, 0.25, 0.04, 0.06, 0.00, 0.50);
        }
        cursor_sec += dur_sec;
    }

    size_t fade_out = (size_t)(sr * 45U / 1000U);
    if (fade_out > n / 4) fade_out = n / 4;
    for (size_t i = 0; i < fade_out; i++) {
        double frac = (double)(i + 1) / (double)fade_out;
        mix[n - fade_out + i] *= 0.5 * (1.0 + cos(M_PI * frac));
    }

    double peak = 0.0;
    for (size_t i = 0; i < n; i++) {
        double a = fabs(mix[i]);
        if (a > peak) peak = a;
    }
    double scale = (peak > 1e-9) ? (0.50 / peak) : 0.0;

    int16_t *pcm = malloc(n * sizeof(int16_t));
    if (!pcm) {
        free(mix);
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        double v = mix[i] * scale * 32767.0;
        if (v > 32767.0) v = 32767.0;
        if (v < -32767.0) v = -32767.0;
        pcm[i] = (int16_t)llround(v);
    }

    free(mix);
    *out_samples = pcm;
    *out_frame_count = n;
    *out_sample_rate = sr;
    return true;
}

MarkeeAudio *audio_create(bool open_device) {
    MarkeeAudio *audio = calloc(1, sizeof(*audio));
    if (!audio) return NULL;

    if (pthread_mutex_init(&audio->lock, NULL) != 0) {
        free(audio);
        return NULL;
    }

    if (!open_device) {
        return audio;
    }

    if (!IsAudioDeviceReady()) {
        InitAudioDevice();
        if (IsAudioDeviceReady()) {
            audio->device_owned = true;
        }
    }

    if (!IsAudioDeviceReady()) {
        fprintf(stderr, "Audio device unavailable; alert sounds will be silent\n");
        return audio;
    }

    bool all_loaded = true;
    for (int i = 0; i < MARKEE_ALERT_COUNT; i++) {
        int16_t *pcm = NULL;
        size_t frames = 0;
        unsigned int sr = 0;
        if (!audio_synthesize_pcm(i + 1, &pcm, &frames, &sr)) {
            all_loaded = false;
            break;
        }

        Wave wave = {
            .frameCount = (unsigned int)frames,
            .sampleRate = sr,
            .sampleSize = 16,
            .channels = 1,
            .data = pcm
        };
        Sound base = LoadSoundFromWave(wave);
        free(pcm);

        if (!IsSoundValid(base)) {
            all_loaded = false;
            break;
        }

        audio->voices[i][0] = base;
        for (int v = 1; v < MARKEE_AUDIO_VOICES; v++) {
            audio->voices[i][v] = LoadSoundAlias(base);
        }
        audio->voices_loaded[i] = true;
    }

    if (all_loaded) {
        audio->ready = true;
    } else {
        fprintf(stderr, "Failed to load synthesized alert sounds\n");
    }

    return audio;
}

void audio_destroy(MarkeeAudio *audio) {
    if (!audio) return;

    for (int i = 0; i < MARKEE_ALERT_COUNT; i++) {
        if (audio->voices_loaded[i]) {
            for (int v = 1; v < MARKEE_AUDIO_VOICES; v++) {
                UnloadSoundAlias(audio->voices[i][v]);
            }
            UnloadSound(audio->voices[i][0]);
            audio->voices_loaded[i] = false;
        }
    }

    if (audio->has_active_melody) {
        UnloadSound(audio->active_melody_sound);
        audio->has_active_melody = false;
    }
    free(audio->pending_melody_pcm);
    audio->pending_melody_pcm = NULL;

    if (audio->device_owned && IsAudioDeviceReady()) {
        CloseAudioDevice();
    }

    pthread_mutex_destroy(&audio->lock);
    free(audio);
}

bool audio_is_ready(const MarkeeAudio *audio) {
    if (!audio) return false;
    return audio->ready;
}

bool audio_trigger(MarkeeAudio *audio, int id, uint8_t volume) {
    if (!audio || id < 1 || id > MARKEE_ALERT_COUNT) return false;
    if (volume > 100) volume = 100;

    pthread_mutex_lock(&audio->lock);
    audio->trigger_count++;
    audio->last_alert_id = id;
    audio->last_volume = volume;

    bool queued = false;
    if (audio->ready) {
        if (audio->q_count == MARKEE_AUDIO_QUEUE_CAP) {
            /* Drop oldest pending trigger if flooded */
            audio->q_head = (audio->q_head + 1) % MARKEE_AUDIO_QUEUE_CAP;
            audio->q_count--;
        }
        audio->queue[audio->q_tail].id = id;
        audio->queue[audio->q_tail].volume = volume;
        audio->q_tail = (audio->q_tail + 1) % MARKEE_AUDIO_QUEUE_CAP;
        audio->q_count++;
        queued = true;
    }
    pthread_mutex_unlock(&audio->lock);
    return queued;
}

bool audio_trigger_melody(MarkeeAudio *audio, const char *melody, uint8_t volume,
                          bool *out_played) {
    if (out_played) *out_played = false;
    if (!melody || !*melody) return false;
    if (volume > 100) volume = 100;

    int16_t *pcm = NULL;
    size_t frames = 0;
    unsigned int sr = 0;
    if (!audio_synthesize_melody_pcm(melody, &pcm, &frames, &sr)) {
        return false;
    }
    if (!audio) {
        free(pcm);
        return true;
    }

    pthread_mutex_lock(&audio->lock);
    audio->trigger_count++;
    audio->last_alert_id = 0;
    audio->last_volume = volume;
    if (audio->ready) {
        free(audio->pending_melody_pcm);
        audio->pending_melody_pcm = pcm;
        audio->pending_melody_frames = frames;
        audio->pending_melody_sr = sr;
        audio->pending_melody_vol = volume;
        if (out_played) *out_played = true;
    } else {
        free(pcm);
    }
    pthread_mutex_unlock(&audio->lock);
    return true;
}

void audio_tick(MarkeeAudio *audio) {
    if (!audio || !audio->ready) return;

    AlertRequest batch[MARKEE_AUDIO_QUEUE_CAP];
    size_t count = 0;
    int16_t *mel_pcm = NULL;
    size_t mel_frames = 0;
    unsigned int mel_sr = 0;
    uint8_t mel_vol = 100;

    pthread_mutex_lock(&audio->lock);
    while (audio->q_count > 0 && count < MARKEE_AUDIO_QUEUE_CAP) {
        batch[count++] = audio->queue[audio->q_head];
        audio->q_head = (audio->q_head + 1) % MARKEE_AUDIO_QUEUE_CAP;
        audio->q_count--;
    }
    if (audio->pending_melody_pcm) {
        mel_pcm = audio->pending_melody_pcm;
        mel_frames = audio->pending_melody_frames;
        mel_sr = audio->pending_melody_sr;
        mel_vol = audio->pending_melody_vol;
        audio->pending_melody_pcm = NULL;
        audio->pending_melody_frames = 0;
    }
    pthread_mutex_unlock(&audio->lock);

    if (mel_pcm) {
        if (audio->has_active_melody) {
            StopSound(audio->active_melody_sound);
            UnloadSound(audio->active_melody_sound);
            audio->has_active_melody = false;
        }
        Wave w = {
            .frameCount = (unsigned int)mel_frames,
            .sampleRate = mel_sr,
            .sampleSize = 16,
            .channels = 1,
            .data = mel_pcm
        };
        Sound snd = LoadSoundFromWave(w);
        free(mel_pcm);
        if (IsSoundValid(snd)) {
            audio->active_melody_sound = snd;
            audio->has_active_melody = true;
            SetSoundVolume(snd, (float)mel_vol / 100.0f);
            PlaySound(snd);
        }
    }

    for (size_t k = 0; k < count; k++) {
        int idx = batch[k].id - 1;
        if (idx < 0 || idx >= MARKEE_ALERT_COUNT || !audio->voices_loaded[idx]) continue;

        /* Prefer an idle voice so rapid consecutive alerts ring out smoothly together */
        int chosen = -1;
        for (int v = 0; v < MARKEE_AUDIO_VOICES; v++) {
            int cand = (audio->next_voice[idx] + v) % MARKEE_AUDIO_VOICES;
            if (!IsSoundPlaying(audio->voices[idx][cand])) {
                chosen = cand;
                break;
            }
        }
        if (chosen < 0) {
            chosen = audio->next_voice[idx];
            StopSound(audio->voices[idx][chosen]);
        }
        audio->next_voice[idx] = (chosen + 1) % MARKEE_AUDIO_VOICES;

        float vol = (float)batch[k].volume / 100.0f;
        SetSoundVolume(audio->voices[idx][chosen], vol);
        PlaySound(audio->voices[idx][chosen]);
    }
}

uint64_t audio_get_trigger_count(const MarkeeAudio *audio) {
    if (!audio) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&audio->lock);
    uint64_t c = audio->trigger_count;
    pthread_mutex_unlock((pthread_mutex_t *)&audio->lock);
    return c;
}

int audio_get_last_alert_id(const MarkeeAudio *audio) {
    if (!audio) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&audio->lock);
    int id = audio->last_alert_id;
    pthread_mutex_unlock((pthread_mutex_t *)&audio->lock);
    return id;
}

uint8_t audio_get_last_volume(const MarkeeAudio *audio) {
    if (!audio) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&audio->lock);
    uint8_t v = audio->last_volume;
    pthread_mutex_unlock((pthread_mutex_t *)&audio->lock);
    return v;
}
