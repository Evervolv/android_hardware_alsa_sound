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
            voice_call_inprogress = 0;
            fm_radio_inprogress = 0;
            snd_use_case_mgr_open(&uc_mgr, "snd_soc_msm");
            if (uc_mgr < 0) {
	        LOGE("Failed to open ucm instance: %d", errno);
            } else {
	        LOGI("ucm instance opened: %u", (unsigned)uc_mgr);
            }
        } else {
            LOGE("ALSA Module could not be opened!!!");
        }
    } else {
        LOGE("ALSA Module not found!!!");
    }
}

AudioHardwareALSA::~AudioHardwareALSA()
{
    if (uc_mgr != NULL) {
        LOGV("closing ucm instance: %u", (unsigned)uc_mgr);
        snd_use_case_mgr_close(uc_mgr);
    }
    if (mALSADevice) {
        mALSADevice->common.close(&mALSADevice->common);
    }
    for(ALSAHandleList::iterator it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
        it->useCase[0] = 0;
        mDeviceList.erase(it);
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

    LOGV("doRouting: device %d newMode %d voice_call_inprogress %d fm_radio_inprogress %d",
          device, newMode, voice_call_inprogress, fm_radio_inprogress);
    if((newMode == AudioSystem::MODE_IN_CALL) && (voice_call_inprogress == 0)) {
        // Start voice call
        unsigned long bufferSize = DEFAULT_BUFFER_SIZE;
        alsa_handle_t alsa_handle;
        char *use_case;
        snd_use_case_get(uc_mgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            strcpy(alsa_handle.useCase, SND_USE_CASE_VERB_VOICECALL);
        } else {
            strcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_VOICE);
        }
        free(use_case);

        for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;
        alsa_handle.module = mALSADevice;
        alsa_handle.bufferSize = bufferSize;
        alsa_handle.devices = device;
        alsa_handle.curDev = 0;
        alsa_handle.curMode = newMode;
        alsa_handle.handle = 0;
        alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
        alsa_handle.channels = VOICE_CHANNEL_MODE;
        alsa_handle.sampleRate = VOICE_SAMPLING_RATE;
        alsa_handle.latency = VOICE_LATENCY;
        alsa_handle.recHandle = 0;
        alsa_handle.uc_mgr = uc_mgr;
        voice_call_inprogress = 1;
        mDeviceList.push_back(alsa_handle);
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;
        LOGV("Enabling voice call");
        mALSADevice->route(&(*it), (uint32_t)device, newMode);
        for(ALSAHandleList::iterator handle = mDeviceList.begin();
            handle != mDeviceList.end(); ++handle) {
            handle->curDev = it->curDev;
        }
        if (!strcmp(it->useCase, SND_USE_CASE_VERB_VOICECALL)) {
            snd_use_case_set(uc_mgr, "_verb", SND_USE_CASE_VERB_VOICECALL);
        } else {
            snd_use_case_set(uc_mgr, "_enamod", SND_USE_CASE_MOD_PLAY_VOICE);
        }
        mALSADevice->startVoiceCall(&(*it), it->curDev, newMode);
    } else if(newMode == AudioSystem::MODE_NORMAL && voice_call_inprogress == 1) {
        // End voice call
        for(ALSAHandleList::iterator it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
            if((!strcmp(it->useCase, SND_USE_CASE_VERB_VOICECALL)) ||
               (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_VOICE))) {
                LOGV("Disabling voice call");
                mALSADevice->close(&(*it));
                mDeviceList.erase(it);
                mALSADevice->route(&(*it), (uint32_t)device, newMode);
                for(ALSAHandleList::iterator handle = mDeviceList.begin();
                   handle != mDeviceList.end(); ++handle) {
                   handle->curDev = it->curDev;
                }
                break;
            }
        }
        voice_call_inprogress = 0;
    } else if(device & AudioSystem::DEVICE_OUT_FM && fm_radio_inprogress == 0) {
        // Start FM Radio on current active device
        unsigned long bufferSize = FM_BUFFER_SIZE;
        alsa_handle_t alsa_handle;
        char *use_case;
        LOGV("Start FM");
        snd_use_case_get(uc_mgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            strcpy(alsa_handle.useCase, SND_USE_CASE_VERB_DIGITAL_RADIO);
        } else {
            strcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_FM);
        }
        free(use_case);

        for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;
        alsa_handle.module = mALSADevice;
        alsa_handle.bufferSize = bufferSize;
        alsa_handle.devices = device;
        alsa_handle.curDev = 0;
        alsa_handle.curMode = newMode;
        alsa_handle.handle = 0;
        alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
        alsa_handle.channels = DEFAULT_CHANNEL_MODE;
        alsa_handle.sampleRate = DEFAULT_SAMPLING_RATE;
        alsa_handle.latency = VOICE_LATENCY;
        alsa_handle.recHandle = 0;
        alsa_handle.uc_mgr = uc_mgr;
        fm_radio_inprogress = 1;
        mDeviceList.push_back(alsa_handle);
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;
        mALSADevice->route(&(*it), (uint32_t)device, newMode);
        for(ALSAHandleList::iterator handle = mDeviceList.begin();
            handle != mDeviceList.end(); ++handle) {
            handle->curDev = it->curDev;
        }
        if(!strcmp(it->useCase, SND_USE_CASE_VERB_DIGITAL_RADIO)) {
            snd_use_case_set(uc_mgr, "_verb", SND_USE_CASE_VERB_DIGITAL_RADIO);
        } else {
            snd_use_case_set(uc_mgr, "_enamod", SND_USE_CASE_MOD_PLAY_FM);
        }
        mALSADevice->startFm(&(*it), it->curDev, newMode);
    } else if(!(device & AudioSystem::DEVICE_OUT_FM) && fm_radio_inprogress == 1) {
        // Stop FM Radio
        LOGV("Stop FM");
        for(ALSAHandleList::iterator it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
            if((!strcmp(it->useCase, SND_USE_CASE_VERB_DIGITAL_RADIO)) ||
              (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_FM))) {
                mALSADevice->close(&(*it));
                mDeviceList.erase(it);
                mALSADevice->route(&(*it), (uint32_t)device, newMode);
                for(ALSAHandleList::iterator handle = mDeviceList.begin();
                   handle != mDeviceList.end(); ++handle) {
                   handle->curDev = it->curDev;
                }
                break;
            }
        }
        fm_radio_inprogress = 0;
    } else {
        for(ALSAHandleList::iterator it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
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

    alsa_handle_t alsa_handle;
    unsigned long bufferSize = DEFAULT_BUFFER_SIZE;

    for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;

    alsa_handle.module = mALSADevice;
    alsa_handle.bufferSize = bufferSize;
    alsa_handle.devices = devices;
    alsa_handle.curDev = 0;
    alsa_handle.curMode = mode();
    alsa_handle.handle = 0;
    alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
    alsa_handle.channels = DEFAULT_CHANNEL_MODE;
    alsa_handle.sampleRate = DEFAULT_SAMPLING_RATE;
    alsa_handle.latency = PLAYBACK_LATENCY;
    alsa_handle.recHandle = 0;
    alsa_handle.uc_mgr = uc_mgr;

    char *use_case;
    snd_use_case_get(uc_mgr, "_verb", (const char **)&use_case);
    if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
        strcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI);
    } else {
        strcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_MUSIC);
    }
    free(use_case);
    mDeviceList.push_back(alsa_handle);
    ALSAHandleList::iterator it = mDeviceList.end();
    it--;
    LOGV("useCase %s", it->useCase);
    mALSADevice->route(&(*it), devices, mode());
    if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI)) {
        snd_use_case_set(uc_mgr, "_verb", SND_USE_CASE_VERB_HIFI);
    } else {
        snd_use_case_set(uc_mgr, "_enamod", SND_USE_CASE_MOD_PLAY_MUSIC);
    }
    err = mALSADevice->open(&(*it), it->curDev, mode());
    if (err) {
        LOGE("Device open failed");
    } else {
        out = new AudioStreamOutALSA(this, &(*it));
        err = out->set(format, channels, sampleRate);
    }

    if (status) *status = err;
    return out;
}

