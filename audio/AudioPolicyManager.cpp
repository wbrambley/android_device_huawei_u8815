/*
 * Copyright (C) 2009 The Android Open Source Project
 * Copyright (c) 2012, Code Aurora Forum. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "AudioPolicyManager7627a"
//#define LOG_NDEBUG 0

//#define VERY_VERBOSE_LOGGING
#ifdef VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif

// A device mask for all audio input devices that are considered "virtual" when evaluating
// active inputs in getActiveInput()
#define APM_AUDIO_IN_DEVICE_VIRTUAL_ALL  AUDIO_DEVICE_IN_REMOTE_SUBMIX
// A device mask for all audio output devices that are considered "remote" when evaluating
// active output devices in isStreamActiveRemotely()
#define APM_AUDIO_OUT_DEVICE_REMOTE_ALL  AUDIO_DEVICE_OUT_REMOTE_SUBMIX

#include <utils/Log.h>
#include "AudioPolicyManager.h"
#include <hardware/audio_effect.h>
#include <media/mediarecorder.h>
#include <hardware/audio.h>
#include <math.h>
#include <hardware_legacy/audio_policy_conf.h>
#include <fcntl.h>
#include <cutils/properties.h> // for property_get

namespace android_audio_legacy {


// ----------------------------------------------------------------------------
// AudioPolicyManager for msm7k platform
// Common audio policy manager code is implemented in AudioPolicyManagerBase class
// ----------------------------------------------------------------------------

// ---  class factory


extern "C" AudioPolicyInterface* createAudioPolicyManager(AudioPolicyClientInterface *clientInterface)
{
    return new AudioPolicyManager(clientInterface);
}

extern "C" void destroyAudioPolicyManager(AudioPolicyInterface *interface)
{
    delete interface;
}

audio_io_handle_t AudioPolicyManager::getOutput(AudioSystem::stream_type stream,
                                    uint32_t samplingRate,
                                    uint32_t format,
                                    uint32_t channelMask,
                                    AudioSystem::output_flags flags,
                                    const audio_offload_info_t *offloadInfo)
{
    audio_io_handle_t output = 0;
    uint32_t latency = 0;
    routing_strategy strategy = getStrategy((AudioSystem::stream_type)stream);
    audio_devices_t device = getDeviceForStrategy(strategy, false /*fromCache*/);
    ALOGV("getOutput() device %d, stream %d, samplingRate %d, format %d, channelMask %x, flags %x",
          device, stream, samplingRate, format, channelMask, flags);

#ifdef AUDIO_POLICY_TEST
    if (mCurOutput != 0) {
        ALOGV("getOutput() test output mCurOutput %d, samplingRate %d, format %d, channelMask %x, mDirectOutput %d",
                mCurOutput, mTestSamplingRate, mTestFormat, mTestChannels, mDirectOutput);

        if (mTestOutputs[mCurOutput] == 0) {
            ALOGV("getOutput() opening test output");
            AudioOutputDescriptor *outputDesc = new AudioOutputDescriptor(NULL);
            outputDesc->mDevice = mTestDevice;
            outputDesc->mSamplingRate = mTestSamplingRate;
            outputDesc->mFormat = mTestFormat;
            outputDesc->mChannelMask = mTestChannels;
            outputDesc->mLatency = mTestLatencyMs;
            outputDesc->mFlags = (audio_output_flags_t)(mDirectOutput ? AudioSystem::OUTPUT_FLAG_DIRECT : 0);
            outputDesc->mRefCount[stream] = 0;
            mTestOutputs[mCurOutput] = mpClientInterface->openOutput(0, &outputDesc->mDevice,
                                            &outputDesc->mSamplingRate,
                                            &outputDesc->mFormat,
                                            &outputDesc->mChannelMask,
                                            &outputDesc->mLatency,
                                            outputDesc->mFlags,
                                            offloadInfo);
            if (mTestOutputs[mCurOutput]) {
                AudioParameter outputCmd = AudioParameter();
                outputCmd.addInt(String8("set_id"),mCurOutput);
                mpClientInterface->setParameters(mTestOutputs[mCurOutput],outputCmd.toString());
                addOutput(mTestOutputs[mCurOutput], outputDesc);
            }
        }
        return mTestOutputs[mCurOutput];
    }
#endif //AUDIO_POLICY_TEST

    // open a direct output if required by specified parameters
    // force direct flag if offload flag is set: offloading implies a direct output stream
    // and all common behaviors are driven by checking only the direct flag
    // this should normally be set appropriately in the policy configuration file
    if ((flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0) {
        flags = (AudioSystem::output_flags)(flags | AUDIO_OUTPUT_FLAG_DIRECT);
    }

    IOProfile *profile = NULL;
    if (flags & AUDIO_OUTPUT_FLAG_DIRECT) {
        profile = getProfileForDirectOutput(device,
                                            samplingRate,
                                            format,
                                            channelMask,
                                            (audio_output_flags_t)flags);
    }
    if (profile != NULL) {
        AudioOutputDescriptor *outputDesc = NULL;

        for (size_t i = 0; i < mOutputs.size(); i++) {
            AudioOutputDescriptor *desc = mOutputs.valueAt(i);
            if (!desc->isDuplicated() && (profile == desc->mProfile)) {
                outputDesc = desc;
                // reuse direct output if currently open and configured with same parameters
                if ((samplingRate == outputDesc->mSamplingRate) &&
                        (format == outputDesc->mFormat) &&
                        (channelMask == outputDesc->mChannelMask)) {
                    outputDesc->mDirectOpenCount++;
                    ALOGV("getOutput() reusing direct output %d", mOutputs.keyAt(i));
                    return mOutputs.keyAt(i);
                }
            }
        }
        // close direct output if currently open and configured with different parameters
        if (outputDesc != NULL) {
            closeOutput(outputDesc->mId);
        }
        outputDesc = new AudioOutputDescriptor(profile);
        outputDesc->mDevice = device;
        outputDesc->mSamplingRate = samplingRate;
        outputDesc->mFormat = (audio_format_t)format;
        outputDesc->mChannelMask = (audio_channel_mask_t)channelMask;
        outputDesc->mLatency = 0;
        outputDesc->mFlags = (audio_output_flags_t) (outputDesc->mFlags | flags);
        outputDesc->mRefCount[stream] = 0;
        outputDesc->mStopTime[stream] = 0;
        outputDesc->mDirectOpenCount = 1;
        output = mpClientInterface->openOutput(profile->mModule->mHandle,
                                        &outputDesc->mDevice,
                                        &outputDesc->mSamplingRate,
                                        &outputDesc->mFormat,
                                        &outputDesc->mChannelMask,
                                        &outputDesc->mLatency,
                                        outputDesc->mFlags,
                                        offloadInfo);

        // only accept an output with the requested parameters
        if (output == 0 ||
            (samplingRate != 0 && samplingRate != outputDesc->mSamplingRate) ||
            (format != 0 && format != outputDesc->mFormat) ||
            (channelMask != 0 && channelMask != outputDesc->mChannelMask)) {
            ALOGV("getOutput() failed opening direct output: output %d samplingRate %d %d,"
                    "format %d %d, channelMask %04x %04x", output, samplingRate,
                    outputDesc->mSamplingRate, format, outputDesc->mFormat, channelMask,
                    outputDesc->mChannelMask);
            if (output != 0) {
                mpClientInterface->closeOutput(output);
            }
            delete outputDesc;
            return 0;
        }
        addOutput(output, outputDesc);
        mPreviousOutputs = mOutputs;
        ALOGV("getOutput() returns new direct output %d", output);
        return output;
    }

    // ignoring channel mask due to downmix capability in mixer

    // for non direct outputs, only PCM is supported
    if (audio_is_linear_pcm((audio_format_t)format)) {
        // get which output is suitable for the specified stream. The actual
        // routing change will happen when startOutput() will be called
        SortedVector<audio_io_handle_t> outputs = getOutputsForDevice(device, mOutputs);

        output = selectOutput(outputs, flags);
    }

    ALOGW_IF((output ==0), "getOutput() could not find output for stream %d, samplingRate %d,"
            "format %d, channels %x, flags %x", stream, samplingRate, format, channelMask, flags);

    ALOGV("getOutput() returns output %d", output);

    return output;
}

