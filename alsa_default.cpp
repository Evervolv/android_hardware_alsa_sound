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

#define ALSA_DEVICE_DEFAULT         "hw:0,0"
#define ALSA_DEVICE_VOICE_CALL      "hw:0,2"
#define ALSA_DEVICE_FM_RADIO_PLAY   "hw:0,5"
#define ALSA_DEVICE_FM_RADIO_REC    "hw:0,6"
#define ALSA_DEVICE_LPA             "hw:0,4"
#define ALSA_DEVICE_HDMI            "hw:0,7"

#ifndef ALSA_DEFAULT_SAMPLE_RATE
#define ALSA_DEFAULT_SAMPLE_RATE 44100 // in Hz
#endif

namespace android
{

static int      s_device_open(const hw_module_t*, const char*, hw_device_t**);
static int      s_device_close(hw_device_t*);
static status_t s_init(alsa_device_t *, ALSAHandleList &);
static status_t s_open(alsa_handle_t *, uint32_t, int);
static status_t s_close(alsa_handle_t *);
static status_t s_standby(alsa_handle_t *);
static status_t s_route(alsa_handle_t *, uint32_t, int);
static status_t s_start_voice_call(alsa_handle_t *, uint32_t, int);
static status_t s_start_fm(alsa_handle_t *, uint32_t, int);

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

    *device = &dev->common;

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
static char *getUCMDevice(uint32_t devices);
static void disableDevice(alsa_handle_t *handle);

uint32_t curTxSoundDevice = 0;
uint32_t curRxSoundDevice = 0;

// ----------------------------------------------------------------------------

const char *deviceName(char *useCase)
{
    if ((!strcmp(useCase, SND_USE_CASE_VERB_HIFI)) ||
        (!strcmp(useCase, SND_USE_CASE_VERB_HIFI_REC)) ||
        (!strcmp(useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC)) ||
        (!strcmp(useCase, SND_USE_CASE_MOD_PLAY_MUSIC))) {
        return ALSA_DEVICE_DEFAULT;
    } else if (!strcmp(useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER)) {
        return ALSA_DEVICE_LPA;
    } else if ((!strcmp(useCase, SND_USE_CASE_VERB_VOICECALL)) ||
               (!strcmp(useCase, SND_USE_CASE_MOD_PLAY_VOICE)) ||
               (!strcmp(useCase, SND_USE_CASE_MOD_CAPTURE_VOICE))) {
        return ALSA_DEVICE_VOICE_CALL;
    } else {
        LOGE("Unknown use case # %s", useCase);
    }
    return NULL;
}

status_t setHardwareParams(alsa_handle_t *handle)
{
    struct snd_pcm_hw_params *params;
    unsigned long periodSize, bufferSize, reqBuffSize;
    unsigned int periodTime, bufferTime;
    unsigned int requestedRate = handle->sampleRate;
    int numPeriods = 8;
    int status = 0;

    const char* device = deviceName(handle->useCase);

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
    char *device, *curdev;
    LOGV("%s: device %d handle->curDev %d", __FUNCTION__, devices, handle->curDev);

    if ((!strcmp(handle->useCase, SND_USE_CASE_VERB_VOICECALL)) ||
        (!strcmp(handle->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC)) ||
        (!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_VOICE))) {
        if ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
            (devices & AudioSystem::DEVICE_IN_WIRED_HEADSET)) {
                 devices = devices | (AudioSystem::DEVICE_OUT_WIRED_HEADSET |
                           AudioSystem::DEVICE_IN_WIRED_HEADSET);
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
        }
    }

    if ((curRxSoundDevice != (devices & AudioSystem::DEVICE_OUT_ALL)) ||
        (curRxSoundDevice != (handle->curDev & AudioSystem::DEVICE_OUT_ALL)))  {
        device = getUCMDevice(devices & AudioSystem::DEVICE_OUT_ALL);
        if (device != NULL) {
            if (curRxSoundDevice != 0) {
                curdev = getUCMDevice(curRxSoundDevice);
                if (!strcmp(device, curdev)) {
                    LOGV("Required device is already set, ignoring device enable");
                    snd_use_case_set(handle->uc_mgr, "_enadev", device);
                } else {
                    char *ident = (char *)malloc((strlen(curdev)+strlen("_swdev/")+1)*sizeof(char));
                    strcpy(ident, "_swdev/");
                    strcat(ident, curdev);
                    snd_use_case_set(handle->uc_mgr, ident, device);
                    free(ident);
                }
                free(curdev);
            } else {
                snd_use_case_set(handle->uc_mgr, "_enadev", device);
            }
            curRxSoundDevice = (devices & AudioSystem::DEVICE_OUT_ALL);
            free(device);
        } else if (device == 0 && handle->curDev == 0) {
            LOGV("No valid output device, enabling current Rx device");
            if (curRxSoundDevice != 0) {
                curdev = getUCMDevice(curRxSoundDevice);
                snd_use_case_set(handle->uc_mgr, "_enadev", curdev);
                free(curdev);
            }
        }
    }
    if ((curTxSoundDevice != (devices & AudioSystem::DEVICE_IN_ALL)) ||
        (curTxSoundDevice != (handle->curDev & AudioSystem::DEVICE_IN_ALL))) {
        device = getUCMDevice(devices & AudioSystem::DEVICE_IN_ALL);
        if (device != NULL) {
            if (curTxSoundDevice != 0) {
                curdev = getUCMDevice(curTxSoundDevice);
                if (!strcmp(device, curdev)) {
                    LOGV("Required device is already set, ignoring device enable");
                    snd_use_case_set(handle->uc_mgr, "_enadev", device);
                } else {
                    char *ident = (char *)malloc((strlen(curdev)+strlen("_swdev/")+1)*sizeof(char));
                    strcpy(ident, "_swdev/");
                    strcat(ident, curdev);
                    snd_use_case_set(handle->uc_mgr, ident, device);
                    free(ident);
                }
                free(curdev);
            } else {
                snd_use_case_set(handle->uc_mgr, "_enadev", device);
            }
            curTxSoundDevice = (devices & AudioSystem::DEVICE_IN_ALL);
            free(device);
        } else if (device == 0 && handle->curDev == 0) {
            LOGV("No valid output device, enabling current Tx device");
            if (curTxSoundDevice != 0) {
                curdev = getUCMDevice(curTxSoundDevice);
                snd_use_case_set(handle->uc_mgr, "_enadev", curdev);
                free(curdev);
            }
        }
    }
    handle->curDev = (curTxSoundDevice | curRxSoundDevice);
    handle->curMode = mode;
    LOGV("switchDevice: curTxDev %d curRxDev %d", curTxSoundDevice, curRxSoundDevice);
}

