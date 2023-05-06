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

#ifdef ENABLE_AUDIO_OUTPUT

#include <SdFat.h>
#include <stdbool.h>
#include <hardware/dma.h>
#include <hardware/irq.h>
#include <hardware/spi.h>
#include <pico/multicore.h>
#include "audio.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_platform.h"

extern SdFs SD;

// Table with the number of '1' bits for each index.
// Used for SP/DIF parity calculations.
// Placed in SRAM5 for the second core to use with reduced contention.
const uint8_t snd_parity[256] __attribute__((aligned(256), section(".scratch_y.snd_parity"))) = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 
    4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8, };

/*
 * Precomputed biphase-mark patterns for data. For an 8-bit value this has
 * 16-bits in MSB-first order for the correct high/low transitions to
 * represent the data, given an output clocking rate twice the bitrate (so the
 * bits '11' or '00' reflect a zero and '10' or '01' represent a one). Each
 * value below starts with a '1' and will need to be inverted if the last bit
 * of the previous mask was also a '1'. These values can be written to an
 * appropriately configured SPI peripheral to blast biphase data at a
 * receiver.
 * 
 * To facilitate fast lookups this table should be put in SRAM with low
 * contention, aligned to an apppropriate boundry.
 */
const uint16_t biphase[256] __attribute__((aligned(512), section(".scratch_y.biphase"))) = {
    0xCCCC, 0xB333, 0xD333, 0xACCC, 0xCB33, 0xB4CC, 0xD4CC, 0xAB33,
    0xCD33, 0xB2CC, 0xD2CC, 0xAD33, 0xCACC, 0xB533, 0xD533, 0xAACC,
    0xCCB3, 0xB34C, 0xD34C, 0xACB3, 0xCB4C, 0xB4B3, 0xD4B3, 0xAB4C,
    0xCD4C, 0xB2B3, 0xD2B3, 0xAD4C, 0xCAB3, 0xB54C, 0xD54C, 0xAAB3,
    0xCCD3, 0xB32C, 0xD32C, 0xACD3, 0xCB2C, 0xB4D3, 0xD4D3, 0xAB2C,
    0xCD2C, 0xB2D3, 0xD2D3, 0xAD2C, 0xCAD3, 0xB52C, 0xD52C, 0xAAD3,
    0xCCAC, 0xB353, 0xD353, 0xACAC, 0xCB53, 0xB4AC, 0xD4AC, 0xAB53,
    0xCD53, 0xB2AC, 0xD2AC, 0xAD53, 0xCAAC, 0xB553, 0xD553, 0xAAAC,
    0xCCCB, 0xB334, 0xD334, 0xACCB, 0xCB34, 0xB4CB, 0xD4CB, 0xAB34,
    0xCD34, 0xB2CB, 0xD2CB, 0xAD34, 0xCACB, 0xB534, 0xD534, 0xAACB,
    0xCCB4, 0xB34B, 0xD34B, 0xACB4, 0xCB4B, 0xB4B4, 0xD4B4, 0xAB4B,
    0xCD4B, 0xB2B4, 0xD2B4, 0xAD4B, 0xCAB4, 0xB54B, 0xD54B, 0xAAB4,
    0xCCD4, 0xB32B, 0xD32B, 0xACD4, 0xCB2B, 0xB4D4, 0xD4D4, 0xAB2B,
    0xCD2B, 0xB2D4, 0xD2D4, 0xAD2B, 0xCAD4, 0xB52B, 0xD52B, 0xAAD4,
    0xCCAB, 0xB354, 0xD354, 0xACAB, 0xCB54, 0xB4AB, 0xD4AB, 0xAB54,
    0xCD54, 0xB2AB, 0xD2AB, 0xAD54, 0xCAAB, 0xB554, 0xD554, 0xAAAB,
    0xCCCD, 0xB332, 0xD332, 0xACCD, 0xCB32, 0xB4CD, 0xD4CD, 0xAB32,
    0xCD32, 0xB2CD, 0xD2CD, 0xAD32, 0xCACD, 0xB532, 0xD532, 0xAACD,
    0xCCB2, 0xB34D, 0xD34D, 0xACB2, 0xCB4D, 0xB4B2, 0xD4B2, 0xAB4D,
    0xCD4D, 0xB2B2, 0xD2B2, 0xAD4D, 0xCAB2, 0xB54D, 0xD54D, 0xAAB2,
    0xCCD2, 0xB32D, 0xD32D, 0xACD2, 0xCB2D, 0xB4D2, 0xD4D2, 0xAB2D,
    0xCD2D, 0xB2D2, 0xD2D2, 0xAD2D, 0xCAD2, 0xB52D, 0xD52D, 0xAAD2,
    0xCCAD, 0xB352, 0xD352, 0xACAD, 0xCB52, 0xB4AD, 0xD4AD, 0xAB52,
    0xCD52, 0xB2AD, 0xD2AD, 0xAD52, 0xCAAD, 0xB552, 0xD552, 0xAAAD,
    0xCCCA, 0xB335, 0xD335, 0xACCA, 0xCB35, 0xB4CA, 0xD4CA, 0xAB35,
    0xCD35, 0xB2CA, 0xD2CA, 0xAD35, 0xCACA, 0xB535, 0xD535, 0xAACA,
    0xCCB5, 0xB34A, 0xD34A, 0xACB5, 0xCB4A, 0xB4B5, 0xD4B5, 0xAB4A,
    0xCD4A, 0xB2B5, 0xD2B5, 0xAD4A, 0xCAB5, 0xB54A, 0xD54A, 0xAAB5,
    0xCCD5, 0xB32A, 0xD32A, 0xACD5, 0xCB2A, 0xB4D5, 0xD4D5, 0xAB2A,
    0xCD2A, 0xB2D5, 0xD2D5, 0xAD2A, 0xCAD5, 0xB52A, 0xD52A, 0xAAD5,
    0xCCAA, 0xB355, 0xD355, 0xACAA, 0xCB55, 0xB4AA, 0xD4AA, 0xAB55,
    0xCD55, 0xB2AA, 0xD2AA, 0xAD55, 0xCAAA, 0xB555, 0xD555, 0xAAAA };