void AudioPolicyManager::releaseOutput(audio_io_handle_t output)
{
    ALOGV("releaseOutput() %d", output);
    ssize_t index = mOutputs.indexOfKey(output);
    if (index < 0) {
        ALOGW("releaseOutput() releasing unknown output %d", output);
        return;
    }

    AudioOutputDescriptor *desc = mOutputs.valueAt(index);
    if (desc->mFlags & AudioSystem::OUTPUT_FLAG_DIRECT) {
        if ((desc->mDirectOpenCount <= 0) && !(desc->mFlags & AUDIO_OUTPUT_FLAG_LPA || desc->mFlags & AUDIO_OUTPUT_FLAG_TUNNEL ||
                desc->mFlags & AUDIO_OUTPUT_FLAG_VOIP_RX)) {
            ALOGW("releaseOutput() invalid open count %d for output %d",
                                                              desc->mDirectOpenCount, output);
            return;
        }
        if ((--desc->mDirectOpenCount == 0) || ((desc->mFlags & AUDIO_OUTPUT_FLAG_LPA || desc->mFlags & AUDIO_OUTPUT_FLAG_TUNNEL ||
                desc->mFlags & AUDIO_OUTPUT_FLAG_VOIP_RX))) {
            ALOGV("releaseOutput() closing output");
            closeOutput(output);
        }
    }

}

audio_devices_t AudioPolicyManager::getDeviceForStrategy(routing_strategy strategy, bool fromCache)
{
    uint32_t device = 0;

    if (fromCache) {
        ALOGV("getDeviceForStrategy() from cache strategy %d, device %x",
              strategy, mDeviceForStrategy[strategy]);
        return mDeviceForStrategy[strategy];
    }

    switch (strategy) {

    case STRATEGY_SONIFICATION_RESPECTFUL:
        if (isInCall()) {
            device = getDeviceForStrategy(STRATEGY_SONIFICATION, false /*fromCache*/);
        } else if (isStreamActiveRemotely(AudioSystem::MUSIC,
                SONIFICATION_RESPECTFUL_AFTER_MUSIC_DELAY)) {
            // while media is playing on a remote device, use the the sonification behavior.
            // Note that we test this usecase before testing if media is playing because
            //   the isStreamActive() method only informs about the activity of a stream, not
            //   if it's for local playback. Note also that we use the same delay between both tests
            device = getDeviceForStrategy(STRATEGY_SONIFICATION, false /*fromCache*/);
        } else if (isStreamActive(AudioSystem::MUSIC, SONIFICATION_RESPECTFUL_AFTER_MUSIC_DELAY)) {
            // while media is playing (or has recently played), use the same device
            device = getDeviceForStrategy(STRATEGY_MEDIA, false /*fromCache*/);
        } else {
            // when media is not playing anymore, fall back on the sonification behavior
            device = getDeviceForStrategy(STRATEGY_SONIFICATION, false /*fromCache*/);
        }

        break;

    case STRATEGY_DTMF:
        if (!isInCall()) {
            // when off call, DTMF strategy follows the same rules as MEDIA strategy
            device = getDeviceForStrategy(STRATEGY_MEDIA, false /*fromCache*/);
            break;
        }
        // when in call, DTMF and PHONE strategies follow the same rules
        // FALL THROUGH

    case STRATEGY_PHONE:
        // for phone strategy, we first consider the forced use and then the available devices by order
        // of priority
        switch (mForceUse[AudioSystem::FOR_COMMUNICATION]) {
        case AudioSystem::FORCE_BT_SCO:
            if (!isInCall() || strategy != STRATEGY_DTMF) {
                device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT;
                if (device) break;
            }
            device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET;
            if (device) break;
            device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO;
            if (device) break;
            // if SCO device is requested but no SCO device is available, fall back to default case
            // FALL THROUGH

        default:    // FORCE_NONE
            // when not in a phone call, phone strategy should route STREAM_VOICE_CALL to A2DP
            if (mHasA2dp && !isInCall() &&
                    (mForceUse[AudioSystem::FOR_MEDIA] != AudioSystem::FORCE_NO_BT_A2DP) &&
                    (getA2dpOutput() != 0) && !mA2dpSuspended) {
                device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP;
                if (device) break;
                device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES;
                if (device) break;
            }
            device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE;
            if (device) break;
            device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADSET;
            if (device) break;
            device = mAvailableOutputDevices & AUDIO_DEVICE_OUT_USB_DEVICE;
            if (device) break;
            device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET;
            if (device) break;
            device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_AUX_DIGITAL;
            if (device) break;
            device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET;
            if (device) break;
            device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_EARPIECE;
            if (device) break;
            device = mDefaultOutputDevice;
            if (device == AUDIO_DEVICE_NONE) {
                ALOGE("getDeviceForStrategy() no device found for STRATEGY_PHONE");
            }
            break;

        case AudioSystem::FORCE_SPEAKER:
            // when not in a phone call, phone strategy should route STREAM_VOICE_CALL to
            // A2DP speaker when forcing to speaker output
            if (mHasA2dp && !isInCall() &&
                    (mForceUse[AudioSystem::FOR_MEDIA] != AudioSystem::FORCE_NO_BT_A2DP) &&
                    (getA2dpOutput() != 0) && !mA2dpSuspended) {
                device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER;
                if (device) break;
            }
            device = mAvailableOutputDevices & AUDIO_DEVICE_OUT_USB_DEVICE;
            if (device) break;
            device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET;
            if (device) break;
            device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_AUX_DIGITAL;
            if (device) break;
            device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET;
            if (device) break;
            device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_SPEAKER;
            if (device) break;
            device = mDefaultOutputDevice;
            if (device == AUDIO_DEVICE_NONE) {
                ALOGE("getDeviceForStrategy() no device found for STRATEGY_PHONE, FORCE_SPEAKER");
            }
            break;
        }
    break;

    case STRATEGY_SONIFICATION:

        // If incall, just select the STRATEGY_PHONE device: The rest of the behavior is handled by
        // handleIncallSonification().
        if (isInCall()) {
            device = getDeviceForStrategy(STRATEGY_PHONE, false /*fromCache*/);
            break;
        }
        // FALL THROUGH

    case STRATEGY_ENFORCED_AUDIBLE:
        // strategy STRATEGY_ENFORCED_AUDIBLE uses same routing policy as STRATEGY_SONIFICATION
        // except:
        //   - when in call where it doesn't default to STRATEGY_PHONE behavior
        //   - in countries where not enforced in which case it follows STRATEGY_MEDIA

        if (strategy == STRATEGY_SONIFICATION ||
                !mStreams[AUDIO_STREAM_ENFORCED_AUDIBLE].mCanBeMuted) {
            device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_SPEAKER;
            if (device == AUDIO_DEVICE_NONE) {
                ALOGE("getDeviceForStrategy() speaker device not found for STRATEGY_SONIFICATION");
            }
        }
        // The second device used for sonification is the same as the device used by media strategy
        // FALL THROUGH

        // for analog FM alerts should be played on the speaker only
        if(FM_ANALOG == getFMMode())
            break;
    case STRATEGY_MEDIA: {
        uint32_t device2 = 0;
        switch (mForceUse[AudioSystem::FOR_MEDIA]) {
        default:{
        if ((mHasA2dp && (mForceUse[AudioSystem::FOR_MEDIA] != AudioSystem::FORCE_NO_BT_A2DP) &&
                (getA2dpOutput() != 0) && !mA2dpSuspended ) && !(FM_ANALOG == getFMMode())) {
            device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP;
            if (device2 == 0) {
                device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES;
            }
            if (device2 == 0) {
                device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER;
            }
        }
        if (device2 == 0) {
            device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE;
        }
        if (device2 == 0) {
            device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADSET;
        }
        if (device2 == 0) {
            device2 = mAvailableOutputDevices & AUDIO_DEVICE_OUT_USB_ACCESSORY;
        }
        if (device2 == 0) {
            device2 = mAvailableOutputDevices & AUDIO_DEVICE_OUT_USB_DEVICE;
        }
        if (device2 == 0) {
            device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET;
        }
        if (device2 == 0) {
            device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_AUX_DIGITAL;
        }
        if (device2 == 0) {
            device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET;
        }
        if (device2 == 0) {
            device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_SPEAKER;
        }

        // device is DEVICE_OUT_SPEAKER if we come from case STRATEGY_SONIFICATION or
        // STRATEGY_ENFORCED_AUDIBLE, 0 otherwise
        device |= device2;
        if (device) break;
        device = mDefaultOutputDevice;
      }
      case AudioSystem::FORCE_SPEAKER:
          device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_SPEAKER;
          break;
      }
#ifdef QCOM_FM_ENABLED
      if (mAvailableOutputDevices & AudioSystem::DEVICE_OUT_FM) {
         device |= AudioSystem::DEVICE_OUT_FM;
         if(FM_ANALOG == getFMMode()){
           if (device == (AudioSystem::DEVICE_OUT_SPEAKER | AudioSystem::DEVICE_OUT_WIRED_HEADSET | AudioSystem::DEVICE_OUT_FM))
                device = AudioSystem::DEVICE_OUT_SPEAKER;
           else if(device & AudioSystem::DEVICE_OUT_WIRED_HEADSET)
                device &= ~(device & AudioSystem::DEVICE_OUT_WIRED_HEADSET);
         }
      }
#endif
      // Do not play media stream if in call and the requested device would change the hardware
      // output routing
      if (mPhoneState == AudioSystem::MODE_IN_CALL &&
         !AudioSystem::isA2dpDevice((AudioSystem::audio_devices)device) &&
           device != getDeviceForStrategy(STRATEGY_PHONE) &&
           strategy == STRATEGY_ENFORCED_AUDIBLE) {
          if (!mStreams[AUDIO_STREAM_ENFORCED_AUDIBLE].mCanBeMuted) {
              ALOGV("getDeviceForStrategy() do not change to phone device for ENFORCED_AUDIBLE");
          } else {
              device = getDeviceForStrategy(STRATEGY_PHONE);
              ALOGV("getDeviceForStrategy() incompatible media and phone devices");
          }
      }
      if (device == AUDIO_DEVICE_NONE) {
        ALOGE("getDeviceForStrategy() no device found for STRATEGY_MEDIA");
      }
    } break;

    default:
        ALOGW("getDeviceForStrategy() unknown strategy: %d", strategy);
        break;
    }

    ALOGV("getDeviceForStrategy() strategy %d, device %x", strategy, device);
    return (audio_devices_t)device;
}

