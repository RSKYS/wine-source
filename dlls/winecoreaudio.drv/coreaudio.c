/*
 * Unixlib for winecoreaudio driver.
 *
 * Copyright 2011 Andrew Eikum for CodeWeavers
 * Copyright 2021 Huw Davies
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#if 0
#pragma makedep unix
#endif

#include "config.h"

#define LoadResource __carbon_LoadResource
#define CompareString __carbon_CompareString
#define GetCurrentThread __carbon_GetCurrentThread
#define GetCurrentProcess __carbon_GetCurrentProcess

#include <stdarg.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <fenv.h>
#include <unistd.h>

#include <libkern/OSAtomic.h>
#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioFormat.h>
#include <AudioToolbox/AudioConverter.h>
#include <AudioUnit/AudioUnit.h>

#undef LoadResource
#undef CompareString
#undef GetCurrentThread
#undef GetCurrentProcess
#undef _CDECL

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "mmdeviceapi.h"
#include "initguid.h"
#include "audioclient.h"
#include "wine/debug.h"
#include "wine/unicode.h"
#include "wine/unixlib.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(coreaudio);

struct coreaudio_stream
{
    OSSpinLock lock;
    AudioComponentInstance unit;
    AudioConverterRef converter;
    AudioStreamBasicDescription dev_desc; /* audio unit format, not necessarily the same as fmt */
    AudioDeviceID dev_id;
    EDataFlow flow;
    AUDCLNT_SHAREMODE share;

    BOOL playing;
    UINT32 period_ms, period_frames;
    UINT32 bufsize_frames, resamp_bufsize_frames;
    UINT32 lcl_offs_frames, held_frames, wri_offs_frames, tmp_buffer_frames;
    UINT32 cap_bufsize_frames, cap_offs_frames, cap_held_frames;
    UINT32 wrap_bufsize_frames;
    UINT64 written_frames;
    INT32 getbuf_last;
    WAVEFORMATEX *fmt;
    BYTE *local_buffer, *cap_buffer, *wrap_buffer, *resamp_buffer, *tmp_buffer;
    SIZE_T local_buffer_size, tmp_buffer_size;
};

static HRESULT osstatus_to_hresult(OSStatus sc)
{
    switch(sc){
    case kAudioFormatUnsupportedDataFormatError:
    case kAudioFormatUnknownFormatError:
    case kAudioDeviceUnsupportedFormatError:
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    case kAudioHardwareBadDeviceError:
        return AUDCLNT_E_DEVICE_INVALIDATED;
    }
    return E_FAIL;
}

/* copied from kernelbase */
static int muldiv( int a, int b, int c )
{
    LONGLONG ret;

    if (!c) return -1;

    /* We want to deal with a positive divisor to simplify the logic. */
    if (c < 0)
    {
        a = -a;
        c = -c;
    }

    /* If the result is positive, we "add" to round. else, we subtract to round. */
    if ((a < 0 && b < 0) || (a >= 0 && b >= 0))
        ret = (((LONGLONG)a * b) + (c / 2)) / c;
    else
        ret = (((LONGLONG)a * b) - (c / 2)) / c;

    if (ret > 2147483647 || ret < -2147483647) return -1;
    return ret;
}

static AudioObjectPropertyScope get_scope(EDataFlow flow)
{
    return (flow == eRender) ? kAudioDevicePropertyScopeOutput : kAudioDevicePropertyScopeInput;
}

static BOOL device_has_channels(AudioDeviceID device, EDataFlow flow)
{
    AudioObjectPropertyAddress addr;
    AudioBufferList *buffers;
    BOOL ret = FALSE;
    OSStatus sc;
    UInt32 size;
    int i;

    addr.mSelector = kAudioDevicePropertyStreamConfiguration;
    addr.mScope = get_scope(flow);
    addr.mElement = 0;

    sc = AudioObjectGetPropertyDataSize(device, &addr, 0, NULL, &size);
    if(sc != noErr){
        WARN("Unable to get _StreamConfiguration property size for device %u: %x\n",
             (unsigned int)device, (int)sc);
        return FALSE;
    }

    buffers = malloc(size);
    if(!buffers) return FALSE;

    sc = AudioObjectGetPropertyData(device, &addr, 0, NULL, &size, buffers);
    if(sc != noErr){
        WARN("Unable to get _StreamConfiguration property for device %u: %x\n",
             (unsigned int)device, (int)sc);
        free(buffers);
        return FALSE;
    }

    for(i = 0; i < buffers->mNumberBuffers; i++){
        if(buffers->mBuffers[i].mNumberChannels > 0){
            ret = TRUE;
            break;
        }
    }
    free(buffers);
    return ret;
}

static NTSTATUS get_endpoint_ids(void *args)
{
    struct get_endpoint_ids_params *params = args;
    unsigned int num_devices, i, needed;
    AudioDeviceID *devices, default_id;
    AudioObjectPropertyAddress addr;
    struct endpoint *endpoint;
    UInt32 devsize, size;
    struct endpoint_info
    {
        CFStringRef name;
        AudioDeviceID id;
    } *info;
    OSStatus sc;
    WCHAR *ptr;

    params->num = 0;
    params->default_idx = 0;

    addr.mScope = kAudioObjectPropertyScopeGlobal;
    addr.mElement = kAudioObjectPropertyElementMaster;
    if(params->flow == eRender) addr.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
    else if(params->flow == eCapture) addr.mSelector = kAudioHardwarePropertyDefaultInputDevice;
    else{
        params->result = E_INVALIDARG;
        return STATUS_SUCCESS;
    }

    size = sizeof(default_id);
    sc = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, &default_id);
    if(sc != noErr){
        WARN("Getting _DefaultInputDevice property failed: %x\n", (int)sc);
        default_id = -1;
    }

    addr.mSelector = kAudioHardwarePropertyDevices;
    sc = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &devsize);
    if(sc != noErr){
        WARN("Getting _Devices property size failed: %x\n", (int)sc);
        params->result = osstatus_to_hresult(sc);
        return STATUS_SUCCESS;
    }

    num_devices = devsize / sizeof(AudioDeviceID);
    devices = malloc(devsize);
    info = malloc(num_devices * sizeof(*info));
    if(!devices || !info){
        free(info);
        free(devices);
        params->result = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }

    sc = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &devsize, devices);
    if(sc != noErr){
        WARN("Getting _Devices property failed: %x\n", (int)sc);
        free(info);
        free(devices);
        params->result = osstatus_to_hresult(sc);
        return STATUS_SUCCESS;
    }

    addr.mSelector = kAudioObjectPropertyName;
    addr.mScope = get_scope(params->flow);
    addr.mElement = 0;

    for(i = 0; i < num_devices; i++){
        if(!device_has_channels(devices[i], params->flow)) continue;

        size = sizeof(CFStringRef);
        sc = AudioObjectGetPropertyData(devices[i], &addr, 0, NULL, &size, &info[params->num].name);
        if(sc != noErr){
            WARN("Unable to get _Name property for device %u: %x\n",
                 (unsigned int)devices[i], (int)sc);
            continue;
        }
        info[params->num++].id = devices[i];
    }
    free(devices);

    needed = sizeof(*endpoint) * params->num;
    endpoint = params->endpoints;
    ptr = (WCHAR *)(endpoint + params->num);

    for(i = 0; i < params->num; i++){
        SIZE_T len = CFStringGetLength(info[i].name);
        needed += (len + 1) * sizeof(WCHAR);

        if(needed <= params->size){
            endpoint->name = ptr;
            CFStringGetCharacters(info[i].name, CFRangeMake(0, len), (UniChar*)endpoint->name);
            ptr[len] = 0;
            endpoint->id = info[i].id;
            endpoint++;
            ptr += len + 1;
        }
        CFRelease(info[i].name);
        if(info[i].id == default_id) params->default_idx = i;
    }
    free(info);

    if(needed > params->size){
        params->size = needed;
        params->result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    else params->result = S_OK;

    return STATUS_SUCCESS;
}

