/* alsa_default.cpp
 **
 ** Copyright 2009 Wind River Systems
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

#define LOG_TAG "ALSAModule"
//#define LOG_NDEBUG 0
#define LOG_NDDEBUG 0
#include <utils/Log.h>
#include <cutils/properties.h>
#include <linux/ioctl.h>
#include "AudioHardwareALSA.h"
#include <media/AudioRecord.h>

#ifndef ALSA_DEFAULT_SAMPLE_RATE
#define ALSA_DEFAULT_SAMPLE_RATE 44100 // in Hz
#endif

#define BTSCO_RATE_16KHZ 16000

namespace android_audio_legacy
{

static int      s_device_open(const hw_module_t*, const char*, hw_device_t**);
static int      s_device_close(hw_device_t*);
static status_t s_init(alsa_device_t *, ALSAHandleList &);
static status_t s_open(alsa_handle_t *);
static status_t s_close(alsa_handle_t *);
static status_t s_standby(alsa_handle_t *);
static status_t s_route(alsa_handle_t *, uint32_t, int);
static status_t s_start_voice_call(alsa_handle_t *);
static status_t s_start_voip_call(alsa_handle_t *);
static status_t s_start_fm(alsa_handle_t *);
static void     s_set_voice_volume(int);
static void     s_set_voip_volume(int);
static void     s_set_mic_mute(int);
static void     s_set_voip_mic_mute(int);
static status_t s_set_fm_vol(int);
static void     s_set_btsco_rate(int);
static status_t s_set_lpa_vol(int);
static void     s_enable_wide_voice(bool flag);
static void     s_set_flags(uint32_t flags);

static char mic_type[25];
static char curRxUCMDevice[50];
static char curTxUCMDevice[50];
static int fluence_mode;
static int fmVolume;
static uint32_t mDevSettingsFlag = TTY_OFF;
static int btsco_samplerate = 8000;

static hw_module_methods_t s_module_methods = {
    open            : s_device_open
};

extern "C" const hw_module_t HAL_MODULE_INFO_SYM = {
    tag             : HARDWARE_MODULE_TAG,
    version_major   : 1,
    version_minor   : 0,
    id              : ALSA_HARDWARE_MODULE_ID,
    name            : "QCOM ALSA module",
    author          : "QuIC Inc",
    methods         : &s_module_methods,
    dso             : 0,
    reserved        : {0,},
};

static int s_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device)
{
    char value[128];
    alsa_device_t *dev;
    dev = (alsa_device_t *) malloc(sizeof(*dev));
    if (!dev) return -ENOMEM;

    memset(dev, 0, sizeof(*dev));

    /* initialize the procs */
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (hw_module_t *) module;
    dev->common.close = s_device_close;
    dev->init = s_init;
    dev->open = s_open;
    dev->close = s_close;
    dev->route = s_route;
    dev->standby = s_standby;
    dev->startVoiceCall = s_start_voice_call;
    dev->startVoipCall = s_start_voip_call;
    dev->startFm = s_start_fm;
    dev->setVoiceVolume = s_set_voice_volume;
    dev->setVoipVolume = s_set_voip_volume;
    dev->setMicMute = s_set_mic_mute;
    dev->setVoipMicMute = s_set_voip_mic_mute;
    dev->setFmVolume = s_set_fm_vol;
    dev->setBtscoRate = s_set_btsco_rate;
    dev->setLpaVolume = s_set_lpa_vol;
    dev->enableWideVoice = s_enable_wide_voice;
    dev->setFlags = s_set_flags;

    *device = &dev->common;

    property_get("persist.audio.handset.mic",value,"0");
    strlcpy(mic_type, value, sizeof(mic_type));
    property_get("persist.audio.fluence.mode",value,"0");
    if (!strcmp("broadside", value)) {
        fluence_mode = FLUENCE_MODE_BROADSIDE;
    } else {
        fluence_mode = FLUENCE_MODE_ENDFIRE;
    }
    strlcpy(curRxUCMDevice, "None", sizeof(curRxUCMDevice));
    strlcpy(curTxUCMDevice, "None", sizeof(curTxUCMDevice));
    LOGD("ALSA module opened");

    return 0;
}

static int s_device_close(hw_device_t* device)
{
    free(device);
    return 0;
}

// ----------------------------------------------------------------------------

static const int DEFAULT_SAMPLE_RATE = ALSA_DEFAULT_SAMPLE_RATE;

static void switchDevice(alsa_handle_t *handle, uint32_t devices, uint32_t mode);
static char *getUCMDevice(uint32_t devices, int input);
static void disableDevice(alsa_handle_t *handle);

static int callMode = AudioSystem::MODE_NORMAL;
// ----------------------------------------------------------------------------