/*
 * Biphase frame headers for SP/DIF, including the special bit framing
 * errors used to detect (sub)frame start conditions. See above table
 * for details.
 */
const uint16_t x_preamble = 0xE2CC;
const uint16_t y_preamble = 0xE4CC;
const uint16_t z_preamble = 0xE8CC;

// DMA configuration info
static dma_channel_config snd_dma_a_cfg;
static dma_channel_config snd_dma_b_cfg;

// some chonky buffers to store audio samples
static uint8_t sample_buf_a[AUDIO_BUFFER_SIZE];
static uint8_t sample_buf_b[AUDIO_BUFFER_SIZE];

// buffers for storing biphase patterns
#define SAMPLE_CHUNK_SIZE 1024
#define WIRE_BUFFER_SIZE (SAMPLE_CHUNK_SIZE * 2)
static uint16_t wire_buf_a[WIRE_BUFFER_SIZE];
static uint16_t wire_buf_b[WIRE_BUFFER_SIZE];

// tracking for the state of the above buffers
// first two are shared between cores, second two are Core1 only
enum bufstate { STALE, FILLING, READY };
static volatile bufstate abufst = STALE;
static volatile bufstate bbufst = STALE;
enum bufselect { A, B };
static bufselect sbufsel = A;
static uint16_t sbufpos = 0;

// trackers for the below function call
static uint16_t sfcnt = 0; // sub-frame count; 2 per frame, 192 frames/block
static uint8_t invert = 0; // biphase encode help: set if last wire bit was '1'

/*
 * Translates 16-bit stereo sound samples to biphase wire patterns for the
 * SPI peripheral. Produces 8 patterns (128 bits, or 1 SP/DIF frame) per pair
 * of input samples. Provided length is the total number of sample bytes present,
 * _twice_ the number of samples (little-endian order assumed)
 * 
 * This function operates with side-effects and is not safe to call from both
 * cores. It must also be called in the same order data is intended to be
 * output.
 */
