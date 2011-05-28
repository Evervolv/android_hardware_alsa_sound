/* alsa_default.cpp
 **
 ** Copyright 2009 Wind River Systems
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

#include "AudioHardwareALSA.h"
#include <media/AudioRecord.h>

#undef DISABLE_HARWARE_RESAMPLING

#define ALSA_NAME_MAX 128

#define ALSA_STRCAT(x,y) \
    if (strlen(x) + strlen(y) < ALSA_NAME_MAX) \
        strcat(x, y);

#define MM_DEFAULT_DEVICE    "hw:0,0"
#define BLUETOOTH_SCO_DEVICE "hw:0,0"
#define FM_TRANSMIT_DEVICE   "hw:0,0"
#define FM_CAPTURE_DEVICE    "hw:0,1"
#define MM_LP_DEVICE         "hw:0,6"
#define HDMI_DEVICE          "hw:0,7"

#ifndef ALSA_DEFAULT_SAMPLE_RATE
#define ALSA_DEFAULT_SAMPLE_RATE 44100 // in Hz
#endif

#ifndef MM_LP_SAMPLE_RATE
//not used for now
#define MM_LP_SAMPLE_RATE 44100        // in Hz
#endif

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
static bool fm_enable = false;
static bool mActive = false;

namespace android
{

static int s_device_open(const hw_module_t*, const char*, hw_device_t**);
static int s_device_close(hw_device_t*);
static status_t s_init(alsa_device_t *, ALSAHandleList &);
static status_t s_open(alsa_handle_t *, uint32_t, int);
static status_t s_close(alsa_handle_t *);
static status_t s_route(alsa_handle_t *, uint32_t, int);

static hw_module_methods_t s_module_methods = {
    open            : s_device_open
};

extern "C" const hw_module_t HAL_MODULE_INFO_SYM = {
    tag             : HARDWARE_MODULE_TAG,
    version_major   : 1,
    version_minor   : 0,
    id              : ALSA_HARDWARE_MODULE_ID,
    name            : "ALSA module",
    author          : "Wind River",
    methods         : &s_module_methods,
    dso             : 0,
    reserved        : { 0, },
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

static const char *devicePrefix[SND_PCM_STREAM_LAST + 1] = {
        /* SND_PCM_STREAM_PLAYBACK : */"AndroidPlayback",
        /* SND_PCM_STREAM_CAPTURE  : */"AndroidCapture",
};

static void setAlsaControls(alsa_handle_t *handle, uint32_t devices, int mode);

#define DEVICE_OUT_SCO      (\
        AudioSystem::DEVICE_OUT_BLUETOOTH_SCO |\
        AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET |\
        AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT)

#define DEVICE_OUT_HDMI        (\
        AudioSystem::DEVICE_OUT_AUX_DIGITAL)

#define DEVICE_OUT_DEFAULT   (\
        AudioSystem::DEVICE_OUT_ALL &\
        ~DEVICE_OUT_SCO &\
        ~DEVICE_OUT_HDMI)

#define DEVICE_IN_SCO        (\
        AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET)


#define DEVICE_IN_DEFAULT    (\
        AudioSystem::DEVICE_IN_ALL &\
        ~DEVICE_IN_SCO)