int deviceName(alsa_handle_t *handle, unsigned flags, char **value)
{
    int ret = 0;
    char ident[70];

    if (flags & PCM_IN) {
        strlcpy(ident, "CapturePCM/", sizeof(ident));
    } else {
        strlcpy(ident, "PlaybackPCM/", sizeof(ident));
    }
    strlcat(ident, handle->useCase, sizeof(ident));
    ret = snd_use_case_get(handle->ucMgr, ident, (const char **)value);
    LOGD("Device value returned is %s", (*value));
    return ret;
}

status_t setHardwareParams(alsa_handle_t *handle)
{
    struct snd_pcm_hw_params *params;
    unsigned long bufferSize, reqBuffSize;
    unsigned int periodTime, bufferTime;
    unsigned int requestedRate = handle->sampleRate;
    int status = 0;

    params = (snd_pcm_hw_params*) calloc(1, sizeof(struct snd_pcm_hw_params));
    if (!params) {
		//SMANI:: Commented to fix build issues. FIX IT.
        //LOG_ALWAYS_FATAL("Failed to allocate ALSA hardware parameters!");
        return NO_INIT;
    }

    reqBuffSize = handle->bufferSize;
    LOGD("setHardwareParams: reqBuffSize %d channels %d sampleRate %d",
         (int) reqBuffSize, handle->channels, handle->sampleRate);

    param_init(params);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS,
                   SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
                   SNDRV_PCM_FORMAT_S16_LE);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT,
                   SNDRV_PCM_SUBFORMAT_STD);
    param_set_min(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, reqBuffSize);
    param_set_int(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_FRAME_BITS,
                   handle->channels - 1 ? 32 : 16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_CHANNELS,
                  handle->channels);
    param_set_int(params, SNDRV_PCM_HW_PARAM_RATE, handle->sampleRate);
    param_set_hw_refine(handle->handle, params);

    if (param_set_hw_params(handle->handle, params)) {
        LOGE("cannot set hw params");
        return NO_INIT;
    }
    param_dump(params);

    handle->handle->buffer_size = pcm_buffer_size(params);
    handle->handle->period_size = pcm_period_size(params);
    handle->handle->period_cnt = handle->handle->buffer_size/handle->handle->period_size;
    LOGD("setHardwareParams: buffer_size %d, period_size %d, period_cnt %d",
        handle->handle->buffer_size, handle->handle->period_size,
        handle->handle->period_cnt);
    handle->handle->rate = handle->sampleRate;
    handle->handle->channels = handle->channels;
    handle->periodSize = handle->handle->period_size;
    handle->bufferSize = handle->handle->period_size;
    return NO_ERROR;
}

status_t setSoftwareParams(alsa_handle_t *handle)
{
    struct snd_pcm_sw_params* params;
    struct pcm* pcm = handle->handle;

    unsigned long periodSize = pcm->period_size;

    params = (snd_pcm_sw_params*) calloc(1, sizeof(struct snd_pcm_sw_params));
    if (!params) {
        LOG_ALWAYS_FATAL("Failed to allocate ALSA software parameters!");
        return NO_INIT;
    }

    // Get the current software parameters
    params->tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    params->period_step = 1;
    if(((!strcmp(handle->useCase,SND_USE_CASE_MOD_PLAY_VOIP)) ||
        (!strcmp(handle->useCase,SND_USE_CASE_VERB_IP_VOICECALL)))){
          LOGV("setparam:  start & stop threshold for Voip ");
          params->avail_min = handle->channels - 1 ? periodSize/4 : periodSize/2;
          params->start_threshold = periodSize/2;
          params->stop_threshold = INT_MAX;
     } else {
         params->avail_min = periodSize/2;
         params->start_threshold = handle->channels - 1 ? periodSize/2 : periodSize/4;
         params->stop_threshold = INT_MAX;
     }
    params->silence_threshold = 0;
    params->silence_size = 0;

    if (param_set_sw_params(handle->handle, params)) {
        LOGE("cannot set sw params");
        return NO_INIT;
    }
    return NO_ERROR;
}

