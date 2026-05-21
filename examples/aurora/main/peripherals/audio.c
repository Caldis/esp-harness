/*
 * audio.c — ES8311 speaker init + sine-tone generator.
 *
 * Pipeline:
 *   bsp_audio_init(NULL)             — opens I²S RX+TX, mono, 22050 Hz, 16 bit
 *   bsp_audio_codec_speaker_init()   — wires ES8311 over I²C, exposes codec handle
 *   esp_codec_dev_open(fs)           — agrees on sample format
 *   esp_codec_dev_set_out_vol(N)     — hardware volume 0..100
 *   esp_codec_dev_write(buf, len)    — blocking PCM write
 *
 * Tone generation: int16 PCM, mono, 22050 Hz. For freq_hz, we step the
 * phase by `freq_hz / sample_rate` per sample and emit
 * `(int16_t)(amp * sinf(phase * 2π))`. Amplitude scaled by volume_pct.
 * Chunked write so we don't allocate a giant buffer for long tones.
 */

#include "audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "audio";

#define SAMPLE_RATE_HZ        22050
#define BITS_PER_SAMPLE       16
#define CHANNELS              1
#define DEFAULT_VOLUME_PCT    70
#define TONE_CHUNK_SAMPLES    512    /* ~23 ms per chunk @ 22050 Hz */

static esp_codec_dev_handle_t s_spk = NULL;
static esp_codec_dev_handle_t s_mic = NULL;
static bool s_ready = false;
static bool s_mic_ready = false;
/* Persistent speaker volume. esp_codec_dev_open resets the codec's
 * hardware volume each time, so we re-apply this after every open. */
static int s_volume_pct = 70;
/* Loopback normalisation target. Captured PCM is scaled so the peak
 * sample reaches this value before playback. 28000 ≈ −1.3 dBFS,
 * leaves comfortable headroom and avoids clipping rounding artefacts.
 * 0 = no normalisation (faithful 1:1 playback). */
static int s_boost_target = 28000;

static void normalise_pcm(int16_t *buf, int n, int peak, int target);

static void pa_force_high(void)
{
    /* Power-amp enable pin. On this board it must be HIGH for any
     * sound to come out — the I²S DMA can be slamming PCM into ES8311
     * but if the NS-series amp downstream is disabled, the speaker
     * stays silent. The BSP-supplied es8311 codec driver was observed
     * to leave GPIO 46 at LOW even during playback (diagnosed via
     * `audio diag`, which sampled gpio_get_level mid-tone). We
     * configure as OUTPUT once and hold HIGH from audio_init onward.
     * Re-asserted after each esp_codec_dev_open(spk) call because the
     * codec driver toggles the pin on open/close. */
    static bool inited = false;
    if (!inited) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << BSP_POWER_AMP_IO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        inited = true;
    }
    gpio_set_level(BSP_POWER_AMP_IO, 1);
}

bool audio_init(void)
{
    /* bsp_audio_init opens the I²S channel. NULL = defaults (mono,
     * duplex, 16-bit, 22050 Hz). */
    esp_err_t err = bsp_audio_init(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init: %s", esp_err_to_name(err));
        return false;
    }
    s_spk = bsp_audio_codec_speaker_init();
    if (s_spk == NULL) {
        ESP_LOGE(TAG, "bsp_audio_codec_speaker_init returned NULL");
        return false;
    }
    /* Force PA enable HIGH — see comment on pa_force_high above. */
    pa_force_high();
    int rc = esp_codec_dev_set_out_vol(s_spk, DEFAULT_VOLUME_PCT);
    if (rc != 0) {
        ESP_LOGW(TAG, "set_out_vol returned %d (continuing)", rc);
    }
    s_ready = true;
    ESP_LOGI(TAG, "audio_init ok · ES8311 speaker @ %d Hz / %d-bit / %dch · vol=%d%%",
             SAMPLE_RATE_HZ, BITS_PER_SAMPLE, CHANNELS, DEFAULT_VOLUME_PCT);

    /* Microphone — same codec, separate handle. Failure is non-fatal:
     * Listen scene will report mic-not-ready instead of crashing. */
    s_mic = bsp_audio_codec_microphone_init();
    if (s_mic == NULL) {
        ESP_LOGW(TAG, "mic init returned NULL — Listen scene disabled");
    } else {
        s_mic_ready = true;
        ESP_LOGI(TAG, "ES8311 mic handle attached");
    }
    return true;
}