uint32_t AudioPolicyManager::checkDeviceMuteStrategies(AudioOutputDescriptor *outputDesc,
                                                       audio_devices_t prevDevice,
                                                       uint32_t delayMs)
{
    // mute/unmute strategies using an incompatible device combination
    // if muting, wait for the audio in pcm buffer to be drained before proceeding
    // if unmuting, unmute only after the specified delay
    if (outputDesc->isDuplicated()) {
        return 0;
    }

    uint32_t muteWaitMs = 0;
    audio_devices_t device = outputDesc->device();
#ifdef QCOM_FM_ENABLED
    bool shouldMute = outputDesc->isActive() &&
                    (AudioSystem::popCount(device) >= (device & AUDIO_DEVICE_OUT_FM ? 3 : 2));
#else
    bool shouldMute = outputDesc->isActive();
#endif
    // temporary mute output if device selection changes to avoid volume bursts due to
    // different per device volumes
    bool tempMute = outputDesc->isActive() && (getDeviceForVolume(device) != getDeviceForVolume(prevDevice));

    for (size_t i = 0; i < NUM_STRATEGIES; i++) {
        audio_devices_t curDevice = getDeviceForStrategy((routing_strategy)i, false /*fromCache*/);
        bool mute = shouldMute && (curDevice & device) && (curDevice != device);
        bool doMute = false;

        if (mute && !outputDesc->mStrategyMutedByDevice[i]) {
            doMute = true;
            outputDesc->mStrategyMutedByDevice[i] = true;
        } else if (!mute && outputDesc->mStrategyMutedByDevice[i]){
            doMute = true;
            outputDesc->mStrategyMutedByDevice[i] = false;
        }
        if (doMute || tempMute) {
            for (size_t j = 0; j < mOutputs.size(); j++) {
                AudioOutputDescriptor *desc = mOutputs.valueAt(j);
                if ((desc->supportedDevices() & outputDesc->supportedDevices()) == AUDIO_DEVICE_NONE) {
                    continue;
                }
                audio_io_handle_t curOutput = mOutputs.keyAt(j);
                ALOGVV("checkDeviceMuteStrategies() %s strategy %d (curDevice %04x) on output %d",
                      mute ? "muting" : "unmuting", i, curDevice, curOutput);
                setStrategyMute((routing_strategy)i, mute, curOutput, mute ? 0 : delayMs);
                if (desc->isStrategyActive((routing_strategy)i)) {
                    if (tempMute && (desc == outputDesc)) {
                        setStrategyMute((routing_strategy)i, true, curOutput);
                        setStrategyMute((routing_strategy)i, false, curOutput,
                                            desc->latency() * 2, device);
                    }
                    if ((tempMute && (desc == outputDesc)) || mute) {
                        if (muteWaitMs < desc->latency()) {
                            muteWaitMs = desc->latency();
                        }
                    }
                }
            }
        }
    }

    // FIXME: should not need to double latency if volume could be applied immediately by the
    // audioflinger mixer. We must account for the delay between now and the next time
    // the audioflinger thread for this output will process a buffer (which corresponds to
    // one buffer size, usually 1/2 or 1/4 of the latency).
    muteWaitMs *= 2;
    // wait for the PCM output buffers to empty before proceeding with the rest of the command
    if (muteWaitMs > delayMs) {
        muteWaitMs -= delayMs;
        usleep(muteWaitMs * 1000);
        return muteWaitMs;
    }
    return 0;
}

