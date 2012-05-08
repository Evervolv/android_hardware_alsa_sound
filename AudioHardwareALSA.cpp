/* AudioHardwareALSA.cpp
 **
 ** Copyright 2008-2010 Wind River Systems
 ** Copyright (c) 2011-2012, Code Aurora Forum. All rights reserved.
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

#define LOG_TAG "AudioHardwareALSA"
//#define LOG_NDEBUG 0
#define LOG_NDDEBUG 0
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
    android_audio_legacy::AudioHardwareInterface *createAudioHardware(void) {
        return android_audio_legacy::AudioHardwareALSA::create();
    }
}         // extern "C"

namespace android_audio_legacy
{

// ----------------------------------------------------------------------------

AudioHardwareInterface *AudioHardwareALSA::create() {
    return new AudioHardwareALSA();
}

AudioHardwareALSA::AudioHardwareALSA() :
    mALSADevice(0),mVoipStreamCount(0),mVoipMicMute(false)
{
    FILE *fp;
    char soundCardInfo[200];
    hw_module_t *module;
    int err = hw_get_module(ALSA_HARDWARE_MODULE_ID,
            (hw_module_t const**)&module);
    int codec_rev = 2;
    LOGD("hw_get_module(ALSA_HARDWARE_MODULE_ID) returned err %d", err);
    if (err == 0) {
        hw_device_t* device;
        err = module->methods->open(module, ALSA_HARDWARE_NAME, &device);
        if (err == 0) {
            mALSADevice = (alsa_device_t *)device;
            mALSADevice->init(mALSADevice, mDeviceList);
            mIsVoiceCallActive = 0;
            mIsFmActive = 0;
            mDevSettingsFlag = 0;
            mDevSettingsFlag |= TTY_OFF;
            mBluetoothVGS = false;


            if((fp = fopen("/proc/asound/cards","r")) == NULL) {
                LOGE("Cannot open /proc/asound/cards file to get sound card info");
            } else {
                while((fgets(soundCardInfo, sizeof(soundCardInfo), fp) != NULL)) {
                    LOGV("SoundCardInfo %s", soundCardInfo);
                    if (strstr(soundCardInfo, "msm8960-tabla1x-snd-card")) {
                        codec_rev = 1;
                        break;
                    }
                }
                fclose(fp);
            }

            if (codec_rev == 1) {
                    LOGV("Detected tabla 1.x sound card");
                    snd_use_case_mgr_open(&mUcMgr, "snd_soc_msm");
            } else {
                    LOGV("Detected tabla 2.x sound card");
                    snd_use_case_mgr_open(&mUcMgr, "snd_soc_msm_2x");
            }

            if (mUcMgr < 0) {
                LOGE("Failed to open ucm instance: %d", errno);
            } else {
                LOGI("ucm instance opened: %u", (unsigned)mUcMgr);
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
    if (mUcMgr != NULL) {
        LOGD("closing ucm instance: %u", (unsigned)mUcMgr);
        snd_use_case_mgr_close(mUcMgr);
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

status_t AudioHardwareALSA::setVoiceVolume(float v)
{
    LOGD("setVoiceVolume(%f)\n", v);
    if (v < 0.0) {
        LOGW("setVoiceVolume(%f) under 0.0, assuming 0.0\n", v);
        v = 0.0;
    } else if (v > 1.0) {
        LOGW("setVoiceVolume(%f) over 1.0, assuming 1.0\n", v);
        v = 1.0;
    }

    int newMode = mode();
    LOGD("setVoiceVolume  newMode %d",newMode);
    int vol = lrint(v * 100.0);

    // Voice volume levels from android are mapped to driver volume levels as follows.
    // 0 -> 5, 20 -> 4, 40 ->3, 60 -> 2, 80 -> 1, 100 -> 0
    // So adjust the volume to get the correct volume index in driver
    vol = 100 - vol;

    // ToDo: Send mixer command only when voice call is active
    if(mALSADevice) {
        if(newMode == AudioSystem::MODE_IN_COMMUNICATION) {
            mALSADevice->setVoipVolume(vol);
        } else {
            mALSADevice->setVoiceVolume(vol);
        }
    }

    return NO_ERROR;
}

status_t  AudioHardwareALSA::setFmVolume(float value)
{

    status_t status = NO_ERROR;

    int vol;

    if (value < 0.0) {
        LOGW("setFmVolume(%f) under 0.0, assuming 0.0\n", value);
        value = 0.0;
    } else if (value > 1.0) {
        LOGW("setFmVolume(%f) over 1.0, assuming 1.0\n", value);
        value = 1.0;
    }
    vol  = lrint((value * 0x2000) + 0.5);

    LOGD("setFmVolume(%f)\n", value);
    LOGD("Setting FM volume to %d (available range is 0 to 0x2000)\n", vol);

    mALSADevice->setFmVolume(vol);

    return status;
}

status_t AudioHardwareALSA::setMasterVolume(float volume)
{
    return NO_ERROR;
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
    String8 key;
    String8 value;
    status_t status = NO_ERROR;
    int device;
    int btRate;
    LOGD("setParameters() %s", keyValuePairs.string());

    key = String8(TTY_MODE_KEY);
    if (param.get(key, value) == NO_ERROR) {
        mDevSettingsFlag &= TTY_CLEAR;
        if (value == "full") {
            mDevSettingsFlag |= TTY_FULL;
        } else if (value == "hco") {
            mDevSettingsFlag |= TTY_HCO;
        } else if (value == "vco") {
            mDevSettingsFlag |= TTY_VCO;
        } else {
            mDevSettingsFlag |= TTY_OFF;
        }
        LOGI("Changed TTY Mode=%s", value.string());
        mALSADevice->setFlags(mDevSettingsFlag);
        if(mMode != AudioSystem::MODE_IN_CALL){
           return NO_ERROR;
        }
        doRouting(0);
    }

    key = String8(FLUENCE_KEY);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "quadmic") {
            mDevSettingsFlag |= QMIC_FLAG;
            mDevSettingsFlag &= (~DMIC_FLAG);
            LOGV("Fluence quadMic feature Enabled");
        } else if (value == "dualmic") {
            mDevSettingsFlag |= DMIC_FLAG;
            mDevSettingsFlag &= (~QMIC_FLAG);
            LOGV("Fluence dualmic feature Enabled");
        } else if (value == "none") {
            mDevSettingsFlag &= (~DMIC_FLAG);
            mDevSettingsFlag &= (~QMIC_FLAG);
            LOGV("Fluence feature Disabled");
        }
        mALSADevice->setFlags(mDevSettingsFlag);
        doRouting(0);
    }

    key = String8(ANC_KEY);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "true") {
            LOGV("Enabling ANC setting in the setparameter\n");
            mDevSettingsFlag |= ANC_FLAG;
        } else {
            LOGV("Disabling ANC setting in the setparameter\n");
            mDevSettingsFlag &= (~ANC_FLAG);
        }
        mALSADevice->setFlags(mDevSettingsFlag);
        doRouting(0);
    }

    key = String8(AudioParameter::keyRouting);
    if (param.getInt(key, device) == NO_ERROR) {
        // Ignore routing if device is 0.
        if(device) {
            doRouting(device);
        }
        param.remove(key);
    }

    key = String8(BT_SAMPLERATE_KEY);
    if (param.getInt(key, btRate) == NO_ERROR) {
        mALSADevice->setBtscoRate(btRate);
        param.remove(key);
    }

    key = String8(BTHEADSET_VGS);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "on") {
            mBluetoothVGS = true;
        } else {
            mBluetoothVGS = false;
        }
    }

    key = String8(WIDEVOICE_KEY);
    if (param.get(key, value) == NO_ERROR) {
        bool flag = false;
        if (value == "true") {
            flag = true;
        }
        if(mALSADevice) {
            mALSADevice->enableWideVoice(flag);
        }
        param.remove(key);
    }

    key = String8(FENS_KEY);
    if (param.get(key, value) == NO_ERROR) {
        bool flag = false;
        if (value == "true") {
            flag = true;
        }
        if(mALSADevice) {
            mALSADevice->enableFENS(flag);
        }
        param.remove(key);
    }
#ifdef FM_RADIO
    key = String8(AudioParameter::keyHandleFm);
    if (param.getInt(key, device) == NO_ERROR) {
        // Ignore if device is 0
        if(device) {
            handleFm(device);
        }
        param.remove(key);
    }
#endif
    if (param.size()) {
        status = BAD_VALUE;
    }
    return status;
}

String8 AudioHardwareALSA::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    String8 value;

    String8 key = String8(DUALMIC_KEY);
    if (param.get(key, value) == NO_ERROR) {
        value = String8("false");
        param.add(key, value);
    }

    key = String8(FLUENCE_KEY);
    if (param.get(key, value) == NO_ERROR) {
	if ((mDevSettingsFlag & QMIC_FLAG) &&
                               (mDevSettingsFlag & ~DMIC_FLAG))
            value = String8("quadmic");
	else if ((mDevSettingsFlag & DMIC_FLAG) &&
                                (mDevSettingsFlag & ~QMIC_FLAG))
            value = String8("dualmic");
	else if ((mDevSettingsFlag & ~DMIC_FLAG) &&
                                (mDevSettingsFlag & ~QMIC_FLAG))
            value = String8("none");
        param.add(key, value);
    }

    key = String8("Fm-radio");
    if ( param.get(key,value) == NO_ERROR ) {
        if ( mIsFmActive ) {
            param.addInt(String8("isFMON"), true );
        }
    }

    key = String8(BTHEADSET_VGS);
    if (param.get(key, value) == NO_ERROR) {
        if(mBluetoothVGS)
           param.addInt(String8("isVGS"), true);
    }

    LOGV("AudioHardwareALSA::getParameters() %s", param.toString().string());
    return param.toString();
}

void AudioHardwareALSA::doRouting(int device)
{
    Mutex::Autolock autoLock(mLock);
    int newMode = mode();
    if ((device == AudioSystem::DEVICE_IN_VOICE_CALL) ||
        (device == AudioSystem::DEVICE_IN_FM_RX) ||
        (device == AudioSystem::DEVICE_OUT_DIRECTOUTPUT) ||
        (device == AudioSystem::DEVICE_IN_COMMUNICATION) ||
        (device == AudioSystem::DEVICE_IN_FM_RX_A2DP)) {
        LOGV("Ignoring routing for FM/INCALL/VOIP recording");
        return;
    }
    if (device == 0)
        device = mCurDevice;
    LOGV("doRouting: device %d newMode %d mIsVoiceCallActive %d mIsFmActive %d",
          device, newMode, mIsVoiceCallActive, mIsFmActive);
    if((newMode == AudioSystem::MODE_IN_CALL) && (mIsVoiceCallActive == 0)) {
        // Start voice call
        unsigned long bufferSize = DEFAULT_BUFFER_SIZE;
        alsa_handle_t alsa_handle;
        char *use_case;
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_VOICECALL, sizeof(alsa_handle.useCase));
        } else {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_VOICE, sizeof(alsa_handle.useCase));
        }
        free(use_case);

        for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;
        alsa_handle.module = mALSADevice;
        alsa_handle.bufferSize = bufferSize;
        alsa_handle.devices = device;
        alsa_handle.handle = 0;
        alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
        alsa_handle.channels = VOICE_CHANNEL_MODE;
        alsa_handle.sampleRate = VOICE_SAMPLING_RATE;
        alsa_handle.latency = VOICE_LATENCY;
        alsa_handle.rxHandle = 0;
        alsa_handle.ucMgr = mUcMgr;
        mIsVoiceCallActive = 1;
        mDeviceList.push_back(alsa_handle);
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;
        LOGV("Enabling voice call");
        mALSADevice->route(&(*it), (uint32_t)device, newMode);
        if (!strcmp(it->useCase, SND_USE_CASE_VERB_VOICECALL)) {
            snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_VOICECALL);
        } else {
            snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_VOICE);
        }
        mALSADevice->startVoiceCall(&(*it));
    } else if(newMode == AudioSystem::MODE_NORMAL && mIsVoiceCallActive == 1) {
        // End voice call
        for(ALSAHandleList::iterator it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
            if((!strcmp(it->useCase, SND_USE_CASE_VERB_VOICECALL)) ||
               (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_VOICE))) {
                LOGV("Disabling voice call");
                mALSADevice->close(&(*it));
                mALSADevice->route(&(*it), (uint32_t)device, newMode);
                mDeviceList.erase(it);
                break;
            }
        }
        mIsVoiceCallActive = 0;
    } else if((((mCurDevice & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
              (mCurDevice & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE)) &&
              (mCurDevice & AudioSystem::DEVICE_OUT_SPEAKER) &&
              ((device & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
              (device & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE))) ||
              (((device & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
              (device & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE))  &&
              (device & AudioSystem::DEVICE_OUT_SPEAKER) &&
              ((mCurDevice & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
              (mCurDevice & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE)))) {
              for(ALSAHandleList::iterator it = mDeviceList.begin();
                  it != mDeviceList.end(); ++it) {
                  if((!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI,
                       strlen(SND_USE_CASE_VERB_HIFI))) ||
                     (!strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_MUSIC,
                       strlen(SND_USE_CASE_MOD_PLAY_MUSIC)))) {
                     mALSADevice->route(&(*it), (uint32_t)device, newMode);
                     break;
                  }
             }
     } else {
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;
        mALSADevice->route(&(*it), (uint32_t)device, newMode);
    }
    mCurDevice = device;
}

AudioStreamOut *
AudioHardwareALSA::openOutputStream(uint32_t devices,
                                    int *format,
                                    uint32_t *channels,
                                    uint32_t *sampleRate,
                                    status_t *status)
{
    Mutex::Autolock autoLock(mLock);
    LOGD("openOutputStream: devices 0x%x channels %d sampleRate %d",
         devices, *channels, *sampleRate);

    status_t err = BAD_VALUE;
    AudioStreamOutALSA *out = 0;
    ALSAHandleList::iterator it;

    if (devices & (devices - 1)) {
        if (status) *status = err;
        LOGE("openOutputStream called with bad devices");
        return out;
    }

    if(devices == AudioSystem::DEVICE_OUT_DIRECTOUTPUT) {
        bool voipstream_active = false;
        for(it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
                if((!strcmp(it->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) ||
                   (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
                    LOGD("openOutput:  it->rxHandle %d it->handle %d",it->rxHandle,it->handle);
                    voipstream_active = true;
                    break;
                }
        }

      if(voipstream_active == false) {
         mVoipStreamCount = 0;
         mVoipMicMute = false;
         alsa_handle_t alsa_handle;
         unsigned long bufferSize;
         if(*sampleRate == VOIP_SAMPLING_RATE_8K) {
             bufferSize = VOIP_BUFFER_SIZE_8K;
         }
         else if(*sampleRate == VOIP_SAMPLING_RATE_16K) {
             bufferSize = VOIP_BUFFER_SIZE_16K;
         }
         else {
             LOGE("unsupported samplerate %d for voip",*sampleRate);
             if (status) *status = err;
                 return out;
          }
          alsa_handle.module = mALSADevice;
          alsa_handle.bufferSize = bufferSize;
          alsa_handle.devices = devices;
          alsa_handle.handle = 0;
          alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
          alsa_handle.channels = VOIP_DEFAULT_CHANNEL_MODE;
          alsa_handle.sampleRate = *sampleRate;
          alsa_handle.latency = VOIP_PLAYBACK_LATENCY;
          alsa_handle.rxHandle = 0;
          alsa_handle.ucMgr = mUcMgr;
          char *use_case;
          snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
          if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
              strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_IP_VOICECALL, sizeof(alsa_handle.useCase));
          } else {
              strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_VOIP, sizeof(alsa_handle.useCase));
          }
          free(use_case);
          mDeviceList.push_back(alsa_handle);
          it = mDeviceList.end();
          it--;
          LOGV("openoutput: mALSADevice->route useCase %s mCurDevice %d mVoipStreamCount %d mode %d", it->useCase,mCurDevice,mVoipStreamCount, mode());
          mALSADevice->route(&(*it), mCurDevice, AudioSystem::MODE_IN_COMMUNICATION);
          if(!strcmp(it->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) {
              snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_IP_VOICECALL);
          } else {
              snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_VOIP);
          }
          err = mALSADevice->startVoipCall(&(*it));
          if (err) {
              LOGE("Device open failed");
              return NULL;
          }
      }
      out = new AudioStreamOutALSA(this, &(*it));
      err = out->set(format, channels, sampleRate, devices);
      if(err == NO_ERROR) {
          mVoipStreamCount++;   //increment VoipstreamCount only if success
          LOGD("openoutput mVoipStreamCount %d",mVoipStreamCount);
      }
      if (status) *status = err;
      return out;
    } else
    {

      alsa_handle_t alsa_handle;
      unsigned long bufferSize = DEFAULT_BUFFER_SIZE;

      for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
          bufferSize &= ~b;

      alsa_handle.module = mALSADevice;
      alsa_handle.bufferSize = bufferSize;
      alsa_handle.devices = devices;
      alsa_handle.handle = 0;
      alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
      alsa_handle.channels = DEFAULT_CHANNEL_MODE;
      alsa_handle.sampleRate = DEFAULT_SAMPLING_RATE;
      alsa_handle.latency = PLAYBACK_LATENCY;
      alsa_handle.rxHandle = 0;
      alsa_handle.ucMgr = mUcMgr;

      char *use_case;
      snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
      if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
          strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI, sizeof(alsa_handle.useCase));
      } else {
          strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_MUSIC, sizeof(alsa_handle.useCase));
      }
      free(use_case);
      mDeviceList.push_back(alsa_handle);
      ALSAHandleList::iterator it = mDeviceList.end();
      it--;
      LOGD("useCase %s", it->useCase);
      mALSADevice->route(&(*it), devices, mode());
      if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI)) {
          snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_HIFI);
      } else {
          snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_MUSIC);
      }
      err = mALSADevice->open(&(*it));
      if (err) {
          LOGE("Device open failed");
      } else {
          out = new AudioStreamOutALSA(this, &(*it));
          err = out->set(format, channels, sampleRate, devices);
      }

      if (status) *status = err;
      return out;
    }
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
    Mutex::Autolock autoLock(mLock);
    LOGD("openOutputSession");
    AudioStreamOutALSA *out = 0;
    status_t err = BAD_VALUE;

    alsa_handle_t alsa_handle;
    unsigned long bufferSize = DEFAULT_BUFFER_SIZE;

    for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;

    alsa_handle.module = mALSADevice;
    alsa_handle.bufferSize = bufferSize;
    alsa_handle.devices = devices;
    alsa_handle.handle = 0;
    alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
    alsa_handle.channels = DEFAULT_CHANNEL_MODE;
    alsa_handle.sampleRate = DEFAULT_SAMPLING_RATE;
    alsa_handle.latency = VOICE_LATENCY;
    alsa_handle.rxHandle = 0;
    alsa_handle.ucMgr = mUcMgr;

    char *use_case;
    snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
    if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
        strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER, sizeof(alsa_handle.useCase));
    } else {
        strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_LPA, sizeof(alsa_handle.useCase));
    }
    free(use_case);
    mDeviceList.push_back(alsa_handle);
    ALSAHandleList::iterator it = mDeviceList.end();
    it--;
    LOGD("useCase %s", it->useCase);
    mALSADevice->route(&(*it), devices, mode());
    if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER)) {
        snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_HIFI_LOW_POWER);
    } else {
        snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_LPA);
    }
    err = mALSADevice->open(&(*it));
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
    Mutex::Autolock autoLock(mLock);
    char *use_case;
    int newMode = mode();
    uint32_t route_devices;

    status_t err = BAD_VALUE;
    AudioStreamInALSA *in = 0;
    ALSAHandleList::iterator it;

    LOGD("openInputStream: devices 0x%x channels %d sampleRate %d", devices, *channels, *sampleRate);
    if (devices & (devices - 1)) {
        if (status) *status = err;
        return in;
    }

    if((devices == AudioSystem::DEVICE_IN_COMMUNICATION) )  {
        bool voipstream_active = false;
        for(it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
                if((!strcmp(it->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) ||
                   (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
                    LOGD("openInput:  it->rxHandle %d it->handle %d",it->rxHandle,it->handle);
                    voipstream_active = true;
                    break;
                }
        }
        if(voipstream_active == false) {
           mVoipStreamCount = 0;
           mVoipMicMute = false;
           alsa_handle_t alsa_handle;
           unsigned long bufferSize;
           if(*sampleRate == VOIP_SAMPLING_RATE_8K) {
               bufferSize = VOIP_BUFFER_SIZE_8K;
           }
           else if(*sampleRate == VOIP_SAMPLING_RATE_16K) {
               bufferSize = VOIP_BUFFER_SIZE_16K;
           }
           else {
               LOGE("unsupported samplerate %d for voip",*sampleRate);
               if (status) *status = err;
               return in;
           }
           alsa_handle.module = mALSADevice;
           alsa_handle.bufferSize = bufferSize;
           alsa_handle.devices = devices;
           alsa_handle.handle = 0;
           alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
           alsa_handle.channels = VOIP_DEFAULT_CHANNEL_MODE;
           alsa_handle.sampleRate = *sampleRate;
           alsa_handle.latency = VOIP_RECORD_LATENCY;
           alsa_handle.rxHandle = 0;
           alsa_handle.ucMgr = mUcMgr;
           snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
           if ((use_case != NULL) && (strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
                strcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_VOIP);
           } else {
                strcpy(alsa_handle.useCase, SND_USE_CASE_VERB_IP_VOICECALL);
           }
           free(use_case);
           mDeviceList.push_back(alsa_handle);
           it = mDeviceList.end();
           it--;
           mALSADevice->route(&(*it),mCurDevice, AudioSystem::MODE_IN_COMMUNICATION);
           if(!strcmp(it->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) {
               snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_IP_VOICECALL);
           } else {
               snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_VOIP);
           }
           if(sampleRate) {
               it->sampleRate = *sampleRate;
           }
           if(channels)
               it->channels = AudioSystem::popCount(*channels);
           err = mALSADevice->startVoipCall(&(*it));
           if (err) {
               LOGE("Error opening pcm input device");
               return NULL;
           }
        }
        in = new AudioStreamInALSA(this, &(*it), acoustics);
        err = in->set(format, channels, sampleRate, devices);
        if(err == NO_ERROR) {
            mVoipStreamCount++;   //increment VoipstreamCount only if success
            LOGD("OpenInput mVoipStreamCount %d",mVoipStreamCount);
        }
        LOGE("openInput: After Get alsahandle");
        if (status) *status = err;
        return in;
      } else
      {
        for(ALSAHandleList::iterator itDev = mDeviceList.begin();
              itDev != mDeviceList.end(); ++itDev)
        {
            if((0 == strncmp(itDev->useCase, SND_USE_CASE_VERB_HIFI_REC, MAX_UC_LEN))
              ||(0 == strncmp(itDev->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC, MAX_UC_LEN))
              ||(0 == strncmp(itDev->useCase, SND_USE_CASE_MOD_CAPTURE_FM, MAX_UC_LEN))
              ||(0 == strncmp(itDev->useCase, SND_USE_CASE_VERB_FM_REC, MAX_UC_LEN)))
            {
                if(!(devices == AudioSystem::DEVICE_IN_FM_RX_A2DP)){
                    LOGD("Input stream already exists, new stream not permitted: useCase:%s, devices:0x%x, module:%p",
                        itDev->useCase, itDev->devices, itDev->module);
                    return in;
                }
            }
        else if ((0 == strncmp(itDev->useCase, SND_USE_CASE_VERB_FM_A2DP_REC, MAX_UC_LEN))
                ||(0 == strncmp(itDev->useCase, SND_USE_CASE_MOD_CAPTURE_A2DP_FM, MAX_UC_LEN)))
             {
                 if((devices == AudioSystem::DEVICE_IN_FM_RX_A2DP)){
                     LOGD("Input stream already exists, new stream not permitted: useCase:%s, devices:0x%x, module:%p",
                         itDev->useCase, itDev->devices, itDev->module);
                     return in;
                 }
             }
        }

        alsa_handle_t alsa_handle;
        unsigned long bufferSize = DEFAULT_IN_BUFFER_SIZE;

        alsa_handle.module = mALSADevice;
        alsa_handle.bufferSize = bufferSize;
        alsa_handle.devices = devices;
        alsa_handle.handle = 0;
        alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
        alsa_handle.channels = VOICE_CHANNEL_MODE;
        alsa_handle.sampleRate = android::AudioRecord::DEFAULT_SAMPLE_RATE;
        alsa_handle.latency = RECORD_LATENCY;
        alsa_handle.rxHandle = 0;
        alsa_handle.ucMgr = mUcMgr;
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case != NULL) && (strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            if ((devices == AudioSystem::DEVICE_IN_VOICE_CALL) &&
                (newMode == AudioSystem::MODE_IN_CALL)) {
                LOGD("openInputStream: into incall recording, channels %d", *channels);
                mIncallMode = *channels;
                if ((*channels & AudioSystem::CHANNEL_IN_VOICE_UPLINK) &&
                    (*channels & AudioSystem::CHANNEL_IN_VOICE_DNLINK)) {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_VOICE_UL_DL,
                            sizeof(alsa_handle.useCase));
                } else if (*channels & AudioSystem::CHANNEL_IN_VOICE_DNLINK) {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_VOICE_DL,
                            sizeof(alsa_handle.useCase));
                }
            } else if((devices == AudioSystem::DEVICE_IN_FM_RX)) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_FM, sizeof(alsa_handle.useCase));
            } else if(devices == AudioSystem::DEVICE_IN_FM_RX_A2DP) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_A2DP_FM, sizeof(alsa_handle.useCase));
            } else {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC, sizeof(alsa_handle.useCase));
            }
        } else {
            if ((devices == AudioSystem::DEVICE_IN_VOICE_CALL) &&
                (newMode == AudioSystem::MODE_IN_CALL)) {
                LOGD("openInputStream: incall recording, channels %d", *channels);
                mIncallMode = *channels;
                if ((*channels & AudioSystem::CHANNEL_IN_VOICE_UPLINK) &&
                    (*channels & AudioSystem::CHANNEL_IN_VOICE_DNLINK)) {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_UL_DL_REC, sizeof(alsa_handle.useCase));
                } else if (*channels & AudioSystem::CHANNEL_IN_VOICE_DNLINK) {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_DL_REC, sizeof(alsa_handle.useCase));
                }
            } else if(devices == AudioSystem::DEVICE_IN_FM_RX) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_FM_REC, sizeof(alsa_handle.useCase));
            } else if (devices == AudioSystem::DEVICE_IN_FM_RX_A2DP) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_FM_A2DP_REC, sizeof(alsa_handle.useCase));
            } else {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_REC, sizeof(alsa_handle.useCase));
            }
        }
        free(use_case);
        mDeviceList.push_back(alsa_handle);
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;
        if (devices == AudioSystem::DEVICE_IN_VOICE_CALL){
           /* Add current devices info to devices to do route */
            route_devices = devices | mCurDevice;
            mALSADevice->route(&(*it), route_devices, mode());
        } else {
            mALSADevice->route(&(*it), devices, mode());
        }
        if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_REC) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_FM_REC) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_FM_A2DP_REC) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_DL_REC) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_UL_DL_REC)) {
            snd_use_case_set(mUcMgr, "_verb", it->useCase);
        } else {
            snd_use_case_set(mUcMgr, "_enamod", it->useCase);
        }
        if(sampleRate) {
            it->sampleRate = *sampleRate;
        }
        if(channels) {
            it->channels = AudioSystem::popCount((*channels) &
                      (AudioSystem::CHANNEL_IN_STEREO |
                       AudioSystem::CHANNEL_IN_MONO));
            LOGD("channels %d", it->channels);
        }
        err = mALSADevice->open(&(*it));
        if (err) {
           LOGE("Error opening pcm input device");
        } else {
           in = new AudioStreamInALSA(this, &(*it), acoustics);
           err = in->set(format, channels, sampleRate, devices);
        }
        if (status) *status = err;
        return in;
      }
}