bool audio_is_ready(void) { return s_ready; }

/* PA enable pin diagnostic. The board's power-amp chip (NS4150 or
 * similar) is enabled by BSP_POWER_AMP_IO (GPIO 46) going HIGH. The
 * BSP's es8311 driver is supposed to toggle this around codec_dev
 * open/close, but if it doesn't (different driver fork, missing GPIO
 * config, etc.) the speaker stays muted regardless of how loud the
 * I²S output is. This helper samples the pin's actual level before /
 * during / after a probe tone, and can optionally force it HIGH to
 * confirm the speaker is alive. */
void audio_diag_check_pa(bool force_pa_high, audio_diag_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!out) return;

    /* Snapshot the pin as it currently sits. */
    out->pa_level_before = gpio_get_level(BSP_POWER_AMP_IO);

    if (force_pa_high) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << BSP_POWER_AMP_IO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        gpio_set_level(BSP_POWER_AMP_IO, 1);
        out->forced_pa = true;
        /* Tiny settle — some amps take a few ms after enable. */
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    /* Play a 1 kHz tone at max volume so any working speaker chain
     * produces obvious sound. */
    int saved_vol = s_volume_pct;
    s_volume_pct = 100;
    out->tone_rc = audio_play_tone(1000, 700, 100);
    /* Sample mid-playback to see if the BSP toggles it. */
    out->pa_level_during = gpio_get_level(BSP_POWER_AMP_IO);
    s_volume_pct = saved_vol;

    /* After playback completes (audio_play_tone is sync), check again. */
    out->pa_level_after = gpio_get_level(BSP_POWER_AMP_IO);

    if (force_pa_high) {
        /* Leave HIGH so subsequent test commands also hear sound while
         * we're diagnosing. Caller can issue a follow-up to release. */
    }
}

int audio_play_tone(int freq_hz, int duration_ms, int volume_pct)
{
    if (!s_ready || s_spk == NULL) return -1;
    if (freq_hz <= 0 || freq_hz > SAMPLE_RATE_HZ / 2) {
        ESP_LOGW(TAG, "tone freq %d out of range (1..%d)", freq_hz, SAMPLE_RATE_HZ / 2);
        return -1;
    }
    if (duration_ms <= 0) duration_ms = 200;
    if (duration_ms > 5000) duration_ms = 5000;    /* don't wedge console */
    if (volume_pct < 0)   volume_pct = 0;
    if (volume_pct > 100) volume_pct = 100;

    /* Cap peak well below INT16_MAX to keep some headroom for the
     * codec's internal mixer. */
    const float amp = (float)volume_pct * 200.0f;   /* 0..20000 */

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = BITS_PER_SAMPLE,
        .channel         = CHANNELS,
        .channel_mask    = 0,
        .sample_rate     = SAMPLE_RATE_HZ,
        .mclk_multiple   = 0,
    };
    int rc = esp_codec_dev_open(s_spk, &fs);
    if (rc != 0) {
        ESP_LOGE(TAG, "codec_dev_open: %d", rc);
        return rc;
    }
    /* Re-apply volume per open — some codec_dev backends reset it. */
    esp_codec_dev_set_out_vol(s_spk, volume_pct);
    /* And re-assert PA enable: codec_dev_open toggles GPIO 46 on this
     * board's BSP, sometimes leaving it LOW. */
    pa_force_high();

    const float two_pi = 2.0f * (float)M_PI;
    const float step   = (float)freq_hz / (float)SAMPLE_RATE_HZ;
    float phase = 0.0f;

    /* PCM chunk goes on the heap, not the stack — keeps the task that
     * runs us small enough to fit in the constrained internal-SRAM
     * budget when called via audio_play_tone_async. */
    int16_t *chunk = (int16_t *)malloc(TONE_CHUNK_SAMPLES * sizeof(int16_t));
    if (chunk == NULL) {
        ESP_LOGE(TAG, "chunk alloc failed");
        esp_codec_dev_close(s_spk);
        return -1;
    }
    int total_samples = (int)((int64_t)duration_ms * SAMPLE_RATE_HZ / 1000);
    int total_bytes = 0;

    while (total_samples > 0) {
        int n = total_samples < TONE_CHUNK_SAMPLES ? total_samples : TONE_CHUNK_SAMPLES;
        for (int i = 0; i < n; ++i) {
            chunk[i] = (int16_t)(amp * sinf(phase * two_pi));
            phase += step;
            if (phase >= 1.0f) phase -= 1.0f;
        }
        int written = esp_codec_dev_write(s_spk, chunk, n * (int)sizeof(int16_t));
        if (written < 0) {
            ESP_LOGE(TAG, "codec_dev_write: %d", written);
            free(chunk);
            esp_codec_dev_close(s_spk);
            return written;
        }
        total_bytes += written;
        total_samples -= n;
    }

    free(chunk);
    esp_codec_dev_close(s_spk);
    return total_bytes;
}