static WAVEFORMATEX *clone_format(const WAVEFORMATEX *fmt)
{
    WAVEFORMATEX *ret;
    size_t size;

    if(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        size = sizeof(WAVEFORMATEXTENSIBLE);
    else
        size = sizeof(WAVEFORMATEX);

    ret = malloc(size);
    if(!ret)
        return NULL;

    memcpy(ret, fmt, size);

    ret->cbSize = size - sizeof(WAVEFORMATEX);

    return ret;
}

static void silence_buffer(struct coreaudio_stream *stream, BYTE *buffer, UINT32 frames)
{
    WAVEFORMATEXTENSIBLE *fmtex = (WAVEFORMATEXTENSIBLE*)stream->fmt;
    if((stream->fmt->wFormatTag == WAVE_FORMAT_PCM ||
        (stream->fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))) &&
       stream->fmt->wBitsPerSample == 8)
        memset(buffer, 128, frames * stream->fmt->nBlockAlign);
    else
        memset(buffer, 0, frames * stream->fmt->nBlockAlign);
}

/* CA is pulling data from us */
static OSStatus ca_render_cb(void *user, AudioUnitRenderActionFlags *flags,
        const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes,
        AudioBufferList *data)
{
    struct coreaudio_stream *stream = user;
    UINT32 to_copy_bytes, to_copy_frames, chunk_bytes, lcl_offs_bytes;

    OSSpinLockLock(&stream->lock);

    if(stream->playing){
        lcl_offs_bytes = stream->lcl_offs_frames * stream->fmt->nBlockAlign;
        to_copy_frames = min(nframes, stream->held_frames);
        to_copy_bytes = to_copy_frames * stream->fmt->nBlockAlign;

        chunk_bytes = (stream->bufsize_frames - stream->lcl_offs_frames) * stream->fmt->nBlockAlign;

        if(to_copy_bytes > chunk_bytes){
            memcpy(data->mBuffers[0].mData, stream->local_buffer + lcl_offs_bytes, chunk_bytes);
            memcpy(((BYTE *)data->mBuffers[0].mData) + chunk_bytes, stream->local_buffer, to_copy_bytes - chunk_bytes);
        }else
            memcpy(data->mBuffers[0].mData, stream->local_buffer + lcl_offs_bytes, to_copy_bytes);

        stream->lcl_offs_frames += to_copy_frames;
        stream->lcl_offs_frames %= stream->bufsize_frames;
        stream->held_frames -= to_copy_frames;
    }else
        to_copy_bytes = to_copy_frames = 0;

    if(nframes > to_copy_frames)
        silence_buffer(stream, ((BYTE *)data->mBuffers[0].mData) + to_copy_bytes, nframes - to_copy_frames);

    OSSpinLockUnlock(&stream->lock);

    return noErr;
}

static void ca_wrap_buffer(BYTE *dst, UINT32 dst_offs, UINT32 dst_bytes,
                           BYTE *src, UINT32 src_bytes)
{
    UINT32 chunk_bytes = dst_bytes - dst_offs;

    if(chunk_bytes < src_bytes){
        memcpy(dst + dst_offs, src, chunk_bytes);
        memcpy(dst, src + chunk_bytes, src_bytes - chunk_bytes);
    }else
        memcpy(dst + dst_offs, src, src_bytes);
}

/* we need to trigger CA to pull data from the device and give it to us
 *
 * raw data from CA is stored in cap_buffer, possibly via wrap_buffer
 *
 * raw data is resampled from cap_buffer into resamp_buffer in period-size
 * chunks and copied to local_buffer
 */
static OSStatus ca_capture_cb(void *user, AudioUnitRenderActionFlags *flags,
                              const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes,
                              AudioBufferList *data)
{
    struct coreaudio_stream *stream = user;
    AudioBufferList list;
    OSStatus sc;
    UINT32 cap_wri_offs_frames;

    OSSpinLockLock(&stream->lock);

    cap_wri_offs_frames = (stream->cap_offs_frames + stream->cap_held_frames) % stream->cap_bufsize_frames;

    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = stream->fmt->nChannels;
    list.mBuffers[0].mDataByteSize = nframes * stream->fmt->nBlockAlign;

    if(!stream->playing || cap_wri_offs_frames + nframes > stream->cap_bufsize_frames){
        if(stream->wrap_bufsize_frames < nframes){
            free(stream->wrap_buffer);
            stream->wrap_buffer = malloc(list.mBuffers[0].mDataByteSize);
            stream->wrap_bufsize_frames = nframes;
        }

        list.mBuffers[0].mData = stream->wrap_buffer;
    }else
        list.mBuffers[0].mData = stream->cap_buffer + cap_wri_offs_frames * stream->fmt->nBlockAlign;

    sc = AudioUnitRender(stream->unit, flags, ts, bus, nframes, &list);
    if(sc != noErr){
        OSSpinLockUnlock(&stream->lock);
        return sc;
    }

    if(stream->playing){
        if(list.mBuffers[0].mData == stream->wrap_buffer){
            ca_wrap_buffer(stream->cap_buffer,
                    cap_wri_offs_frames * stream->fmt->nBlockAlign,
                    stream->cap_bufsize_frames * stream->fmt->nBlockAlign,
                    stream->wrap_buffer, list.mBuffers[0].mDataByteSize);
        }

        stream->cap_held_frames += list.mBuffers[0].mDataByteSize / stream->fmt->nBlockAlign;
        if(stream->cap_held_frames > stream->cap_bufsize_frames){
            stream->cap_offs_frames += stream->cap_held_frames % stream->cap_bufsize_frames;
            stream->cap_offs_frames %= stream->cap_bufsize_frames;
            stream->cap_held_frames = stream->cap_bufsize_frames;
        }
    }

    OSSpinLockUnlock(&stream->lock);
    return noErr;
}