void switchDevice(alsa_handle_t *handle, uint32_t devices, uint32_t mode)
{
    bool inCallDevSwitch = false;
    char *rxDevice, *txDevice, ident[70];
    LOGV("%s: device %d", __FUNCTION__, devices);

    if ((mode == AudioSystem::MODE_IN_CALL)  || (mode == AudioSystem::MODE_IN_COMMUNICATION)) {
        if ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
            (devices & AudioSystem::DEVICE_IN_WIRED_HEADSET)) {
            devices = devices | (AudioSystem::DEVICE_OUT_WIRED_HEADSET |
                      AudioSystem::DEVICE_IN_WIRED_HEADSET);
        } else if (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE) {
            devices = devices | (AudioSystem::DEVICE_OUT_WIRED_HEADPHONE |
                      AudioSystem::DEVICE_IN_BUILTIN_MIC);
        } else if ((devices & AudioSystem::DEVICE_OUT_EARPIECE) ||
                  (devices & AudioSystem::DEVICE_IN_BUILTIN_MIC)) {
            devices = devices | (AudioSystem::DEVICE_IN_BUILTIN_MIC |
                      AudioSystem::DEVICE_OUT_EARPIECE);
        } else if (devices & AudioSystem::DEVICE_OUT_SPEAKER) {
            devices = devices | (AudioSystem::DEVICE_IN_DEFAULT |
                       AudioSystem::DEVICE_OUT_SPEAKER);
        } else if ((devices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO) ||
                   (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET) ||
                   (devices & AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET)) {
            devices = devices | (AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET |
                      AudioSystem::DEVICE_OUT_BLUETOOTH_SCO);
        } else if ((devices & AudioSystem::DEVICE_OUT_ANC_HEADSET) ||
                   (devices & AudioSystem::DEVICE_IN_ANC_HEADSET)) {
            devices = devices | (AudioSystem::DEVICE_OUT_ANC_HEADSET |
                      AudioSystem::DEVICE_IN_ANC_HEADSET);
        } else if (devices & AudioSystem::DEVICE_OUT_ANC_HEADPHONE) {
            devices = devices | (AudioSystem::DEVICE_OUT_ANC_HEADPHONE |
                      AudioSystem::DEVICE_IN_BUILTIN_MIC);
        } else if (devices & AudioSystem::DEVICE_OUT_AUX_DIGITAL) {
            devices = devices | (AudioSystem::DEVICE_OUT_AUX_DIGITAL |
                      AudioSystem::DEVICE_IN_BUILTIN_MIC);
        }
    }

    rxDevice = getUCMDevice(devices & AudioSystem::DEVICE_OUT_ALL, 0);
    txDevice = getUCMDevice(devices & AudioSystem::DEVICE_IN_ALL, 1);
    if ((rxDevice != NULL) && (txDevice != NULL)) {
        if (((strcmp(rxDevice, curRxUCMDevice)) || (strcmp(txDevice, curTxUCMDevice))) &&
            (mode == AudioSystem::MODE_IN_CALL))
            inCallDevSwitch = true;
    }
    if (rxDevice != NULL) {
        if (strcmp(curRxUCMDevice, "None")) {
            if ((!strcmp(rxDevice, curRxUCMDevice)) && (inCallDevSwitch != true)){
                LOGV("Required device is already set, ignoring device enable");
                snd_use_case_set(handle->ucMgr, "_enadev", rxDevice);
            } else {
                strlcpy(ident, "_swdev/", sizeof(ident));
                strlcat(ident, curRxUCMDevice, sizeof(ident));
                snd_use_case_set(handle->ucMgr, ident, rxDevice);
            }
        } else {
            snd_use_case_set(handle->ucMgr, "_enadev", rxDevice);
        }
        strlcpy(curRxUCMDevice, rxDevice, sizeof(curRxUCMDevice));
        free(rxDevice);
        if (devices & AudioSystem::DEVICE_OUT_FM)
            s_set_fm_vol(fmVolume);
    }
    if (txDevice != NULL) {
       if (strcmp(curTxUCMDevice, "None")) {
           if ((!strcmp(txDevice, curTxUCMDevice)) && (inCallDevSwitch != true)){
                LOGV("Required device is already set, ignoring device enable");
                snd_use_case_set(handle->ucMgr, "_enadev", txDevice);
            } else {
                strlcpy(ident, "_swdev/", sizeof(ident));
                strlcat(ident, curTxUCMDevice, sizeof(ident));
                snd_use_case_set(handle->ucMgr, ident, txDevice);
            }
        } else {
            snd_use_case_set(handle->ucMgr, "_enadev", txDevice);
        }
        strlcpy(curTxUCMDevice, txDevice, sizeof(curTxUCMDevice));
        free(txDevice);
    }
    LOGD("switchDevice: curTxUCMDevivce %s curRxDevDevice %s", curTxUCMDevice, curRxUCMDevice);
}

// ----------------------------------------------------------------------------

static status_t s_init(alsa_device_t *module, ALSAHandleList &list)
{
    LOGD("s_init: Initializing devices for ALSA module");

    list.clear();

    return NO_ERROR;
}