/* ── async variant ─────────────────────────────────────────────────── */

typedef struct {
    int freq_hz;
    int duration_ms;
    int volume_pct;
} tone_req_t;

static void tone_task(void *arg)
{
    tone_req_t *req = (tone_req_t *)arg;
    if (req) {
        audio_play_tone(req->freq_hz, req->duration_ms, req->volume_pct);
        free(req);
    }
    vTaskDelete(NULL);
}

/* ── microphone capture ───────────────────────────────────────────── */

int audio_record_peak(int duration_ms, float *peak_dbfs, float *rms_dbfs)
{
    if (peak_dbfs) *peak_dbfs = -100.0f;
    if (rms_dbfs)  *rms_dbfs  = -100.0f;
    if (!s_mic_ready || s_mic == NULL) return -1;
    if (duration_ms <= 0)    duration_ms = 500;
    if (duration_ms > 5000)  duration_ms = 5000;

    /* The board's mic is ES7210 — a 4-channel ADC with MIC1+MIC2 wired
     * up. The BSP's bsp_audio_init defaults to mono duplex, so the
     * codec_dev driver expects mono on read. channel_mask=1 selects
     * the first channel explicitly; mask=0 ("default filter all") has
     * been observed to silently produce zero-byte reads on this BSP. */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = BITS_PER_SAMPLE,
        .channel         = CHANNELS,
        .channel_mask    = 0x01,
        .sample_rate     = SAMPLE_RATE_HZ,
        .mclk_multiple   = 0,
    };
    int rc = esp_codec_dev_open(s_mic, &fs);
    if (rc != 0) {
        ESP_LOGE(TAG, "mic open: %d", rc);
        return rc;
    }
    /* 18 dB gain leaves headroom — 30 dB clips on ambient room noise
     * (we measured -20 dBFS RMS in a typical office). 18 dB still
     * picks up normal-volume speech at arm's length without clipping. */
    esp_codec_dev_set_in_gain(s_mic, 18.0f);

    int total_samples = (int)((int64_t)duration_ms * SAMPLE_RATE_HZ / 1000);
    int16_t *chunk = (int16_t *)malloc(TONE_CHUNK_SAMPLES * sizeof(int16_t));
    if (!chunk) {
        esp_codec_dev_close(s_mic);
        return -1;
    }

    int32_t peak = 0;
    int64_t sumsq = 0;
    int64_t n_total = 0;
    while (total_samples > 0) {
        int n = total_samples < TONE_CHUNK_SAMPLES ? total_samples : TONE_CHUNK_SAMPLES;
        int bytes = n * (int)sizeof(int16_t);
        /* esp_codec_dev_read semantics (verified in
         * managed_components/.../audio_codec_data_i2s.c::_i2s_data_read):
         * returns 0 on success with `size` bytes filled, negative on
         * error. NOT a byte count — confusing but documented in code. */
        int rc2 = esp_codec_dev_read(s_mic, chunk, bytes);
        if (rc2 != 0) {
            ESP_LOGE(TAG, "codec_dev_read: %d (after %lld samples)", rc2, n_total);
            free(chunk);
            esp_codec_dev_close(s_mic);
            return rc2;
        }
        for (int i = 0; i < n; ++i) {
            int v = (int)chunk[i];
            int abs_v = v < 0 ? -v : v;
            if (abs_v > peak) peak = abs_v;
            sumsq += (int64_t)v * (int64_t)v;
        }
        n_total += n;
        total_samples -= n;
    }
    free(chunk);
    esp_codec_dev_close(s_mic);

    ESP_LOGI(TAG, "mic captured %lld samples, peak raw %d", n_total, (int)peak);
    if (n_total == 0) return -1;

    /* dBFS: 0 dB = full-scale int16 (32767). Floor at -100 so the
     * displayed value stays readable in silence. */
    if (peak_dbfs) {
        float p = (float)peak / 32767.0f;
        *peak_dbfs = p > 0.0f ? 20.0f * log10f(p) : -100.0f;
        if (*peak_dbfs < -100.0f) *peak_dbfs = -100.0f;
    }
    if (rms_dbfs) {
        float rms = sqrtf((float)sumsq / (float)n_total) / 32767.0f;
        *rms_dbfs = rms > 0.0f ? 20.0f * log10f(rms) : -100.0f;
        if (*rms_dbfs < -100.0f) *rms_dbfs = -100.0f;
    }
    return 0;
}

