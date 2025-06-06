//
// Copyright (C) 1999 by
// id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
// Copyright(C) 2020-2023 Fabian Greffrath
// Copyright(C) 2023 ceski
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//      System interface for WinMBF sound.
//

#include <stdlib.h>

#include "doomstat.h"
#include "doomtype.h"
#include "i_oalsound.h"
#include "i_sound.h"
#include "m_config.h"
#include "m_fixed.h"
#include "p_mobj.h"
#include "r_main.h"
#include "sounds.h"
#include "tables.h"

static boolean force_flip_pan;

static boolean I_MBF_AdjustSoundParams(const mobj_t *listener,
                                       const mobj_t *source,
                                       sfxparams_t *params)
{
    int adx, ady, dist;
    angle_t angle;

    // haleyjd 05/29/06: allow per-channel volume scaling
    params->volume = snd_SfxVolume * params->volume_scale / 15;

    if (params->volume < 1)
    {
        return false;
    }
    else if (params->volume > 127)
    {
        params->volume = 127;
    }

    params->separation = NORM_SEP;

    if (!source || source == players[displayplayer].mo)
    {
        return true;
    }

    // haleyjd 08/12/04: we cannot adjust a sound for a NULL listener.
    if (!listener)
    {
        return true;
    }

    // calculate the distance to sound origin
    //  and clip it if necessary
    //
    // killough 11/98: scale coordinates down before calculations start
    // killough 12/98: use exact distance formula instead of approximation

    adx = abs((listener->x >> FRACBITS) - (source->x >> FRACBITS));
    ady = abs((listener->y >> FRACBITS) - (source->y >> FRACBITS));

    if (ady > adx)
    {
        const int temp = adx;
        adx = ady;
        ady = temp;
    }

    if (adx)
    {
        const int slope = FixedDiv(ady, adx) >> DBITS;
        const int angle = tantoangle[slope] >> ANGLETOFINESHIFT;
        dist = FixedDiv(adx, finecosine[angle]);
    }
    else
    {
        dist = 0;
    }

    if (!dist) // killough 11/98: handle zero-distance as special case
    {
        return true;
    }

    if (dist >= params->clipping_dist)
    {
        return false;
    }

    if (source->x != players[displayplayer].mo->x
        || source->y != players[displayplayer].mo->y)
    {
        // angle of source to listener
        angle = R_PointToAngle2(listener->x, listener->y, source->x, source->y);

        if (angle <= listener->angle)
        {
            angle += 0xffffffff;
        }
        angle -= listener->angle;
        angle >>= ANGLETOFINESHIFT;

        // stereo separation
        params->separation -= FixedMul(S_STEREO_SWING, finesine[angle]);
    }

    // volume calculation
    if (dist > params->close_dist)
    {
        params->volume = params->volume * (params->clipping_dist - dist)
                         / (params->clipping_dist - params->close_dist);
    }

    // haleyjd 09/27/06: decrease priority with volume attenuation
    params->priority += (127 - params->volume);

    if (params->priority > 255) // cap to 255
    {
        params->priority = 255;
    }

    return (params->volume > 0);
}

static void I_MBF_UpdateSoundParams(int channel, const sfxparams_t *params)
{
    int separation = params->separation;

    // SoM 7/1/02: forceFlipPan accounted for here
    if (force_flip_pan)
    {
        separation = 254 - separation;
    }

    I_OAL_SetVolume(channel, params->volume);
    I_OAL_SetPan(channel, separation);
}

static boolean I_MBF_InitSound(void)
{
    return I_OAL_InitSound(SND_MODULE_MBF);
}

static boolean I_MBF_ReinitSound(void)
{
    return I_OAL_ReinitSound(SND_MODULE_MBF);
}

static void I_MBF_BindVariables(void)
{
    BIND_BOOL(force_flip_pan, false, "Force reversal of stereo audio channels");
}

const sound_module_t sound_mbf_module =
{
    I_MBF_InitSound,
    I_MBF_ReinitSound,
    I_OAL_AllowReinitSound,
    I_OAL_CacheSound,
    I_MBF_AdjustSoundParams,
    I_MBF_UpdateSoundParams,
    NULL,
    I_OAL_SetGain,
    I_OAL_GetOffset,
    I_OAL_StartSound,
    I_OAL_StopSound,
    I_OAL_PauseSound,
    I_OAL_ResumeSound,
    I_OAL_SoundIsPlaying,
    I_OAL_SoundIsPaused,
    I_OAL_ShutdownSound,
    I_OAL_ShutdownModule,
    I_OAL_DeferUpdates,
    I_OAL_ProcessUpdates,
    I_MBF_BindVariables,
};
