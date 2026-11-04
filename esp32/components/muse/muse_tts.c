/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include "muse_tts.h"

#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"

#include "muse_audio.h"

static const char *TAG = "muse_tts";

/* Set in menuconfig: Muse > TTS PCM endpoint URL. */
#define TTS_URL CONFIG_MUSE_TTS_PCM_URL
#define HTTP_TIMEOUT_MS 120000

typedef struct {
    int16_t output[MUSE_AUDIO_CHUNK];
    size_t output_len;
    size_t played_frames;
    uint8_t pending_byte;
    bool have_pending_byte;
    bool failed;
} tts_ctx_t;

static bool flush_output(tts_ctx_t *ctx)
{
    if (!ctx->output_len) return true;
    if (muse_audio_write(ctx->output, ctx->output_len) != ESP_OK) {
        ctx->failed = true;
        return false;
    }
    ctx->played_frames += ctx->output_len;
    ctx->output_len = 0;
    return true;
}

static bool push_sample(tts_ctx_t *ctx, uint8_t low, uint8_t high)
{
    uint16_t bits = (uint16_t)low | ((uint16_t)high << 8);
    ctx->output[ctx->output_len++] = (int16_t)bits;
    return ctx->output_len < MUSE_AUDIO_CHUNK || flush_output(ctx);
}

static bool feed_pcm(tts_ctx_t *ctx, const uint8_t *data, size_t len)
{
    if (ctx->have_pending_byte && len) {
        if (!push_sample(ctx, ctx->pending_byte, *data++)) return false;
        ctx->have_pending_byte = false;
        len--;
    }
    while (len >= 2) {
        if (!push_sample(ctx, data[0], data[1])) return false;
        data += 2;
        len -= 2;
    }
    if (len) {
        ctx->pending_byte = *data;
        ctx->have_pending_byte = true;
    }
    return true;
}

static esp_err_t on_http_event(esp_http_client_event_t *event)
{
    tts_ctx_t *ctx = event ? event->user_data : NULL;
    if (!ctx) return ESP_FAIL;
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        if (esp_http_client_get_status_code(event->client) != 200) return ESP_OK;
        if (!feed_pcm(ctx, (const uint8_t *)event->data, (size_t)event->data_len)) {
            return ESP_FAIL;
        }
    } else if (event->event_id == HTTP_EVENT_ON_FINISH) {
        if (ctx->have_pending_byte) {
            ESP_LOGW(TAG, "PCM response ended on a partial sample");
            ctx->failed = true;
            return ESP_FAIL;
        }
        if (!flush_output(ctx)) return ESP_FAIL;
    }
    return ESP_OK;
}

bool muse_tts_speak(const char *text, size_t *played_frames)
{
    if (played_frames) *played_frames = 0;
    if (!text || !text[0]) return false;

    tts_ctx_t ctx = {0};
    esp_http_client_config_t config = {
        .url = TTS_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .event_handler = on_http_event,
        .user_data = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    esp_http_client_set_header(client, "Content-Type", "text/plain; charset=utf-8");
    esp_http_client_set_header(client, "Accept", "application/octet-stream");
    esp_http_client_set_post_field(client, text, (int)strlen(text));

    ESP_LOGI(TAG, "requesting PCM speech for %u characters", (unsigned)strlen(text));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || ctx.failed) {
        ESP_LOGW(TAG, "TTS request failed: err=%s status=%d", esp_err_to_name(err), status);
        return false;
    }
    if (!ctx.played_frames) {
        ESP_LOGW(TAG, "TTS returned no PCM audio");
        return false;
    }
    if (played_frames) *played_frames = ctx.played_frames;
    ESP_LOGI(TAG, "played %.2fs of TTS audio", (double)ctx.played_frames / MUSE_AUDIO_RATE);
    return true;
}
