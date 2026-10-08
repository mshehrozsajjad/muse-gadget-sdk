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

// Push-to-talk voice chat with the agent. Hold the button and speak; on
// release the recording goes to the agent's chat as a voice note, which the
// VM transcribes, over the voice session borrowed from Muse (muse_chat.h).
// The reply is text and shows up in the Muse app; a TTS API of your own can
// speak it (start_tts in muse_chat_session.cpp).

#pragma once

#include "cJSON.h"
#include "sdkconfig.h"

// Start the voice task. It brings up the audio hardware and then takes over
// the button whenever a turn can run.
void voice_init(void);

// voice.configure: sets the speaker volume (0-100), kept in NVS. The dial on
// top sets it too.
cJSON *voice_configure_command(cJSON *params);

// Turn the volume by dial clicks (5 % each), show it, and store it once the
// turning stops. At 0 the speaker is off: replies are shown, not spoken.
void voice_turn_volume(int steps);
// The speaker volume, 0 to 100.
int voice_volume(void);
// Mute the speaker, keeping the volume for when it's unmuted (turning the
// volume unmutes too). Muted, or at volume 0, replies are shown, not spoken.
void voice_set_muted(bool muted);
bool voice_muted(void);
// Whether replies, cues and chimes are heard: volume above 0 and not muted.
bool voice_speaker_on(void);
// Mic off: the paddle sends nothing, keeping only its setup role. Neither
// this nor mute outlasts a restart.
void voice_set_mic(bool on);
bool voice_mic_on(void);

#if CONFIG_HOMEHUB_NOTE_COMMAND
// display.show_note: puts {"text": "..."} on the reply card as a note, which
// stays until the dial's push or the next voice turn, and chimes. Returns at
// once with {"ok": true}, or an invalid_params error.
cJSON *voice_note_command(cJSON *params);
#endif
