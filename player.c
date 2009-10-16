/*
 * Copyright (C) 2009 Mark Hills <mark@pogo.org.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "device.h"
#include "player.h"
#include "track.h"
#include "timecoder.h"


/* minimum movement of the timecode to switch to PLAYER_PLAYING status */

#define PLAYING_DELTA 0.00001


/* Bend playback speed to compensate for the difference between our
 * current position and that given by the timecode */

#define SYNC_TIME (1.0 / 2) /* time taken to reach sync */
#define SYNC_PITCH 0.05 /* don't sync at low pitches */
#define SYNC_RC 0.05 /* filter to 1.0 when no timecodes available */


/* If the difference between our current position and that given by
 * the timecode is greater than this value, recover by jumping
 * straight to the position given by the timecode. */

#define SKIP_THRESHOLD (1.0 / 8) /* before dropping audio */


/* Smooth the pitch returned from the timecoder. Smooth it too much
 * and we end up having to use sync to bend in line with the
 * timecode. Smooth too little and there will be audible
 * distortion. */

#define PITCH_RC 0.004


/* The base volume level. A value of 1.0 leaves no headroom to play
 * louder when the record is going faster than 1.0. */

#define VOLUME (7.0/8)


#define SQ(x) ((x)*(x))


/* Build a block of PCM audio, resampled from the track. Always builds
 * 'frame' samples, and returns the number of seconds to advance the
 * track position by. This is just a basic resampler which has
 * particular problems where pitch > 1.0. */

static double build_pcm(signed short *pcm, int samples, int rate,
                        struct track_t *tr, double position, float pitch,
                        float start_vol, float end_vol)
{
    signed short a, b, *pa, *pb;
    int s, c, sa, sb;
    double sample, step;
    float f, vol;

    sample = position * tr->rate;
    step = (double)pitch * tr->rate / rate;

    for(s = 0; s < samples; s++) {

        /* Calculate the pcm samples which sample falls
         * inbetween. sample can be positive or negative */

        sa = (int)sample;
        if(sample < 0.0)
            sa--;
        sb = sa + 1;
        f = sample - sa;

        vol = start_vol + ((end_vol - start_vol) * s / samples);

        if(sa >= 0 && sa < tr->length)
            pa = track_get_sample(tr, sa);
        else
            pa = NULL;

        if(sb >= 0 && sb < tr->length)
            pb = track_get_sample(tr, sb);
        else
            pb = NULL;

        for(c = 0; c < PLAYER_CHANNELS; c++) {
            a = pa ? *(pa + c) : 0;
            b = pb ? *(pb + c) : 0;
            *pcm++ = vol * ((1.0 - f) * a + f * b);
        }

        sample += step;
    }

    return (double)pitch * samples / rate;
}


void player_init(struct player_t *pl)
{
    pl->status = PLAYER_STOPPED;
    pl->reconnect = 0;

    pl->position = 0.0;
    pl->offset = 0.0;
    pl->target_valid = 0;
    pl->last_difference = 0.0;

    pl->pitch = 0.0;
    pl->sync_pitch = 1.0;
    pl->volume = 0.0;

    pl->track = NULL;
    pl->timecoder = NULL;
}


void player_clear(struct player_t *pl)
{
}


void player_connect_timecoder(struct player_t *pl, struct timecoder_t *tc)
{
    pl->timecoder = tc;
    pl->reconnect = 1;
}


void player_disconnect_timecoder(struct player_t *pl)
{
    pl->timecoder = NULL;
}


static int sync_to_timecode(struct player_t *pl)
{
    float when;
    double tcpos;
    signed int timecode;

    timecode = timecoder_get_position(pl->timecoder, &when);

    /* Instruct the caller to disconnect the timecoder if the needle
     * is outside the 'safe' zone of the record */

    if(timecode != -1 && timecode > timecoder_get_safe(pl->timecoder))
        return -1;

    /* If the timecoder is alive, use the pitch from the sine wave */

    pl->pitch = timecoder_get_pitch(pl->timecoder);

    /* If we can read an absolute time from the timecode, then use it */
    
    if(timecode == -1)
	pl->target_valid = 0;

    else {
        tcpos = (double)timecode / timecoder_get_resolution(pl->timecoder);
        pl->target_position = tcpos + pl->pitch * when;
	pl->target_valid = 1;
    }

    return 0;
}


/* Return to the zero of the track */

int player_recue(struct player_t *pl)
{
    pl->offset = pl->position;
    return 0;
}


/* Get a block of PCM audio data to send to the soundcard. */

int player_collect(struct player_t *pl, signed short *pcm,
                   int samples, int rate)
{
    double diff;
    float dt, target_volume;

    dt = (float)samples / rate;

    if(pl->timecoder) {
        if (sync_to_timecode(pl) == -1)
            player_disconnect_timecoder(pl);
    }

    if(!pl->target_valid) {

        /* Without timecode sync, tend sync_pitch towards 1.0, to
         * avoid using outlier values from scratching for too long */

        pl->sync_pitch += dt / (SYNC_RC + dt) * (1.0 - pl->sync_pitch);

    } else {

        /* If reconnection has been requested, move the logical record
         * on the vinyl so that the current position is right under
         * the needle, and continue */

        if(pl->reconnect) {
            pl->offset += pl->target_position - pl->position;
	    pl->position = pl->target_position;
            pl->reconnect = 0;
        }

        /* Calculate the pitch compensation required to get us back on
         * track with the absolute timecode position */

        diff = pl->position - pl->target_position;
        pl->last_difference = diff; /* to print in user interface */
        
        if(fabs(diff) > SKIP_THRESHOLD) {

            /* Jump the track to the time */
            
            pl->position = pl->target_position;
            fprintf(stderr, "Seek to new position %.2lfs.\n", pl->position);

        } else if(fabs(pl->pitch) > SYNC_PITCH) {

            /* Re-calculate the drift between the timecoder pitch from
             * the sine wave and the timecode values */

            pl->sync_pitch = pl->pitch / (diff / SYNC_TIME + pl->pitch);

        }

        /* Acknowledge that we've accounted for the target position */
        
        pl->target_valid = 0;
    }

    target_volume = fabs(pl->pitch) * VOLUME;
    if(target_volume > 1.0)
        target_volume = 1.0;

    /* Sync pitch is applied post-filtering */

    double seconds = build_pcm(pcm, samples, rate,
			      pl->track,
                              pl->position - pl->offset,
                              pl->pitch * pl->sync_pitch,
                              pl->volume, target_volume);
                              
    if (fabs(seconds) > PLAYING_DELTA)
      pl->status = PLAYER_PLAYING;
    else      
      pl->status = PLAYER_STOPPED;
      
    pl->position += seconds;    
    pl->volume = target_volume;

    return 0;
}


void player_connect_track(struct player_t *pl, struct track_t *tr)
{
    pl->track = tr;
}