static alsa_handle_t _defaults[] = {
/*
    Desriptions and expectations for how this module interprets
    the fields of the alsa_handle_t struct

        module      : pointer to a alsa_device_t struct
        devices     : mapping Android devices to Front end devices
        curDev      : current Android device used by this handle
        curMode     : current Android mode used by this handle
        handle      : pointer to a snd_pcm_t ALSA handle
        format      : bit, endianess according to ALSA definitions
        channels    : Integer number of channels
        sampleRate  : Desired sample rate in Hz
        latency     : Desired Delay in usec for the ALSA buffer
        bufferSize  : Desired Number of samples for the ALSA buffer
        mmap        : true (1) to use mmap, false (0) to use standard writei
        modPrivate  : pointer to the function specific to this handle
*/
    {
        module      : 0,
        devices     : DEVICE_OUT_SCO,
        curDev      : 0,
        curMode     : 0,
        handle      : 0,
        format      : SNDRV_PCM_FORMAT_S16_LE,
        channels    : 2,
        sampleRate  : DEFAULT_SAMPLE_RATE,
        latency     : 200000,
        bufferSize  : DEFAULT_SAMPLE_RATE / 5,
        modPrivate  : (void *)&setAlsaControls,
    },
    {
        module      : 0,
        devices     : DEVICE_OUT_HDMI,
        curDev      : 0,
        curMode     : 0,
        handle      : 0,
        format      : SNDRV_PCM_FORMAT_S16_LE,
        channels    : 2,
        sampleRate  : DEFAULT_SAMPLE_RATE,
        latency     : 200000,
        bufferSize  : DEFAULT_SAMPLE_RATE / 5,
        modPrivate  : (void *)&setAlsaControls,
    },
    {
        module      : 0,
        devices     : DEVICE_OUT_DEFAULT,
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
        devices     : DEVICE_IN_SCO,
        curDev      : 0,
        curMode     : 0,
        handle      : 0,
        format      : SNDRV_PCM_FORMAT_S16_LE,
        channels    : 1,
        sampleRate  : AudioRecord::DEFAULT_SAMPLE_RATE,
        latency     : 250000,
        bufferSize  : 2048,
        modPrivate  : (void *)&setAlsaControls,
    },
    {
        module      : 0,
        devices     : DEVICE_IN_DEFAULT,
        curDev      : 0,
        curMode     : 0,
        handle      : 0,
        format      : SNDRV_PCM_FORMAT_S16_LE,
        channels    : 1,
        sampleRate  : AudioRecord::DEFAULT_SAMPLE_RATE,
        latency     : 250000,
        bufferSize  : 4096,
        modPrivate  : (void *)&setAlsaControls,
    },
};


struct device_suffix_t {
    const AudioSystem::audio_devices device;
    const char *suffix;
};

/* The following table(s) need to match in order of the route bits
 */
static const device_suffix_t deviceSuffix[] = {
        {AudioSystem::DEVICE_OUT_EARPIECE,       "_Earpiece"},
        {AudioSystem::DEVICE_OUT_SPEAKER,        "_Speaker"},
        {AudioSystem::DEVICE_OUT_BLUETOOTH_SCO,  "_Bluetooth"},
        {AudioSystem::DEVICE_OUT_WIRED_HEADSET,  "_Headset"},
        {AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP, "_Bluetooth-A2DP"},
};

static const int deviceSuffixLen = (sizeof(deviceSuffix)
        / sizeof(device_suffix_t));

// ----------------------------------------------------------------------------

const char *deviceName(alsa_handle_t *handle, uint32_t device, int mode)
{
    // ToDo: To decide based on actual device
    return MM_DEFAULT_DEVICE;
}

enum snd_pcm_stream_t direction(alsa_handle_t *handle)
{
    LOGV("direction: handle->devices 0x%08x", handle->devices);
    return (handle->devices & AudioSystem::DEVICE_OUT_ALL) ? SND_PCM_STREAM_PLAYBACK
            : SND_PCM_STREAM_CAPTURE;
}


const char *streamName(alsa_handle_t *handle)
{
    return direction(handle)? "Playback" : "Capture";
}

status_t setHardwareParams(alsa_handle_t *handle)
{
    struct snd_pcm_hw_params *params;
    status_t err = -1;

    unsigned long periodSize, bufferSize, reqBuffSize;
    unsigned int periodTime, bufferTime;
    unsigned int requestedRate = handle->sampleRate;
    int numPeriods = 0;
    int status = 0;

    // snd_pcm_format_description() and snd_pcm_format_name() do not perform
    // proper bounds checking.
    bool validFormat = (static_cast<int> (handle->format)
            <= SNDRV_PCM_FORMAT_LAST);
    const char *formatDesc = validFormat ? "Signed 16 bit Little Endian"
            : "Invalid Format";
    const char *formatName = validFormat ? "SND_PCM_FORMAT_S16_LE"
            : "UNKNOWN";

    // device name will only return LP device hw06 if the property is set
    // or if the system is explicitly opening and routing to OMAP4_OUT_LP
    const char* device = deviceName(handle,
                                    handle->devices,
                                    AudioSystem::MODE_NORMAL);

    params = (snd_pcm_hw_params*) calloc(1, sizeof(struct snd_pcm_hw_params));
    if (!params) {
        LOG_ALWAYS_FATAL("Failed to allocate ALSA hardware parameters!");
        return NO_INIT;
    }

    if (strcmp(device, MM_LP_DEVICE) == 0) {
        numPeriods = 2;
        LOGI("Using ping-pong!");
    } else {
        numPeriods = 8;
        LOGI("Using FIFO");
    }
    //get the default array index
    for (size_t i = 0; i < ARRAY_SIZE(_defaults); i++) {
        LOGV("setHWParams: device %d bufferSize %d", _defaults[i].devices, _defaults[i].bufferSize);
        if (_defaults[i].devices == handle->devices) {
            reqBuffSize = _defaults[i].bufferSize;
            break;
        }
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
        goto done;
    }
    param_dump(params);
    handle->bufferSize = handle->handle->period_size;

    LOGV("Buffer size: %d", (int)(handle->bufferSize));
    LOGV("Latency: %d", (int)(handle->latency));
    err = NO_ERROR;
done:

    return err;
}

