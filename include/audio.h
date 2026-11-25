#ifndef MARKEE_AUDIO_H
#define MARKEE_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MARKEE_ALERT_COUNT 3

typedef struct {
    int id;                  /* 1..MARKEE_ALERT_COUNT */
    const char *name;        /* "ding", "chime", "bell" */
    const char *description; /* Short human-readable description */
    int duration_ms;         /* Duration of synthesized sound in milliseconds */
} MarkeeAlertInfo;

typedef struct MarkeeAudio MarkeeAudio;

/* Return metadata for a built-in alert sound (id: 1..MARKEE_ALERT_COUNT), or NULL if invalid. */
const MarkeeAlertInfo *audio_get_alert_info(int id);

/* Parse an alert identifier ("1".."3" or "ding"/"chime"/"bell", case-insensitive).
 * Returns 1..MARKEE_ALERT_COUNT on success, or 0 if invalid. */
int audio_parse_alert_id(const char *s);

/* Synthesize 16-bit signed mono PCM samples (44100 Hz) for alert id (1..MARKEE_ALERT_COUNT).
 * Caller must free(*out_samples) with free(). */
bool audio_synthesize_pcm(int id, int16_t **out_samples, size_t *out_frame_count,
                          unsigned int *out_sample_rate);

/* Synthesize a custom bell/chime note sequence such as "C5:120,E5:120,G5:240" (R for rest).
 * Caller frees *out_samples with free(). */
bool audio_synthesize_melody_pcm(const char *melody, int16_t **out_samples,
                                 size_t *out_frame_count, unsigned int *out_sample_rate);

/* Thread-safe: validate, synthesize, and queue playback of a custom note-sequence melody.
 * Returns false if melody syntax is invalid; sets *out_played (if non-NULL) to whether
 * audio hardware was ready to play it. */
bool audio_trigger_melody(MarkeeAudio *audio, const char *melody, uint8_t volume,
                          bool *out_played);

/* Create the alert audio engine.
 * When open_device is true, initializes raylib audio and loads the synthesized sounds.
 * When open_device is false (e.g. --mute or unit tests), creates a silent engine that still
 * validates and counts alert triggers without touching system audio hardware.
 * Returns NULL only on allocation failure. */
MarkeeAudio *audio_create(bool open_device);

/* Destroy the audio engine and release any loaded sounds / audio device. */
void audio_destroy(MarkeeAudio *audio);

/* True when an audio output device is ready and alert sounds are loaded. */
bool audio_is_ready(const MarkeeAudio *audio);

/* Thread-safe: request playback of alert sound id (1..MARKEE_ALERT_COUNT) at volume (0..100).
 * Returns true if audio is ready and playback was queued; false if muted/unavailable
 * (valid triggers are still counted) or if id is invalid. */
bool audio_trigger(MarkeeAudio *audio, int id, uint8_t volume);

/* Main-thread tick (60 Hz): drains queued alert triggers and plays them via raylib. */
void audio_tick(MarkeeAudio *audio);

/* Thread-safe telemetry for /alert status and unit testing. */
uint64_t audio_get_trigger_count(const MarkeeAudio *audio);
int audio_get_last_alert_id(const MarkeeAudio *audio);
uint8_t audio_get_last_volume(const MarkeeAudio *audio);

#endif /* MARKEE_AUDIO_H */
