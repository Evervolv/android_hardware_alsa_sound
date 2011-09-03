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
#define LOG_NDEBUG 0
#include <utils/Log.h>
#include <cutils/properties.h>
#include <linux/ioctl.h>
#include "AudioHardwareALSA.h"
#include <media/AudioRecord.h>

#ifndef ALSA_DEFAULT_SAMPLE_RATE
#define ALSA_DEFAULT_SAMPLE_RATE 44100 // in Hz
#endif

#define BTSCO_RATE_16KHZ 16000

namespace android
{

static int      s_device_open(const hw_module_t*, const char*, hw_device_t**);
static int      s_device_close(hw_device_t*);
static status_t s_init(alsa_device_t *, ALSAHandleList &);
static status_t s_open(alsa_handle_t *);
static status_t s_close(alsa_handle_t *);
static status_t s_standby(alsa_handle_t *);
static status_t s_route(alsa_handle_t *, uint32_t, int, int);
static status_t s_start_voice_call(alsa_handle_t *);
static status_t s_start_fm(alsa_handle_t *);
static void     s_set_voice_volume(int);
static void     s_set_mic_mute(int);
static status_t s_set_fm_vol(int);
static void     s_set_btsco_rate(int);
static status_t s_set_lpa_vol(int);

static char mic_type[25];
static char curRxUCMDevice[50];
static char curTxUCMDevice[50];
static int fluence_mode;
static int fmVolume;

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
    dev->startFm = s_start_fm;
    dev->setVoiceVolume = s_set_voice_volume;
    dev->setMicMute = s_set_mic_mute;
    dev->setFmVolume = s_set_fm_vol;
    dev->setBtscoRate = s_set_btsco_rate;
    dev->setLpaVolume = s_set_lpa_vol;

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
    LOGV("ALSA module opened");

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

static int ttyMode = 0;
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
    LOGV("Device value returned is %s", (*value));
    return ret;
}

status_t setHardwareParams(alsa_handle_t *handle)
{
    struct snd_pcm_hw_params *params;
    unsigned long periodSize, bufferSize, reqBuffSize;
    unsigned int periodTime, bufferTime;
    unsigned int requestedRate = handle->sampleRate;
    int numPeriods = 8;
    int status = 0;

    params = (snd_pcm_hw_params*) calloc(1, sizeof(struct snd_pcm_hw_params));
    if (!params) {
        LOG_ALWAYS_FATAL("Failed to allocate ALSA hardware parameters!");
        return NO_INIT;
    }

    LOGV("setHWParams: bufferSize %d", handle->bufferSize);
    reqBuffSize = handle->bufferSize;

    if ((!strcmp(handle->useCase, SND_USE_CASE_VERB_VOICECALL)) ||
        (!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_VOICE))) {
        numPeriods = 2;
    }
    if ((!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_FM)) ||
        (!strcmp(handle->useCase, SND_USE_CASE_VERB_DIGITAL_RADIO))) {
        numPeriods = 4;
    }

    periodSize = reqBuffSize;
    bufferSize = reqBuffSize * numPeriods;
    LOGV("setHardwareParams: bufferSize %d periodSize %d channels %d sampleRate %d",
         bufferSize, periodSize, handle->channels, handle->sampleRate);

    param_init(params);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS,
                   SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
                   SNDRV_PCM_FORMAT_S16_LE);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT,
                   SNDRV_PCM_SUBFORMAT_STD);
    param_set_min(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, periodSize);
    param_set_int(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_FRAME_BITS,
                   handle->channels - 1 ? 32 : 16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_CHANNELS,
                  handle->channels);
    param_set_int(params, SNDRV_PCM_HW_PARAM_PERIODS, numPeriods);
    param_set_int(params, SNDRV_PCM_HW_PARAM_RATE, handle->sampleRate);
    param_set_hw_refine(handle->handle, params);

    handle->handle->rate = handle->sampleRate;
    handle->handle->channels = handle->channels;
    handle->handle->period_size = periodSize;
    handle->handle->buffer_size = bufferSize;

    if (param_set_hw_params(handle->handle, params)) {
        LOGE("cannot set hw params");
        return NO_INIT;
    }
    param_dump(params);
    handle->bufferSize = handle->handle->period_size;
    return NO_ERROR;
}