status_t setSoftwareParams(alsa_handle_t *handle)
{
    struct snd_pcm_sw_params* params;
    struct pcm* pcm = handle->handle;
    int err = -1;

    unsigned long bufferSize = handle->handle->buffer_size;
    unsigned long periodSize = handle->handle->period_size;
    unsigned long startThreshold, stopThreshold;

    params = (snd_pcm_sw_params*) calloc(1, sizeof(struct snd_pcm_sw_params));
    if (!params) {
        LOG_ALWAYS_FATAL("Failed to allocate ALSA software parameters!");
        return NO_INIT;
    }

    if (handle->devices & AudioSystem::DEVICE_OUT_ALL) {
        // For playback, configure ALSA to start the transfer when the
        // buffer is full.
        startThreshold = bufferSize - 1;
        stopThreshold = bufferSize;
    } else {
        // For recording, configure ALSA to start the transfer on the
        // first frame.
        startThreshold = periodSize;
        stopThreshold = bufferSize;
    }

    // Get the current software parameters
    params->tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    params->period_step = 1;
    params->avail_min = handle->channels - 1 ? periodSize/4 : pcm->period_size/2;
    params->start_threshold = handle->channels - 1 ? startThreshold/4 : startThreshold/2;
    params->stop_threshold = handle->channels - 1 ? stopThreshold/4 : stopThreshold/2;
    params->silence_threshold = 0;
    params->silence_size = 0;

    if (param_set_sw_params(handle->handle, params)) {
        LOGE("cannot set sw params");
        goto done;
    }
    return NO_ERROR;
done:
    return err;
}