static AudioComponentInstance get_audiounit(EDataFlow dataflow, AudioDeviceID adevid)
{
    AudioComponentInstance unit;
    AudioComponent comp;
    AudioComponentDescription desc;
    OSStatus sc;

    memset(&desc, 0, sizeof(desc));
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    if(!(comp = AudioComponentFindNext(NULL, &desc))){
        WARN("AudioComponentFindNext failed\n");
        return NULL;
    }

    sc = AudioComponentInstanceNew(comp, &unit);
    if(sc != noErr){
        WARN("AudioComponentInstanceNew failed: %x\n", (int)sc);
        return NULL;
    }

    if(dataflow == eCapture){
        UInt32 enableio;

        enableio = 1;
        sc = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                kAudioUnitScope_Input, 1, &enableio, sizeof(enableio));
        if(sc != noErr){
            WARN("Couldn't enable I/O on input element: %x\n", (int)sc);
            AudioComponentInstanceDispose(unit);
            return NULL;
        }

        enableio = 0;
        sc = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                kAudioUnitScope_Output, 0, &enableio, sizeof(enableio));
        if(sc != noErr){
            WARN("Couldn't disable I/O on output element: %x\n", (int)sc);
            AudioComponentInstanceDispose(unit);
            return NULL;
        }
    }

    sc = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
            kAudioUnitScope_Global, 0, &adevid, sizeof(adevid));
    if(sc != noErr){
        WARN("Couldn't set audio unit device\n");
        AudioComponentInstanceDispose(unit);
        return NULL;
    }

    return unit;
}

static void dump_adesc(const char *aux, AudioStreamBasicDescription *desc)
{
    TRACE("%s: mSampleRate: %f\n", aux, desc->mSampleRate);
    TRACE("%s: mBytesPerPacket: %u\n", aux, (unsigned int)desc->mBytesPerPacket);
    TRACE("%s: mFramesPerPacket: %u\n", aux, (unsigned int)desc->mFramesPerPacket);
    TRACE("%s: mBytesPerFrame: %u\n", aux, (unsigned int)desc->mBytesPerFrame);
    TRACE("%s: mChannelsPerFrame: %u\n", aux, (unsigned int)desc->mChannelsPerFrame);
    TRACE("%s: mBitsPerChannel: %u\n", aux, (unsigned int)desc->mBitsPerChannel);
}

static HRESULT ca_get_audiodesc(AudioStreamBasicDescription *desc,
                                const WAVEFORMATEX *fmt)
{
    const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;

    desc->mFormatFlags = 0;

    if(fmt->wFormatTag == WAVE_FORMAT_PCM ||
            (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))){
        desc->mFormatID = kAudioFormatLinearPCM;
        if(fmt->wBitsPerSample > 8)
            desc->mFormatFlags = kAudioFormatFlagIsSignedInteger;
    }else if(fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))){
        desc->mFormatID = kAudioFormatLinearPCM;
        desc->mFormatFlags = kAudioFormatFlagIsFloat;
    }else if(fmt->wFormatTag == WAVE_FORMAT_MULAW ||
            (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_MULAW))){
        desc->mFormatID = kAudioFormatULaw;
    }else if(fmt->wFormatTag == WAVE_FORMAT_ALAW ||
            (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_ALAW))){
        desc->mFormatID = kAudioFormatALaw;
    }else
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    desc->mSampleRate = fmt->nSamplesPerSec;
    desc->mBytesPerPacket = fmt->nBlockAlign;
    desc->mFramesPerPacket = 1;
    desc->mBytesPerFrame = fmt->nBlockAlign;
    desc->mChannelsPerFrame = fmt->nChannels;
    desc->mBitsPerChannel = fmt->wBitsPerSample;
    desc->mReserved = 0;

    return S_OK;
}

static HRESULT ca_setup_audiounit(EDataFlow dataflow, AudioComponentInstance unit,
                                  const WAVEFORMATEX *fmt, AudioStreamBasicDescription *dev_desc,
                                  AudioConverterRef *converter)
{
    OSStatus sc;
    HRESULT hr;

    if(dataflow == eCapture){
        AudioStreamBasicDescription desc;
        UInt32 size;
        Float64 rate;
        fenv_t fenv;
        BOOL fenv_stored = TRUE;

        hr = ca_get_audiodesc(&desc, fmt);
        if(FAILED(hr))
            return hr;
        dump_adesc("requested", &desc);

        /* input-only units can't perform sample rate conversion, so we have to
         * set up our own AudioConverter to support arbitrary sample rates. */
        size = sizeof(*dev_desc);
        sc = AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input, 1, dev_desc, &size);
        if(sc != noErr){
            WARN("Couldn't get unit format: %x\n", (int)sc);
            return osstatus_to_hresult(sc);
        }
        dump_adesc("hardware", dev_desc);

        rate = dev_desc->mSampleRate;
        *dev_desc = desc;
        dev_desc->mSampleRate = rate;

        dump_adesc("final", dev_desc);
        sc = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Output, 1, dev_desc, sizeof(*dev_desc));
        if(sc != noErr){
            WARN("Couldn't set unit format: %x\n", (int)sc);
            return osstatus_to_hresult(sc);
        }

        /* AudioConverterNew requires divide-by-zero SSE exceptions to be masked */
        if(feholdexcept(&fenv)){
            WARN("Failed to store fenv state\n");
            fenv_stored = FALSE;
        }

        sc = AudioConverterNew(dev_desc, &desc, converter);

        if(fenv_stored && fesetenv(&fenv))
            WARN("Failed to restore fenv state\n");

        if(sc != noErr){
            WARN("Couldn't create audio converter: %x\n", (int)sc);
            return osstatus_to_hresult(sc);
        }
    }else{
        hr = ca_get_audiodesc(dev_desc, fmt);
        if(FAILED(hr))
            return hr;

        dump_adesc("final", dev_desc);
        sc = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input, 0, dev_desc, sizeof(*dev_desc));
        if(sc != noErr){
            WARN("Couldn't set format: %x\n", (int)sc);
            return osstatus_to_hresult(sc);
        }
    }

    return S_OK;
}

