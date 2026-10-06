/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Speech for Muse's replies from ElevenLabs' streaming text-to-speech API
 * (POST /v1/text-to-speech/{voice}/stream), as MP3. A task of its own does
 * the HTTPS request; the session task takes the bytes with muse_tts_read()
 * and decodes them like any reply audio. One reply at a time: starting
 * another, or cancelling, abandons the one before.
 */

typedef enum {
    MUSE_TTS_IDLE,
    MUSE_TTS_STARTING,  /* asked for; nothing to read yet */
    MUSE_TTS_FETCHING,  /* MP3 arriving */
    MUSE_TTS_DONE,      /* all of it fetched; read what's left */
    MUSE_TTS_FAILED,    /* the request failed; read what came, if anything */
} muse_tts_state_t;

/* The MP3 comes at 32 kbps: this many bytes a second of speech. */
#define MUSE_TTS_MP3_BYTES_PER_S 4000

/* An API key and voice are set (CONFIG_MUSE_TTS_ELEVENLABS_*). */
bool muse_tts_configured(void);

/* Starts fetching speech for `text` (copied). False if it can't start. */
bool muse_tts_start(const char *text);

muse_tts_state_t muse_tts_state(void);

/* Non-blocking: copies up to `cap` bytes of MP3 into `buf`; returns how many. */
size_t muse_tts_read(uint8_t *buf, size_t cap);

/* Abandons the speech being fetched. */
void muse_tts_cancel(void);

#ifdef __cplusplus
}
#endif
