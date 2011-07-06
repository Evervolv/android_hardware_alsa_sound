/* AudioHardwareALSA.cpp
 **
 ** Copyright 2008-2010 Wind River Systems
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

#define LOG_TAG "AudioHardwareALSA"
#define LOG_NDEBUG 0
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>

#include "AudioHardwareALSA.h"

extern "C"
{
    //
    // Function for dlsym() to look up for creating a new AudioHardwareInterface.
    //
    android::AudioHardwareInterface *createAudioHardware(void) {
        return android::AudioHardwareALSA::create();
    }
}         // extern "C"

namespace android
{

// ----------------------------------------------------------------------------

AudioHardwareInterface *AudioHardwareALSA::create() {
    return new AudioHardwareALSA();
}

AudioHardwareALSA::AudioHardwareALSA() :
    mALSADevice(0)
{
    hw_module_t *module;
    int err = hw_get_module(ALSA_HARDWARE_MODULE_ID,
            (hw_module_t const**)&module);
    LOGV("hw_get_module(ALSA_HARDWARE_MODULE_ID) returned err %d", err);
    if (err == 0) {
        hw_device_t* device;
        err = module->methods->open(module, ALSA_HARDWARE_NAME, &device);
        if (err == 0) {
            mALSADevice = (alsa_device_t *)device;
            mALSADevice->init(mALSADevice, mDeviceList);
        } else {
            LOGE("ALSA Module could not be opened!!!");
        }
    } else {
        LOGE("ALSA Module not found!!!");
    }
}

AudioHardwareALSA::~AudioHardwareALSA()
{
    if (mALSADevice) {
        mALSADevice->common.close(&mALSADevice->common);
    }
}

status_t AudioHardwareALSA::initCheck()
{
    if (!mALSADevice)
        return NO_INIT;

    return NO_ERROR;
}

status_t AudioHardwareALSA::setVoiceVolume(float volume)
{
    // The voice volume is used by the VOICE_CALL audio stream.
    return INVALID_OPERATION;
}

status_t AudioHardwareALSA::setMasterVolume(float volume)
{
    return INVALID_OPERATION;
}

status_t AudioHardwareALSA::setMode(int mode)
{
    status_t status = NO_ERROR;

    if (mode != mMode) {
        status = AudioHardwareBase::setMode(mode);
    }

    return status;
}

status_t AudioHardwareALSA::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key = String8(AudioParameter::keyRouting);
    status_t status = NO_ERROR;
    int device;
    LOGV("setParameters() %s", keyValuePairs.string());

    if (param.getInt(key, device) == NO_ERROR) {
        doRouting(device);
        param.remove(key);
    }

    if (param.size()) {
        status = BAD_VALUE;
    }
    return status;
}

void AudioHardwareALSA::doRouting(int device)
{
    int newMode = mode();

    for(ALSAHandleList::iterator it = mDeviceList.begin();
        it != mDeviceList.end(); ++it) {
        if(newMode == AudioSystem::MODE_IN_CALL && it->useCase == ALSA_VOICE_CALL &&
           it->curMode != newMode) {
            // Start voice call
            mALSADevice->startVoiceCall(&(*it), device, newMode);
        } else if(newMode == AudioSystem::MODE_NORMAL && it->useCase == ALSA_VOICE_CALL &&
           it->curMode == AudioSystem::MODE_IN_CALL) {
            // End voice call
            mALSADevice->close(&(*it));
        } else if(device & AudioSystem::DEVICE_OUT_FM && it->useCase == ALSA_FM_RADIO &&
                  !it->handle) {
            // Start FM Radio on current active device
            mALSADevice->startFm(&(*it), device, newMode);
        } else if(!(device & AudioSystem::DEVICE_OUT_FM) && it->useCase == ALSA_FM_RADIO &&
                  it->handle) {
            // Stop FM Radio
            mALSADevice->close(&(*it));
        } else {
            // Route the stream to new sound device
            mALSADevice->route(&(*it), (uint32_t)device, newMode);
        }
    }
}

AudioStreamOut *
AudioHardwareALSA::openOutputStream(uint32_t devices,
                                    int *format,
                                    uint32_t *channels,
                                    uint32_t *sampleRate,
                                    status_t *status)
{
    LOGV("openOutputStream: devices 0x%x channels %d sampleRate %d",
         devices, *channels, *sampleRate);

    status_t err = BAD_VALUE;
    AudioStreamOutALSA *out = 0;

    if (devices & (devices - 1)) {
        if (status) *status = err;
        LOGE("openOutputStream called with bad devices");
        return out;
    }

    // Find the appropriate alsa device
    for(ALSAHandleList::iterator it = mDeviceList.begin();
        it != mDeviceList.end(); ++it){
        LOGV("useCase %d", it->useCase);

        if (it->useCase == ALSA_PLAYBACK) {
            err = mALSADevice->open(&(*it), devices, mode());
            if (err) break;
            out = new AudioStreamOutALSA(this, &(*it));
            err = out->set(format, channels, sampleRate, devices);
            break;
        }
    }

    if (status) *status = err;
    return out;
}

void
AudioHardwareALSA::closeOutputStream(AudioStreamOut* out)
{
    delete out;
}

AudioStreamIn *
AudioHardwareALSA::openInputStream(uint32_t devices,
                                   int *format,
                                   uint32_t *channels,
                                   uint32_t *sampleRate,
                                   status_t *status,
                                   AudioSystem::audio_in_acoustics acoustics)
{
    status_t err = BAD_VALUE;
    AudioStreamInALSA *in = 0;

    LOGV("openInputStream: devices 0x%x channels %d sampleRate %d", devices, *channels, *sampleRate);
    if (devices & (devices - 1)) {
        if (status) *status = err;
        return in;
    }

    // Find the appropriate alsa device
    for(ALSAHandleList::iterator it = mDeviceList.begin();
        it != mDeviceList.end(); ++it)
        if (it->useCase == ALSA_RECORD) {
            if(sampleRate) {
                it->sampleRate = *sampleRate;
            }
            if(channels) {
                it->channels = AudioSystem::popCount(*channels);
            }
            err = mALSADevice->open(&(*it), devices, mode());
            if (err) break;
            in = new AudioStreamInALSA(this, &(*it), acoustics);
            err = in->set(format, channels, sampleRate, devices);
            break;
        }

    if (status) *status = err;
    return in;
}

void
AudioHardwareALSA::closeInputStream(AudioStreamIn* in)
{
    delete in;
}

status_t AudioHardwareALSA::setMicMute(bool state)
{
    return NO_INIT;
}

status_t AudioHardwareALSA::getMicMute(bool *state)
{
    return NO_ERROR;
}

status_t AudioHardwareALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

size_t AudioHardwareALSA::getInputBufferSize(uint32_t sampleRate, int format, int channelCount)
{
    if (format != AudioSystem::PCM_16_BIT) {
        LOGW("getInputBufferSize bad format: %d", format);
        return 0;
    }

    return 4096;
}

}       // namespace android
