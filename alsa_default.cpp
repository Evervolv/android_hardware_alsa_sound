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
#define ALSA_DEVICE_LPA             "hw:0,4"
#define ALSA_DEVICE_FM_RADIO_PLAY   "hw:0,5"
#define ALSA_DEVICE_FM_RADIO_REC    "hw:0,6"
#define ALSA_DEVICE_HDMI            "hw:0,7"

#ifndef ALSA_DEFAULT_SAMPLE_RATE
#define ALSA_DEFAULT_SAMPLE_RATE 44100 // in Hz
#endif

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

namespace android
{

static int      s_device_open(const hw_module_t*, const char*, hw_device_t**);
static int      s_device_close(hw_device_t*);
static status_t s_init(alsa_device_t *, ALSAHandleList &);
static status_t s_open(alsa_handle_t *, uint32_t, int);
static status_t s_open_lpa(alsa_handle_t *, uint32_t, int);
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
    dev->open_lpa = s_open_lpa;
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

static void setAlsaControls(alsa_handle_t *handle, uint32_t devices, uint32_t mode);
static void resetAlsaControls(alsa_handle_t *handle);
static void resetRoutingControls(alsa_handle_t *handle);
static uint32_t getNewSoundDevice(uint32_t devices);

uint32_t curSoundDevice = 0;
bool     curSoundDeviceActive = false;
uint32_t numActiveUseCases = 0;

#define DEVICE_OUT_SCO      (\
        AudioSystem::DEVICE_OUT_BLUETOOTH_SCO |\
        AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET |\
        AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT)

#define DEVICE_OUT_DEFAULT   (\
        AudioSystem::DEVICE_OUT_ALL &\
        ~AudioSystem::DEVICE_OUT_FM)

#define DEVICE_IN_DEFAULT    (\
        AudioSystem::DEVICE_IN_ALL &\
        ~AudioSystem::DEVICE_IN_FM_RX &\
        ~AudioSystem::DEVICE_IN_FM_RX_A2DP &\
        ~AudioSystem::DEVICE_IN_VOICE_CALL &\
        ~AudioSystem::DEVICE_IN_COMMUNICATION &\
        ~DEVICE_IN_SCO)

static alsa_handle_t _defaults[] = {
/*
    Desriptions and expectations for how this module interprets
    the fields of the alsa_handle_t struct

        module      : pointer to a alsa_device_t struct
        devices     : mapping Android devices to Front end devices
        useCase     : playback/record/voice call/ .. etc
        curDev      : current Android device used by this handle
        curMode     : current Android mode used by this handle
        handle      : pointer to a snd_pcm_t ALSA handle
        format      : bit, endianess according to ALSA definitions
        channels    : Integer number of channels
        sampleRate  : Desired sample rate in Hz
        latency     : Desired Delay in usec for the ALSA buffer
        bufferSize  : Desired Number of samples for the ALSA buffer
        modPrivate  : pointer to the function specific to this handle
*/
    {
        module      : 0,
        devices     : 0,
        useCase     : ALSA_PLAYBACK,
        curDev      : 0,
        curMode     : 0,
        handle      : 0,
        format      : SNDRV_PCM_FORMAT_S16_LE,
        channels    : 2,
        sampleRate  : DEFAULT_SAMPLE_RATE,
        latency     : 96000,
        bufferSize  : 4096,
        modPrivate  : (void *)&setAlsaControls,
    },
    {
        module      : 0,
        devices     : 0,
        useCase     : ALSA_PLAYBACK_LPA,
        curDev      : 0,
        curMode     : 0,
        handle      : 0,
        format      : SNDRV_PCM_FORMAT_S16_LE,
        channels    : 2,
        sampleRate  : DEFAULT_SAMPLE_RATE,
        latency     : 85333,
        bufferSize  : 4096,
        modPrivate  : (void *)&setAlsaControls,
    },
    {
        module      : 0,
        devices     : 0,
        useCase     : ALSA_FM_RADIO,
        curDev      : 0,
        curMode     : 0,
        handle      : 0,
        format      : SNDRV_PCM_FORMAT_S16_LE,
        channels    : 2,
        sampleRate  : DEFAULT_SAMPLE_RATE,
        latency     : 85333,
        bufferSize  : 1024,
        modPrivate  : (void *)NULL,
    },
    {
        module      : 0,
        devices     : AudioSystem::DEVICE_OUT_EARPIECE,
        useCase     : ALSA_VOICE_CALL,
        curDev      : 0,
        curMode     : AudioSystem::MODE_NORMAL,
        handle      : 0,
        format      : SNDRV_PCM_FORMAT_S16_LE,
        channels    : 1,
        sampleRate  : 8000,
        latency     : 85333,
        bufferSize  : 4096,
        modPrivate  : (void *)NULL,
    },
    {
        module      : 0,
        devices     : 0,
        useCase     : ALSA_RECORD_FM,
        curDev      : 0,
        curMode     : 0,
        handle      : 0,
        format      : SNDRV_PCM_FORMAT_S16_LE,
        channels    : 1,
        sampleRate  : AudioRecord::DEFAULT_SAMPLE_RATE,
        latency     : 96000,
        bufferSize  : 4096,
        modPrivate  : (void *)&setAlsaControls,
    },
    {
        module      : 0,
        devices     : 0,
        useCase     : ALSA_RECORD_VOICE_CALL,
        curDev      : 0,
        curMode     : 0,
        handle      : 0,
        format      : SNDRV_PCM_FORMAT_S16_LE,
        channels    : 1,
        sampleRate  : AudioRecord::DEFAULT_SAMPLE_RATE,
        latency     : 96000,
        bufferSize  : 4096,
        modPrivate  : (void *)&setAlsaControls,
    },
    {
        module      : 0,
        devices     : 0,
        useCase     : ALSA_RECORD,
        curDev      : 0,
        curMode     : 0,
        handle      : 0,
        format      : SNDRV_PCM_FORMAT_S16_LE,
        channels    : 1,
        sampleRate  : AudioRecord::DEFAULT_SAMPLE_RATE,
        latency     : 96000,
        bufferSize  : 4096,
        modPrivate  : (void *)&setAlsaControls,
    },
};