void
AudioHardwareALSA::closeInputStream(AudioStreamIn* in)
{
    delete in;
}

status_t AudioHardwareALSA::setMicMute(bool state)
{
    int newMode = mode();
    LOGD("setMicMute  newMode %d",newMode);
    if(newMode == AudioSystem::MODE_IN_COMMUNICATION) {
        if (mVoipMicMute != state) {
             mVoipMicMute = state;
            LOGD("setMicMute: mVoipMicMute %d", mVoipMicMute);
            if(mALSADevice) {
                mALSADevice->setVoipMicMute(state);
            }
        }
    } else {
        if (mMicMute != state) {
              mMicMute = state;
              LOGD("setMicMute: mMicMute %d", mMicMute);
              if(mALSADevice) {
                 mALSADevice->setMicMute(state);
              }
        }
    }
    return NO_ERROR;
}

status_t AudioHardwareALSA::getMicMute(bool *state)
{
    int newMode = mode();
    if(newMode == AudioSystem::MODE_IN_COMMUNICATION) {
        *state = mVoipMicMute;
    } else {
        *state = mMicMute;
    }
    return NO_ERROR;
}

status_t AudioHardwareALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

size_t AudioHardwareALSA::getInputBufferSize(uint32_t sampleRate, int format, int channelCount)
{
    size_t bufferSize;
    if (format != AudioSystem::PCM_16_BIT) {
         LOGW("getInputBufferSize bad format: %d", format);
         return 0;
    }
    if(sampleRate == 16000) {
        bufferSize = DEFAULT_IN_BUFFER_SIZE * 2 * channelCount;
    } else if(sampleRate < 44100) {
        bufferSize = DEFAULT_IN_BUFFER_SIZE * channelCount;
    } else {
        bufferSize = DEFAULT_IN_BUFFER_SIZE * 12;
    }
    return bufferSize;
}