static status_t s_open(alsa_handle_t *handle)
{
    char *devName;
    unsigned flags = 0;
    int err = NO_ERROR;

    /* No need to call s_close for LPA as pcm device open and close is handled by LPAPlayer in stagefright */
    if((!strcmp(handle->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER)) || (!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_LPA))) {
        LOGD("s_open: Opening LPA playback");
        return NO_ERROR;
    }

    s_close(handle);

    LOGD("s_open: handle %p", handle);

    // ASoC multicomponent requires a valid path (frontend/backend) for
    // the device to be opened

    // The PCM stream is opened in blocking mode, per ALSA defaults.  The
    // AudioFlinger seems to assume blocking mode too, so asynchronous mode
    // should not be used.
    if ((!strcmp(handle->useCase, SND_USE_CASE_VERB_HIFI)) ||
        (!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_MUSIC))) {
        flags = PCM_OUT;
    } else {
        flags = PCM_IN;
    }
    if (handle->channels == 1) {
        flags |= PCM_MONO;
    } else {
        flags |= PCM_STEREO;
    }
    if (deviceName(handle, flags, &devName) < 0) {
        LOGE("Failed to get pcm device node: %s", devName);
        return NO_INIT;
    }
    handle->handle = pcm_open(flags, (char*)devName);

    if (!handle->handle) {
        LOGE("s_open: Failed to initialize ALSA device '%s'", devName);
        free(devName);
        return NO_INIT;
    }

    handle->handle->flags = flags;
    err = setHardwareParams(handle);

    if (err == NO_ERROR) {
        err = setSoftwareParams(handle);
    }

    if(err != NO_ERROR) {
        LOGE("Set HW/SW params failed: Closing the pcm stream");
        s_standby(handle);
    }

    free(devName);
    return NO_ERROR;
}

static status_t s_start_voip_call(alsa_handle_t *handle)
{

    char* devName;
    char* devName1;
    unsigned flags = 0;
    int err = NO_ERROR;
    uint8_t voc_pkt[VOIP_BUFFER_MAX_SIZE];

    s_close(handle);
    flags = PCM_OUT;
    flags |= PCM_MONO;
    LOGV("s_open:s_start_voip_call  handle %p", handle);

    if (deviceName(handle, flags, &devName) < 0) {
         LOGE("Failed to get pcm device node");
         return NO_INIT;
    }

     handle->handle = pcm_open(flags, (char*)devName);

     if (!handle->handle) {
          free(devName);
          LOGE("s_open: Failed to initialize ALSA device '%s'", devName);
          return NO_INIT;
     }

     if (!pcm_ready(handle->handle)) {
         LOGE(" pcm ready failed");
     }

     handle->handle->flags = flags;
     err = setHardwareParams(handle);

     if (err == NO_ERROR) {
         err = setSoftwareParams(handle);
     }

     err = pcm_prepare(handle->handle);
     if(err != NO_ERROR) {
         LOGE("DEVICE_OUT_DIRECTOUTPUT: pcm_prepare failed");
     }

     /* first write required start dsp */
     memset(&voc_pkt,0,sizeof(voc_pkt));
     pcm_write(handle->handle,&voc_pkt,handle->handle->period_size);
     handle->rxHandle = handle->handle;
     free(devName);
     LOGV("s_open: DEVICE_IN_COMMUNICATION ");
     flags = PCM_IN;
     flags |= PCM_MONO;
     handle->handle = 0;

     if (deviceName(handle, flags, &devName1) < 0) {
        LOGE("Failed to get pcm device node");
        return NO_INIT;
     }
     handle->handle = pcm_open(flags, (char*)devName1);

     if (!handle->handle) {
         free(devName);
         LOGE("s_open: Failed to initialize ALSA device '%s'", devName);
         return NO_INIT;
     }

     if (!pcm_ready(handle->handle)) {
        LOGE(" pcm ready in failed");
     }

     handle->handle->flags = flags;

     err = setHardwareParams(handle);

     if (err == NO_ERROR) {
         err = setSoftwareParams(handle);
     }


     err = pcm_prepare(handle->handle);
     if(err != NO_ERROR) {
         LOGE("DEVICE_IN_COMMUNICATION: pcm_prepare failed");
     }

     /* first read required start dsp */
     memset(&voc_pkt,0,sizeof(voc_pkt));
     pcm_read(handle->handle,&voc_pkt,handle->handle->period_size);
     return NO_ERROR;
}