void
AudioHardwareALSA::closeOutputStream(AudioStreamOut* out)
{
    delete out;
}

AudioStreamOut *
AudioHardwareALSA::openOutputSession(uint32_t devices,
                                     int *format,
                                     status_t *status,
                                     int sessionId)
{
    LOGE("openOutputSession");
    AudioStreamOutALSA *out = 0;
    status_t err = BAD_VALUE;

    if (devices & (devices - 1)) {
        if (status) *status = err;
        LOGE("openOutputSession called with bad devices");
        return out;
    }

    alsa_handle_t alsa_handle;
    unsigned long bufferSize = DEFAULT_BUFFER_SIZE;

    for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;

    alsa_handle.module = mALSADevice;
    alsa_handle.bufferSize = bufferSize;
    alsa_handle.devices = devices;
    alsa_handle.curDev = 0;
    alsa_handle.curMode = mode();
    alsa_handle.handle = 0;
    alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
    alsa_handle.channels = DEFAULT_CHANNEL_MODE;
    alsa_handle.sampleRate = DEFAULT_SAMPLING_RATE;
    alsa_handle.latency = VOICE_LATENCY;
    alsa_handle.recHandle = 0;
    alsa_handle.uc_mgr = uc_mgr;

    char *use_case;
    snd_use_case_get(uc_mgr, "_verb", (const char **)&use_case);
    if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
        strcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER);
    } else {
        strcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_LPA);
    }
    free(use_case);
    mDeviceList.push_back(alsa_handle);
    ALSAHandleList::iterator it = mDeviceList.end();
    it--;
    LOGV("useCase %s", it->useCase);
    mALSADevice->route(&(*it), devices, mode());
    if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI)) {
        snd_use_case_set(uc_mgr, "_verb", SND_USE_CASE_VERB_HIFI_LOW_POWER);
    } else {
        snd_use_case_set(uc_mgr, "_enamod", SND_USE_CASE_MOD_PLAY_LPA);
    }
    err = mALSADevice->open(&(*it), devices, mode());
    out = new AudioStreamOutALSA(this, &(*it));

    if (status) *status = err;
       return out;
}