typedef struct {
    int duration_ms;
    audio_record_done_cb cb;
    void *arg;
} rec_req_t;

static void rec_task(void *arg)
{
    rec_req_t *r = (rec_req_t *)arg;
    if (r) {
        float pk = -100.0f, rms = -100.0f;
        audio_record_peak(r->duration_ms, &pk, &rms);
        if (r->cb) r->cb(pk, rms, r->arg);
        free(r);
    }
    vTaskDelete(NULL);
}

bool audio_record_peak_async(int duration_ms,
                              audio_record_done_cb cb, void *cb_arg)
{
    if (!s_mic_ready) return false;
    rec_req_t *r = (rec_req_t *)malloc(sizeof(*r));
    if (!r) return false;
    r->duration_ms = duration_ms;
    r->cb = cb;
    r->arg = cb_arg;
    BaseType_t ok = xTaskCreate(rec_task, "rec", 2560, r, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "rec task create failed");
        free(r);
        return false;
    }
    return true;
}

/* ── loopback: record + play back ─────────────────────────────────── */

int audio_record_loopback(int16_t *buf, int n_samples,
                          audio_loopback_phase_cb cb, void *cb_arg,
                          float *peak_dbfs, float *rms_dbfs)
{
    if (peak_dbfs) *peak_dbfs = -100.0f;
    if (rms_dbfs)  *rms_dbfs  = -100.0f;
    if (!s_mic_ready || s_mic == NULL || !s_ready || s_spk == NULL ||
        buf == NULL || n_samples <= 0) {
        return -1;
    }

    /* ---- phase 1: capture mic into buf ---- */
    if (cb) cb(AUDIO_LB_RECORDING, 0.0f, 0.0f, cb_arg);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = BITS_PER_SAMPLE,
        .channel         = CHANNELS,
        .channel_mask    = 0x01,
        .sample_rate     = SAMPLE_RATE_HZ,
        .mclk_multiple   = 0,
    };
    int rc = esp_codec_dev_open(s_mic, &fs);
    if (rc != 0) {
        ESP_LOGE(TAG, "loopback mic open: %d", rc);
        return rc;
    }
    esp_codec_dev_set_in_gain(s_mic, 18.0f);

    int remaining = n_samples;
    int16_t *p = buf;
    while (remaining > 0) {
        int n = remaining < TONE_CHUNK_SAMPLES ? remaining : TONE_CHUNK_SAMPLES;
        int bytes = n * (int)sizeof(int16_t);
        int rc2 = esp_codec_dev_read(s_mic, p, bytes);
        if (rc2 != 0) {
            ESP_LOGE(TAG, "loopback mic read: %d", rc2);
            esp_codec_dev_close(s_mic);
            return rc2;
        }
        p += n;
        remaining -= n;
    }
    esp_codec_dev_close(s_mic);

    /* Compute metrics on the captured PCM. */
    int32_t peak = 0;
    int64_t sumsq = 0;
    for (int i = 0; i < n_samples; ++i) {
        int v = (int)buf[i];
        int abs_v = v < 0 ? -v : v;
        if (abs_v > peak) peak = abs_v;
        sumsq += (int64_t)v * (int64_t)v;
    }
    float pk_db = -100.0f, rms_db = -100.0f;
    if (peak > 0) {
        float p_lin = (float)peak / 32767.0f;
        pk_db = 20.0f * log10f(p_lin);
        if (pk_db < -100.0f) pk_db = -100.0f;
    }
    if (sumsq > 0) {
        float rms_lin = sqrtf((float)sumsq / (float)n_samples) / 32767.0f;
        rms_db = 20.0f * log10f(rms_lin);
        if (rms_db < -100.0f) rms_db = -100.0f;
    }
    if (peak_dbfs) *peak_dbfs = pk_db;
    if (rms_dbfs)  *rms_dbfs  = rms_db;

    /* Normalise captured PCM so playback hits a useful amplitude — see
     * the dynamic variant's commentary for the rationale. */
    normalise_pcm(buf, n_samples, (int)peak, s_boost_target);

    /* ---- phase 2: play the same PCM back ---- */
    if (cb) cb(AUDIO_LB_PLAYING, pk_db, rms_db, cb_arg);

    rc = esp_codec_dev_open(s_spk, &fs);
    if (rc != 0) {
        ESP_LOGE(TAG, "loopback spk open: %d", rc);
        if (cb) cb(AUDIO_LB_DONE, pk_db, rms_db, cb_arg);
        return rc;
    }
    /* Honour the user-selected volume — Listen scene tweaks this via
     * the BOOT / USER buttons. */
    esp_codec_dev_set_out_vol(s_spk, s_volume_pct);
    pa_force_high();

    remaining = n_samples;
    p = buf;
    while (remaining > 0) {
        int n = remaining < TONE_CHUNK_SAMPLES ? remaining : TONE_CHUNK_SAMPLES;
        int rc2 = esp_codec_dev_write(s_spk, p, n * (int)sizeof(int16_t));
        if (rc2 < 0) {
            ESP_LOGE(TAG, "loopback spk write: %d", rc2);
            esp_codec_dev_close(s_spk);
            if (cb) cb(AUDIO_LB_DONE, pk_db, rms_db, cb_arg);
            return rc2;
        }
        p += n;
        remaining -= n;
    }
    esp_codec_dev_close(s_spk);

    if (cb) cb(AUDIO_LB_DONE, pk_db, rms_db, cb_arg);
    ESP_LOGI(TAG, "loopback ok: peak %.1f dB / rms %.1f dB", pk_db, rms_db);
    return 0;
}

