/** 
 * Copyright (C) 2023 saybur
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version. 
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details. 
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
**/

#pragma once
#ifdef ENABLE_AUDIO_OUTPUT

#include <Arduino.h>

#ifdef __cplusplus
extern "C" {
#endif

// audio subsystem DMA channels
#define SOUND_DMA_CHA 6
#define SOUND_DMA_CHB 7

// size of the two audio sample buffers, in bytes
// these must be divisible by 1024
#define AUDIO_BUFFER_SIZE 8192 // ~46.44ms

extern bool audio_active;
// Tracker for determining if audio playback is occurring. This will be true
// whenever the audio stream is active, including during pause events.
static inline bool audio_is_active() { return audio_active; }

// performs initial setup of the audio subsystem
void audio_setup();

// handler for DMA interrupts
// This is called from scsi_dma_irq() in scsi_accel_rp2040; this is obviously
// a silly way to handle things, but irq_add_shared_handler() causes a lockup,
// likely due to pico-sdk issue #724 fixed in 1.3.1. This builds against 1.3.0
// and thus still has the bug. To work around the problem the above exclusive
// handler will delegate to this function if its mask is not matched.
void audio_dma_irq();

// called from platform_poll() to fill sample buffer(s) if needed
bool audio_poll();

/**
 * Begins audio playback for a file.
 *
 * \param file   Path of a file containing PCM samples to play.
 * \param start  Byte offset within file where playback will begin, inclusive.
 * \param end    Byte offset within file where playback will end, exclusive.
 * \param swap   If false, little-endian sample order, otherwise big-endian.
 * \return       True if successful, false otherwise.
 */
bool audio_play(const char* file, uint64_t start, uint64_t end, bool swap);

/**
 * Stops audio playback.
 */
void audio_stop();

#ifdef __cplusplus
}
#endif

#endif // ENABLE_AUDIO_OUTPUT