void
AudioHardwareALSA::closeOutputSession(AudioStreamOut* out)
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
    char *use_case;
    int newMode = mode();

    status_t err = BAD_VALUE;
    AudioStreamInALSA *in = 0;

    LOGV("openInputStream: devices 0x%x channels %d sampleRate %d", devices, *channels, *sampleRate);
    if (devices & (devices - 1)) {
        if (status) *status = err;
        return in;
    }

    alsa_handle_t alsa_handle;
    unsigned long bufferSize = DEFAULT_BUFFER_SIZE;

    for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;

    alsa_handle.module = mALSADevice;
    alsa_handle.bufferSize = bufferSize;
    alsa_handle.devices = devices;
    alsa_handle.curDev = 0;
    alsa_handle.curMode = mode();
    alsa_handle.handle = 0;
    alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
    alsa_handle.channels = VOICE_CHANNEL_MODE;
    alsa_handle.sampleRate = AudioRecord::DEFAULT_SAMPLE_RATE;
    alsa_handle.latency = RECORD_LATENCY;
    alsa_handle.recHandle = 0;
    alsa_handle.uc_mgr = uc_mgr;

    snd_use_case_get(uc_mgr, "_verb", (const char **)&use_case);
    if ((use_case != NULL) && (strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
        if ((devices == AudioSystem::DEVICE_IN_VOICE_CALL) &&
            (newMode == AudioSystem::MODE_IN_CALL)) {
                strcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_VOICE);
        } else if((devices == AudioSystem::DEVICE_IN_FM_RX) ||
                  (devices == AudioSystem::DEVICE_IN_FM_RX_A2DP)) {
            strcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_FM);
        } else {
            strcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC);
        }
    } else {
        if ((devices == AudioSystem::DEVICE_IN_VOICE_CALL) &&
            (newMode == AudioSystem::MODE_IN_CALL)) {
            LOGE("Error opening input stream: In-call recording without voice call");
            return 0;
        } else if((devices == AudioSystem::DEVICE_IN_FM_RX) ||
                  (devices == AudioSystem::DEVICE_IN_FM_RX_A2DP)) {
            LOGE("Error opening input stream: FM recording without enabling FM");
            return 0;
        } else {
            strcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_REC);
        }
    }
    free(use_case);
    mDeviceList.push_back(alsa_handle);
    ALSAHandleList::iterator it = mDeviceList.end();
    it--;
    mALSADevice->route(&(*it), devices, mode());
    if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_REC)) {
        snd_use_case_set(uc_mgr, "_verb", SND_USE_CASE_VERB_HIFI_REC);
    } else {
        snd_use_case_set(uc_mgr, "_enamod", it->useCase);
    }
    if(sampleRate) {
        it->sampleRate = *sampleRate;
    }
    if(channels) {
        it->channels = AudioSystem::popCount(*channels);
    }
    err = mALSADevice->open(&(*it), (it->curDev & AudioSystem::DEVICE_IN_ALL), mode());
    if (err) {
        LOGE("Error opening pcm input device");
    } else {
        in = new AudioStreamInALSA(this, &(*it), acoustics);
        err = in->set(format, channels, sampleRate);
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