typedef struct {
    int duration_ms;
    audio_loopback_phase_cb cb;
    void *arg;
} lb_req_t;

static void lb_task(void *arg)
{
    lb_req_t *r = (lb_req_t *)arg;
    if (r) {
        int n = (int)((int64_t)r->duration_ms * SAMPLE_RATE_HZ / 1000);
        /* PCM lives in PSRAM — 1 s ≈ 44 KB. Internal SRAM is too
         * tight in v0.8 to hold a buffer this large. */
        int16_t *buf = (int16_t *)heap_caps_malloc(
            (size_t)n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) {
            ESP_LOGE(TAG, "lb PSRAM alloc failed (%d samples)", n);
            if (r->cb) r->cb(AUDIO_LB_DONE, -100.0f, -100.0f, r->arg);
        } else {
            float pk = -100.0f, rms = -100.0f;
            audio_record_loopback(buf, n, r->cb, r->arg, &pk, &rms);
            heap_caps_free(buf);
        }
        free(r);
    }
    vTaskDelete(NULL);
}

bool audio_loopback_async(int duration_ms,
                          audio_loopback_phase_cb cb, void *cb_arg)
{
    if (!s_mic_ready || !s_ready) return false;
    if (duration_ms < 200)  duration_ms = 200;
    if (duration_ms > 3000) duration_ms = 3000;
    lb_req_t *r = (lb_req_t *)malloc(sizeof(*r));
    if (!r) return false;
    r->duration_ms = duration_ms;
    r->cb = cb;
    r->arg = cb_arg;
    BaseType_t ok = xTaskCreate(lb_task, "lb", 3072, r, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "lb task create failed");
        free(r);
        return false;
    }
    return true;
}

/* ── volume + dynamic-duration loopback ──────────────────────────── */

int audio_get_volume(void) { return s_volume_pct; }

void audio_set_volume(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    s_volume_pct = pct;
}

int audio_get_boost(void) { return s_boost_target; }

void audio_set_boost(int target_peak)
{
    if (target_peak < 0)      target_peak = 0;
    if (target_peak > 32767)  target_peak = 32767;
    s_boost_target = target_peak;
}

/* In-place: multiply each sample by (target / peak), with int16
 * saturation. No-op if target = 0 or peak = 0. */
static void normalise_pcm(int16_t *buf, int n, int peak, int target)
{
    if (target <= 0 || peak <= 0 || n <= 0) return;
    if (peak >= target) return;  /* already loud enough, would attenuate */
    /* Compute Q15-ish scale: scale * peak ≈ target. Multiply each
     * sample. Use int32 intermediate so multiplication doesn't roll
     * over until we re-saturate to int16. */
    int32_t scale_num = (int32_t)target;
    int32_t scale_den = (int32_t)peak;
    for (int i = 0; i < n; ++i) {
        int32_t v = (int32_t)buf[i] * scale_num / scale_den;
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        buf[i] = (int16_t)v;
    }
}