void setAlsaControls(alsa_handle_t *handle, uint32_t devices, int mode)
{
    LOGV("%s: device %d mode %d", __FUNCTION__, devices, mode);

    ALSAControl control("/dev/snd/controlC0");

    // ToDo: If curDevice = newDevice don't do anything
    // If different, disable previous and enable new controls
    // Handle both input and output devices

    /* check whether the devices is input or not */
    /* for output devices */
    if (devices & AudioSystem::DEVICE_OUT_ALL) {
        if (devices & AudioSystem::DEVICE_OUT_SPEAKER) {
            LOGV("Enabling DEVICE_OUT_SPEAKER");
            control.set("SLIMBUS_0_RX Audio Mixer MultiMedia1", 1, -1);       // HFDAC L -> HF Mux
            // These controls work on 8960 target only
            control.set("RX3 MIX1 INP1", "RX1");
            control.set("RX4 MIX1 INP1", "RX2");
            control.set("LINEOUT1 DAC Switch", 1, 0);
            control.set("LINEOUT3 DAC Switch", 1, 0);
            control.set("Speaker Function", "On");
            control.set("LINEOUT1 Volume", "100");
            control.set("LINEOUT3 Volume", "100");

            // For 8660 comment above and uncomment below
            //control.set("EAR Mixer CDC_RX1L Switch", 1, -1);
        } else if (devices & AudioSystem::DEVICE_OUT_WIRED_HEADSET) {
            LOGV("Enabling DEVICE_OUT_HEADSET");
            control.set("SLIMBUS_0_RX Audio Mixer MultiMedia1", 1, -1);       // HFDAC L -> HF Mux
            control.set("RX1 MIX1 INP1", "RX1");
            control.set("RX2 MIX1 INP1", "RX2");
            control.set("HPHL DAC Switch", 1, 0);
            control.set("HPHR DAC Switch", 1, 0);
            control.set("HPHL Volume", "100");
            control.set("HPHR Volume", "100");
        } else {
            LOGV("Disabling DEVICE_OUT_SPEAKER");
            control.set("SLIMBUS_0_RX Audio Mixer MultiMedia1", 0, -1);       // HFDAC L -> HF Mux
            control.set("EAR Mixer CDC_RX1L Switch", 0, -1);       // MM_DL    -> DL2 Mixer
            //control.set("LINE_R Mixer CDC_RX1R Switch", 0, -1);
        }
    } else if (devices & AudioSystem::DEVICE_IN_ALL) {
        if (devices & AudioSystem::DEVICE_IN_BUILTIN_MIC) {
            LOGV("Enabling DEVICE_IN_BUILTIN_MIC");
            control.set("MultiMedia1 Mixer SLIM_0_TX", 1, -1);
            // uncomment the below for 8660
            //control.set("DMIC Left Mux", "DMIC0");
            //control.set("TX1L Filter Mux", "Digital");
            //These control are for 8960 target
            control.set("SLIM TX6 MUX", "DEC6");
            control.set("SLIM TX6 Digital Volume", "100");
            control.set("DEC6 MUX", "ADC1");

        } else {
            LOGV("Disabling DEVICE_IN_BUILTIN_MIC");
            control.set("MultiMedia1 Mixer SLIM_0_TX", 0, -1);
            //uncomment this for 8660
            //control.set("DMIC Left DMIC1", "DMIC0");
            //control.set("TX1L Filter Digital", "Digital");
            // These are for 8960
            control.set("SLIM TX6 MUX", "DEC6");
            control.set("DEC6 MUX", "ADC1");
        }
    }
    handle->curDev = devices;
    handle->curMode = mode;
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

static status_t s_open(alsa_handle_t *handle, uint32_t devices, int mode)
{
    // Close off previously opened device.
    // It would be nice to determine if the underlying device actually
    // changes, but we might be recovering from an error or manipulating
    // mixer settings (see asound.conf).
    //
    unsigned flags = 0;
    int err = NO_ERROR;
    s_close(handle);

    LOGV("s_open: handle %p devices 0x%x in mode %d", handle, devices, mode);

    const char *stream = streamName(handle);
    const char *devName = deviceName(handle, devices, mode);

    // ASoC multicomponent requires a valid path (frontend/backend) for
    // the device to be opened
    setAlsaControls(handle, devices, mode);

    // The PCM stream is opened in blocking mode, per ALSA defaults.  The
    // AudioFlinger seems to assume blocking mode too, so asynchronous mode
    // should not be used.
    //if(direction(handle) == SND_PCM_STREAM_PLAYBACK) {
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
        LOGE("s_open: Failed to initialize ALSA %s device '%s': %s", stream, devName, strerror(err));
        return NO_INIT;
    }

    mActive = true;

    handle->handle->flags = flags;
    err = setHardwareParams(handle);

    if (err == NO_ERROR) {
        err = setSoftwareParams(handle);
    }

    if(err != NO_ERROR) {
        LOGE("Set HW/SW params failed: Closing the pcm stream");
        s_close(handle);
    }

    return NO_ERROR;
}

static status_t s_close(alsa_handle_t *handle)
{
    status_t err = NO_ERROR;
    struct pcm *h = handle->handle;
    ALSAControl control("/dev/snd/controlC0");

    if (handle->curDev & AudioSystem::DEVICE_OUT_SPEAKER) {
        LOGV("Disabling DEVICE_OUT_SPEAKER");
        control.set("SLIMBUS_0_RX Audio Mixer MultiMedia1", 0, -1);
        control.set("RX3 MIX1 INP1", "ZERO");
        control.set("RX4 MIX1 INP1", "ZERO");
        control.set("LINEOUT1 DAC Switch", 0, 0);
        control.set("LINEOUT3 DAC Switch", 0, 0);
        control.set("Speaker Function", "Off");
    }
    handle->handle = 0;
    handle->curDev = 0;
    handle->curMode = 0;
    LOGV("s_close: handle %p", handle);
    if (h) {
        err = pcm_close(h);
        mActive = false;
    }

    return err;
}

static status_t s_route(alsa_handle_t *handle, uint32_t devices, int mode)
{
    LOGD("route called for devices %08x in mode %d...", devices, mode);

    if (handle->handle && handle->curDev == devices && handle->curMode == mode) return NO_ERROR;

    return s_open(handle, devices, mode);
}

}