void AudioHardwareALSA::handleFm(int device)
{
int newMode = mode();
    if(device & AudioSystem::DEVICE_OUT_FM && mIsFmActive == 0) {
        // Start FM Radio on current active device
        unsigned long bufferSize = FM_BUFFER_SIZE;
        alsa_handle_t alsa_handle;
        char *use_case;
        LOGV("Start FM");
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_DIGITAL_RADIO, sizeof(alsa_handle.useCase));
        } else {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_FM, sizeof(alsa_handle.useCase));
        }
        free(use_case);

        for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;
        alsa_handle.module = mALSADevice;
        alsa_handle.bufferSize = bufferSize;
        alsa_handle.devices = device;
        alsa_handle.handle = 0;
        alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
        alsa_handle.channels = DEFAULT_CHANNEL_MODE;
        alsa_handle.sampleRate = DEFAULT_SAMPLING_RATE;
        alsa_handle.latency = VOICE_LATENCY;
        alsa_handle.rxHandle = 0;
        alsa_handle.ucMgr = mUcMgr;
        mIsFmActive = 1;
        mDeviceList.push_back(alsa_handle);
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;
        mALSADevice->route(&(*it), (uint32_t)device, newMode);
        if(!strcmp(it->useCase, SND_USE_CASE_VERB_DIGITAL_RADIO)) {
            snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_DIGITAL_RADIO);
        } else {
            snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_FM);
        }
        mALSADevice->startFm(&(*it));
    } else if (!(device & AudioSystem::DEVICE_OUT_FM) && mIsFmActive == 1) {
        //i Stop FM Radio
        LOGV("Stop FM");
        for(ALSAHandleList::iterator it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
            if((!strcmp(it->useCase, SND_USE_CASE_VERB_DIGITAL_RADIO)) ||
              (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_FM))) {
                mALSADevice->close(&(*it));
                //mALSADevice->route(&(*it), (uint32_t)device, newMode);
                mDeviceList.erase(it);
                break;
            }
        }
        mIsFmActive = 0;
    }
}
}       // namespace android_audio_legacy