static NTSTATUS create_stream(void *args)
{
    struct create_stream_params *params = args;
    struct coreaudio_stream *stream = calloc(1, sizeof(*stream));
    AURenderCallbackStruct input;
    OSStatus sc;

    if(!stream){
        params->result = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }

    stream->fmt = clone_format(params->fmt);
    if(!stream->fmt){
        params->result = E_OUTOFMEMORY;
        goto end;
    }

    stream->period_ms = params->period / 10000;
    stream->period_frames = muldiv(params->period, stream->fmt->nSamplesPerSec, 10000000);
    stream->dev_id = params->dev_id;
    stream->flow = params->flow;
    stream->share = params->share;

    stream->bufsize_frames = muldiv(params->duration, stream->fmt->nSamplesPerSec, 10000000);
    if(params->share == AUDCLNT_SHAREMODE_EXCLUSIVE)
        stream->bufsize_frames -= stream->bufsize_frames % stream->period_frames;

    if(!(stream->unit = get_audiounit(stream->flow, stream->dev_id))){
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        goto end;
    }

    params->result = ca_setup_audiounit(stream->flow, stream->unit, stream->fmt, &stream->dev_desc, &stream->converter);
    if(FAILED(params->result)) goto end;

    input.inputProcRefCon = stream;
    if(stream->flow == eCapture){
        input.inputProc = ca_capture_cb;
        sc = AudioUnitSetProperty(stream->unit, kAudioOutputUnitProperty_SetInputCallback,
                                  kAudioUnitScope_Output, 1, &input, sizeof(input));
    }else{
        input.inputProc = ca_render_cb;
        sc = AudioUnitSetProperty(stream->unit, kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Input, 0, &input, sizeof(input));
    }
    if(sc != noErr){
        WARN("Couldn't set callback: %x\n", (int)sc);
        params->result = osstatus_to_hresult(sc);
        goto end;
    }

    sc = AudioUnitInitialize(stream->unit);
    if(sc != noErr){
        WARN("Couldn't initialize: %x\n", (int)sc);
        params->result = osstatus_to_hresult(sc);
        goto end;
    }

    /* we play audio continuously because AudioOutputUnitStart sometimes takes
     * a while to return */
    sc = AudioOutputUnitStart(stream->unit);
    if(sc != noErr){
        WARN("Unit failed to start: %x\n", (int)sc);
        params->result = osstatus_to_hresult(sc);
        goto end;
    }

    stream->local_buffer_size = stream->bufsize_frames * stream->fmt->nBlockAlign;
    if(NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer, 0, &stream->local_buffer_size,
                               MEM_COMMIT, PAGE_READWRITE)){
        params->result = E_OUTOFMEMORY;
        goto end;
    }
    silence_buffer(stream, stream->local_buffer, stream->bufsize_frames);

    if(stream->flow == eCapture){
        stream->cap_bufsize_frames = muldiv(params->duration, stream->dev_desc.mSampleRate, 10000000);
        stream->cap_buffer = malloc(stream->cap_bufsize_frames * stream->fmt->nBlockAlign);
    }
    params->result = S_OK;

end:
    if(FAILED(params->result)){
        if(stream->converter) AudioConverterDispose(stream->converter);
        if(stream->unit) AudioComponentInstanceDispose(stream->unit);
        free(stream->fmt);
        free(stream);
    } else
        params->stream = stream;

    return STATUS_SUCCESS;
}

static NTSTATUS release_stream( void *args )
{
    struct release_stream_params *params = args;
    struct coreaudio_stream *stream = params->stream;

    if(stream->unit){
        AudioOutputUnitStop(stream->unit);
        AudioComponentInstanceDispose(stream->unit);
    }

    if(stream->converter) AudioConverterDispose(stream->converter);
    free(stream->resamp_buffer);
    free(stream->wrap_buffer);
    free(stream->cap_buffer);
    if(stream->local_buffer)
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer,
                            &stream->local_buffer_size, MEM_RELEASE);
    if(stream->tmp_buffer)
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer,
                            &stream->tmp_buffer_size, MEM_RELEASE);
    free(stream->fmt);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static DWORD ca_channel_layout_to_channel_mask(const AudioChannelLayout *layout)
{
    int i;
    DWORD mask = 0;

    for (i = 0; i < layout->mNumberChannelDescriptions; ++i) {
        switch (layout->mChannelDescriptions[i].mChannelLabel) {
            default: FIXME("Unhandled channel 0x%x\n",
                           (unsigned int)layout->mChannelDescriptions[i].mChannelLabel); break;
            case kAudioChannelLabel_Left: mask |= SPEAKER_FRONT_LEFT; break;
            case kAudioChannelLabel_Mono:
            case kAudioChannelLabel_Center: mask |= SPEAKER_FRONT_CENTER; break;
            case kAudioChannelLabel_Right: mask |= SPEAKER_FRONT_RIGHT; break;
            case kAudioChannelLabel_LeftSurround: mask |= SPEAKER_BACK_LEFT; break;
            case kAudioChannelLabel_CenterSurround: mask |= SPEAKER_BACK_CENTER; break;
            case kAudioChannelLabel_RightSurround: mask |= SPEAKER_BACK_RIGHT; break;
            case kAudioChannelLabel_LFEScreen: mask |= SPEAKER_LOW_FREQUENCY; break;
            case kAudioChannelLabel_LeftSurroundDirect: mask |= SPEAKER_SIDE_LEFT; break;
            case kAudioChannelLabel_RightSurroundDirect: mask |= SPEAKER_SIDE_RIGHT; break;
            case kAudioChannelLabel_TopCenterSurround: mask |= SPEAKER_TOP_CENTER; break;
            case kAudioChannelLabel_VerticalHeightLeft: mask |= SPEAKER_TOP_FRONT_LEFT; break;
            case kAudioChannelLabel_VerticalHeightCenter: mask |= SPEAKER_TOP_FRONT_CENTER; break;
            case kAudioChannelLabel_VerticalHeightRight: mask |= SPEAKER_TOP_FRONT_RIGHT; break;
            case kAudioChannelLabel_TopBackLeft: mask |= SPEAKER_TOP_BACK_LEFT; break;
            case kAudioChannelLabel_TopBackCenter: mask |= SPEAKER_TOP_BACK_CENTER; break;
            case kAudioChannelLabel_TopBackRight: mask |= SPEAKER_TOP_BACK_RIGHT; break;
            case kAudioChannelLabel_LeftCenter: mask |= SPEAKER_FRONT_LEFT_OF_CENTER; break;
            case kAudioChannelLabel_RightCenter: mask |= SPEAKER_FRONT_RIGHT_OF_CENTER; break;
        }
    }

    return mask;
}

/* For most hardware on Windows, users must choose a configuration with an even
 * number of channels (stereo, quad, 5.1, 7.1). Users can then disable
 * channels, but those channels are still reported to applications from
 * GetMixFormat! Some applications behave badly if given an odd number of
 * channels (e.g. 2.1).  Here, we find the nearest configuration that Windows
 * would report for a given channel layout. */