int audio_record_loopback_dynamic(int16_t *buf, int max_samples,
                                   volatile bool *stop_flag,
                                   audio_loopback_phase_cb cb, void *cb_arg,
                                   int *out_recorded_samples,
                                   float *peak_dbfs, float *rms_dbfs)
{
    if (peak_dbfs) *peak_dbfs = -100.0f;
    if (rms_dbfs)  *rms_dbfs  = -100.0f;
    if (out_recorded_samples) *out_recorded_samples = 0;
    if (!s_mic_ready || s_mic == NULL || !s_ready || s_spk == NULL ||
        buf == NULL || max_samples <= 0) {
        return -1;
    }

    /* ---- record phase ---- */
    if (cb) cb(AUDIO_LB_RECORDING, 0.0f, 0.0f, cb_arg);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = BITS_PER_SAMPLE,
        .channel         = CHANNELS,
        .channel_mask    = 0x01,
        .sample_rate     = SAMPLE_RATE_HZ,
        .mclk_multiple   = 0,
    };
    int rc = esp_codec_dev_open(s_mic, &fs);
    if (rc != 0) {
        ESP_LOGE(TAG, "lb-dyn mic open: %d", rc);
        return rc;
    }
    esp_codec_dev_set_in_gain(s_mic, 18.0f);

    int captured = 0;
    while (captured < max_samples) {
        if (stop_flag && *stop_flag) break;
        int n = max_samples - captured;
        if (n > TONE_CHUNK_SAMPLES) n = TONE_CHUNK_SAMPLES;
        int rc2 = esp_codec_dev_read(s_mic, &buf[captured],
                                     n * (int)sizeof(int16_t));
        if (rc2 != 0) {
            ESP_LOGE(TAG, "lb-dyn mic read: %d (after %d samples)", rc2, captured);
            esp_codec_dev_close(s_mic);
            return rc2;
        }
        captured += n;
    }
    esp_codec_dev_close(s_mic);
    if (out_recorded_samples) *out_recorded_samples = captured;

    /* Below the chunk size we'd produce a click instead of audio.
     * Floor at one chunk so the playback isn't useless. */
    if (captured < TONE_CHUNK_SAMPLES) {
        if (cb) cb(AUDIO_LB_DONE, -100.0f, -100.0f, cb_arg);
        return -1;
    }

    int32_t peak = 0;
    int64_t sumsq = 0;
    for (int i = 0; i < captured; ++i) {
        int v = (int)buf[i];
        int abs_v = v < 0 ? -v : v;
        if (abs_v > peak) peak = abs_v;
        sumsq += (int64_t)v * (int64_t)v;
    }
    float pk_db = -100.0f, rms_db = -100.0f;
    if (peak > 0) {
        float p_lin = (float)peak / 32767.0f;
        pk_db = 20.0f * log10f(p_lin);
        if (pk_db < -100.0f) pk_db = -100.0f;
    }
    if (sumsq > 0) {
        float rms_lin = sqrtf((float)sumsq / (float)captured) / 32767.0f;
        rms_db = 20.0f * log10f(rms_lin);
        if (rms_db < -100.0f) rms_db = -100.0f;
    }
    if (peak_dbfs) *peak_dbfs = pk_db;
    if (rms_dbfs)  *rms_dbfs  = rms_db;

    /* Pre-playback normalisation: speech off a hand-held mic typically
     * peaks at −5..−10 dBFS, so playing raw means the speaker is
     * driven at maybe 30 % of its dynamic range and sounds quiet at
     * any volume. Scale captured PCM so the peak reaches s_boost_target
     * (~28000 = −1.3 dBFS) before sending to the speaker.
     *
     * This raises the playback noise floor proportionally, but with a
     * proper ES7210 capture (16 effective bits, very low self-noise)
     * the audible difference is "loud playback" instead of "noisy
     * playback". Set boost = 0 to disable. */
    normalise_pcm(buf, captured, (int)peak, s_boost_target);

    /* ---- play back ---- */
    if (cb) cb(AUDIO_LB_PLAYING, pk_db, rms_db, cb_arg);

    rc = esp_codec_dev_open(s_spk, &fs);
    if (rc != 0) {
        ESP_LOGE(TAG, "lb-dyn spk open: %d", rc);
        if (cb) cb(AUDIO_LB_DONE, pk_db, rms_db, cb_arg);
        return rc;
    }
    esp_codec_dev_set_out_vol(s_spk, s_volume_pct);
    pa_force_high();

    int remaining = captured;
    int16_t *p = buf;
    while (remaining > 0) {
        int n = remaining < TONE_CHUNK_SAMPLES ? remaining : TONE_CHUNK_SAMPLES;
        int rc2 = esp_codec_dev_write(s_spk, p, n * (int)sizeof(int16_t));
        if (rc2 < 0) {
            ESP_LOGE(TAG, "lb-dyn spk write: %d", rc2);
            esp_codec_dev_close(s_spk);
            if (cb) cb(AUDIO_LB_DONE, pk_db, rms_db, cb_arg);
            return rc2;
        }
        p += n;
        remaining -= n;
    }
    esp_codec_dev_close(s_spk);

    if (cb) cb(AUDIO_LB_DONE, pk_db, rms_db, cb_arg);
    ESP_LOGI(TAG, "lb-dyn ok: %d samples (%.1f s), peak %.1f dB rms %.1f dB, vol=%d%%",
             captured, (float)captured / (float)SAMPLE_RATE_HZ,
             pk_db, rms_db, s_volume_pct);
    return 0;
}