// ----------------------------------------------------------------------------

static status_t s_init(alsa_device_t *module, ALSAHandleList &list)
{
    LOGV("s_init: Initializing devices for ALSA module");

    list.clear();

    return NO_ERROR;
}

static status_t s_open(alsa_handle_t *handle, uint32_t devices, int mode)
{
    unsigned flags = 0;
    int err = NO_ERROR;

    s_close(handle);

    if((!strcmp(handle->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER)) || (!strcmp(handle->useCase, SND_USE_CASE_MOD_PLAY_LPA))) {
        LOGV("s_open: Opening LPA playback");
        return NO_ERROR;
    }

    LOGV("s_open: handle %p devices 0x%x in mode %d", handle, devices, mode);

    const char *devName = deviceName(handle->useCase);

    // ASoC multicomponent requires a valid path (frontend/backend) for
    // the device to be opened

    // The PCM stream is opened in blocking mode, per ALSA defaults.  The
    // AudioFlinger seems to assume blocking mode too, so asynchronous mode
    // should not be used.
    if(devices & AudioSystem::DEVICE_OUT_ALL) {
        flags = PCM_OUT;
    } else {
        flags = PCM_IN;
    }
    if (handle->channels == 1) {
        flags |= PCM_MONO;
    } else {
        flags |= PCM_STEREO;
    }
    handle->handle = pcm_open(flags, (char*)devName);

    if (!handle->handle) {
        LOGE("s_open: Failed to initialize ALSA device '%s'", devName);
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

    handle->curDev = devices;
    handle->curMode = mode;
    return NO_ERROR;
}

static status_t s_start_voice_call(alsa_handle_t *handle, uint32_t devices, int mode)
{
    unsigned flags = 0;
    int err = NO_ERROR;

    LOGV("s_start_voice_call: handle %p devices 0x%x in mode %d", handle, devices, mode);
    const char *devName = deviceName(handle->useCase);

    // ASoC multicomponent requires a valid path (frontend/backend) for
    // the device to be opened

    flags = PCM_OUT | PCM_MONO;
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

    // Open PCM capture device
    flags = PCM_IN | PCM_MONO;
    handle->handle = pcm_open(flags, (char*)devName);
    if (!handle->handle) {
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

    handle->curDev = devices;
    handle->curMode = mode;

    return NO_ERROR;

Error:
    LOGE("s_start_voice_call: Failed to initialize ALSA device '%s'", devName);
    s_close(handle);
    return NO_INIT;
}

static status_t s_start_fm(alsa_handle_t *handle, uint32_t devices, int mode)
{
    unsigned flags = 0;
    int err = NO_ERROR;

    LOGV("s_start_fm: handle %p devices 0x%x in mode %d", handle, devices, mode);

    // ASoC multicomponent requires a valid path (frontend/backend) for
    // the device to be opened

    flags = PCM_OUT | PCM_STEREO;
    handle->handle = pcm_open(flags, (char*)ALSA_DEVICE_FM_RADIO_PLAY);
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

    // Open PCM capture device
    flags = PCM_IN | PCM_STEREO;
    handle->handle = pcm_open(flags, (char*)ALSA_DEVICE_FM_RADIO_REC);
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

    handle->curDev = devices;
    handle->curMode = mode;

    return NO_ERROR;

Error:
    s_close(handle);
    return NO_INIT;
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

static status_t s_route(alsa_handle_t *handle, uint32_t devices, int mode)
{
    status_t status = NO_ERROR;

    LOGV("s_route: devices 0x%x in mode %d", devices, mode);
    switchDevice(handle, devices, mode);
    handle->curDev = (curTxSoundDevice | curRxSoundDevice);
    handle->curMode = mode;
    return status;
}

static void disableDevice(alsa_handle_t *handle)
{
    char *curDev;

    snd_use_case_get(handle->uc_mgr, "_verb", (const char **)&curDev);
    if (curDev != NULL) {
        if (!strcmp(curDev, handle->useCase)) {
            snd_use_case_set(handle->uc_mgr, "_verb", SND_USE_CASE_VERB_INACTIVE);
        } else {
            snd_use_case_set(handle->uc_mgr, "_dismod", handle->useCase);
        }
        handle->useCase[0] = 0;
    } else {
        LOGE("Invalid state, no valid use case found to disable");
    }
    free(curDev);
    curDev = getUCMDevice(handle->curDev & AudioSystem::DEVICE_OUT_ALL);
    if (curDev != NULL)
        snd_use_case_set(handle->uc_mgr, "_disdev", curDev);
    free(curDev);
    curDev = getUCMDevice(handle->curDev & AudioSystem::DEVICE_IN_ALL);
    if (curDev != NULL)
        snd_use_case_set(handle->uc_mgr, "_disdev", curDev);
    free(curDev);
    handle->curDev = 0;
}

char *getUCMDevice(uint32_t devices)
{
    if ((devices & AudioSystem::DEVICE_OUT_SPEAKER) &&
        ((devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) ||
         (devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE))) {
        return strdup(SND_USE_CASE_DEV_SPEAKER_HEADSET); /* COMBO SPEAKER+HEADSET RX */
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
        return strdup(SND_USE_CASE_DEV_BTSCO_RX); /* BTSCO RX*/
    } else if ((devices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP) ||
               (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES) ||
               (devices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER)) {
        /* Nothing to be done, use current active device */
        return (getUCMDevice(curRxSoundDevice));
    } else if ((devices & AudioSystem::DEVICE_OUT_AUX_DIGITAL) ||
               (devices & AudioSystem::DEVICE_OUT_AUX_HDMI)) {
        return strdup(SND_USE_CASE_DEV_HDMI); /* HDMI RX */
    } else if (devices & AudioSystem::DEVICE_OUT_FM_TX) {
        return strdup(SND_USE_CASE_DEV_FM_TX); /* FM Tx */
    } else if (devices & AudioSystem::DEVICE_OUT_DEFAULT) {
        return strdup(SND_USE_CASE_DEV_SPEAKER); /* SPEAKER RX */
    } else {
        LOGV("No valid output device: %u", devices);
    }

    if (devices & AudioSystem::DEVICE_IN_BUILTIN_MIC) {
        /* TODO: Check if DMIC is enabled and return the
         *       required UCM device name for Speaker DMIC
         */
        return strdup(SND_USE_CASE_DEV_HANDSET); /* HANDSET TX */
    } else if ((devices & AudioSystem::DEVICE_IN_WIRED_HEADSET) ||
               (devices & AudioSystem::DEVICE_IN_ANC_HEADSET)) {
        return strdup(SND_USE_CASE_DEV_HEADSET); /* HEADSET TX */
    } else if (devices & AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
        return strdup(SND_USE_CASE_DEV_BTSCO_TX); /* BTSCO TX*/
    } else if (devices & AudioSystem::DEVICE_IN_DEFAULT) {
        /* TODO: Check if DMIC is enabled and return the
         *       required UCM device name for Handset DMIC
         */
        return strdup(SND_USE_CASE_DEV_LINE); /* BUILTIN-MIC TX */
    } else if ((devices & AudioSystem::DEVICE_IN_FM_RX) ||
               (devices & AudioSystem::DEVICE_IN_FM_RX_A2DP) ||
               (devices & AudioSystem::DEVICE_IN_VOICE_CALL)) {
        /* Nothing to be done, use current active device */
        return (getUCMDevice(curTxSoundDevice));
    } else if ((devices & AudioSystem::DEVICE_IN_COMMUNICATION) ||
               (devices & AudioSystem::DEVICE_IN_AMBIENT) ||
               (devices & AudioSystem::DEVICE_IN_BACK_MIC) ||
               (devices & AudioSystem::DEVICE_IN_AUX_DIGITAL)) {
        LOGI("No proper mapping found with UCM device list, setting default");
        /* TODO: Check if DMIC is enabled and return the
         *       required UCM device name for Handset DMIC
         */
        return strdup(SND_USE_CASE_DEV_HANDSET); /* HANDSET TX */
    } else {
        LOGV("No valid input device: %u", devices);
    }
    return NULL;
}

}