status_t AudioPolicyManager::setDeviceConnectionState(audio_devices_t device,
                                                      AudioSystem::device_connection_state state,
                                                      const char *device_address)
{
    SortedVector <audio_io_handle_t> outputs;

    ALOGV("setDeviceConnectionState() device: %x, state %d, address %s", device, state, device_address);
    // connect/disconnect only 1 device at a time
    if (!audio_is_output_device(device) && !audio_is_input_device(device)) return BAD_VALUE;

    if (strlen(device_address) >= MAX_DEVICE_ADDRESS_LEN) {
        ALOGE("setDeviceConnectionState() invalid address: %s", device_address);
        return BAD_VALUE;
    }

    // handle output devices
    if (audio_is_output_device(device)) {
        if (!mHasA2dp && audio_is_a2dp_device(device)) {
            ALOGE("setDeviceConnectionState() invalid A2DP device: %x", device);
            return BAD_VALUE;
        }
        if (!mHasUsb && audio_is_usb_device(device)) {
            ALOGE("setDeviceConnectionState() invalid USB audio device: %x", device);
            return BAD_VALUE;
        }
        if (!mHasRemoteSubmix && audio_is_remote_submix_device((audio_devices_t)device)) {
            ALOGE("setDeviceConnectionState() invalid remote submix audio device: %x", device);
            return BAD_VALUE;
        }

        // save a copy of the opened output descriptors before any output is opened or closed
        // by checkOutputsForDevice(). This will be needed by checkOutputForAllStrategies()
        mPreviousOutputs = mOutputs;
        switch (state)
        {
        // handle output device connection
        case AudioSystem::DEVICE_STATE_AVAILABLE:
#ifdef QCOM_FM_ENABLED
            if(device == AudioSystem::DEVICE_OUT_FM){
                char value[PROPERTY_VALUE_MAX];
                fm_modes fmMode = FM_DIGITAL;

                if (property_get("hw.fm.isAnalog", value, NULL)
                && !strcasecmp(value, "true")){
                    fmMode = FM_ANALOG ;
                }

                ALOGD("Current FM mode %d, New Fm Mode %d",getFMMode(),fmMode);

                if (fmMode == getFMMode()){
                    ALOGE("FM is already connected in %d Mode",fmMode);
                    return INVALID_OPERATION;
                } else if (FM_NONE != getFMMode()){
                    ALOGE("Rejctng dev conction:Anlg FM & Dgtl FM Mutuly xclusve");
                    return INVALID_OPERATION;
                }else{
                    setFmMode(fmMode);
                    ALOGW("FM started in %d Mode",fmMode);
                }
            }
#endif
            if (mAvailableOutputDevices & device) {
                ALOGW("setDeviceConnectionState() device already connected: %x", device);
                return INVALID_OPERATION;
            }
            ALOGV("setDeviceConnectionState() connecting device %x", device);

            if (checkOutputsForDevice(device, state, outputs) != NO_ERROR) {
                return INVALID_OPERATION;
            }
            ALOGV("setDeviceConnectionState() checkOutputsForDevice() returned %d outputs",
                  outputs.size());
            // register new device as available
            mAvailableOutputDevices = (audio_devices_t)(mAvailableOutputDevices | device);
            ALOGV("setDeviceConnectionState() connecting device %x", mAvailableOutputDevices);

            if (!outputs.isEmpty()) {
                String8 paramStr;
                if (mHasA2dp && audio_is_a2dp_device(device)) {
                    // handle A2DP device connection
                    AudioParameter param;
                    param.add(String8(AUDIO_PARAMETER_A2DP_SINK_ADDRESS), String8(device_address));
                    paramStr = param.toString();
                    mA2dpDeviceAddress = String8(device_address, MAX_DEVICE_ADDRESS_LEN);
                    mA2dpSuspended = false;
                } else if (audio_is_bluetooth_sco_device(device)) {
                    // handle SCO device connection
                    mScoDeviceAddress = String8(device_address, MAX_DEVICE_ADDRESS_LEN);
                } else if (mHasUsb && audio_is_usb_device(device)) {
                    // handle USB device connection
                    mUsbCardAndDevice = String8(device_address, MAX_DEVICE_ADDRESS_LEN);
                    paramStr = mUsbCardAndDevice;
                }
                if (!paramStr.isEmpty()) {
                    for (size_t i = 0; i < outputs.size(); i++) {
                        mpClientInterface->setParameters(outputs[i], paramStr);
                    }
                }
            }
            break;
        // handle output device disconnection
        case AudioSystem::DEVICE_STATE_UNAVAILABLE: {
#ifdef QCOM_FM_ENABLED
            if(device == AudioSystem::DEVICE_OUT_FM){
                uint32_t newDevice;
                fm_modes prevFmMode = getFMMode();

                ALOGD("turning off Fm device in Mode %d",getFMMode());
                setFmMode(FM_NONE);
                newDevice = getDeviceForStrategy(STRATEGY_MEDIA, false);
                if((FM_ANALOG == prevFmMode) && ((newDevice & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP) ||
                   (newDevice & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES)||
                   (newDevice & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER))) {
                    ALOGW("setDeviceConnectionState() FM off, switch to Wired Headset");
                    setOutputDevice(mPrimaryOutput, AUDIO_DEVICE_OUT_WIRED_HEADSET, true);
                }
            }
#endif
            if (!(mAvailableOutputDevices & device)) {
                ALOGW("setDeviceConnectionState() device not connected: %x", device);
                return INVALID_OPERATION;
            }

            ALOGV("setDeviceConnectionState() disconnecting device %x", device);
            // remove device from available output devices
            mAvailableOutputDevices = (audio_devices_t)(mAvailableOutputDevices & ~device);

            checkOutputsForDevice((audio_devices_t)device, state, outputs);
            if (mHasA2dp && audio_is_a2dp_device(device)) {
                // handle A2DP device disconnection
                mA2dpDeviceAddress = "";
                mA2dpSuspended = false;
            } else if (audio_is_bluetooth_sco_device(device)) {
                // handle SCO device disconnection
                mScoDeviceAddress = "";
            } else if (mHasUsb && audio_is_usb_device(device)) {
                // handle USB device disconnection
                mUsbCardAndDevice = "";
            }
        } break;

        default:
            ALOGE("setDeviceConnectionState() invalid state: %x", state);
            return BAD_VALUE;
        }

        audio_devices_t newDevice = AudioPolicyManagerBase::getNewDevice(mPrimaryOutput, false /*fromCache*/);
#ifdef QCOM_FM_ENABLED
        if (device == AudioSystem::DEVICE_OUT_FM) {
            if (state == AudioSystem::DEVICE_STATE_AVAILABLE) {
                mOutputs.valueFor(mPrimaryOutput)->changeRefCount(AudioSystem::FM, 1);
            }
            else {
                mOutputs.valueFor(mPrimaryOutput)->changeRefCount(AudioSystem::FM, -1);
            }
            if (newDevice == 0) {
                newDevice = getDeviceForStrategy(STRATEGY_MEDIA, false);
            }
        }
#endif
        setOutputDevice(mPrimaryOutput, newDevice);
        checkA2dpSuspend();
        AudioPolicyManagerBase::checkOutputForAllStrategies();
        // outputs must be closed after checkOutputForAllStrategies() is executed
        if (!outputs.isEmpty()) {
            for (size_t i = 0; i < outputs.size(); i++) {
                 AudioOutputDescriptor *desc = mOutputs.valueFor(outputs[i]);
                 // close unused outputs after device disconnection or direct outputs that have been
                 // opened by checkOutputsForDevice() to query dynamic parameters
                 if ((state == AudioSystem::DEVICE_STATE_UNAVAILABLE) ||
                         (((desc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) != 0) &&
                          (desc->mDirectOpenCount == 0))) {
                     closeOutput(outputs[i]);
                 }
            }
        }

        updateDevicesAndOutputs();
        for (size_t i = 0; i < mOutputs.size(); i++) {
            // do not force device change on duplicated output because if device is 0, it will
            // also force a device 0 for the two outputs it is duplicated to which may override
            // a valid device selection on those outputs.
            setOutputDevice(mOutputs.keyAt(i), getNewDevice(mOutputs.keyAt(i), true /*fromCache*/),
                            !mOutputs.valueAt(i)->isDuplicated(),
                            0);
        }

        if (device == AUDIO_DEVICE_OUT_WIRED_HEADSET) {
            device = AUDIO_DEVICE_IN_WIRED_HEADSET;
        } else if (device == AUDIO_DEVICE_OUT_BLUETOOTH_SCO ||
                   device == AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET ||
                   device == AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT) {
            device = AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET;
        } else {
            return NO_ERROR;
        }
    }
    // handle input devices
    if (audio_is_input_device(device)) {

        switch (state)
        {
        // handle input device connection
        case AudioSystem::DEVICE_STATE_AVAILABLE: {
            if (mAvailableInputDevices & device) {
                ALOGW("setDeviceConnectionState() device already connected: %d", device);
                return INVALID_OPERATION;
            }
            mAvailableInputDevices = mAvailableInputDevices | (device & ~AUDIO_DEVICE_BIT_IN);
            }
            break;

        // handle input device disconnection
        case AudioSystem::DEVICE_STATE_UNAVAILABLE: {
            if (!(mAvailableInputDevices & device)) {
                ALOGW("setDeviceConnectionState() device not connected: %d", device);
                return INVALID_OPERATION;
            }
            mAvailableInputDevices = (audio_devices_t) (mAvailableInputDevices & ~device);
            } break;

        default:
            ALOGE("setDeviceConnectionState() invalid state: %x", state);
            return BAD_VALUE;
        }

        audio_io_handle_t activeInput = getActiveInput();
        if (activeInput != 0) {
            AudioInputDescriptor *inputDesc = mInputs.valueFor(activeInput);
            audio_devices_t newDevice = getDeviceForInputSource(inputDesc->mInputSource);
            if ((newDevice != AUDIO_DEVICE_NONE) && (newDevice != inputDesc->mDevice)) {
                ALOGV("setDeviceConnectionState() changing device from %x to %x for input %d",
                        inputDesc->mDevice, newDevice, activeInput);
                inputDesc->mDevice = newDevice;
                AudioParameter param = AudioParameter();
                param.addInt(String8(AudioParameter::keyRouting), (int)newDevice);
                mpClientInterface->setParameters(activeInput, param.toString());
            }
        }

        return NO_ERROR;
    }

    ALOGW("setDeviceConnectionState() invalid device: %x", device);
    return BAD_VALUE;
}

