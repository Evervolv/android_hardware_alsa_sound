/* AudioStreamOutALSA.cpp
 **
 ** Copyright 2008-2009 Wind River Systems
 ** Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <math.h>

#define LOG_TAG "AudioStreamOutALSA"
//#define LOG_NDEBUG 0
#define LOG_NDDEBUG 0
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>

#include "AudioHardwareALSA.h"

#ifndef ALSA_DEFAULT_SAMPLE_RATE
#define ALSA_DEFAULT_SAMPLE_RATE 44100 // in Hz
#endif

namespace android_audio_legacy
{

// ----------------------------------------------------------------------------

static const int DEFAULT_SAMPLE_RATE = ALSA_DEFAULT_SAMPLE_RATE;

// ----------------------------------------------------------------------------

AudioStreamOutALSA::AudioStreamOutALSA(AudioHardwareALSA *parent, alsa_handle_t *handle) :
    ALSAStreamOps(parent, handle),
    mParent(parent),
    mFrameCount(0)
{
}

AudioStreamOutALSA::~AudioStreamOutALSA()
{
    close();
}

uint32_t AudioStreamOutALSA::channels() const
{
    int c = ALSAStreamOps::channels();
    return c;
}

status_t AudioStreamOutALSA::setVolume(float left, float right)
{
    int lpa_vol;
    float volume;
    status_t status = NO_ERROR;

    if(!strcmp(mHandle->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER) ||
       !strcmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_LPA)) {
        volume = (left + right) / 2;
        if (volume < 0.0) {
            LOGW("AudioSessionOutMSM7xxx::setVolume(%f) under 0.0, assuming 0.0\n", volume);
            volume = 0.0;
        } else if (volume > 1.0) {
            LOGW("AudioSessionOutMSM7xxx::setVolume(%f) over 1.0, assuming 1.0\n", volume);
            volume = 1.0;
        }
        lpa_vol = sqrt(volume) * 100;
        LOGD("setLpaVolume(%f)\n", volume);
        LOGD("Setting LPA volume to %d (available range is 0 to 100)\n", lpa_vol);
        mHandle->module->setLpaVolume(lpa_vol);

        return status;
    }
    else if(!strncmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL,
            sizeof(mHandle->useCase)) || !strncmp(mHandle->useCase,
            SND_USE_CASE_MOD_PLAY_VOIP, sizeof(mHandle->useCase))) {
        LOGV("Avoid Software volume by returning success\n");
        return status;
    }
    return INVALID_OPERATION;
}

ssize_t AudioStreamOutALSA::write(const void *buffer, size_t bytes)
{
    int period_size;
    char *use_case;

    LOGV("write:: buffer %p, bytes %d", buffer, bytes);
    if (!mPowerLock) {
        acquire_wake_lock (PARTIAL_WAKE_LOCK, "AudioOutLock");
        mPowerLock = true;
    }

    snd_pcm_sframes_t n;
    size_t            sent = 0;
    status_t          err;

    int write_pending = bytes;

    if((mHandle->handle == NULL) && (mHandle->rxHandle == NULL) &&
         (strcmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) &&
         (strcmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
        mParent->mLock.lock();
        snd_use_case_get(mHandle->ucMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            if(!strcmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL)){
                 strlcpy(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL,sizeof(mHandle->useCase));
             }
             else {
                 strlcpy(mHandle->useCase, SND_USE_CASE_VERB_HIFI, sizeof(mHandle->useCase));
             }
        } else {
            if(!strcmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP)) {
                strlcpy(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP,sizeof(mHandle->useCase));
             } else {
                 strlcpy(mHandle->useCase, SND_USE_CASE_MOD_PLAY_MUSIC, sizeof(mHandle->useCase));
             }
        }
        free(use_case);
        if((!strcmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) ||
           (!strcmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
              mHandle->module->route(mHandle, mDevices , AudioSystem::MODE_IN_COMMUNICATION);
        } else {
              mHandle->module->route(mHandle, mDevices , mParent->mode());
        }
        if (!strcmp(mHandle->useCase, SND_USE_CASE_VERB_HIFI) ||
            !strcmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) {
            snd_use_case_set(mHandle->ucMgr, "_verb", mHandle->useCase);
        } else {
            snd_use_case_set(mHandle->ucMgr, "_enamod", mHandle->useCase);
        }
        if((!strcmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) ||
          (!strcmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
             err = mHandle->module->startVoipCall(mHandle);
        }
        else
             mHandle->module->open(mHandle);
        if(mHandle->handle == NULL) {
            LOGE("write:: device open failed");
            mParent->mLock.unlock();
            return 0;
        }
        mParent->mLock.unlock();
    }

    period_size = mHandle->periodSize;
    do {
        if (write_pending < period_size) {
            write_pending = period_size;
        }
        if((mParent->mVoipStreamCount) && (mHandle->rxHandle != 0)) {
            n = pcm_write(mHandle->rxHandle,
                     (char *)buffer + sent,
                      period_size);
        } else if (mHandle->handle != 0){
            n = pcm_write(mHandle->handle,
                     (char *)buffer + sent,
                      period_size);
        }
        if (n < 0) {
	    mParent->mLock.lock();
            LOGE("pcm_write returned error %d, trying to recover\n", n);
            pcm_close(mHandle->handle);
            mHandle->handle = NULL;
            if((!strncmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL, strlen(SND_USE_CASE_VERB_IP_VOICECALL))) ||
              (!strncmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP, strlen(SND_USE_CASE_MOD_PLAY_VOIP)))) {
                 pcm_close(mHandle->rxHandle);
                 mHandle->rxHandle = NULL;
                 mHandle->module->startVoipCall(mHandle);
            }
            else
            mHandle->module->open(mHandle);
            mParent->mLock.unlock();
            continue;
        }
        else {
            mFrameCount += n;
            sent += static_cast<ssize_t>((period_size));
            write_pending -= period_size;
        }

    } while ((mHandle->handle||(mHandle->rxHandle && mParent->mVoipStreamCount)) && sent < bytes);

    return sent;
}

status_t AudioStreamOutALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

status_t AudioStreamOutALSA::open(int mode)
{
    Mutex::Autolock autoLock(mParent->mLock);

    return ALSAStreamOps::open(mode);
}

status_t AudioStreamOutALSA::close()
{
    Mutex::Autolock autoLock(mParent->mLock);


    if((!strcmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) ||
        (!strcmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
         if((mParent->mVoipStreamCount)) {
                return NO_ERROR;
         }
         mParent->mVoipStreamCount = 0;
         mParent->mVoipMicMute = 0;
     }

    LOGD("close");
    ALSAStreamOps::close();

    if (mPowerLock) {
        release_wake_lock ("AudioOutLock");
        mPowerLock = false;
    }

    return NO_ERROR;
}

status_t AudioStreamOutALSA::standby()
{
    Mutex::Autolock autoLock(mParent->mLock);

     if((!strcmp(mHandle->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) ||
       (!strcmp(mHandle->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
         return NO_ERROR;
     }

    LOGD("standby");

    mHandle->module->standby(mHandle);

    if (mPowerLock) {
        release_wake_lock ("AudioOutLock");
        mPowerLock = false;
    }

    mFrameCount = 0;

    return NO_ERROR;
}

#define USEC_TO_MSEC(x) ((x + 999) / 1000)

uint32_t AudioStreamOutALSA::latency() const
{
    // Android wants latency in milliseconds.
    return USEC_TO_MSEC (mHandle->latency);
}

// return the number of audio frames written by the audio dsp to DAC since
// the output has exited standby
status_t AudioStreamOutALSA::getRenderPosition(uint32_t *dspFrames)
{
    *dspFrames = mFrameCount;
    return NO_ERROR;
}

}       // namespace android_audio_legacy
