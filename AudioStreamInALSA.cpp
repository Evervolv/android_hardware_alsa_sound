/* AudioStreamInALSA.cpp
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

#define LOG_TAG "AudioStreamInALSA"
#define LOG_NDEBUG 0
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>

#include "AudioHardwareALSA.h"

namespace android
{

AudioStreamInALSA::AudioStreamInALSA(AudioHardwareALSA *parent,
        alsa_handle_t *handle,
        AudioSystem::audio_in_acoustics audio_acoustics) :
    ALSAStreamOps(parent, handle),
    mFramesLost(0),
    mParent(parent),
    mAcoustics(audio_acoustics)
{
}

AudioStreamInALSA::~AudioStreamInALSA()
{
    close();
}

status_t AudioStreamInALSA::setGain(float gain)
{
    return 0; //mixer() ? mixer()->setMasterGain(gain) : (status_t)NO_INIT;
}

ssize_t AudioStreamInALSA::read(void *buffer, ssize_t bytes)
{
    AutoMutex lock(mLock);

    LOGV("read:: buffer %p, bytes %d", buffer, bytes);

    if (!mPowerLock) {
        acquire_wake_lock (PARTIAL_WAKE_LOCK, "AudioInLock");
        mPowerLock = true;
    }

    int n;
    status_t          err;
    size_t            read = 0;
    char *use_case;
    int newMode = mParent->mode();

    if(mHandle->handle == NULL) {
        snd_use_case_get(mHandle->uc_mgr, "_verb", (const char **)&use_case);
        if ((use_case != NULL) && (strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            if ((mHandle->devices == AudioSystem::DEVICE_IN_VOICE_CALL) &&
                (newMode == AudioSystem::MODE_IN_CALL)) {
                strcpy(mHandle->useCase, SND_USE_CASE_MOD_CAPTURE_VOICE);
            } else if((mHandle->devices == AudioSystem::DEVICE_IN_FM_RX) ||
                      (mHandle->devices == AudioSystem::DEVICE_IN_FM_RX_A2DP)) {
                strcpy(mHandle->useCase, SND_USE_CASE_MOD_CAPTURE_FM);
            } else {
                strcpy(mHandle->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC);
            }
        } else {
            if ((mHandle->devices == AudioSystem::DEVICE_IN_VOICE_CALL) &&
                (newMode == AudioSystem::MODE_IN_CALL)) {
                LOGE("Error reading: In call recording without voice call");
                return 0;
            } else if((mHandle->devices == AudioSystem::DEVICE_IN_FM_RX) ||
                      (mHandle->devices == AudioSystem::DEVICE_IN_FM_RX_A2DP)) {
                LOGE("Error reading: FM recording without enabling FM");
                return 0;
            } else {
                strcpy(mHandle->useCase, SND_USE_CASE_VERB_HIFI_REC);
            }
        }
        free(use_case);
        mHandle->module->route(mHandle, mHandle->curDev, mHandle->curMode);
        if (!strcmp(mHandle->useCase, SND_USE_CASE_VERB_HIFI_REC)) {
            snd_use_case_set(mHandle->uc_mgr, "_verb", SND_USE_CASE_VERB_HIFI_REC);
        } else {
            snd_use_case_set(mHandle->uc_mgr, "_enamod", mHandle->useCase);
        }
        mHandle->module->open(mHandle, (mHandle->curDev & AudioSystem::DEVICE_IN_ALL), mHandle->curMode);
        if(mHandle->handle == NULL) {
            LOGE("read:: PCM device open failed");
            return 0;
        }
    }

    int read_pending = bytes;
    do {
        if (read_pending < mHandle->handle->period_size) {
            read_pending = mHandle->handle->period_size;
        }

        n = pcm_read(mHandle->handle, buffer,
		mHandle->handle->period_size);
        LOGV("pcm_read() returned n = %d", n);
        if (n && n != -EAGAIN) {
            //Recovery part of pcm_read. TODO:split recovery.
            return static_cast<ssize_t>(n);
        }
        else if (n < 0) {
            // Recovery is part of pcm_write. TODO split is later.
            return static_cast<ssize_t>(n);
        }
        else {
            read += static_cast<ssize_t>((mHandle->handle->period_size));
            read_pending -= mHandle->handle->period_size;
        }

    } while (mHandle->handle && read < bytes);

    return read;
}

status_t AudioStreamInALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

status_t AudioStreamInALSA::open(int mode)
{
    AutoMutex lock(mLock);

    status_t status = ALSAStreamOps::open(mode);

    return status;
}

status_t AudioStreamInALSA::close()
{
    AutoMutex lock(mLock);
    LOGV("close");
    ALSAStreamOps::close();

    if (mPowerLock) {
        release_wake_lock ("AudioInLock");
        mPowerLock = false;
    }

    return NO_ERROR;
}

status_t AudioStreamInALSA::standby()
{
    AutoMutex lock(mLock);

    LOGV("standby");

    mHandle->module->standby(mHandle);

    if (mPowerLock) {
        release_wake_lock ("AudioInLock");
        mPowerLock = false;
    }

    return NO_ERROR;
}

void AudioStreamInALSA::resetFramesLost()
{
    AutoMutex lock(mLock);
    mFramesLost = 0;
}

unsigned int AudioStreamInALSA::getInputFramesLost() const
{
    unsigned int count = mFramesLost;
    // Stupid interface wants us to have a side effect of clearing the count
    // but is defined as a const to prevent such a thing.
    ((AudioStreamInALSA *)this)->resetFramesLost();
    return count;
}

status_t AudioStreamInALSA::setAcousticParams(void *params)
{
    AutoMutex lock(mLock);

    return (status_t)NO_ERROR;
}

}       // namespace android