void AudioPolicyManager::setForceUse(AudioSystem::force_use usage, AudioSystem::forced_config config)
{
    ALOGV("setForceUse() usage %d, config %d, mPhoneState %d", usage, config, mPhoneState);

    bool forceVolumeReeval = false;
    switch(usage) {
    case AudioSystem::FOR_COMMUNICATION:
        if (config != AudioSystem::FORCE_SPEAKER && config != AudioSystem::FORCE_BT_SCO &&
            config != AudioSystem::FORCE_NONE) {
            ALOGW("setForceUse() invalid config %d for FOR_COMMUNICATION", config);
            return;
        }
        forceVolumeReeval = true;
        mForceUse[usage] = config;
        break;
    case AudioSystem::FOR_MEDIA:
        if (config != AudioSystem::FORCE_HEADPHONES && config != AudioSystem::FORCE_BT_A2DP &&
            config != AudioSystem::FORCE_WIRED_ACCESSORY &&
            config != AudioSystem::FORCE_ANALOG_DOCK &&
            config != AudioSystem::FORCE_DIGITAL_DOCK && config != AudioSystem::FORCE_NONE &&
            config != AudioSystem::FORCE_NO_BT_A2DP) {
            ALOGW("setForceUse() invalid config %d for FOR_MEDIA", config);
            return;
        }
        mForceUse[usage] = config;
        break;
    case AudioSystem::FOR_RECORD:
        if (config != AudioSystem::FORCE_BT_SCO && config != AudioSystem::FORCE_WIRED_ACCESSORY &&
            config != AudioSystem::FORCE_NONE) {
            ALOGW("setForceUse() invalid config %d for FOR_RECORD", config);
            return;
        }
        mForceUse[usage] = config;
        break;
    case AudioSystem::FOR_DOCK:
        if (config != AudioSystem::FORCE_NONE && config != AudioSystem::FORCE_BT_CAR_DOCK &&
            config != AudioSystem::FORCE_BT_DESK_DOCK &&
            config != AudioSystem::FORCE_WIRED_ACCESSORY &&
            config != AudioSystem::FORCE_ANALOG_DOCK &&
            config != AudioSystem::FORCE_DIGITAL_DOCK) {
            ALOGW("setForceUse() invalid config %d for FOR_DOCK", config);
        }
        forceVolumeReeval = true;
        mForceUse[usage] = config;
        break;
    case AudioSystem::FOR_SYSTEM:
        if (config != AudioSystem::FORCE_NONE &&
            config != AudioSystem::FORCE_SYSTEM_ENFORCED) {
            ALOGW("setForceUse() invalid config %d for FOR_SYSTEM", config);
        }
        forceVolumeReeval = true;
        mForceUse[usage] = config;
        break;
    default:
        ALOGW("setForceUse() invalid usage %d", usage);
        break;
    }

    // check for device and output changes triggered by new force usage
    checkA2dpSuspend();
    checkOutputForAllStrategies();
    updateDevicesAndOutputs();
    for (size_t i = 0; i < mOutputs.size(); i++) {
        audio_io_handle_t output = mOutputs.keyAt(i);
        audio_devices_t newDevice = getNewDevice(output, true /*fromCache*/);
        setOutputDevice(output, newDevice, (newDevice != AUDIO_DEVICE_NONE));
        if (forceVolumeReeval && (newDevice != AUDIO_DEVICE_NONE)) {
            applyStreamVolumes(output, newDevice, 0, true);
        }
    }

    audio_io_handle_t activeInput = getActiveInput();
    if (activeInput != 0) {
        AudioInputDescriptor *inputDesc = mInputs.valueFor(activeInput);
        audio_devices_t newDevice = getDeviceForInputSource(inputDesc->mInputSource);
        if ((newDevice != AUDIO_DEVICE_NONE) && (newDevice != inputDesc->mDevice)) {
            ALOGV("setForceUse() changing device from %x to %x for input %d",
                    inputDesc->mDevice, newDevice, activeInput);
            inputDesc->mDevice = newDevice;
            AudioParameter param = AudioParameter();
            param.addInt(String8(AudioParameter::keyRouting), (int)newDevice);
            mpClientInterface->setParameters(activeInput, param.toString());
        }
    }

}


AudioPolicyManagerBase::IOProfile *AudioPolicyManager::getProfileForDirectOutput(
                                                               audio_devices_t device,
                                                               uint32_t samplingRate,
                                                               uint32_t format,
                                                               uint32_t channelMask,
                                                               audio_output_flags_t flags)
{


    for (size_t i = 0; i < mHwModules.size(); i++) {
        if (mHwModules[i]->mHandle == 0) {
            continue;
        }
        for (size_t j = 0; j < mHwModules[i]->mOutputProfiles.size(); j++) {
           AudioPolicyManagerBase::IOProfile *profile = mHwModules[i]->mOutputProfiles[j];
            if (flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) {
                if (profile->isCompatibleProfile(device, samplingRate, format,
                                           channelMask,
                                           AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD)) {
                    if (mAvailableOutputDevices & profile->mSupportedDevices) {
                        return mHwModules[i]->mOutputProfiles[j];
                    }
                }
            } else if (flags & AUDIO_OUTPUT_FLAG_DIRECT) {
                if (profile->isCompatibleProfile(device, samplingRate, format,
                                           channelMask,
                                           AUDIO_OUTPUT_FLAG_DIRECT)) {
                    if (mAvailableOutputDevices & profile->mSupportedDevices) {
                        return mHwModules[i]->mOutputProfiles[j];
                    }
                }
            }
        }
    }
    return 0;
}


bool AudioPolicyManager::isCompatibleProfile(AudioPolicyManagerBase::IOProfile *profile,
                                             audio_devices_t device,
                                             uint32_t samplingRate,
                                             uint32_t format,
                                             uint32_t channelMask,
                                            audio_output_flags_t flags)
{
    if ((profile->mSupportedDevices & device) != device) {
        return false;
    }
    if (profile->mFlags != flags) {
        return false;
    }
    if (samplingRate != 0) {
        size_t i;
        for (i = 0; i < profile->mSamplingRates.size(); i++)
        {
            if (profile->mSamplingRates[i] == samplingRate) {
                break;
            }
        }
        if (i == profile->mSamplingRates.size()) {
            return false;
        }
    }
    if (format != 0) {
        size_t i;
        for (i = 0; i < profile->mFormats.size(); i++)
        {
            if (profile->mFormats[i] == format) {
                break;
            }
        }
        if (i == profile->mFormats.size()) {
            return false;
        }
    }
    if (channelMask != 0) {
        size_t i;
       for (i = 0; i < profile->mChannelMasks.size(); i++)
        {
            if (profile->mChannelMasks[i] == channelMask) {
                break;
            }
        }
        if (i == profile->mChannelMasks.size()) {
            return false;
        }
    }
    ALOGD(" profile found: device %x, flags %x, samplingrate %d,\
            format %x, channelMask %d",
            device, flags, samplingRate, format, channelMask);
    return true;
}