static status_t s_start_voice_call(alsa_handle_t *handle)
{
    char* devName;
    unsigned flags = 0;
    int err = NO_ERROR;

    LOGD("s_start_voice_call: handle %p", handle);
    // ASoC multicomponent requires a valid path (frontend/backend) for
    // the device to be opened

    flags = PCM_OUT | PCM_MONO;
    if (deviceName(handle, flags, &devName) < 0) {
        LOGE("Failed to get pcm device node");
        return NO_INIT;
    }
    handle->handle = pcm_open(flags, (char*)devName);
    if (!handle->handle) {
        LOGE("s_start_voicecall: could not open PCM device");
        goto Error;
    }

    handle->handle->flags = flags;
    err = setHardwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("s_start_voice_call: setHardwareParams failed");
        goto Error;
    }

    err = setSoftwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("s_start_voice_call: setSoftwareParams failed");
        goto Error;
    }

    err = pcm_prepare(handle->handle);
    if(err != NO_ERROR) {
        LOGE("s_start_voice_call: pcm_prepare failed");
        goto Error;
    }

    if (ioctl(handle->handle->fd, SNDRV_PCM_IOCTL_START)) {
        LOGE("s_start_voice_call:SNDRV_PCM_IOCTL_START failed\n");
        goto Error;
    }

    // Store the PCM playback device pointer in rxHandle
    handle->rxHandle = handle->handle;
    free(devName);

    // Open PCM capture device
    flags = PCM_IN | PCM_MONO;
    if (deviceName(handle, flags, &devName) < 0) {
        LOGE("Failed to get pcm device node");
        goto Error;
    }
    handle->handle = pcm_open(flags, (char*)devName);
    if (!handle->handle) {
        free(devName);
        goto Error;
    }

    handle->handle->flags = flags;
    err = setHardwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("s_start_voice_call: setHardwareParams failed");
        goto Error;
    }

    err = setSoftwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("s_start_voice_call: setSoftwareParams failed");
        goto Error;
    }

    err = pcm_prepare(handle->handle);
    if(err != NO_ERROR) {
        LOGE("s_start_voice_call: pcm_prepare failed");
        goto Error;
    }

    if (ioctl(handle->handle->fd, SNDRV_PCM_IOCTL_START)) {
        LOGE("s_start_voice_call:SNDRV_PCM_IOCTL_START failed\n");
        goto Error;
    }

    free(devName);
    return NO_ERROR;

Error:
    LOGE("s_start_voice_call: Failed to initialize ALSA device '%s'", devName);
    free(devName);
    s_close(handle);
    return NO_INIT;
}

static status_t s_start_fm(alsa_handle_t *handle)
{
    char *devName;
    unsigned flags = 0;
    int err = NO_ERROR;

    LOGE("s_start_fm: handle %p", handle);

    // ASoC multicomponent requires a valid path (frontend/backend) for
    // the device to be opened

    flags = PCM_OUT | PCM_STEREO;
    if (deviceName(handle, flags, &devName) < 0) {
        LOGE("Failed to get pcm device node");
        goto Error;
    }
    handle->handle = pcm_open(flags, (char*)devName);
    if (!handle->handle) {
        LOGE("s_start_fm: could not open PCM device");
        goto Error;
    }

    handle->handle->flags = flags;
    err = setHardwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("s_start_fm: setHardwareParams failed");
        goto Error;
    }

    err = setSoftwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("s_start_fm: setSoftwareParams failed");
        goto Error;
    }

    err = pcm_prepare(handle->handle);
    if(err != NO_ERROR) {
        LOGE("s_start_fm: setSoftwareParams failed");
        goto Error;
    }

    if (ioctl(handle->handle->fd, SNDRV_PCM_IOCTL_START)) {
        LOGE("s_start_fm: SNDRV_PCM_IOCTL_START failed\n");
        goto Error;
    }

    // Store the PCM playback device pointer in rxHandle
    handle->rxHandle = handle->handle;
    free(devName);

    // Open PCM capture device
    flags = PCM_IN | PCM_STEREO;
    if (deviceName(handle, flags, &devName) < 0) {
        LOGE("Failed to get pcm device node");
        goto Error;
    }
    handle->handle = pcm_open(flags, (char*)devName);
    if (!handle->handle) {
        goto Error;
    }

    handle->handle->flags = flags;
    err = setHardwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("s_start_fm: setHardwareParams failed");
        goto Error;
    }

    err = setSoftwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("s_start_fm: setSoftwareParams failed");
        goto Error;
    }

    err = pcm_prepare(handle->handle);
    if(err != NO_ERROR) {
        LOGE("s_start_fm: pcm_prepare failed");
        goto Error;
    }

    if (ioctl(handle->handle->fd, SNDRV_PCM_IOCTL_START)) {
        LOGE("s_start_fm: SNDRV_PCM_IOCTL_START failed\n");
        goto Error;
    }

    s_set_fm_vol(fmVolume);
    free(devName);
    return NO_ERROR;

Error:
    free(devName);
    s_close(handle);
    return NO_INIT;
}

static status_t s_set_fm_vol(int value)
{
    status_t err = NO_ERROR;

    ALSAControl control("/dev/snd/controlC0");
    control.set("Internal FM RX Volume",value,0);
    fmVolume = value;

    return err;
}

static status_t s_set_lpa_vol(int value)
{
    status_t err = NO_ERROR;

    ALSAControl control("/dev/snd/controlC0");
    control.set("LPA RX Volume",value,0);

    return err;
}

