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

void audio_tick(MarkeeAudio *audio) {
    if (!audio || !audio->ready) return;

    AlertRequest batch[MARKEE_AUDIO_QUEUE_CAP];
    size_t count = 0;

    pthread_mutex_lock(&audio->lock);
    while (audio->q_count > 0 && count < MARKEE_AUDIO_QUEUE_CAP) {
        batch[count++] = audio->queue[audio->q_head];
        audio->q_head = (audio->q_head + 1) % MARKEE_AUDIO_QUEUE_CAP;
        audio->q_count--;
    }
    pthread_mutex_unlock(&audio->lock);

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