status_t AudioPolicyManager::checkOutputsForDevice(audio_devices_t device,
                                                       AudioSystem::device_connection_state state,
                                                       SortedVector<audio_io_handle_t>& outputs)
{
    AudioOutputDescriptor *desc;

    if (state == AudioSystem::DEVICE_STATE_AVAILABLE) {
        // first list already open outputs that can be routed to this device
        for (size_t i = 0; i < mOutputs.size(); i++) {
            desc = mOutputs.valueAt(i);
            if (!desc->isDuplicated() && (desc->mProfile->mSupportedDevices & device)) {
                ALOGV("checkOutputsForDevice(): adding opened output %d", mOutputs.keyAt(i));
                outputs.add(mOutputs.keyAt(i));
            }
        }
        // then look for output profiles that can be routed to this device
        SortedVector<IOProfile *> profiles;
        for (size_t i = 0; i < mHwModules.size(); i++)
        {
            if (mHwModules[i]->mHandle == 0) {
                continue;
            }
            for (size_t j = 0; j < mHwModules[i]->mOutputProfiles.size(); j++)
            {
                if (mHwModules[i]->mOutputProfiles[j]->mSupportedDevices & device) {
                    ALOGV("checkOutputsForDevice(): adding profile %d from module %d", j, i);
                    profiles.add(mHwModules[i]->mOutputProfiles[j]);
                }
            }
        }

        if (profiles.isEmpty() && outputs.isEmpty()) {
            ALOGW("checkOutputsForDevice(): No output available for device %04x", device);
            return BAD_VALUE;
        }

        // open outputs for matching profiles if needed. Direct outputs are also opened to
        // query for dynamic parameters and will be closed later by setDeviceConnectionState()
        for (ssize_t profile_index = 0; profile_index < (ssize_t)profiles.size(); profile_index++) {
            IOProfile *profile = profiles[profile_index];

            // nothing to do if one output is already opened for this profile
            size_t j;
            for (j = 0; j < mOutputs.size(); j++) {
                desc = mOutputs.valueAt(j);
                if (!desc->isDuplicated() && desc->mProfile == profile) {
                    break;
                }
            }
            if (j != mOutputs.size()) {
                continue;
            }

            ALOGV("opening output for device %08x", device);
            desc = new AudioOutputDescriptor(profile);
            desc->mDevice = device;
            audio_io_handle_t output = 0;
            if (!(desc->mFlags & AUDIO_OUTPUT_FLAG_LPA || desc->mFlags & AUDIO_OUTPUT_FLAG_TUNNEL ||
                desc->mFlags & AUDIO_OUTPUT_FLAG_VOIP_RX)) {
                output =  mpClientInterface->openOutput(profile->mModule->mHandle,
                                                        &desc->mDevice,
                                                        &desc->mSamplingRate,
                                                        &desc->mFormat,
                                                        &desc->mChannelMask,
                                                        &desc->mLatency,
                                                        desc->mFlags);
            }
            if (output != 0) {
                if (desc->mFlags & AUDIO_OUTPUT_FLAG_DIRECT) {
                    String8 reply;
                    char *value;
                    if (profile->mSamplingRates[0] == 0) {
                        reply = mpClientInterface->getParameters(output,
                                                String8(AUDIO_PARAMETER_STREAM_SUP_SAMPLING_RATES));
                        ALOGV("checkOutputsForDevice() direct output sup sampling rates %s",
                                  reply.string());
                        value = strpbrk((char *)reply.string(), "=");
                        if (value != NULL) {
                            loadSamplingRates(value + 1, profile);
                        }
                    }
                    if (profile->mFormats[0] == 0) {
                        reply = mpClientInterface->getParameters(output,
                                                       String8(AUDIO_PARAMETER_STREAM_SUP_FORMATS));
                        ALOGV("checkOutputsForDevice() direct output sup formats %s",
                                  reply.string());
                        value = strpbrk((char *)reply.string(), "=");
                        if (value != NULL) {
                            loadFormats(value + 1, profile);
                        }
                    }
                    if (profile->mChannelMasks[0] == 0) {
                        reply = mpClientInterface->getParameters(output,
                                                      String8(AUDIO_PARAMETER_STREAM_SUP_CHANNELS));
                        ALOGV("checkOutputsForDevice() direct output sup channel masks %s",
                                  reply.string());
                        value = strpbrk((char *)reply.string(), "=");
                        if (value != NULL) {
                            loadOutChannels(value + 1, profile);
                        }
                    }
                    if (((profile->mSamplingRates[0] == 0) &&
                             (profile->mSamplingRates.size() < 2)) ||
                         ((profile->mFormats[0] == 0) &&
                             (profile->mFormats.size() < 2)) ||
                         ((profile->mFormats[0] == 0) &&
                             (profile->mChannelMasks.size() < 2))) {
                        ALOGW("checkOutputsForDevice() direct output missing param");
                        mpClientInterface->closeOutput(output);
                        output = 0;
                    } else {
                        addOutput(output, desc);
                    }
                } else {
                    audio_io_handle_t duplicatedOutput = 0;
                    // add output descriptor
                    addOutput(output, desc);
                    // set initial stream volume for device
                    applyStreamVolumes(output, device, 0, true);

                    //TODO: configure audio effect output stage here

                    // open a duplicating output thread for the new output and the primary output
                    duplicatedOutput = mpClientInterface->openDuplicateOutput(output,
                                                                              mPrimaryOutput);
                    if (duplicatedOutput != 0) {
                        // add duplicated output descriptor
                        AudioOutputDescriptor *dupOutputDesc = new AudioOutputDescriptor(NULL);
                        dupOutputDesc->mOutput1 = mOutputs.valueFor(mPrimaryOutput);
                        dupOutputDesc->mOutput2 = mOutputs.valueFor(output);
                        dupOutputDesc->mSamplingRate = desc->mSamplingRate;
                        dupOutputDesc->mFormat = desc->mFormat;
                        dupOutputDesc->mChannelMask = desc->mChannelMask;
                        dupOutputDesc->mLatency = desc->mLatency;
                        addOutput(duplicatedOutput, dupOutputDesc);
                        applyStreamVolumes(duplicatedOutput, device, 0, true);
                    } else {
                        ALOGW("checkOutputsForDevice() could not open dup output for %d and %d",
                                mPrimaryOutput, output);
                        mpClientInterface->closeOutput(output);
                        mOutputs.removeItem(output);
                        output = 0;
                    }
                }
            }
            if (output == 0) {
                ALOGW("checkOutputsForDevice() could not open output for device %x", device);
                delete desc;
                profiles.removeAt(profile_index);
                profile_index--;
            } else {
                outputs.add(output);
                ALOGV("checkOutputsForDevice(): adding output %d", output);
            }
        }

        if (profiles.isEmpty()) {
            ALOGW("checkOutputsForDevice(): No output available for device %04x", device);
            return BAD_VALUE;
        }
    } else {
        // check if one opened output is not needed any more after disconnecting one device
        for (size_t i = 0; i < mOutputs.size(); i++) {
            desc = mOutputs.valueAt(i);
            if (!desc->isDuplicated() &&
                    !(desc->mProfile->mSupportedDevices & mAvailableOutputDevices)) {
                ALOGV("checkOutputsForDevice(): disconnecting adding output %d", mOutputs.keyAt(i));
                outputs.add(mOutputs.keyAt(i));
            }
        }
        for (size_t i = 0; i < mHwModules.size(); i++)
        {
            if (mHwModules[i]->mHandle == 0) {
                continue;
            }
            for (size_t j = 0; j < mHwModules[i]->mOutputProfiles.size(); j++)
            {
                IOProfile *profile = mHwModules[i]->mOutputProfiles[j];
                if ((profile->mSupportedDevices & device) &&
                        (profile->mFlags & AUDIO_OUTPUT_FLAG_DIRECT)) {
                    ALOGV("checkOutputsForDevice(): clearing direct output profile %d on module %d",
                          j, i);
                    if (profile->mSamplingRates[0] == 0) {
                        profile->mSamplingRates.clear();
                        profile->mSamplingRates.add(0);
                    }
                    if (profile->mFormats[0] == 0) {
                        profile->mFormats.clear();
                        profile->mFormats.add((audio_format_t)0);
                    }
                    if (profile->mChannelMasks[0] == 0) {
                        profile->mChannelMasks.clear();
                        profile->mChannelMasks.add((audio_channel_mask_t)0);
                    }
                }
            }
        }
    }
    return NO_ERROR;
}