status_t setSoftwareParams(alsa_handle_t *handle)
{
    struct snd_pcm_sw_params* params;
    struct pcm* pcm = handle->handle;

    unsigned long bufferSize = pcm->buffer_size;
    unsigned long periodSize = pcm->period_size;

    params = (snd_pcm_sw_params*) calloc(1, sizeof(struct snd_pcm_sw_params));
    if (!params) {
        LOG_ALWAYS_FATAL("Failed to allocate ALSA software parameters!");
        return NO_INIT;
    }

    // Get the current software parameters
    params->tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    params->period_step = 1;
    params->avail_min = handle->channels - 1 ? periodSize/2 : periodSize/4;
    params->start_threshold = handle->channels - 1 ? periodSize/2 : periodSize/4;
    params->stop_threshold = handle->channels - 1 ? bufferSize/2 : bufferSize/4;
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
    char *device, ident[70];
    LOGV("%s: device %d", __FUNCTION__, devices);

    if (mode == AudioSystem::MODE_IN_CALL) {
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
        }
    }

    device = getUCMDevice(devices & AudioSystem::DEVICE_OUT_ALL, 0);
    if (device != NULL) {
        if (strcmp(curRxUCMDevice, "None")) {
            if ((!strcmp(device, curRxUCMDevice)) && (mode != AudioSystem::MODE_IN_CALL)){
                LOGV("Required device is already set, ignoring device enable");
                snd_use_case_set(handle->ucMgr, "_enadev", device);
            } else {
                strlcpy(ident, "_swdev/", sizeof(ident));
                strlcat(ident, curRxUCMDevice, sizeof(ident));
                snd_use_case_set(handle->ucMgr, ident, device);
            }
        } else {
            snd_use_case_set(handle->ucMgr, "_enadev", device);
        }
        strlcpy(curRxUCMDevice, device, sizeof(curRxUCMDevice));
        free(device);
        if (devices & AudioSystem::DEVICE_OUT_FM)
            s_set_fm_vol(fmVolume);
    }
    device = getUCMDevice(devices & AudioSystem::DEVICE_IN_ALL, 1);
    if (device != NULL) {
       if (strcmp(curTxUCMDevice, "None")) {
           if ((!strcmp(device, curTxUCMDevice)) && (mode != AudioSystem::MODE_IN_CALL)){
                LOGV("Required device is already set, ignoring device enable");
                snd_use_case_set(handle->ucMgr, "_enadev", device);
            } else {
                strlcpy(ident, "_swdev/", sizeof(ident));
                strlcat(ident, curTxUCMDevice, sizeof(ident));
                snd_use_case_set(handle->ucMgr, ident, device);
            }
        } else {
            snd_use_case_set(handle->ucMgr, "_enadev", device);
        }
        strlcpy(curTxUCMDevice, device, sizeof(curTxUCMDevice));
        free(device);
    }
    LOGV("switchDevice: curTxUCMDevivce %s curRxDevDevice %s", curTxUCMDevice, curRxUCMDevice);
}

// ----------------------------------------------------------------------------

static status_t s_init(alsa_device_t *module, ALSAHandleList &list)
{
    LOGV("s_init: Initializing devices for ALSA module");

    list.clear();

    return NO_ERROR;
}

static status_t s_open(alsa_handle_t *handle)
{
    char *devName;
    unsigned flags = 0;
    int err = NO_ERROR;

    s_close(handle);

    if((!strcmp(handle->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER)) || (!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_LPA))) {
        LOGV("s_open: Opening LPA playback");
        return NO_ERROR;
    }

    LOGV("s_open: handle %p", handle);

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

static status_t s_start_voice_call(alsa_handle_t *handle)
{
    char* devName;
    unsigned flags = 0;
    int err = NO_ERROR;

    LOGV("s_start_voice_call: handle %p", handle);
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

    // Store the PCM playback device pointer in recHandle
    handle->recHandle = handle->handle;
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

    LOGV("s_start_fm: handle %p", handle);

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

    // Store the PCM playback device pointer in recHandle
    handle->recHandle = handle->handle;
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
    struct pcm *h = handle->handle;

    handle->handle = 0;
    LOGV("s_close: handle %p", handle);

    if (h) {
        err = pcm_close(h);
        if(err != NO_ERROR) {
            LOGE("s_close: pcm_close failed with err %d", err);
        }

        if(((!strcmp(handle->useCase, SND_USE_CASE_VERB_VOICECALL)) || (!strcmp(handle->useCase, SND_USE_CASE_VERB_DIGITAL_RADIO)) ||
            (!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_VOICE)) || (!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_FM))) &&
            handle->recHandle != NULL) {
            err = pcm_close(handle->recHandle);
            if(err != NO_ERROR) {
                LOGE("s_close: pcm_close failed with err %d", err);
            }
            handle->recHandle = NULL;
        }
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
    struct pcm *h = handle->handle;
    handle->handle = 0;
    LOGV("s_standby\n");
    if (h) {
        err = pcm_close(h);
        if(err != NO_ERROR) {
            LOGE("s_standby: pcm_close failed with err %d", err);
        }
        disableDevice(handle);
    }

    return err;
}