// ----------------------------------------------------------------------------

const char *deviceName(uint32_t useCase)
{
    if(useCase == ALSA_PLAYBACK || useCase == ALSA_RECORD) {
        return ALSA_DEVICE_DEFAULT;
    } else if(useCase == ALSA_PLAYBACK_LPA) {
        return ALSA_DEVICE_LPA;
    } else if(useCase == ALSA_VOICE_CALL) {
        return ALSA_DEVICE_VOICE_CALL;
    } else {
        LOGE("Unknown use case # %d", useCase);
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

    //get the default array index
    for (size_t i = 0; i < ARRAY_SIZE(_defaults); i++) {
        LOGV("setHWParams: device %d bufferSize %d", _defaults[i].devices, _defaults[i].bufferSize);
        if (_defaults[i].devices == handle->devices) {
            reqBuffSize = _defaults[i].bufferSize;
            break;
        }
    }
    if(handle->useCase == ALSA_VOICE_CALL) {
        numPeriods = 2;
    }
    if(handle->useCase == ALSA_FM_RADIO) {
        numPeriods = 4;
        reqBuffSize = 1024;
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

uint32_t getNewSoundDevice(uint32_t devices)
{
    uint32_t newDevice = 0;
    if(devices & AudioSystem::DEVICE_OUT_SPEAKER) {
        newDevice |= SND_DEVICE_SPEAKER;
    }

    if(devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET ||
       devices & AudioSystem::DEVICE_IN_WIRED_HEADSET) {
        newDevice |= SND_DEVICE_HEADSET;
    } else if(devices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE) {
        newDevice |= SND_DEVICE_HEADPHONE;
    } else if(devices & AudioSystem::DEVICE_OUT_ANC_HEADSET ||
              devices & AudioSystem::DEVICE_IN_ANC_HEADSET) {
        newDevice |= SND_DEVICE_ANC_HEADSET;
    } else if(devices & AudioSystem::DEVICE_OUT_ANC_HEADPHONE) {
        newDevice |= SND_DEVICE_ANC_HEADPHONE;
    }
    if(devices == AudioSystem::DEVICE_OUT_BLUETOOTH_SCO ||
       devices == AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET ||
       devices == AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
        newDevice = SND_DEVICE_BT_SCO;
    }
    if(devices & AudioSystem::DEVICE_OUT_AUX_DIGITAL ||
       devices & AudioSystem::DEVICE_OUT_AUX_HDMI) {
        newDevice |= SND_DEVICE_HDMI;
    }
    if(devices & AudioSystem::DEVICE_OUT_EARPIECE ||
       devices & AudioSystem::DEVICE_IN_BUILTIN_MIC) {
        newDevice |= SND_DEVICE_HANDSET;
    }
    if(devices & AudioSystem::DEVICE_OUT_FM_TX) {
        newDevice |= SND_DEVICE_FM_TX;
    }

    if(devices & AudioSystem::DEVICE_IN_FM_RX ||
       devices & AudioSystem::DEVICE_IN_FM_RX_A2DP ||
       devices & AudioSystem::DEVICE_IN_VOICE_CALL) {
        newDevice = curSoundDevice;
    }
    if(!newDevice) {
        newDevice = curSoundDevice;
    }
    return newDevice;
}

uint32_t getCodecType(uint32_t soundDevice)
{
    uint32_t codecType = 0;
    if(soundDevice == SND_DEVICE_HANDSET || soundDevice == SND_DEVICE_HEADSET ||
       soundDevice == SND_DEVICE_HEADPHONE || soundDevice == SND_DEVICE_SPEAKER) {
        codecType |= CODEC_ICODEC;
    }
    if(soundDevice == SND_DEVICE_HDMI) {
        codecType |= CODEC_HDMI;
    }
    if(soundDevice == SND_DEVICE_FM_TX || soundDevice == SND_DEVICE_BT_SCO) {
        codecType |= CODEC_RIVA;
    }
    return codecType;
}

void setAlsaControls(alsa_handle_t *handle, uint32_t devices, uint32_t mode)
{
    LOGV("%s: E - handle %p useCase %d device %d", __FUNCTION__, handle, handle->useCase, devices);

    ALSAControl control("/dev/snd/controlC0");

    uint32_t newSoundDevice = getNewSoundDevice(devices);
    uint32_t codecType = getCodecType(newSoundDevice);

    if(handle->useCase == ALSA_VOICE_CALL) {
        if(codecType == CODEC_ICODEC) {
           LOGV("Enabling Mixer controls for voice call");
           control.set("SLIM_0_RX_Voice Mixer CSVoice", 1, 0);
           control.set("Voice_Tx Mixer SLIM_0_TX_Voice", 1, 0);
        } else if (codecType == CODEC_RIVA) {
           LOGV("Enabling Mixer controls for voice call over BT");
           control.set("INTERNAL_BT_SCO_RX_Voice Mixer CSVoice",1,0);
           control.set("Voice_Tx Mixer INTERNAL_BT_SCO_TX_Voice", 1, 0);
        }
    }

    if(handle->useCase == ALSA_FM_RADIO) {
        LOGV("Enabling Mixer controls for FM Raadio");
        control.set("SLIMBUS_0_RX Port Mixer INTERNAL_FM_TX", 1, 0);
    }

    if(handle->useCase == ALSA_PLAYBACK_LPA) {
        if (codecType & CODEC_ICODEC) {
            control.set("SLIMBUS_0_RX Audio Mixer MultiMedia3", 1, 0);
        }
        if (codecType & CODEC_HDMI) {
            //TODO
        }
        if (codecType & CODEC_RIVA) {
            //TODO
        }

    }


    if(handle->useCase == ALSA_PLAYBACK || handle->useCase == ALSA_RECORD) {
        if(codecType & CODEC_ICODEC) {
            control.set("SLIMBUS_0_RX Audio Mixer MultiMedia1", 1, 0);
            control.set("MultiMedia1 Mixer SLIM_0_TX", 1, 0);
        }
        if (codecType & CODEC_HDMI) {
            control.set("HDMI Mixer MultiMedia1", 1, 0);
        }
        if (codecType & CODEC_RIVA) {
           control.set("INTERNAL_BT_SCO_RX Audio Mixer MultiMedia1", 1, 0);
           control.set("MultiMedia1 Mixer INTERNAL_BT_SCO_TX", 1, 0);
        }
    }

    LOGV("setAlsaControls: curDevice %d active %d newDevice %d ",
         curSoundDevice, curSoundDeviceActive, newSoundDevice);
    if(!newSoundDevice || (curSoundDevice == newSoundDevice && curSoundDeviceActive)) {
        // New device is same as current active sound device.
        return;
    }

    curSoundDevice = newSoundDevice;
    // Enable new sound device
    if(newSoundDevice != 0) {
        curSoundDeviceActive = true;
        if (newSoundDevice & SND_DEVICE_SPEAKER) {
            LOGV("Enabling SPEAKER_RX");
            control.set("RX3 MIX1 INP1", "RX1");
            control.set("RX4 MIX1 INP1", "RX2");
            control.set("LINEOUT1 DAC Switch", 1, 0);
            control.set("LINEOUT3 DAC Switch", 1, 0);
            control.set("Speaker Function", "On");
            control.set("LINEOUT1 Volume", "100");
            control.set("LINEOUT3 Volume", "100");

            LOGV("Enabling SPEAKER_TX");
            control.set("SLIM TX1 MUX", "DEC1");
            control.set("DEC1 MUX", "DMIC1");
        }
        if (newSoundDevice & SND_DEVICE_HEADSET ||
            newSoundDevice & SND_DEVICE_HEADPHONE) {
            LOGV("Enabling HEADSET_RX");
            control.set("RX1 MIX1 INP1", "RX1");
            control.set("RX2 MIX1 INP1", "RX2");
            control.set("HPHL DAC Switch", 1, 0);
            control.set("HPHR DAC Switch", 1, 0);
            control.set("HPHL Volume", "100");
            control.set("HPHR Volume", "100");
            if(newSoundDevice & SND_DEVICE_HEADSET) {
                LOGV("Enabling HEADSET_TX");
                control.set("SLIM TX7 MUX", "DEC5");
                control.set("DEC5 MUX", "ADC2");
                control.set("DEC5 Volume", "0");
            } else {
                LOGV("Enabling HANDSET_TX");
                control.set("SLIM TX7 MUX", "DEC6");
                control.set("DEC6 Volume", "0");
                control.set("DEC6 MUX", "ADC1");
            }
        }
        if (newSoundDevice & SND_DEVICE_HANDSET) {
            LOGV("Enabling HANDSET_RX");
            control.set("RX1 MIX1 INP1", "RX1");
            control.set("DAC1 Switch", 1, 0);
            control.set("RX1 Digital Volume", "100");

            LOGV("Enabling HANDSET_TX");
            control.set("SLIM TX7 MUX", "DEC6");
            control.set("DEC6 Volume", "0");
            control.set("DEC6 MUX", "ADC1");
        }
    }

    handle->curDev = devices;
    handle->curMode = mode;
    LOGV("setAlsaControls: X - handle %p curDev %d curMode %d",
         handle, handle->curDev, handle->curMode);
}

void resetRoutingControls(alsa_handle_t *handle)
{
    LOGV("%s: handle %p useCase %d", __FUNCTION__, handle, handle->useCase);

    ALSAControl control("/dev/snd/controlC0");
    uint32_t codecType = getCodecType(curSoundDevice);

    if(handle->useCase == ALSA_VOICE_CALL) {
        if(codecType == CODEC_ICODEC) {
           LOGV("Disabling Mixer controls for voice call");
           control.set("SLIM_0_RX_Voice Mixer CSVoice", 0, 0);
           control.set("Voice_Tx Mixer SLIM_0_TX_Voice", 0, 0);
        } else if(codecType == CODEC_RIVA) {
           LOGV("Disabling Mixer controls for voice call over BT");
           control.set("INTERNAL_BT_SCO_RX_Voice Mixer CSVoice",0,0);
           control.set("Voice_Tx Mixer INTERNAL_BT_SCO_TX_Voice", 0, 0);
        }
    }

    if(handle->useCase == ALSA_FM_RADIO) {
        LOGV("Disabling Mixer controls for FM Radio");
        control.set("SLIMBUS_0_RX Port Mixer INTERNAL_FM_TX", 0, 0);
    }

    if(handle->useCase == ALSA_PLAYBACK_LPA) {
        if (codecType & CODEC_ICODEC) {
            control.set("SLIMBUS_0_RX Audio Mixer MultiMedia3", 0, 0);
        }
        if (codecType & CODEC_HDMI) {
            //TODO
        }
        if (codecType & CODEC_RIVA) {
            //TODO
        }
    }

    if(handle->useCase == ALSA_PLAYBACK || handle->useCase == ALSA_RECORD) {
        if(codecType & CODEC_ICODEC) {
            control.set("SLIMBUS_0_RX Audio Mixer MultiMedia1", 0, 0);
            control.set("MultiMedia1 Mixer SLIM_0_TX", 0, 0);
        }
        if (codecType & CODEC_HDMI) {
            control.set("HDMI Mixer MultiMedia1", 0, 0);
        }
        if (codecType & CODEC_RIVA) {
            control.set("INTERNAL_BT_SCO_RX Audio Mixer MultiMedia1", 0, 0);
            control.set("MultiMedia1 Mixer INTERNAL_BT_SCO_TX", 0, 0);
        }
    }
}

void resetAlsaControls(alsa_handle_t *handle)
{
    LOGV("%s: E - handle %p useCase %d", __FUNCTION__, handle, handle->useCase);

    ALSAControl control("/dev/snd/controlC0");

    LOGV("resetAlsaControls: curDevice %d active %d", curSoundDevice, curSoundDeviceActive);

    // Disable the current RX device
    if(curSoundDevice != 0 && curSoundDeviceActive) {
        curSoundDeviceActive = false;
        if (curSoundDevice & SND_DEVICE_SPEAKER) {
            LOGV("Disabling SPEAKER_RX");
            control.set("RX3 MIX1 INP1", "ZERO");
            control.set("RX4 MIX1 INP1", "ZERO");
            control.set("LINEOUT1 DAC Switch", 0, 0);
            control.set("LINEOUT3 DAC Switch", 0, 0);
            control.set("Speaker Function", "Off");

            LOGV("Disabling SPEAKER_TX");
            control.set("SLIM TX1 MUX", "ZERO");
            control.set("DEC1 MUX", "ZERO");
        }
        // ToDo: ANC headset/headphone/DMIC/TTY ??
        // ToDo: We should not disable ANC Headset/Headphone even if there is no
        // active use case.
        if (curSoundDevice & SND_DEVICE_HEADSET ||
            curSoundDevice & SND_DEVICE_HEADPHONE) {
            LOGV("Disabling HEADSET_RX");
            control.set("RX1 MIX1 INP1", "ZERO");
            control.set("RX2 MIX1 INP1", "ZERO");
            control.set("HPHL DAC Switch", 0, 0);
            control.set("HPHR DAC Switch", 0, 0);

            if(curSoundDevice & SND_DEVICE_HEADSET) {
                LOGV("Disabling HEADSET_TX");
                control.set("SLIM TX7 MUX", "ZERO");
                control.set("DEC5 MUX", "ZERO");
            } else {
                LOGV("Disabling HANDSET_TX");
                control.set("SLIM TX7 MUX", "ZERO");
                control.set("DEC6 MUX", "ZERO");
            }
        }

        if (curSoundDevice & SND_DEVICE_HANDSET) {
            LOGV("Disabling HANDSET_RX");
            control.set("RX1 MIX1 INP1", "ZERO");
            control.set("DAC1 Switch", 0, 0);

            LOGV("Disabling HANDSET_TX");
            control.set("SLIM TX7 MUX", "ZERO");
            control.set("DEC6 MUX", "ZERO");
        }
    }

    LOGV("resetAlsaControls: X - handle %p curDev %d curMode %d", handle, handle->curDev, handle->curMode);
}

// ----------------------------------------------------------------------------

static status_t s_init(alsa_device_t *module, ALSAHandleList &list)
{
    LOGV("s_init: Initializing devices for ALSA module");

    list.clear();

    for (size_t i = 0; i < ARRAY_SIZE(_defaults); i++) {

        unsigned long bufferSize = _defaults[i].bufferSize;

        for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
            bufferSize &= ~b;

        _defaults[i].module = module;
        _defaults[i].bufferSize = bufferSize;

        list.push_back(_defaults[i]);
    }

    return NO_ERROR;
}

static status_t s_open_lpa(alsa_handle_t *handle, uint32_t devices, int mode) {

    LOGV("Opening LPA playback s_open_lpa");
    LOGE("s_open_lpa calling close");
    s_close(handle);
    setAlsaControls(handle, devices, mode);

    return NO_ERROR;
}

static status_t s_open(alsa_handle_t *handle, uint32_t devices, int mode)
{
    unsigned flags = 0;
    int err = NO_ERROR;

    s_close(handle);

    LOGV("s_open: handle %p devices 0x%x in mode %d", handle, devices, mode);

    const char *devName = deviceName(handle->useCase);

    // ASoC multicomponent requires a valid path (frontend/backend) for
    // the device to be opened
    setAlsaControls(handle, devices, mode);

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

    // Increment bnumber of  active useCases on every successful PCM device open
    numActiveUseCases ++;

    handle->handle->flags = flags;
    err = setHardwareParams(handle);

    if (err == NO_ERROR) {
        err = setSoftwareParams(handle);
    }

    if(err != NO_ERROR) {
        LOGE("Set HW/SW params failed: Closing the pcm stream");
        s_standby(handle);
    }

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
    setAlsaControls(handle, devices, mode);

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

    // Store the PCM playback device pointer in modPrivate
    handle->modPrivate = (void*)handle->handle;

    // Open PCM capture device
    flags = PCM_IN | PCM_MONO;
    handle->handle = pcm_open(flags, (char*)devName);
    if (!handle->handle) {
        goto Error;
    }

    numActiveUseCases ++;

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
    setAlsaControls(handle, devices, mode);

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

    // Store the PCM playback device pointer in modPrivate
    handle->modPrivate = (void*)handle->handle;

    // Open PCM capture device
    flags = PCM_IN | PCM_STEREO;
    handle->handle = pcm_open(flags, (char*)ALSA_DEVICE_FM_RADIO_REC);
    if (!handle->handle) {
        goto Error;
    }

    numActiveUseCases ++;

    handle->handle->flags = flags;
    err = setHardwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("s_start_fm: setHardwareParams failed");
        goto Error;
    }

    err = setSoftwareParams(handle);
    if(err != NO_ERROR) {
        LOGE("s_start_voice_call: setSoftwareParams failed");
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
    status_t err = NO_ERROR;
    struct pcm *h = handle->handle;

    handle->handle = 0;
    handle->curDev = 0;
    handle->curMode = 0;
    LOGV("s_close: handle %p", handle);

    if (h) {
        numActiveUseCases --;
        err = pcm_close(h);
        if(err != NO_ERROR) {
            LOGE("s_close: pcm_close failed with err %d", err);
        }

        if((handle->useCase == ALSA_VOICE_CALL || handle->useCase == ALSA_FM_RADIO) &&
           handle->modPrivate != NULL) {
            err = pcm_close((struct pcm*)handle->modPrivate);
            if(err != NO_ERROR) {
                LOGE("s_close: pcm_close failed with err %d", err);
            }
            handle->modPrivate = NULL;
        }

        resetRoutingControls(handle);
        // If there are no other active use cases, disable the device
        if(!numActiveUseCases) {
            resetAlsaControls(handle);
        }
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
    status_t err = NO_ERROR;
    struct pcm *h = handle->handle;
    handle->handle = 0;
    LOGV("s_standby\n");
    if (h) {
        numActiveUseCases --;
        err = pcm_close(h);
        if(err != NO_ERROR) {
            LOGE("s_standby: pcm_close failed with err %d", err);
        }
        resetRoutingControls(handle);
        // If there are no other active use cases, disable the device
        if(!numActiveUseCases) {
            resetAlsaControls(handle);
        }
    }

    return err;
}

static status_t s_route(alsa_handle_t *handle, uint32_t devices, int mode)
{
    status_t status = NO_ERROR;

    LOGV("s_route: devices 0x%x in mode %d useCase %d", devices, mode, handle->useCase);
    if (handle->handle) {
        resetRoutingControls(handle);
        resetAlsaControls(handle);
        setAlsaControls(handle,devices,mode);
    } else {
        handle->curDev = devices;
        handle->curMode = mode;
    }

    return status;
}

}