static status_t s_start(alsa_handle_t *handle)
{
    status_t err = NO_ERROR;

    if(!handle->handle) {
        LOGE("No active PCM driver to start");
        return err;
    }

    err = pcm_prepare(handle->handle);

    return err;
}

static status_t s_close(alsa_handle_t *handle)
{
    int ret;
    status_t err = NO_ERROR;
     struct pcm *h = handle->rxHandle;

    handle->rxHandle = 0;
    LOGD("s_close: handle %p h %p", handle, h);
    if (h) {
        LOGV("s_close rxHandle\n");
        err = pcm_close(h);
        if(err != NO_ERROR) {
            LOGE("s_close: pcm_close failed for rxHandle with err %d", err);
        }
    }

    h = handle->handle;
    handle->handle = 0;

    if (h) {
          LOGV("s_close handle h %p\n", h);
        err = pcm_close(h);
        if(err != NO_ERROR) {
            LOGE("s_close: pcm_close failed for handle with err %d", err);
        }
        disableDevice(handle);
    } else if((!strcmp(handle->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER)) ||
              (!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_LPA))) {
        disableDevice(handle);
    }

    return err;
}

/*
    this is same as s_close, but don't discard
    the device/mode info. This way we can still
    close the device, hit idle and power-save, reopen the pcm
    for the same device/mode after resuming
*/
static status_t s_standby(alsa_handle_t *handle)
{
    int ret;
    status_t err = NO_ERROR;  
    struct pcm *h = handle->rxHandle;
    handle->rxHandle = 0;
    LOGD("s_standby: handle %p h %p", handle, h);
    if (h) {
        LOGE("s_standby  rxHandle\n");
        err = pcm_close(h);
        if(err != NO_ERROR) {
            LOGE("s_standby: pcm_close failed for rxHandle with err %d", err);
        }
    }

    h = handle->handle;
    handle->handle = 0;

    if (h) {
          LOGE("s_standby handle h %p\n", h);
        err = pcm_close(h);
        if(err != NO_ERROR) {
            LOGE("s_standby: pcm_close failed for handle with err %d", err);
        }
        disableDevice(handle);
    } else if((!strcmp(handle->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER)) ||
              (!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_LPA))) {
        disableDevice(handle);
    }

    return err;
}

static status_t s_route(alsa_handle_t *handle, uint32_t devices, int mode)
{
    status_t status = NO_ERROR;

    LOGD("s_route: devices 0x%x in mode %d", devices, mode);
    callMode = mode;
    switchDevice(handle, devices, mode);
    return status;
}

static void disableDevice(alsa_handle_t *handle)
{
    char *useCase;

    snd_use_case_get(handle->ucMgr, "_verb", (const char **)&useCase);
    if (useCase != NULL) {
        if (!strcmp(useCase, handle->useCase)) {
            snd_use_case_set(handle->ucMgr, "_verb", SND_USE_CASE_VERB_INACTIVE);
        } else {
            snd_use_case_set(handle->ucMgr, "_dismod", handle->useCase);
        }
    } else {
        LOGE("Invalid state, no valid use case found to disable");
    }
    free(useCase);
    if (strcmp(curTxUCMDevice, "None"))
        snd_use_case_set(handle->ucMgr, "_disdev", curTxUCMDevice);
    if (strcmp(curRxUCMDevice, "None"))
        snd_use_case_set(handle->ucMgr, "_disdev", curRxUCMDevice);
}