static void snd_encode(uint8_t* samples, uint16_t* wire_patterns, uint16_t len) {
    uint16_t widx = 0;
    for (uint16_t i = 0; i < len; i += 2) {
        uint32_t sample = samples[i] + (samples[i + 1] << 8);
        // determine parity, simplified to one lookup via an XOR
        uint8_t parity = (sample >> 8) ^ sample;
        parity = snd_parity[parity];

        /*
        * Shift sample into the correct bit positions of the sub-frame. This
        * would normally be << 12, but with my DACs I've had persistent issues
        * with signal clipping when sending data in the highest bit position.
        */
        sample = sample << 11;
        if (sample & 0x04000000) {
            // restore "negative sign"
            sample |= 0x08000000;
            parity++;
        }

        // if needed, establish even parity with P bit
        if (parity % 2) sample |= 0x80000000;

        // translate sample into biphase encoding
        // first is low 8 bits: preamble and 4 least-significant bits of 
        // 24-bit audio, pre-encoded as all '0' due to 16-bit samples
        uint16_t wp;
        if (sfcnt == 0) {
            wp = z_preamble; // left channel, block start
        } else if (sfcnt % 2) {
            wp = y_preamble; // right channel
        } else {
            wp = x_preamble; // left channel, not block start
        }
        if (invert) wp = ~wp;
        invert = wp & 1;
        wire_patterns[widx++] = wp;
        // next 8 bits (only high 4 have data)
        wp = biphase[(uint8_t) (sample >> 8)];
        if (invert) wp = ~wp;
        invert = wp & 1;
        wire_patterns[widx++] = wp;
        // next 8 again, all audio data
        wp = biphase[(uint8_t) (sample >> 16)];
        if (invert) wp = ~wp;
        invert = wp & 1;
        wire_patterns[widx++] = wp;
        // final 8, low 4 audio data and high 4 control bits
        wp = biphase[(uint8_t) (sample >> 24)];
        if (invert) wp = ~wp;
        invert = wp & 1;
        wire_patterns[widx++] = wp;
        // increment subframe counter for next pass
        sfcnt++;
        if (sfcnt == 384) sfcnt = 0; // if true, block complete
    }
}

// functions for passing to Core1
static void snd_process_a() {
    if (sbufsel == A) {
        snd_encode(sample_buf_a + sbufpos, wire_buf_a, SAMPLE_CHUNK_SIZE);
        sbufpos += SAMPLE_CHUNK_SIZE;
        if (sbufpos >= AUDIO_BUFFER_SIZE) {
            sbufsel = B;
            sbufpos = 0;
            abufst = STALE;
        }
    } else {
        snd_encode(sample_buf_b + sbufpos, wire_buf_a, SAMPLE_CHUNK_SIZE);
        sbufpos += SAMPLE_CHUNK_SIZE;
        if (sbufpos >= AUDIO_BUFFER_SIZE) {
            sbufsel = A;
            sbufpos = 0;
            bbufst = STALE;
        }
    }
}
static void snd_process_b() {
    // clone of above for the other wire buffer
    if (sbufsel == A) {
        snd_encode(sample_buf_a + sbufpos, wire_buf_b, SAMPLE_CHUNK_SIZE);
        sbufpos += SAMPLE_CHUNK_SIZE;
        if (sbufpos >= AUDIO_BUFFER_SIZE) {
            sbufsel = B;
            sbufpos = 0;
            abufst = STALE;
        }
    } else {
        snd_encode(sample_buf_b + sbufpos, wire_buf_b, SAMPLE_CHUNK_SIZE);
        sbufpos += SAMPLE_CHUNK_SIZE;
        if (sbufpos >= AUDIO_BUFFER_SIZE) {
            sbufsel = A;
            sbufpos = 0;
            bbufst = STALE;
        }
    }
}

// Allows execution on Core1 via function pointers. Each function can take
// no parameters and should return nothing, operating via side-effects only.
static void core1_handler() {
    while (1) {
        void (*function)() = (void (*)()) multicore_fifo_pop_blocking();
        (*function)();
    }
}

/* ------------------------------------------------------------------------ */
/* ---------- VISIBLE FUNCTIONS ------------------------------------------- */
/* ------------------------------------------------------------------------ */

// interrupt handler for resetting DMA units
// note use of blocking call, this shouldn't be an issue unless
// a stall occurs... however, if that happens debugging it will be
// a PITA, so potentially consider a different approach
void audio_dma_irq() {
    if (dma_hw->ints0 & (1 << SOUND_DMA_CHA)) {
        dma_hw->ints0 = (1 << SOUND_DMA_CHA);
        multicore_fifo_push_blocking((uintptr_t) &snd_process_a);
        dma_channel_configure(SOUND_DMA_CHA,
                &snd_dma_a_cfg,
                &(spi_get_hw(AUDIO_SPI)->dr),
                &wire_buf_a,
                WIRE_BUFFER_SIZE,
                false);
    } else if (dma_hw->ints0 & (1 << SOUND_DMA_CHB)) {
        dma_hw->ints0 = (1 << SOUND_DMA_CHB);
        multicore_fifo_push_blocking((uintptr_t) &snd_process_b);
        dma_channel_configure(SOUND_DMA_CHB,
                &snd_dma_b_cfg,
                &(spi_get_hw(AUDIO_SPI)->dr),
                &wire_buf_b,
                WIRE_BUFFER_SIZE,
                false);
    }
}