static status_t s_route(alsa_handle_t *handle, uint32_t devices, int mode, int tty)
{
    status_t status = NO_ERROR;

    LOGV("s_route: devices 0x%x in mode %d ttyMode %d", devices, mode, tty);
    ttyMode = tty; callMode = mode;
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
        handle->useCase[0] = 0;
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
        if ((ttyMode != TTY_OFF) &&
            (callMode == AudioSystem::MODE_IN_CALL) &&
            ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
             (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE) ||
             (devices & AudioSystem::DEVICE_OUT_ANC_HEADSET) ||
             (devices & AudioSystem::DEVICE_OUT_ANC_HEADPHONE))) {
             if (ttyMode == TTY_VCO) {
                 return strdup(SND_USE_CASE_DEV_TTY_HEADSET_RX);
             } else if (ttyMode == TTY_FULL) {
                 return strdup(SND_USE_CASE_DEV_TTY_FULL_RX);
             } else if (ttyMode == TTY_HCO) {
                 return strdup(SND_USE_CASE_DEV_EARPIECE); /* HANDSET RX */
             }
        } else if ((devices & AudioSystem::DEVICE_OUT_SPEAKER) &&
            ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
            (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE))) {
            return strdup(SND_USE_CASE_DEV_SPEAKER_HEADSET); /* COMBO SPEAKER+HEADSET RX */
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
            /* TODO: Check if TTY is enabled and get the TTY mode
             *       return different UCM device name for the
             *       corresponding TTY mode if required
             */
            return strdup(SND_USE_CASE_DEV_HEADPHONES); /* HEADSET RX */
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
            LOGV("No valid output device: %u", devices);
        }
    } else {
        if ((ttyMode != TTY_OFF) &&
            (callMode == AudioSystem::MODE_IN_CALL) &&
            ((devices & AudioSystem::DEVICE_IN_WIRED_HEADSET) ||
             (devices & AudioSystem::DEVICE_IN_ANC_HEADSET))) {
             if (ttyMode == TTY_HCO) {
                 return strdup(SND_USE_CASE_DEV_TTY_HEADSET_TX);
             } else if (ttyMode == TTY_FULL) {
                 return strdup(SND_USE_CASE_DEV_TTY_FULL_TX);
             } else if (ttyMode == TTY_VCO) {
                 return strdup(SND_USE_CASE_DEV_HANDSET); /* HANDSET TX */
             }
        } else if (devices & AudioSystem::DEVICE_IN_BUILTIN_MIC) {
            if (!strncmp(mic_type, "analog", 6)) {
                return strdup(SND_USE_CASE_DEV_HANDSET); /* HANDSET TX */
            } else {
                if (devices & AudioSystem::DEVICE_IN_BACK_MIC) {
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
                if (devices & AudioSystem::DEVICE_IN_BACK_MIC) {
                    if (fluence_mode == FLUENCE_MODE_ENDFIRE) {
                        return strdup(SND_USE_CASE_DEV_SPEAKER_DUAL_MIC_ENDFIRE); /* DUALMIC EF TX */
                    } else if (fluence_mode == FLUENCE_MODE_BROADSIDE) {
                        return strdup(SND_USE_CASE_DEV_SPEAKER_DUAL_MIC_BROADSIDE); /* DUALMIC BS TX */
                    }
                } else {
                    return strdup(SND_USE_CASE_DEV_LINE); /* BUILTIN-MIC TX */
                }
            }
        } else if ((devices & AudioSystem::DEVICE_IN_FM_RX) ||
                   (devices & AudioSystem::DEVICE_IN_FM_RX_A2DP) ||
                   (devices & AudioSystem::DEVICE_IN_VOICE_CALL)) {
            /* Nothing to be done, use current active device */
            return strdup(curTxUCMDevice);
        } else if ((devices & AudioSystem::DEVICE_IN_COMMUNICATION) ||
                   (devices & AudioSystem::DEVICE_IN_AMBIENT) ||
                   (devices & AudioSystem::DEVICE_IN_BACK_MIC) ||
                   (devices & AudioSystem::DEVICE_IN_AUX_DIGITAL)) {
            LOGI("No proper mapping found with UCM device list, setting default");
             return strdup(SND_USE_CASE_DEV_HANDSET); /* HANDSET TX */
        } else {
            LOGV("No valid input device: %u", devices);
        }
    }
    return NULL;
}

void s_set_voice_volume(int vol)
{
    LOGV("s_set_voice_volume: volume %d", vol);
    ALSAControl control("/dev/snd/controlC0");
    control.set("Voice Rx Volume", vol, 0);
}

void s_set_mic_mute(int state)
{
    LOGV("s_set_mic_mute: state %d", state);
    ALSAControl control("/dev/snd/controlC0");
    control.set("Voice Tx Mute", state, 0);
}

void s_set_btsco_rate(int rate)
{
    btsco_samplerate = rate;
}

}