char *getUCMDevice(uint32_t devices, int input)
{
    if (!input) {
        if (!(mDevSettingsFlag & TTY_OFF) &&
            (callMode == AudioSystem::MODE_IN_CALL) &&
            ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
             (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE) ||
             (devices & AudioSystem::DEVICE_OUT_ANC_HEADSET) ||
             (devices & AudioSystem::DEVICE_OUT_ANC_HEADPHONE))) {
             if (mDevSettingsFlag & TTY_VCO) {
                 return strdup(SND_USE_CASE_DEV_TTY_HEADSET_RX);
             } else if (mDevSettingsFlag & TTY_FULL) {
                 return strdup(SND_USE_CASE_DEV_TTY_FULL_RX);
             } else if (mDevSettingsFlag & TTY_HCO) {
                 return strdup(SND_USE_CASE_DEV_EARPIECE); /* HANDSET RX */
             }
        } else if ((devices & AudioSystem::DEVICE_OUT_SPEAKER) &&
            ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
            (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE))) {
            if (mDevSettingsFlag & ANC_FLAG) {
                return strdup(SND_USE_CASE_DEV_SPEAKER_ANC_HEADSET); /* COMBO SPEAKER+ANC HEADSET RX */
            } else {
                return strdup(SND_USE_CASE_DEV_SPEAKER_HEADSET); /* COMBO SPEAKER+HEADSET RX */
            }
        } else if ((devices & AudioSystem::DEVICE_OUT_SPEAKER) &&
            ((devices & AudioSystem::DEVICE_OUT_ANC_HEADSET) ||
            (devices & AudioSystem::DEVICE_OUT_ANC_HEADPHONE))) {
            return strdup(SND_USE_CASE_DEV_SPEAKER_ANC_HEADSET); /* COMBO SPEAKER+ANC HEADSET RX */
        } else if ((devices & AudioSystem::DEVICE_OUT_SPEAKER) &&
                 (devices & AudioSystem::DEVICE_OUT_FM_TX)) {
            return strdup(SND_USE_CASE_DEV_SPEAKER_FM_TX); /* COMBO SPEAKER+FM_TX RX */
        } else if (devices & AudioSystem::DEVICE_OUT_EARPIECE) {
            return strdup(SND_USE_CASE_DEV_EARPIECE); /* HANDSET RX */
        } else if (devices & AudioSystem::DEVICE_OUT_SPEAKER) {
            return strdup(SND_USE_CASE_DEV_SPEAKER); /* SPEAKER RX */
        } else if ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
                   (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE)) {
            if (mDevSettingsFlag & ANC_FLAG) {
                return strdup(SND_USE_CASE_DEV_ANC_HEADSET); /* ANC HEADSET RX */
            } else {
                return strdup(SND_USE_CASE_DEV_HEADPHONES); /* HEADSET RX */
            }
        } else if ((devices & AudioSystem::DEVICE_OUT_ANC_HEADSET) ||
                   (devices & AudioSystem::DEVICE_OUT_ANC_HEADPHONE)) {
            return strdup(SND_USE_CASE_DEV_ANC_HEADSET); /* ANC HEADSET RX */
        } else if ((devices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO) ||
                  (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET) ||
                  (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT)) {
            if (btsco_samplerate == BTSCO_RATE_16KHZ)
                return strdup(SND_USE_CASE_DEV_BTSCO_WB_RX); /* BTSCO RX*/
            else
                return strdup(SND_USE_CASE_DEV_BTSCO_NB_RX); /* BTSCO RX*/
        } else if ((devices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP) ||
                   (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES) ||
                   (devices & AudioSystem::DEVICE_OUT_DIRECTOUTPUT) ||
                   (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER)) {
            /* Nothing to be done, use current active device */
            return strdup(curRxUCMDevice);
        } else if (devices & AudioSystem::DEVICE_OUT_AUX_DIGITAL) {
            return strdup(SND_USE_CASE_DEV_HDMI); /* HDMI RX */
        } else if (devices & AudioSystem::DEVICE_OUT_FM_TX) {
            return strdup(SND_USE_CASE_DEV_FM_TX); /* FM Tx */
        } else if (devices & AudioSystem::DEVICE_OUT_DEFAULT) {
            return strdup(SND_USE_CASE_DEV_SPEAKER); /* SPEAKER RX */
        } else {
            LOGD("No valid output device: %u", devices);
        }
    } else {
        if (!(mDevSettingsFlag & TTY_OFF) &&
            (callMode == AudioSystem::MODE_IN_CALL) &&
            ((devices & AudioSystem::DEVICE_IN_WIRED_HEADSET) ||
             (devices & AudioSystem::DEVICE_IN_ANC_HEADSET))) {
             if (mDevSettingsFlag & TTY_HCO) {
                 return strdup(SND_USE_CASE_DEV_TTY_HEADSET_TX);
             } else if (mDevSettingsFlag & TTY_FULL) {
                 return strdup(SND_USE_CASE_DEV_TTY_FULL_TX);
             } else if (mDevSettingsFlag & TTY_VCO) {
                 if (!strncmp(mic_type, "analog", 6)) {
                     return strdup(SND_USE_CASE_DEV_HANDSET); /* HANDSET TX */
                 } else {
                     return strdup(SND_USE_CASE_DEV_LINE); /* BUILTIN-MIC TX */
                 }
             }
        } else if (devices & AudioSystem::DEVICE_IN_BUILTIN_MIC) {
            if (!strncmp(mic_type, "analog", 6)) {
                return strdup(SND_USE_CASE_DEV_HANDSET); /* HANDSET TX */
            } else {
                if ((callMode == AudioSystem::MODE_IN_CALL) &&
                    (!strcmp(curRxUCMDevice, SND_USE_CASE_DEV_HDMI))) {
                    return strdup(SND_USE_CASE_DEV_HDMI_TX); /* HDMI TX */
                }
                if (mDevSettingsFlag & DMIC_FLAG) {
                    if (fluence_mode == FLUENCE_MODE_ENDFIRE) {
                        return strdup(SND_USE_CASE_DEV_DUAL_MIC_ENDFIRE); /* DUALMIC EF TX */
                    } else if (fluence_mode == FLUENCE_MODE_BROADSIDE) {
                        return strdup(SND_USE_CASE_DEV_DUAL_MIC_BROADSIDE); /* DUALMIC BS TX */
                    }
                } else {
                    return strdup(SND_USE_CASE_DEV_LINE); /* BUILTIN-MIC TX */
                }
            }
        } else if ((devices & AudioSystem::DEVICE_IN_WIRED_HEADSET) ||
                   (devices & AudioSystem::DEVICE_IN_ANC_HEADSET)) {
            return strdup(SND_USE_CASE_DEV_HEADSET); /* HEADSET TX */
        } else if (devices & AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
             if (btsco_samplerate == BTSCO_RATE_16KHZ)
                 return strdup(SND_USE_CASE_DEV_BTSCO_WB_TX); /* BTSCO TX*/
             else
                 return strdup(SND_USE_CASE_DEV_BTSCO_NB_TX); /* BTSCO TX*/
        } else if (devices & AudioSystem::DEVICE_IN_DEFAULT) {
            if (!strncmp(mic_type, "analog", 6)) {
                return strdup(SND_USE_CASE_DEV_HANDSET); /* HANDSET TX */
            } else {
                if (mDevSettingsFlag & DMIC_FLAG) {
                    if (fluence_mode == FLUENCE_MODE_ENDFIRE) {
                        return strdup(SND_USE_CASE_DEV_SPEAKER_DUAL_MIC_ENDFIRE); /* DUALMIC EF TX */
                    } else if (fluence_mode == FLUENCE_MODE_BROADSIDE) {
                        return strdup(SND_USE_CASE_DEV_SPEAKER_DUAL_MIC_BROADSIDE); /* DUALMIC BS TX */
                    }
                } else {
                    return strdup(SND_USE_CASE_DEV_LINE); /* BUILTIN-MIC TX */
                }
            }
        } else if ((devices & AudioSystem::DEVICE_IN_COMMUNICATION) ||
                   (devices & AudioSystem::DEVICE_IN_FM_RX) ||
                   (devices & AudioSystem::DEVICE_IN_FM_RX_A2DP) ||
                   (devices & AudioSystem::DEVICE_IN_VOICE_CALL)) {
            /* Nothing to be done, use current active device */
            return strdup(curTxUCMDevice);
        } else if ((devices & AudioSystem::DEVICE_IN_COMMUNICATION) ||
                   (devices & AudioSystem::DEVICE_IN_AMBIENT) ||
                   (devices & AudioSystem::DEVICE_IN_BACK_MIC) ||
                   (devices & AudioSystem::DEVICE_IN_AUX_DIGITAL)) {
            LOGI("No proper mapping found with UCM device list, setting default");
            if (!strncmp(mic_type, "analog", 6)) {
                return strdup(SND_USE_CASE_DEV_HANDSET); /* HANDSET TX */
            } else {
                return strdup(SND_USE_CASE_DEV_LINE); /* BUILTIN-MIC TX */
            }
        } else {
            LOGD("No valid input device: %u", devices);
        }
    }
    return NULL;
}