static void convert_channel_layout(const AudioChannelLayout *ca_layout, WAVEFORMATEXTENSIBLE *fmt)
{
    DWORD ca_mask = ca_channel_layout_to_channel_mask(ca_layout);

    TRACE("Got channel mask for CA: 0x%x\n", ca_mask);

    if (ca_layout->mNumberChannelDescriptions == 1)
    {
        fmt->Format.nChannels = 1;
        fmt->dwChannelMask = ca_mask;
        return;
    }

    /* compare against known configurations and find smallest configuration
     * which is a superset of the given speakers */

    if (ca_layout->mNumberChannelDescriptions <= 2 &&
            (ca_mask & ~KSAUDIO_SPEAKER_STEREO) == 0)
    {
        fmt->Format.nChannels = 2;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_STEREO;
        return;
    }

    if (ca_layout->mNumberChannelDescriptions <= 4 &&
            (ca_mask & ~KSAUDIO_SPEAKER_QUAD) == 0)
    {
        fmt->Format.nChannels = 4;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_QUAD;
        return;
    }

    if (ca_layout->mNumberChannelDescriptions <= 4 &&
            (ca_mask & ~KSAUDIO_SPEAKER_SURROUND) == 0)
    {
        fmt->Format.nChannels = 4;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_SURROUND;
        return;
    }

    if (ca_layout->mNumberChannelDescriptions <= 6 &&
            (ca_mask & ~KSAUDIO_SPEAKER_5POINT1) == 0)
    {
        fmt->Format.nChannels = 6;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_5POINT1;
        return;
    }

    if (ca_layout->mNumberChannelDescriptions <= 6 &&
            (ca_mask & ~KSAUDIO_SPEAKER_5POINT1_SURROUND) == 0)
    {
        fmt->Format.nChannels = 6;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_5POINT1_SURROUND;
        return;
    }

    if (ca_layout->mNumberChannelDescriptions <= 8 &&
            (ca_mask & ~KSAUDIO_SPEAKER_7POINT1) == 0)
    {
        fmt->Format.nChannels = 8;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_7POINT1;
        return;
    }

    if (ca_layout->mNumberChannelDescriptions <= 8 &&
            (ca_mask & ~KSAUDIO_SPEAKER_7POINT1_SURROUND) == 0)
    {
        fmt->Format.nChannels = 8;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND;
        return;
    }

    /* oddball format, report truthfully */
    fmt->Format.nChannels = ca_layout->mNumberChannelDescriptions;
    fmt->dwChannelMask = ca_mask;
}

static DWORD get_channel_mask(unsigned int channels)
{
    switch(channels){
    case 0:
        return 0;
    case 1:
        return KSAUDIO_SPEAKER_MONO;
    case 2:
        return KSAUDIO_SPEAKER_STEREO;
    case 3:
        return KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY;
    case 4:
        return KSAUDIO_SPEAKER_QUAD;    /* not _SURROUND */
    case 5:
        return KSAUDIO_SPEAKER_QUAD | SPEAKER_LOW_FREQUENCY;
    case 6:
        return KSAUDIO_SPEAKER_5POINT1; /* not 5POINT1_SURROUND */
    case 7:
        return KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER;
    case 8:
        return KSAUDIO_SPEAKER_7POINT1_SURROUND; /* Vista deprecates 7POINT1 */
    }
    FIXME("Unknown speaker configuration: %u\n", channels);
    return 0;
}