void audio_setup() {
    // setup SPI to blast SP/DIF data over the TX pin
    // --- TODO REMOVE THESE
    // --- I don't think they are technically necessary
    reset_block(RESETS_RESET_SPI1_BITS);
    unreset_block_wait(RESETS_RESET_SPI1_BITS);

    spi_set_baudrate(AUDIO_SPI, 5644800); // will be slightly wrong, ~0.03% slow
    hw_write_masked(&spi_get_hw(AUDIO_SPI)->cr0,
            0x1F, // TI mode with 16 bits
            SPI_SSPCR0_DSS_BITS | SPI_SSPCR0_FRF_BITS);
    hw_set_bits(&spi_get_hw(AUDIO_SPI)->dmacr, SPI_SSPDMACR_TXDMAE_BITS | SPI_SSPDMACR_RXDMAE_BITS);
    hw_set_bits(&spi_get_hw(AUDIO_SPI)->cr1, SPI_SSPCR1_SSE_BITS);

    dma_channel_claim(SOUND_DMA_CHA);
	dma_channel_claim(SOUND_DMA_CHB);

    // setup the two units to hand-off to each other
    // there is no limiting or checking on these at present
	snd_dma_a_cfg = dma_channel_get_default_config(SOUND_DMA_CHA);
	channel_config_set_transfer_data_size(&snd_dma_a_cfg, DMA_SIZE_16);
	channel_config_set_dreq(&snd_dma_a_cfg, spi_get_dreq(spi1, true));
	channel_config_set_read_increment(&snd_dma_a_cfg, true);
	channel_config_set_chain_to(&snd_dma_a_cfg, SOUND_DMA_CHB);
	dma_channel_configure(SOUND_DMA_CHA, &snd_dma_a_cfg, &(spi_get_hw(spi1)->dr),
			&wire_buf_a, WIRE_BUFFER_SIZE, false);
    dma_channel_set_irq0_enabled(SOUND_DMA_CHA, true);
	snd_dma_b_cfg = dma_channel_get_default_config(SOUND_DMA_CHB);
	channel_config_set_transfer_data_size(&snd_dma_b_cfg, DMA_SIZE_16);
	channel_config_set_dreq(&snd_dma_b_cfg, spi_get_dreq(spi1, true));
	channel_config_set_read_increment(&snd_dma_b_cfg, true);
	channel_config_set_chain_to(&snd_dma_b_cfg, SOUND_DMA_CHA);
	dma_channel_configure(SOUND_DMA_CHB, &snd_dma_b_cfg, &(spi_get_hw(spi1)->dr),
			&wire_buf_b, WIRE_BUFFER_SIZE, false);
    dma_channel_set_irq0_enabled(SOUND_DMA_CHB, true);

    logmsg("Starting Core1 for audio");
    multicore_launch_core1(core1_handler);
}

static bool running = false;
static uint8_t* audio_buffer() {
    if (!running && abufst == READY && bbufst == READY) {
        dma_channel_start(SOUND_DMA_CHA);
        running = true;
    }

    if (abufst == STALE) {
        abufst = FILLING;
        return sample_buf_a;
    } else if (bbufst == STALE) {
        bbufst = FILLING;
        return sample_buf_b;
    } else {
        return NULL;
    }
}

static void audio_buffer_filled() {
    if (abufst == FILLING) {
        abufst = READY;
    } else if (bbufst == FILLING) {
        bbufst = READY;
    }
}

static uint32_t audioPos = 0;
void audio_poll() {
    uint8_t* audiobuf = audio_buffer();
    if (audiobuf != NULL) {
        platform_set_sd_callback(NULL, NULL);
        FsFile audioFile = SD.open("valk.raw", O_RDONLY);
        audioFile.seek(audioPos);
        audioFile.read(audiobuf, AUDIO_BUFFER_SIZE);
        audioFile.close();
        audioPos += AUDIO_BUFFER_SIZE;
        if(audioPos >= 59768832) audioPos = 0;
        audio_buffer_filled();
    }
}

#endif // ENABLE_AUDIO_OUTPUT