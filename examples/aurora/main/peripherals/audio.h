/*
 * audio.h — ES8311 codec speaker output via I²S.
 *
 * Output only (for now) — the BSP exposes a microphone path too, but
 * Aurora's first audio milestone is "make it beep on command". A
 * microphone-driven scene is a future phase.
 *
 * Threading:
 *   audio_play_tone() blocks the caller for ~duration_ms because
 *   esp_codec_dev_write() doesn't return until DMA drains. Since the
 *   only caller today is the console command, this is fine — the
 *   console task spends most of its life sleeping anyway. Don't call
 *   it from the LVGL task.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

bool audio_init(void);
bool audio_is_ready(void);

/* Diagnostic: read the current level of the Power-Amplifier enable
 * GPIO (BSP_POWER_AMP_IO = GPIO 46 on this board) and optionally
 * manually force it HIGH before playing a 1 kHz tone at full volume.
 * Designed to disambiguate "no sound" failures: if forcing PA HIGH
 * makes tone audible, the codec driver isn't enabling PA on its own.
 * If still silent, the speaker / PMIC audio rail is the problem. */
typedef struct {
    int  pa_level_before;   /* 0 or 1 */
    int  pa_level_during;   /* 0 or 1 (sampled mid-playback) */
    int  pa_level_after;    /* 0 or 1 (after close) */
    bool forced_pa;         /* did we manually drive HIGH? */
    int  tone_rc;           /* return of audio_play_tone */
} audio_diag_t;

void audio_diag_check_pa(bool force_pa_high, audio_diag_t *out);

/* Microphone capture: open the codec for input, read `duration_ms` of
 * PCM at 22050 Hz mono 16-bit, compute peak + RMS amplitude in dBFS,
 * and close. Returns 0 on success, negative esp_err_t on failure. The
 * raw samples are discarded — Aurora's mic story today is "prove the
 * input path works", not "capture audio". `peak_dbfs` reads ≈ 0 dB for
 * a loud signal that saturates the codec; `rms_dbfs` is a softer
 * loudness proxy. Both are -infinity (~ -100 in practice) on silence. */
int audio_record_peak(int duration_ms, float *peak_dbfs, float *rms_dbfs);

/* Async variant: spawns a one-shot task that does the recording and
 * pushes the result to a user-provided callback (called from the
 * record task, not the caller). Returns true if the task was created. */
typedef void (*audio_record_done_cb)(float peak_dbfs, float rms_dbfs, void *arg);
bool audio_record_peak_async(int duration_ms,
                              audio_record_done_cb cb, void *cb_arg);

/* Loopback: record `duration_ms` of mic into a caller-provided int16
 * buffer, compute peak + RMS in dBFS, then play the same buffer out
 * through the speaker. Buffer must hold `sample_rate * duration_ms /
 * 1000` int16 samples (≈ 22050 × ms / 1000). Allocate from PSRAM —
 * 1 second is ~44 KB. Returns 0 on success, negative on failure.
 *
 * Phases the caller can observe via the optional `phase_cb`:
 *   AUDIO_LB_RECORDING → fires when mic open / pcm capture starts
 *   AUDIO_LB_PLAYING   → fires when speaker open / playback starts
 *   AUDIO_LB_DONE      → fires after speaker close, includes metrics */
typedef enum {
    AUDIO_LB_RECORDING = 0,
    AUDIO_LB_PLAYING,
    AUDIO_LB_DONE,
} audio_loopback_phase_t;

typedef void (*audio_loopback_phase_cb)(audio_loopback_phase_t phase,
                                         float peak_dbfs, float rms_dbfs,
                                         void *arg);

int  audio_record_loopback(int16_t *buf, int n_samples,
                           audio_loopback_phase_cb cb, void *cb_arg,
                           float *peak_dbfs, float *rms_dbfs);

/* Volume control for the playback path. Pct is 0..100. The value
 * persists across recordings and is re-applied each time the speaker
 * codec_dev is opened (esp_codec_dev_open resets the codec's hardware
 * volume to its default; without re-applying, every record cycle
 * would play back at 60 %). */
int  audio_get_volume(void);
void audio_set_volume(int pct);

/* Loopback playback boost. The ES7210 mic captures speech at typical
 * RMS −20 dBFS (peaks rarely above −5 dBFS), so playing the raw PCM
 * back through the speaker sounds quiet even at 100 % codec volume —
 * the int16 range is mostly empty. Boost = the target peak amplitude
 * (0..32767) that audio_record_loopback_dynamic scales captured PCM
 * up to before playing. Default 28000 (~−1.3 dBFS, comfortable
 * headroom). Set to 0 to disable normalisation and play raw. */
int  audio_get_boost(void);
void audio_set_boost(int target_peak);

/* Variable-duration loopback. Records into `buf` until either:
 *   - max_samples is reached, OR
 *   - *stop_flag goes true (volatile; can be set from any task)
 * Then plays the captured portion back through the speaker at the
 * current volume. `out_recorded_samples` (optional) reports how many
 * samples were actually captured before the stop signal.
 *
 * Latency to stop: one mic chunk (~23 ms @ 512 samples / 22050 Hz). */
int  audio_record_loopback_dynamic(int16_t *buf, int max_samples,
                                    volatile bool *stop_flag,
                                    audio_loopback_phase_cb cb, void *cb_arg,
                                    int *out_recorded_samples,
                                    float *peak_dbfs, float *rms_dbfs);

bool audio_loopback_dynamic_async(int max_duration_ms,
                                   volatile bool *stop_flag,
                                   audio_loopback_phase_cb cb, void *cb_arg);

/* Async wrapper: spawns a task that allocates a PSRAM PCM buffer,
 * runs the loopback, and frees. Phases are reported via the callback.
 * `duration_ms` clamps to [200, 3000]. */
bool audio_loopback_async(int duration_ms,
                          audio_loopback_phase_cb cb, void *cb_arg);

/* Synchronous sine tone — blocks the caller for ~duration_ms because
 * esp_codec_dev_write doesn't return until DMA drains. Returns bytes
 * written (often 0 on success — codec_dev returns OK as 0), or a
 * negative esp_err_t. */
int audio_play_tone(int freq_hz, int duration_ms, int volume_pct);

/* Asynchronous variant — spawns a one-shot FreeRTOS task to play the
 * tone and returns immediately. Safe to call from the LVGL task or any
 * other latency-sensitive context. Returns true on task creation
 * success. Multiple calls in quick succession will overlap if the
 * codec backend allows it (ours doesn't — second call waits on the
 * audio mutex inside esp_codec_dev_open, so back-to-back calls are
 * naturally serialised). */
bool audio_play_tone_async(int freq_hz, int duration_ms, int volume_pct);

#ifdef __cplusplus
}
#endif