static NTSTATUS get_mix_format(void *args)
{
    struct get_mix_format_params *params = args;
    AudioObjectPropertyAddress addr;
    AudioChannelLayout *layout;
    AudioBufferList *buffers;
    Float64 rate;
    UInt32 size;
    OSStatus sc;
    int i;

    params->fmt->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;

    addr.mScope = get_scope(params->flow);
    addr.mElement = 0;
    addr.mSelector = kAudioDevicePropertyPreferredChannelLayout;

    sc = AudioObjectGetPropertyDataSize(params->dev_id, &addr, 0, NULL, &size);
    if(sc == noErr){
        layout = malloc(size);
        sc = AudioObjectGetPropertyData(params->dev_id, &addr, 0, NULL, &size, layout);
        if(sc == noErr){
            TRACE("Got channel layout: {tag: 0x%x, bitmap: 0x%x, num_descs: %u}\n",
                  (unsigned int)layout->mChannelLayoutTag, (unsigned int)layout->mChannelBitmap,
                  (unsigned int)layout->mNumberChannelDescriptions);

            if(layout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions){
                convert_channel_layout(layout, params->fmt);
            }else{
                WARN("Haven't implemented support for this layout tag: 0x%x, guessing at layout\n",
                     (unsigned int)layout->mChannelLayoutTag);
                params->fmt->Format.nChannels = 0;
            }
        }else{
            TRACE("Unable to get _PreferredChannelLayout property: %x, guessing at layout\n", (int)sc);
            params->fmt->Format.nChannels = 0;
        }

        free(layout);
    }else{
        TRACE("Unable to get size for _PreferredChannelLayout property: %x, guessing at layout\n", (int)sc);
        params->fmt->Format.nChannels = 0;
    }

    if(params->fmt->Format.nChannels == 0){
        addr.mScope = get_scope(params->flow);
        addr.mElement = 0;
        addr.mSelector = kAudioDevicePropertyStreamConfiguration;

        sc = AudioObjectGetPropertyDataSize(params->dev_id, &addr, 0, NULL, &size);
        if(sc != noErr){
            WARN("Unable to get size for _StreamConfiguration property: %x\n", (int)sc);
            params->result = osstatus_to_hresult(sc);
            return STATUS_SUCCESS;
        }

        buffers = malloc(size);
        if(!buffers){
            params->result = E_OUTOFMEMORY;
            return STATUS_SUCCESS;
        }

        sc = AudioObjectGetPropertyData(params->dev_id, &addr, 0, NULL, &size, buffers);
        if(sc != noErr){
            free(buffers);
            WARN("Unable to get _StreamConfiguration property: %x\n", (int)sc);
            params->result = osstatus_to_hresult(sc);
            return STATUS_SUCCESS;
        }

        for(i = 0; i < buffers->mNumberBuffers; ++i)
            params->fmt->Format.nChannels += buffers->mBuffers[i].mNumberChannels;

        free(buffers);

        params->fmt->dwChannelMask = get_channel_mask(params->fmt->Format.nChannels);
    }

    addr.mSelector = kAudioDevicePropertyNominalSampleRate;
    size = sizeof(Float64);
    sc = AudioObjectGetPropertyData(params->dev_id, &addr, 0, NULL, &size, &rate);
    if(sc != noErr){
        WARN("Unable to get _NominalSampleRate property: %x\n", (int)sc);
        params->result = osstatus_to_hresult(sc);
        return STATUS_SUCCESS;
    }
    params->fmt->Format.nSamplesPerSec = rate;

    params->fmt->Format.wBitsPerSample = 32;
    params->fmt->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    params->fmt->Format.nBlockAlign = (params->fmt->Format.wBitsPerSample *
                                       params->fmt->Format.nChannels) / 8;
    params->fmt->Format.nAvgBytesPerSec = params->fmt->Format.nSamplesPerSec *
        params->fmt->Format.nBlockAlign;

    params->fmt->Samples.wValidBitsPerSample = params->fmt->Format.wBitsPerSample;
    params->fmt->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS is_format_supported(void *args)
{
    struct is_format_supported_params *params = args;
    const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)params->fmt_in;
    AudioStreamBasicDescription dev_desc;
    AudioConverterRef converter;
    AudioComponentInstance unit;

    params->result = S_OK;

    if(!params->fmt_in || (params->share == AUDCLNT_SHAREMODE_SHARED && !params->fmt_out))
        params->result = E_POINTER;
    else if(params->share != AUDCLNT_SHAREMODE_SHARED && params->share != AUDCLNT_SHAREMODE_EXCLUSIVE)
        params->result = E_INVALIDARG;
    else if(params->fmt_in->wFormatTag == WAVE_FORMAT_EXTENSIBLE){
        if(params->fmt_in->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
            params->result = E_INVALIDARG;
        else if(params->fmt_in->nAvgBytesPerSec == 0 || params->fmt_in->nBlockAlign == 0 ||
                fmtex->Samples.wValidBitsPerSample > params->fmt_in->wBitsPerSample)
            params->result = E_INVALIDARG;
        else if(fmtex->Samples.wValidBitsPerSample < params->fmt_in->wBitsPerSample)
            goto unsupported;
        else if(params->share == AUDCLNT_SHAREMODE_EXCLUSIVE &&
                (fmtex->dwChannelMask == 0 || fmtex->dwChannelMask & SPEAKER_RESERVED))
            goto unsupported;
    }
    if(FAILED(params->result)) return STATUS_SUCCESS;

    if(params->fmt_in->nBlockAlign != params->fmt_in->nChannels * params->fmt_in->wBitsPerSample / 8 ||
       params->fmt_in->nAvgBytesPerSec != params->fmt_in->nBlockAlign * params->fmt_in->nSamplesPerSec)
        goto unsupported;

    if(params->fmt_in->nChannels == 0){
        params->result = AUDCLNT_E_UNSUPPORTED_FORMAT;
        return STATUS_SUCCESS;
    }
    unit = get_audiounit(params->flow, params->dev_id);

    converter = NULL;
    params->result = ca_setup_audiounit(params->flow, unit, params->fmt_in, &dev_desc, &converter);
    AudioComponentInstanceDispose(unit);
    if(FAILED(params->result)) goto unsupported;
    if(converter) AudioConverterDispose(converter);

    params->result = S_OK;
    return STATUS_SUCCESS;

unsupported:
    if(params->fmt_out){
        struct get_mix_format_params get_mix_params =
        {
            .flow = params->flow,
            .dev_id = params->dev_id,
            .fmt = params->fmt_out,
        };

        get_mix_format(&get_mix_params);
        params->result = get_mix_params.result;
        if(SUCCEEDED(params->result)) params->result = S_FALSE;
    }
    else params->result = AUDCLNT_E_UNSUPPORTED_FORMAT;
    return STATUS_SUCCESS;
}

static UINT buf_ptr_diff(UINT left, UINT right, UINT bufsize)
{
    if(left <= right)
        return right - left;
    return bufsize - (left - right);
}

/* place data from cap_buffer into provided AudioBufferList */
static OSStatus feed_cb(AudioConverterRef converter, UInt32 *nframes, AudioBufferList *data,
                        AudioStreamPacketDescription **packets, void *user)
{
    struct coreaudio_stream *stream = user;

    *nframes = min(*nframes, stream->cap_held_frames);
    if(!*nframes){
        data->mBuffers[0].mData = NULL;
        data->mBuffers[0].mDataByteSize = 0;
        data->mBuffers[0].mNumberChannels = stream->fmt->nChannels;
        return noErr;
    }

    data->mBuffers[0].mDataByteSize = *nframes * stream->fmt->nBlockAlign;
    data->mBuffers[0].mNumberChannels = stream->fmt->nChannels;

    if(stream->cap_offs_frames + *nframes > stream->cap_bufsize_frames){
        UINT32 chunk_frames = stream->cap_bufsize_frames - stream->cap_offs_frames;

        if(stream->wrap_bufsize_frames < *nframes){
            free(stream->wrap_buffer);
            stream->wrap_buffer = malloc(data->mBuffers[0].mDataByteSize);
            stream->wrap_bufsize_frames = *nframes;
        }

        memcpy(stream->wrap_buffer, stream->cap_buffer + stream->cap_offs_frames * stream->fmt->nBlockAlign,
               chunk_frames * stream->fmt->nBlockAlign);
        memcpy(stream->wrap_buffer + chunk_frames * stream->fmt->nBlockAlign, stream->cap_buffer,
               (*nframes - chunk_frames) * stream->fmt->nBlockAlign);

        data->mBuffers[0].mData = stream->wrap_buffer;
    }else
        data->mBuffers[0].mData = stream->cap_buffer + stream->cap_offs_frames * stream->fmt->nBlockAlign;

    stream->cap_offs_frames += *nframes;
    stream->cap_offs_frames %= stream->cap_bufsize_frames;
    stream->cap_held_frames -= *nframes;

    if(packets)
        *packets = NULL;

    return noErr;
}

static void capture_resample(struct coreaudio_stream *stream)
{
    UINT32 resamp_period_frames = muldiv(stream->period_frames, stream->dev_desc.mSampleRate,
                                         stream->fmt->nSamplesPerSec);
    OSStatus sc;

    /* the resampling process often needs more source frames than we'd
     * guess from a straight conversion using the sample rate ratio. so
     * only convert if we have extra source data. */
    while(stream->cap_held_frames > resamp_period_frames * 2){
        AudioBufferList converted_list;
        UInt32 wanted_frames = stream->period_frames;

        converted_list.mNumberBuffers = 1;
        converted_list.mBuffers[0].mNumberChannels = stream->fmt->nChannels;
        converted_list.mBuffers[0].mDataByteSize = wanted_frames * stream->fmt->nBlockAlign;

        if(stream->resamp_bufsize_frames < wanted_frames){
            free(stream->resamp_buffer);
            stream->resamp_buffer = malloc(converted_list.mBuffers[0].mDataByteSize);
            stream->resamp_bufsize_frames = wanted_frames;
        }

        converted_list.mBuffers[0].mData = stream->resamp_buffer;

        sc = AudioConverterFillComplexBuffer(stream->converter, feed_cb,
                                             stream, &wanted_frames, &converted_list, NULL);
        if(sc != noErr){
            WARN("AudioConverterFillComplexBuffer failed: %x\n", (int)sc);
            break;
        }

        ca_wrap_buffer(stream->local_buffer,
                       stream->wri_offs_frames * stream->fmt->nBlockAlign,
                       stream->bufsize_frames * stream->fmt->nBlockAlign,
                       stream->resamp_buffer, wanted_frames * stream->fmt->nBlockAlign);

        stream->wri_offs_frames += wanted_frames;
        stream->wri_offs_frames %= stream->bufsize_frames;
        if(stream->held_frames + wanted_frames > stream->bufsize_frames){
            stream->lcl_offs_frames += buf_ptr_diff(stream->lcl_offs_frames, stream->wri_offs_frames,
                                                    stream->bufsize_frames);
            stream->held_frames = stream->bufsize_frames;
        }else
            stream->held_frames += wanted_frames;
    }
}

static NTSTATUS get_buffer_size(void *args)
{
    struct get_buffer_size_params *params = args;
    struct coreaudio_stream *stream = params->stream;

    OSSpinLockLock(&stream->lock);
    *params->frames = stream->bufsize_frames;
    OSSpinLockUnlock(&stream->lock);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static HRESULT ca_get_max_stream_latency(struct coreaudio_stream *stream, UInt32 *max)
{
    AudioObjectPropertyAddress addr;
    AudioStreamID *ids;
    UInt32 size;
    OSStatus sc;
    int nstreams, i;

    addr.mScope = get_scope(stream->flow);
    addr.mElement = 0;
    addr.mSelector = kAudioDevicePropertyStreams;

    sc = AudioObjectGetPropertyDataSize(stream->dev_id, &addr, 0, NULL, &size);
    if(sc != noErr){
        WARN("Unable to get size for _Streams property: %x\n", (int)sc);
        return osstatus_to_hresult(sc);
    }

    ids = malloc(size);
    if(!ids)
        return E_OUTOFMEMORY;

    sc = AudioObjectGetPropertyData(stream->dev_id, &addr, 0, NULL, &size, ids);
    if(sc != noErr){
        WARN("Unable to get _Streams property: %x\n", (int)sc);
        free(ids);
        return osstatus_to_hresult(sc);
    }

    nstreams = size / sizeof(AudioStreamID);
    *max = 0;

    addr.mSelector = kAudioStreamPropertyLatency;
    for(i = 0; i < nstreams; ++i){
        UInt32 latency;

        size = sizeof(latency);
        sc = AudioObjectGetPropertyData(ids[i], &addr, 0, NULL, &size, &latency);
        if(sc != noErr){
            WARN("Unable to get _Latency property: %x\n", (int)sc);
            continue;
        }

        if(latency > *max)
            *max = latency;
    }

    free(ids);

    return S_OK;
}

static NTSTATUS get_latency(void *args)
{
    struct get_latency_params *params = args;
    struct coreaudio_stream *stream = params->stream;
    UInt32 latency, stream_latency, size;
    AudioObjectPropertyAddress addr;
    OSStatus sc;

    OSSpinLockLock(&stream->lock);

    addr.mScope = get_scope(stream->flow);
    addr.mSelector = kAudioDevicePropertyLatency;
    addr.mElement = 0;

    size = sizeof(latency);
    sc = AudioObjectGetPropertyData(stream->dev_id, &addr, 0, NULL, &size, &latency);
    if(sc != noErr){
        WARN("Couldn't get _Latency property: %x\n", (int)sc);
        OSSpinLockUnlock(&stream->lock);
        params->result = osstatus_to_hresult(sc);
        return STATUS_SUCCESS;
    }

    params->result = ca_get_max_stream_latency(stream, &stream_latency);
    if(FAILED(params->result)){
        OSSpinLockUnlock(&stream->lock);
        return STATUS_SUCCESS;
    }

    latency += stream_latency;
    /* pretend we process audio in Period chunks, so max latency includes
     * the period time */
    *params->latency = muldiv(latency, 10000000, stream->fmt->nSamplesPerSec)
        + stream->period_ms * 10000;

    OSSpinLockUnlock(&stream->lock);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static UINT32 get_current_padding_nolock(struct coreaudio_stream *stream)
{
    if(stream->flow == eCapture) capture_resample(stream);
    return stream->held_frames;
}

static NTSTATUS get_current_padding(void *args)
{
    struct get_current_padding_params *params = args;
    struct coreaudio_stream *stream = params->stream;

    OSSpinLockLock(&stream->lock);
    *params->padding = get_current_padding_nolock(stream);
    OSSpinLockUnlock(&stream->lock);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS start(void *args)
{
    struct start_params *params = args;
    struct coreaudio_stream *stream = params->stream;

    OSSpinLockLock(&stream->lock);

    if(stream->playing)
        params->result = AUDCLNT_E_NOT_STOPPED;
    else{
        stream->playing = TRUE;
        params->result = S_OK;
    }

    OSSpinLockUnlock(&stream->lock);

    return STATUS_SUCCESS;
}

static NTSTATUS stop(void *args)
{
    struct stop_params *params = args;
    struct coreaudio_stream *stream = params->stream;

    OSSpinLockLock(&stream->lock);

    if(!stream->playing)
        params->result = S_FALSE;
    else{
        stream->playing = FALSE;
        params->result = S_OK;
    }

    OSSpinLockUnlock(&stream->lock);

    return STATUS_SUCCESS;
}

static NTSTATUS reset(void *args)
{
    struct reset_params *params = args;
    struct coreaudio_stream *stream = params->stream;

    OSSpinLockLock(&stream->lock);

    if(stream->playing)
        params->result = AUDCLNT_E_NOT_STOPPED;
    else if(stream->getbuf_last)
        params->result = AUDCLNT_E_BUFFER_OPERATION_PENDING;
    else{
        if(stream->flow == eRender)
            stream->written_frames = 0;
        else
            stream->written_frames += stream->held_frames;
        stream->held_frames = 0;
        stream->lcl_offs_frames = 0;
        stream->wri_offs_frames = 0;
        stream->cap_offs_frames = 0;
        stream->cap_held_frames = 0;
        params->result = S_OK;
    }

    OSSpinLockUnlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS get_render_buffer(void *args)
{
    struct get_render_buffer_params *params = args;
    struct coreaudio_stream *stream = params->stream;
    UINT32 pad;

    OSSpinLockLock(&stream->lock);

    pad = get_current_padding_nolock(stream);

    if(stream->getbuf_last){
        params->result = AUDCLNT_E_OUT_OF_ORDER;
        goto end;
    }
    if(!params->frames){
        params->result = S_OK;
        goto end;
    }
    if(pad + params->frames > stream->bufsize_frames){
        params->result = AUDCLNT_E_BUFFER_TOO_LARGE;
        goto end;
    }

    if(stream->wri_offs_frames + params->frames > stream->bufsize_frames){
        if(stream->tmp_buffer_frames < params->frames){
            NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer,
                                &stream->tmp_buffer_size, MEM_RELEASE);
            stream->tmp_buffer_size = params->frames * stream->fmt->nBlockAlign;
            if(NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, 0,
                                       &stream->tmp_buffer_size, MEM_COMMIT, PAGE_READWRITE)){
                stream->tmp_buffer_frames = 0;
                params->result = E_OUTOFMEMORY;
                goto end;
            }
            stream->tmp_buffer_frames = params->frames;
        }
        *params->data = stream->tmp_buffer;
        stream->getbuf_last = -params->frames;
    }else{
        *params->data = stream->local_buffer + stream->wri_offs_frames * stream->fmt->nBlockAlign;
        stream->getbuf_last = params->frames;
    }

    silence_buffer(stream, *params->data, params->frames);
    params->result = S_OK;

end:
    OSSpinLockUnlock(&stream->lock);

    return STATUS_SUCCESS;
}

static NTSTATUS release_render_buffer(void *args)
{
    struct release_render_buffer_params *params = args;
    struct coreaudio_stream *stream = params->stream;
    BYTE *buffer;

    OSSpinLockLock(&stream->lock);

    if(!params->frames){
        stream->getbuf_last = 0;
        params->result = S_OK;
    }else if(!stream->getbuf_last)
        params->result = AUDCLNT_E_OUT_OF_ORDER;
    else if(params->frames > (stream->getbuf_last >= 0 ? stream->getbuf_last : -stream->getbuf_last))
        params->result = AUDCLNT_E_INVALID_SIZE;
    else{
        if(stream->getbuf_last >= 0)
            buffer = stream->local_buffer + stream->wri_offs_frames * stream->fmt->nBlockAlign;
        else
            buffer = stream->tmp_buffer;

        if(params->flags & AUDCLNT_BUFFERFLAGS_SILENT)
            silence_buffer(stream, buffer, params->frames);

        if(stream->getbuf_last < 0)
            ca_wrap_buffer(stream->local_buffer,
                           stream->wri_offs_frames * stream->fmt->nBlockAlign,
                           stream->bufsize_frames * stream->fmt->nBlockAlign,
                           buffer, params->frames * stream->fmt->nBlockAlign);

        stream->wri_offs_frames += params->frames;
        stream->wri_offs_frames %= stream->bufsize_frames;
        stream->held_frames += params->frames;
        stream->written_frames += params->frames;
        stream->getbuf_last = 0;

        params->result = S_OK;
    }

    OSSpinLockUnlock(&stream->lock);

    return STATUS_SUCCESS;
}

static NTSTATUS get_capture_buffer(void *args)
{
    struct get_capture_buffer_params *params = args;
    struct coreaudio_stream *stream = params->stream;
    UINT32 chunk_bytes, chunk_frames;
    LARGE_INTEGER stamp, freq;

    OSSpinLockLock(&stream->lock);

    if(stream->getbuf_last){
        params->result = AUDCLNT_E_OUT_OF_ORDER;
        goto end;
    }

    capture_resample(stream);

    *params->frames = 0;

    if(stream->held_frames < stream->period_frames){
        params->result = AUDCLNT_S_BUFFER_EMPTY;
        goto end;
    }

    *params->flags = 0;
    chunk_frames = stream->bufsize_frames - stream->lcl_offs_frames;
    if(chunk_frames < stream->period_frames){
        chunk_bytes = chunk_frames * stream->fmt->nBlockAlign;
        if(!stream->tmp_buffer){
            stream->tmp_buffer_size = stream->period_frames * stream->fmt->nBlockAlign;
            NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, 0,
                                    &stream->tmp_buffer_size, MEM_COMMIT, PAGE_READWRITE);
        }
        *params->data = stream->tmp_buffer;
        memcpy(*params->data, stream->local_buffer + stream->lcl_offs_frames * stream->fmt->nBlockAlign,
               chunk_bytes);
        memcpy(*params->data + chunk_bytes, stream->local_buffer,
               stream->period_frames * stream->fmt->nBlockAlign - chunk_bytes);
    }else
        *params->data = stream->local_buffer + stream->lcl_offs_frames * stream->fmt->nBlockAlign;

    stream->getbuf_last = *params->frames = stream->period_frames;

    if(params->devpos)
        *params->devpos = stream->written_frames;
    if(params->qpcpos){ /* fixme: qpc of recording time */
        NtQueryPerformanceCounter(&stamp, &freq);
        *params->qpcpos = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }
    params->result = S_OK;

end:
    OSSpinLockUnlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS release_capture_buffer(void *args)
{
    struct release_capture_buffer_params *params = args;
    struct coreaudio_stream *stream = params->stream;

    OSSpinLockLock(&stream->lock);

    if(!params->done){
        stream->getbuf_last = 0;
        params->result = S_OK;
    }else if(!stream->getbuf_last)
        params->result = AUDCLNT_E_OUT_OF_ORDER;
    else if(stream->getbuf_last != params->done)
        params->result = AUDCLNT_E_INVALID_SIZE;
    else{
        stream->written_frames += params->done;
        stream->held_frames -= params->done;
        stream->lcl_offs_frames += params->done;
        stream->lcl_offs_frames %= stream->bufsize_frames;
        stream->getbuf_last = 0;
        params->result = S_OK;
    }

    OSSpinLockUnlock(&stream->lock);

    return STATUS_SUCCESS;
}

static NTSTATUS get_next_packet_size(void *args)
{
    struct get_next_packet_size_params *params = args;
    struct coreaudio_stream *stream = params->stream;

    OSSpinLockLock(&stream->lock);

    capture_resample(stream);

    if(stream->held_frames >= stream->period_frames)
        *params->frames = stream->period_frames;
    else
        *params->frames = 0;

    OSSpinLockUnlock(&stream->lock);

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS get_position(void *args)
{
    struct get_position_params *params = args;
    struct coreaudio_stream *stream = params->stream;
    LARGE_INTEGER stamp, freq;

    OSSpinLockLock(&stream->lock);

    *params->pos = stream->written_frames - stream->held_frames;

    if(stream->share == AUDCLNT_SHAREMODE_SHARED)
        *params->pos *= stream->fmt->nBlockAlign;

    if(params->qpctime){
        NtQueryPerformanceCounter(&stamp, &freq);
        *params->qpctime = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }

    OSSpinLockUnlock(&stream->lock);

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS get_frequency(void *args)
{
    struct get_frequency_params *params = args;
    struct coreaudio_stream *stream = params->stream;

    if(stream->share == AUDCLNT_SHAREMODE_SHARED)
        *params->freq = (UINT64)stream->fmt->nSamplesPerSec * stream->fmt->nBlockAlign;
    else
        *params->freq = stream->fmt->nSamplesPerSec;

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS is_started(void *args)
{
    struct is_started_params *params = args;
    struct coreaudio_stream *stream = params->stream;

    if(stream->playing)
        params->result = S_OK;
    else
        params->result = S_FALSE;

    return STATUS_SUCCESS;
}

static NTSTATUS set_volumes(void *args)
{
    struct set_volumes_params *params = args;
    struct coreaudio_stream *stream = params->stream;
    Float32 level = 1.0, tmp;
    OSStatus sc;
    UINT32 i;

    if(params->channel >= stream->fmt->nChannels || params->channel < -1){
        ERR("Incorrect channel %d\n", params->channel);
        return STATUS_SUCCESS;
    }

    if(params->channel == -1){
        for(i = 0; i < stream->fmt->nChannels; ++i){
            tmp = params->master_volume * params->volumes[i] * params->session_volumes[i];
            level = tmp < level ? tmp : level;
        }
    }else
        level = params->master_volume * params->volumes[params->channel] *
            params->session_volumes[params->channel];

    sc = AudioUnitSetParameter(stream->unit, kHALOutputParam_Volume,
                               kAudioUnitScope_Global, 0, level, 0);
    if(sc != noErr)
        WARN("Couldn't set volume: %x\n", (int)sc);

    return STATUS_SUCCESS;
}

unixlib_entry_t __wine_unix_call_funcs[] =
{
    get_endpoint_ids,
    create_stream,
    release_stream,
    start,
    stop,
    reset,
    get_render_buffer,
    release_render_buffer,
    get_capture_buffer,
    release_capture_buffer,
    get_mix_format,
    is_format_supported,
    get_buffer_size,
    get_latency,
    get_current_padding,
    get_next_packet_size,
    get_position,
    get_frequency,
    is_started,
    set_volumes,
};