void s_set_voice_volume(int vol)
{
    LOGD("s_set_voice_volume: volume %d", vol);
    ALSAControl control("/dev/snd/controlC0");
    control.set("Voice Rx Volume", vol, 0);
}

void s_set_voip_volume(int vol)
{
    LOGD("s_set_voip_volume: volume %d", vol);
    ALSAControl control("/dev/snd/controlC0");
    control.set("Voip Rx Volume", vol, 0);
}
void s_set_mic_mute(int state)
{
    LOGD("s_set_mic_mute: state %d", state);
    ALSAControl control("/dev/snd/controlC0");
    control.set("Voice Tx Mute", state, 0);
}

void s_set_voip_mic_mute(int state)
{
    LOGD("s_set_voip_mic_mute: state %d", state);
    ALSAControl control("/dev/snd/controlC0");
    control.set("Voip Tx Mute", state, 0);
}

void s_set_btsco_rate(int rate)
{
    btsco_samplerate = rate;
}

void s_enable_wide_voice(bool flag)
{
    LOGD("s_enable_wide_voice: flag %d", flag);
    ALSAControl control("/dev/snd/controlC0");
    if(flag == true) {
        control.set("Widevoice Enable", 1, 0);
    } else {
        control.set("Widevoice Enable", 0, 0);
    }
}

void s_set_flags(uint32_t flags)
{
    LOGV("s_set_flags: flags %d", flags);
    mDevSettingsFlag = flags;
}

}