audio_devices_t AudioPolicyManager::getDeviceForInputSource(int inputSource)
{
    uint32_t device = AUDIO_DEVICE_NONE;

    switch(inputSource) {
    case AUDIO_SOURCE_VOICE_UPLINK:
      if (mAvailableInputDevices & AUDIO_DEVICE_IN_VOICE_CALL) {
          device = AUDIO_DEVICE_IN_VOICE_CALL;
          break;
      }
      // FALL THROUGH
    case AUDIO_SOURCE_DEFAULT:
    case AUDIO_SOURCE_MIC:
    case AUDIO_SOURCE_VOICE_RECOGNITION:
    case AUDIO_SOURCE_HOTWORD:
    case AUDIO_SOURCE_VOICE_COMMUNICATION:
        if (mForceUse[AudioSystem::FOR_RECORD] == AudioSystem::FORCE_BT_SCO &&
            mAvailableInputDevices & AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
            device = AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET;
        } else if (mAvailableInputDevices & AUDIO_DEVICE_IN_WIRED_HEADSET) {
            device = AUDIO_DEVICE_IN_WIRED_HEADSET;
        } else if (mAvailableInputDevices & AUDIO_DEVICE_IN_BUILTIN_MIC) {
            device = AUDIO_DEVICE_IN_BUILTIN_MIC;
        }
        break;
    case AUDIO_SOURCE_CAMCORDER:
        if (mAvailableInputDevices & AUDIO_DEVICE_IN_BACK_MIC) {
            device = AUDIO_DEVICE_IN_BACK_MIC;
        } else if (mAvailableInputDevices & AUDIO_DEVICE_IN_BUILTIN_MIC) {
            device = AUDIO_DEVICE_IN_BUILTIN_MIC;
        }
        break;
    case AUDIO_SOURCE_VOICE_DOWNLINK:
    case AUDIO_SOURCE_VOICE_CALL:
        if (mAvailableInputDevices & AUDIO_DEVICE_IN_VOICE_CALL) {
            device = AUDIO_DEVICE_IN_VOICE_CALL;
        }
        break;
    case AUDIO_SOURCE_REMOTE_SUBMIX:
        if (mAvailableInputDevices & AUDIO_DEVICE_IN_REMOTE_SUBMIX) {
            device = AUDIO_DEVICE_IN_REMOTE_SUBMIX;
        }
        break;

#ifdef QCOM_FM_ENABLED
    case AUDIO_SOURCE_FM_RX:
        device = AudioSystem::DEVICE_IN_FM_RX;
        break;
    case AUDIO_SOURCE_FM_RX_A2DP:
        device = AudioSystem::DEVICE_IN_FM_RX_A2DP;
        break;
#endif
    default:
        ALOGW("getDeviceForInputSource() invalid input source %d", inputSource);
        break;
    }
    ALOGV("getDeviceForInputSource()input source %d, device %08x", inputSource, device);
    return device;
}

status_t AudioPolicyManager::startOutput(audio_io_handle_t output,
                                             AudioSystem::stream_type stream,
                                             int session)
{
    ALOGV("startOutput() output %d, stream %d, session %d", output, stream, session);
    ssize_t index = mOutputs.indexOfKey(output);
    if (index < 0) {
        ALOGW("startOutput() unknow output %d", output);
        return BAD_VALUE;
    }

    AudioOutputDescriptor *outputDesc = mOutputs.valueAt(index);

    // increment usage count for this stream on the requested output:
    // NOTE that the usage count is the same for duplicated output and hardware output which is
    // necessary for a correct control of hardware output routing by startOutput() and stopOutput()
    outputDesc->changeRefCount(stream, 1);

    if (outputDesc->mRefCount[stream] == 1) {
        audio_devices_t newDevice = AudioPolicyManagerBase::getNewDevice(output, false /*fromCache*/);
        routing_strategy strategy = AudioPolicyManagerBase::getStrategy(stream);
        bool shouldWait = (strategy == STRATEGY_SONIFICATION) ||
                            (strategy == STRATEGY_SONIFICATION_RESPECTFUL);
        uint32_t waitMs = 0;
        bool force = false;
        uint32_t muteWaitMs;
        for (size_t i = 0; i < mOutputs.size(); i++) {
            AudioOutputDescriptor *desc = mOutputs.valueAt(i);
            if (desc != outputDesc) {
                // force a device change if any other output is managed by the same hw
                // module and has a current device selection that differs from selected device.
                // In this case, the audio HAL must receive the new device selection so that it can
                // change the device currently selected by the other active output.
                if (outputDesc->sharesHwModuleWith(desc) &&
                    desc->device() != newDevice) {
                    force = true;
                }
                // wait for audio on other active outputs to be presented when starting
                // a notification so that audio focus effect can propagate.
                uint32_t latency = desc->latency();
                if (shouldWait && desc->isActive(latency * 2) && (waitMs < latency)) {
                    waitMs = latency;
                }
            }
        }
    uint32_t NewDevice = (uint32_t)AudioPolicyManagerBase::getNewDevice(output, true);
#ifdef QCOM_FM_ENABLED
    if((stream == AudioSystem::SYSTEM) && (FM_ANALOG == getFMMode())
    && (NewDevice == AudioSystem::DEVICE_OUT_FM))
    {
        NewDevice |= AudioSystem::DEVICE_OUT_WIRED_HEADSET;
        ALOGE("Selecting AnlgFM + CODEC device %x",NewDevice);
        muteWaitMs = setOutputDevice(output, (audio_devices_t)NewDevice, true);
    }
    else
#endif
        muteWaitMs = setOutputDevice(output, (audio_devices_t)NewDevice, force);

        // handle special case for sonification while in call
        if (isInCall()) {
            AudioPolicyManagerBase::handleIncallSonification(stream, true, false);
        }

        // apply volume rules for current stream and device if necessary
        checkAndSetVolume(stream,
                          mStreams[stream].getVolumeIndex(newDevice),
                          output,
                          newDevice);

        // update the outputs if starting an output with a stream that can affect notification
        // routing
        handleNotificationRoutingForStream(stream);
        if (waitMs > muteWaitMs) {
            usleep((waitMs - muteWaitMs) * 2 * 1000);
        }
    }
    return NO_ERROR;
}

status_t AudioPolicyManager::stopOutput(audio_io_handle_t output,
                                            AudioSystem::stream_type stream,
                                            int session)
{
    ALOGV("stopOutput() output %d, stream %d, session %d", output, stream, session);
    ssize_t index = mOutputs.indexOfKey(output);
    if (index < 0) {
        ALOGW("stopOutput() unknow output %d", output);
        return BAD_VALUE;
    }

    AudioOutputDescriptor *outputDesc = mOutputs.valueAt(index);

    // handle special case for sonification while in call
    if (isInCall()) {
        handleIncallSonification(stream, false, false);
    }

    if (outputDesc->mRefCount[stream] > 0) {
        // decrement usage count of this stream on the output
        outputDesc->changeRefCount(stream, -1);
        // store time at which the stream was stopped - see isStreamActive()
        if (outputDesc->mRefCount[stream] == 0) {
            outputDesc->mStopTime[stream] = systemTime();
            audio_devices_t newDevice = getNewDevice(output, false /*fromCache*/);
            // delay the device switch by twice the latency because stopOutput() is executed when
            // the track stop() command is received and at that time the audio track buffer can
            // still contain data that needs to be drained. The latency only covers the audio HAL
            // and kernel buffers. Also the latency does not always include additional delay in the
            // audio path (audio DSP, CODEC ...)
         if(FM_ANALOG == getFMMode())
            setOutputDevice(output, newDevice,true);
        else
            setOutputDevice(output, newDevice, false, outputDesc->mLatency*2);

            // force restoring the device selection on other active outputs if it differs from the
            // one being selected for this output
            for (size_t i = 0; i < mOutputs.size(); i++) {
                audio_io_handle_t curOutput = mOutputs.keyAt(i);
                AudioOutputDescriptor *desc = mOutputs.valueAt(i);
                if (curOutput != output &&
                        desc->isActive() &&
                        outputDesc->sharesHwModuleWith(desc) &&
                        (newDevice != desc->device())) {
                    setOutputDevice(curOutput,
                                    getNewDevice(curOutput, false /*fromCache*/),
                                    true,
                                    outputDesc->mLatency*2);
                }
            }
            // update the outputs if stopping one with a stream that can affect notification routing
            handleNotificationRoutingForStream(stream);
        }
        return NO_ERROR;
    } else {
        ALOGW("stopOutput() refcount is already 0 for output %d", output);
        return INVALID_OPERATION;
    }
}


