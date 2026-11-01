/*
 *  This file is part of 'Johnny Reborn'
 *
 *  An open-source engine for the classic
 *  'Johnny Castaway' screensaver by Sierra.
 *
 *  Copyright (C) 2019 Jeremie GUILLAUME
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

#include "mytypes.h"
#include "utils.h"
#include "graphics.h"
#include "sound.h"


#define NUM_OF_SOUNDS  25


struct TSound {
    uint32  length;
    uint8   *data;
};


int soundDisabled = 0;


static struct TSound sounds[NUM_OF_SOUNDS];
static struct TSound *currentSound;

static uint8  *currentPtr;
static uint32 currentRemaining;

// Harness hook buffers (JC_SNDTRACE, see soundPlay()/soundCallback()):
// the value is a path; the rendered device stream (11025 Hz U8 mono) is
// written there verbatim.
static FILE *pcmDumpFp = NULL;

// In lazy regime, the WAVs are only loaded the first time they are
// played. This remembers which files should not be probed again
// (missing files), so that they don't get re-opened at every play.
static uint8 soundProbed[NUM_OF_SOUNDS];


static void loadWav(int nb)
{
    char filename[20];
    SDL_AudioSpec audioSpec;

    sprintf(filename, "sound%d.wav", nb);

    if (SDL_LoadWAV(filename, &audioSpec, &sounds[nb].data, &sounds[nb].length) == NULL) {
        sounds[nb].data   = NULL;
        sounds[nb].length = 0;
        debugMsg("SDL_LoadWAV() warning: %s", SDL_GetError());
    }
    else {
        memTrackLogf("wav %s: length=%zu (SDL-allocated buffer)",
                     filename, (size_t) sounds[nb].length);
        memTrackExternalAdd(sounds[nb].length);
    }

    soundProbed[nb] = 1;
}


// Click-free sample switching: when soundPlay() preempts a still-sounding
// sample with a different one, the preempted sample's natural tail
// (~5 ms) is kept and folded over the top of the new sample's head
// (linear sum, clipped), so the waveform steps through the switch
// instead of jumping at full phase. The new sample still starts at the
// same trigger instant — only the switch edge is smoothed. Tail bytes
// are read-only pointers into SDL-owned buffers; no WAV data is
// modified.
#define SND_TAIL_FADE_BYTES  48   // ~4.4 ms at 11025 Hz

static uint8  *tailPtr;
static uint32  tailRemaining;


static void soundCallback(void *userdata, uint8 *stream, int rqdLen)
{
    uint32 n = ((uint32) currentRemaining > (uint32) rqdLen)
                   ? (uint32) rqdLen : currentRemaining;

    // Current sample from its head
    if (n > 0) {
        memcpy(stream, currentPtr, n);
        currentPtr       += n;
        currentRemaining -= n;
    }

    // Fade-out tail of the preempted sample, mixed over the first bytes
    // (over the new sample's head if still streaming, else over the
    // silence it naturally continues into)
    if (tailRemaining > 0) {
        uint32 m = (tailRemaining < (uint32) rqdLen) ? tailRemaining : (uint32) rqdLen;

        for (uint32 i=0; i < m; i++) {
            int out = stream[i] + (int) tailPtr[i] - 127;
            stream[i] = out > 255 ? 255 : (out < 0 ? 0 : (uint8) out);
        }

        tailPtr       += m;
        tailRemaining -= m;
    }

    if (n < (uint32) rqdLen)
        memset(stream + n, 128, rqdLen - (int) n);   // 128 == U8 silence

    // Harness hook: render the exact device stream to a file, so audio
    // behaviour can be inspected offline (truncation clicks, gaps,
    // silence levels) without a real device.
    if (pcmDumpFp != NULL)
        fwrite(stream, rqdLen, 1, pcmDumpFp);
}


void soundInit()
{
    // Harness hook (JC_SNDTRACE): see soundPlay()/soundCallback().
    if (getenv("JC_SNDTRACE") != NULL) {
        pcmDumpFp = fopen(getenv("JC_SNDTRACE"), "wb");
        if (pcmDumpFp == NULL)
            debugMsg("JC_SNDTRACE: cannot open '%s'", getenv("JC_SNDTRACE"));
    }

    if (soundDisabled)
        return;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        debugMsg("SDL init audio error: %s", SDL_GetError());
        soundDisabled = 1;
        return;
    }

    SDL_AudioSpec audioSpec;

    // The WAV files are loaded on demand by loadWav(), the first time
    // they are played. The engine plays their bytes verbatim in
    // soundCallback() (a memcpy, no conversion), so the audio device is
    // opened in the WAV set's native format: 11025 Hz, mono, unsigned
    // 8-bit, as used by the shipped sound*.wav files.
    audioSpec.freq     = 11025;
    audioSpec.format   = AUDIO_U8;
    audioSpec.channels = 1;

    audioSpec.callback = soundCallback;
    audioSpec.userdata = NULL;
    audioSpec.samples  = 1024;

    if (SDL_OpenAudio(&audioSpec, NULL) < 0) {
        debugMsg("SDL_OpenAudio() error: %s", SDL_GetError());
        soundDisabled = 1;
        return;
    }

    currentRemaining = 0;
    SDL_PauseAudio(0); //SDL_PauseAudio(1);
}


void soundEnd()
{
    if (pcmDumpFp != NULL) {
        fclose(pcmDumpFp);
        pcmDumpFp = NULL;
    }

    if (soundDisabled)
        return;

    SDL_CloseAudio();

    for (int i=0; i < NUM_OF_SOUNDS; i++)
        if (sounds[i].data != NULL) {
            memTrackExternalRemove(sounds[i].length);
            SDL_FreeWAV(sounds[i].data);
        }
}


void soundPlay(int nb)
{
    if (soundDisabled)
        return;

    if (nb < 0 || NUM_OF_SOUNDS <= nb) {
        debugMsg("soundPlay(): wrong sound sample index #%d", nb);
        return;
    }

    // Lazy loading of the WAV file, the first time this sample is used
    if (sounds[nb].data == NULL && !soundProbed[nb])
        loadWav(nb);

    if (sounds[nb].length) {

        // The single-voice channel plays every sample to completion. A
        // retrigger of the sample that is still sounding is dropped, so
        // ambience clips fired at animation cadence play as complete
        // samples rather than being restarted repeatedly. A different
        // sample still preempts normally (see the click-free switch
        // below).
        if (currentSound == &sounds[nb] && currentRemaining > 0) {
            if (getenv("JC_SNDTRACE") != NULL)
                fprintf(stderr, "SNDDROP t=%7ums nb=%2d (still sounding - dropped)\n",
                        grTicks(), nb);
            return;
        }

        // Click-free switching (see soundCallback()): when a different
        // sample preempts a still-sounding one, keep the preempted
        // sample's last bytes as a short fade-out tail so the waveform
        // doesn't step discontinuously into the new sample. The new
        // sound still *starts* at the same trigger instant, so the
        // engine's timeline is untouched - this only smooths the
        // switch edge.
        if (currentRemaining > SND_TAIL_FADE_BYTES) {
            tailPtr       = currentPtr + currentRemaining - SND_TAIL_FADE_BYTES;
            tailRemaining = SND_TAIL_FADE_BYTES;
        } else if (currentRemaining > 0) {
            tailPtr       = currentPtr;
            tailRemaining = currentRemaining;
        } else {
            tailRemaining = 0;
        }

        if (getenv("JC_SNDTRACE") != NULL) {
            static int    prevNb  = -1;
            static uint32 prevLen = 0;

            // Absolute engine time (grTicks ms), same epoch as the
            // SCENE/WALK correlation marks in ads.c.
            uint32 now = grTicks();

            fprintf(stderr,
                    "SND t=%7ums nb=%2d dur=%4ums prev=%2d heardPrev=%4uB (%3.0f%% complete)\n",
                    now, nb,
                    (uint32) (sounds[nb].length * 1000 / 11025),
                    prevNb,
                    prevLen - currentRemaining,
                    prevLen ? 100.0 * (prevLen - currentRemaining) / prevLen : 100.0);

            prevNb  = nb;
            prevLen = sounds[nb].length;
        }

        SDL_LockAudio();
        //SDL_PauseAudio(1);

        currentSound     = &sounds[nb];
        currentPtr       = currentSound->data;
        currentRemaining = currentSound->length;

        //SDL_PauseAudio(0);
        SDL_UnlockAudio();
    }
    else {
        debugMsg("Non-existent sound sample #%d", nb);

        // One-time visible note for a sample that is missing from the
        // sound file set (debug or JC_SNDTRACE). The standard set is
        // sound0..sound10, sound12, sound14..sound24: sound11.wav and
        // sound13.wav do not exist, even though GJCATCH2.TTM references
        // sample 11. The note is informational, not an installation
        // error.
        if (!soundProbed[nb] || getenv("JC_SNDTRACE") != NULL)
            fprintf(stderr, "JC_SND: sound%d.wav missing or empty\n", nb);
    }
}