typedef struct {
    int max_duration_ms;
    volatile bool *stop_flag;
    audio_loopback_phase_cb cb;
    void *arg;
} lb_dyn_req_t;

static void lb_dyn_task(void *arg)
{
    lb_dyn_req_t *r = (lb_dyn_req_t *)arg;
    if (r) {
        int max_n = (int)((int64_t)r->max_duration_ms * SAMPLE_RATE_HZ / 1000);
        int16_t *buf = (int16_t *)heap_caps_malloc(
            (size_t)max_n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) {
            ESP_LOGE(TAG, "lb-dyn PSRAM alloc %d failed", max_n);
            if (r->cb) r->cb(AUDIO_LB_DONE, -100.0f, -100.0f, r->arg);
        } else {
            audio_record_loopback_dynamic(buf, max_n, r->stop_flag,
                                          r->cb, r->arg, NULL, NULL, NULL);
            heap_caps_free(buf);
        }
        free(r);
    }
    vTaskDelete(NULL);
}

bool audio_loopback_dynamic_async(int max_duration_ms,
                                   volatile bool *stop_flag,
                                   audio_loopback_phase_cb cb, void *cb_arg)
{
    if (!s_mic_ready || !s_ready) return false;
    /* Max 10 s per recording. At 22050 Hz mono int16 that's 440 KB —
     * well within PSRAM (~7 MB free), but a sensible cap keeps any
     * stuck-button scenario from eating the budget. */
    if (max_duration_ms < 200)    max_duration_ms = 200;
    if (max_duration_ms > 10000)  max_duration_ms = 10000;
    lb_dyn_req_t *r = (lb_dyn_req_t *)malloc(sizeof(*r));
    if (!r) return false;
    r->max_duration_ms = max_duration_ms;
    r->stop_flag = stop_flag;
    r->cb = cb;
    r->arg = cb_arg;
    BaseType_t ok = xTaskCreate(lb_dyn_task, "lb_dyn", 3072, r, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "lb-dyn task create failed");
        free(r);
        return false;
    }
    return true;
}

bool audio_play_tone_async(int freq_hz, int duration_ms, int volume_pct)
{
    if (!s_ready) return false;
    tone_req_t *req = (tone_req_t *)malloc(sizeof(*req));
    if (!req) return false;
    req->freq_hz     = freq_hz;
    req->duration_ms = duration_ms;
    req->volume_pct  = volume_pct;
    /* 2 KB stack — tone gen + codec_dev_write fit because the PCM chunk
     * itself is on the heap (see audio_play_tone). Internal SRAM is
     * very constrained when BLE + LVGL + audio are all up, so any byte
     * we save matters. Priority < LVGL so it doesn't starve the UI. */
    BaseType_t ok = xTaskCreate(tone_task, "tone", 2560, req, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "tone task create failed — heap caps: int=%u/%u psram=%u/%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        free(req);
        return false;
    }
    return true;
}