uint32_t AudioPolicyManager::setOutputDevice(audio_io_handle_t output, audio_devices_t device, bool force, int delayMs)
{
    ALOGV("setOutputDevice() output %d device %04x delayMs %d", output, device, delayMs);
    uint32_t muteWaitMs;
    AudioOutputDescriptor *outputDesc = mOutputs.valueFor(output);
    AudioParameter param;

    if (outputDesc->isDuplicated()) {
        muteWaitMs = setOutputDevice(outputDesc->mOutput1->mId, device, force, delayMs);
        muteWaitMs += setOutputDevice(outputDesc->mOutput2->mId, device, force, delayMs);
        return muteWaitMs;
    }
    // no need to proceed if new device is not AUDIO_DEVICE_NONE and not supported by current
    // output profile
    if ((device != AUDIO_DEVICE_NONE) &&
            ((device & outputDesc->mProfile->mSupportedDevices) == 0)) {
        return 0;
    }

    // filter devices according to output selected
    device = (audio_devices_t)(device & outputDesc->mProfile->mSupportedDevices);

    audio_devices_t prevDevice = outputDesc->mDevice;

    ALOGV("setOutputDevice() prevDevice %04x", prevDevice);

    if (device != 0) {
        outputDesc->mDevice = device;
    }
    muteWaitMs = checkDeviceMuteStrategies(outputDesc, prevDevice, delayMs);

    // Do not change the routing if:
    //  - the requested device is 0
    //  - the requested device is the same as current device and force is not specified.
    // Doing this check here allows the caller to call setOutputDevice() without conditions
    if (device == 0) {
        ALOGV("setOutputDevice() setting null device for output %d", output);
        return muteWaitMs;
    }

    ALOGV("setOutputDevice() changing device");
    // do the routing
    param.addInt(String8(AudioParameter::keyRouting), (int)device);
    mpClientInterface->setParameters(output, param.toString(), delayMs);

    // update stream volumes according to new device
    applyStreamVolumes(output, device, delayMs);

    return muteWaitMs;
}

audio_devices_t AudioPolicyManager::getDeviceForVolume(audio_devices_t device)
{
    if (device == 0) {
        // this happens when forcing a route update and no track is active on an output.
        // In this case the returned category is not important.
        device =  AUDIO_DEVICE_OUT_SPEAKER;
    } else if (AudioSystem::popCount(device) > 1) {
        // Multiple device selection is either:
        //  - speaker + one other device: give priority to speaker in this case.
        //  - one A2DP device + another device: happens with duplicated output. In this case
        // retain the device on the A2DP output as the other must not correspond to an active
        // selection if not the speaker.
        if (device & AUDIO_DEVICE_OUT_SPEAKER) {
            device = AUDIO_DEVICE_OUT_SPEAKER;
        }
#ifdef QCOM_FM_ENABLED
        else if (device & AUDIO_DEVICE_OUT_FM) {
            device = AUDIO_DEVICE_OUT_FM;
        }
#endif
        else if((device & AUDIO_DEVICE_OUT_WIRED_HEADSET) != 0) {
            device = AUDIO_DEVICE_OUT_WIRED_HEADSET;
        }
        else {
            device = (audio_devices_t)(device & AUDIO_DEVICE_OUT_ALL_A2DP);
        }
    }

    ALOGW_IF(AudioSystem::popCount(device) != 1,
            "getDeviceForVolume() invalid device combination: %08x",
            device);

    return device;
}

status_t AudioPolicyManager::checkAndSetVolume(int stream, int index, audio_io_handle_t output, audio_devices_t device, int delayMs, bool force)
{
    // do not change actual stream volume if the stream is muted
    if (mOutputs.valueFor(output)->mMuteCount[stream] != 0) {
        ALOGVV("checkAndSetVolume() stream %d muted count %d",
              stream, mOutputs.valueFor(output)->mMuteCount[stream]);
        return NO_ERROR;
    }

    // do not change in call volume if bluetooth is connected and vice versa
    if ((stream == AudioSystem::VOICE_CALL && mForceUse[AudioSystem::FOR_COMMUNICATION] == AudioSystem::FORCE_BT_SCO) ||
        (stream == AudioSystem::BLUETOOTH_SCO && mForceUse[AudioSystem::FOR_COMMUNICATION] != AudioSystem::FORCE_BT_SCO)) {
        ALOGV("checkAndSetVolume() cannot set stream %d volume with force use = %d for comm",
             stream, mForceUse[AudioSystem::FOR_COMMUNICATION]);
        return INVALID_OPERATION;
    }

    float volume = computeVolume(stream, index, output, device);
    // We actually change the volume if:
    // - the float value returned by computeVolume() changed
    // - the force flag is set
    if (volume != mOutputs.valueFor(output)->mCurVolume[stream] 
#ifdef QCOM_FM_ENABLED
            || (stream == AudioSystem::FM) 
#endif
            || force) {
        mOutputs.valueFor(output)->mCurVolume[stream] = volume;
        ALOGVV("checkAndSetVolume() for output %d stream %d, volume %f, delay %d", output, stream, volume, delayMs);
        if (stream == AudioSystem::VOICE_CALL ||
            stream == AudioSystem::DTMF ||
            stream == AudioSystem::BLUETOOTH_SCO) {
            // offset value to reflect actual hardware volume that never reaches 0
            // 1% corresponds roughly to first step in VOICE_CALL stream volume setting (see AudioService.java)
            volume = 0.01 + 0.99 * volume;
            // Force VOICE_CALL to track BLUETOOTH_SCO stream volume when bluetooth audio is
            // enabled
            if (stream == AudioSystem::BLUETOOTH_SCO) {
                mpClientInterface->setStreamVolume(AudioSystem::VOICE_CALL, volume, output, delayMs);
            }
        }

        mpClientInterface->setStreamVolume((AudioSystem::stream_type)stream, volume, output, delayMs);
    }

    if (stream == AudioSystem::VOICE_CALL ||
        stream == AudioSystem::BLUETOOTH_SCO) {
        float voiceVolume;
        // Force voice volume to max for bluetooth SCO as volume is managed by the headset
        if (stream == AudioSystem::VOICE_CALL) {
            voiceVolume = (float)index/(float)mStreams[stream].mIndexMax;
        } else {
            voiceVolume = 1.0;
        }

        if ((voiceVolume != mLastVoiceVolume && output == mPrimaryOutput) 
#ifdef QCOM_FM_ENABLED
	    && (!(mAvailableOutputDevices & AudioSystem::DEVICE_OUT_FM))
#endif
            ) {
            mpClientInterface->setVoiceVolume(voiceVolume, delayMs);
            mLastVoiceVolume = voiceVolume;
        }
#ifdef QCOM_FM_ENABLED
    } else if ((stream == AudioSystem::FM) && (mAvailableOutputDevices & AudioSystem::DEVICE_OUT_FM)) {
        float fmVolume = -1.0;
        fmVolume = (float)index/(float)mStreams[stream].mIndexMax;
        if (fmVolume >= 0 && output == mPrimaryOutput) {
            mpClientInterface->setFmVolume(fmVolume, delayMs);
            mLastVoiceVolume = fmVolume;
        }
#endif
      }
    return NO_ERROR;
}

void AudioPolicyManager::handleNotificationRoutingForStream(AudioSystem::stream_type stream) {
    switch(stream) {
    case AudioSystem::MUSIC:
        checkOutputForStrategy(STRATEGY_SONIFICATION_RESPECTFUL);
        updateDevicesAndOutputs();
        break;
    default:
        break;
    }
}

}; // namespace android
