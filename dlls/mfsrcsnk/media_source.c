/*
 * Copyright 2024 Rémi Bernon for CodeWeavers
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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "mfsrcsnk_private.h"

#include "wine/list.h"
#include "wine/debug.h"
#include "wine/winedmo.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

#define DEFINE_MF_ASYNC_CALLBACK_(type, name, impl_from, pfx, mem, expr)                           \
    static struct type *impl_from(IMFAsyncCallback *iface)                                         \
    {                                                                                              \
        return CONTAINING_RECORD(iface, struct type, mem);                                         \
    }                                                                                              \
    static HRESULT WINAPI pfx##_QueryInterface(IMFAsyncCallback *iface, REFIID iid, void **out)    \
    {                                                                                              \
        if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IMFAsyncCallback))              \
        {                                                                                          \
            IMFAsyncCallback_AddRef((*out = iface));                                               \
            return S_OK;                                                                           \
        }                                                                                          \
        *out = NULL;                                                                               \
        return E_NOINTERFACE;                                                                      \
    }                                                                                              \
    static ULONG WINAPI pfx##_AddRef(IMFAsyncCallback *iface)                                      \
    {                                                                                              \
        struct type *object = impl_from(iface);                                                    \
        return IUnknown_AddRef((IUnknown *)(expr));                                                \
    }                                                                                              \
    static ULONG WINAPI pfx##_Release(IMFAsyncCallback *iface)                                     \
    {                                                                                              \
        struct type *object = impl_from(iface);                                                    \
        return IUnknown_Release((IUnknown *)(expr));                                               \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetParameters(IMFAsyncCallback *iface, DWORD *flags, DWORD *queue) \
    {                                                                                              \
        return E_NOTIMPL;                                                                          \
    }                                                                                              \
    static HRESULT WINAPI pfx##_Invoke(IMFAsyncCallback *iface, IMFAsyncResult *result)            \
    {                                                                                              \
        struct type *object = impl_from(iface);                                                    \
        return type##_##name(object, result);                                                      \
    }                                                                                              \
    static const IMFAsyncCallbackVtbl pfx##_vtbl =                                                 \
    {                                                                                              \
            pfx##_QueryInterface,                                                                  \
            pfx##_AddRef,                                                                          \
            pfx##_Release,                                                                         \
            pfx##_GetParameters,                                                                   \
            pfx##_Invoke,                                                                          \
    };

#define DEFINE_MF_ASYNC_CALLBACK(type, name, base_iface)                                           \
    DEFINE_MF_ASYNC_CALLBACK_(type, name, type##_from_##name, type##_##name, name##_iface, &object->base_iface)

struct media_stream
{
    IMFMediaStream IMFMediaStream_iface;
    LONG refcount;

    IMFMediaSource *source;
    IMFMediaEventQueue *queue;
    IMFStreamDescriptor *descriptor;
};

struct media_source
{
    IMFMediaSource IMFMediaSource_iface;
    IMFGetService IMFGetService_iface;
    IMFRateSupport IMFRateSupport_iface;
    IMFRateControl IMFRateControl_iface;
    IMFAsyncCallback async_create_iface;
    LONG refcount;

    CRITICAL_SECTION cs;
    IMFMediaEventQueue *queue;
    IMFByteStream *stream;
    WCHAR *url;
    float rate;

    struct winedmo_demuxer winedmo_demuxer;
    struct winedmo_stream winedmo_stream;
    UINT64 file_size;
    INT64 duration;
    UINT stream_count;
    WCHAR mime_type[256];

    UINT *stream_map;
    struct media_stream **streams;

    enum
    {
        SOURCE_STOPPED,
        SOURCE_SHUTDOWN,
    } state;
};

static struct media_source *media_source_from_IMFMediaSource(IMFMediaSource *iface)
{
    return CONTAINING_RECORD(iface, struct media_source, IMFMediaSource_iface);
}

static struct media_stream *media_stream_from_IMFMediaStream(IMFMediaStream *iface)
{
    return CONTAINING_RECORD(iface, struct media_stream, IMFMediaStream_iface);
}

static HRESULT WINAPI media_stream_QueryInterface(IMFMediaStream *iface, REFIID riid, void **out)
{
    struct media_stream *stream = media_stream_from_IMFMediaStream(iface);

    TRACE("stream %p, riid %s, out %p\n", stream, debugstr_guid(riid), out);

    if (IsEqualIID(riid, &IID_IUnknown)
            || IsEqualIID(riid, &IID_IMFMediaEventGenerator)
            || IsEqualIID(riid, &IID_IMFMediaStream))
    {
        IMFMediaStream_AddRef(&stream->IMFMediaStream_iface);
        *out = &stream->IMFMediaStream_iface;
        return S_OK;
    }

    FIXME("Unsupported interface %s\n", debugstr_guid(riid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI media_stream_AddRef(IMFMediaStream *iface)
{
    struct media_stream *stream = media_stream_from_IMFMediaStream(iface);
    ULONG refcount = InterlockedIncrement(&stream->refcount);
    TRACE("stream %p, refcount %ld\n", stream, refcount);
    return refcount;
}

static ULONG WINAPI media_stream_Release(IMFMediaStream *iface)
{
    struct media_stream *stream = media_stream_from_IMFMediaStream(iface);
    ULONG refcount = InterlockedDecrement(&stream->refcount);

    TRACE("stream %p, refcount %ld\n", stream, refcount);

    if (!refcount)
    {
        IMFMediaSource_Release(stream->source);
        IMFStreamDescriptor_Release(stream->descriptor);
        IMFMediaEventQueue_Release(stream->queue);
        free(stream);
    }

    return refcount;
}

static HRESULT WINAPI media_stream_GetEvent(IMFMediaStream *iface, DWORD flags, IMFMediaEvent **event)
{
    struct media_stream *stream = media_stream_from_IMFMediaStream(iface);
    TRACE("stream %p, flags %#lx, event %p\n", stream, flags, event);
    return IMFMediaEventQueue_GetEvent(stream->queue, flags, event);
}

static HRESULT WINAPI media_stream_BeginGetEvent(IMFMediaStream *iface, IMFAsyncCallback *callback, IUnknown *state)
{
    struct media_stream *stream = media_stream_from_IMFMediaStream(iface);
    TRACE("stream %p, callback %p, state %p\n", stream, callback, state);
    return IMFMediaEventQueue_BeginGetEvent(stream->queue, callback, state);
}

static HRESULT WINAPI media_stream_EndGetEvent(IMFMediaStream *iface, IMFAsyncResult *result, IMFMediaEvent **event)
{
    struct media_stream *stream = media_stream_from_IMFMediaStream(iface);
    TRACE("stream %p, result %p, event %p\n", stream, result, event);
    return IMFMediaEventQueue_EndGetEvent(stream->queue, result, event);
}

static HRESULT WINAPI media_stream_QueueEvent(IMFMediaStream *iface, MediaEventType event_type, REFGUID ext_type,
        HRESULT hr, const PROPVARIANT *value)
{
    struct media_stream *stream = media_stream_from_IMFMediaStream(iface);
    TRACE("stream %p, event_type %#lx, ext_type %s, hr %#lx, value %p\n", stream, event_type, debugstr_guid(ext_type), hr, value);
    return IMFMediaEventQueue_QueueEventParamVar(stream->queue, event_type, ext_type, hr, value);
}

static HRESULT WINAPI media_stream_GetMediaSource(IMFMediaStream *iface, IMFMediaSource **out)
{
    struct media_stream *stream = media_stream_from_IMFMediaStream(iface);
    struct media_source *source = media_source_from_IMFMediaSource(stream->source);
    HRESULT hr = S_OK;

    TRACE("stream %p, out %p\n", stream, out);

    EnterCriticalSection(&source->cs);

    if (source->state == SOURCE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else
    {
        IMFMediaSource_AddRef(&source->IMFMediaSource_iface);
        *out = &source->IMFMediaSource_iface;
    }

    LeaveCriticalSection(&source->cs);

    return hr;
}

static HRESULT WINAPI media_stream_GetStreamDescriptor(IMFMediaStream* iface, IMFStreamDescriptor **descriptor)
{
    struct media_stream *stream = media_stream_from_IMFMediaStream(iface);
    struct media_source *source = media_source_from_IMFMediaSource(stream->source);
    HRESULT hr = S_OK;

    TRACE("stream %p, descriptor %p\n", stream, descriptor);

    EnterCriticalSection(&source->cs);

    if (source->state == SOURCE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else
    {
        IMFStreamDescriptor_AddRef(stream->descriptor);
        *descriptor = stream->descriptor;
    }

    LeaveCriticalSection(&source->cs);

    return hr;
}

static HRESULT WINAPI media_stream_RequestSample(IMFMediaStream *iface, IUnknown *token)
{
    struct media_stream *stream = media_stream_from_IMFMediaStream(iface);
    FIXME("stream %p, token %p, stub!\n", stream, token);
    return E_NOTIMPL;
}

static const IMFMediaStreamVtbl media_stream_vtbl =
{
    media_stream_QueryInterface,
    media_stream_AddRef,
    media_stream_Release,
    media_stream_GetEvent,
    media_stream_BeginGetEvent,
    media_stream_EndGetEvent,
    media_stream_QueueEvent,
    media_stream_GetMediaSource,
    media_stream_GetStreamDescriptor,
    media_stream_RequestSample,
};

static HRESULT media_stream_create(IMFMediaSource *source, IMFStreamDescriptor *descriptor, struct media_stream **out)
{
    struct media_stream *object;
    HRESULT hr;

    TRACE("source %p, descriptor %p, out %p\n", source, descriptor, out);

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->IMFMediaStream_iface.lpVtbl = &media_stream_vtbl;
    object->refcount = 1;

    if (FAILED(hr = MFCreateEventQueue(&object->queue)))
    {
        free(object);
        return hr;
    }

    IMFMediaSource_AddRef((object->source = source));
    IMFStreamDescriptor_AddRef((object->descriptor = descriptor));

    TRACE("Created stream object %p\n", object);

    *out = object;
    return S_OK;
}

static struct media_source *media_source_from_IMFGetService(IMFGetService *iface)
{
    return CONTAINING_RECORD(iface, struct media_source, IMFGetService_iface);
}

static HRESULT WINAPI media_source_IMFGetService_QueryInterface(IMFGetService *iface, REFIID riid, void **obj)
{
    struct media_source *source = media_source_from_IMFGetService(iface);
    return IMFMediaSource_QueryInterface(&source->IMFMediaSource_iface, riid, obj);
}

static ULONG WINAPI media_source_IMFGetService_AddRef(IMFGetService *iface)
{
    struct media_source *source = media_source_from_IMFGetService(iface);
    return IMFMediaSource_AddRef(&source->IMFMediaSource_iface);
}

static ULONG WINAPI media_source_IMFGetService_Release(IMFGetService *iface)
{
    struct media_source *source = media_source_from_IMFGetService(iface);
    return IMFMediaSource_Release(&source->IMFMediaSource_iface);
}

static HRESULT WINAPI media_source_IMFGetService_GetService(IMFGetService *iface, REFGUID service,
        REFIID riid, void **obj)
{
    struct media_source *source = media_source_from_IMFGetService(iface);

    TRACE("source %p, service %s, riid %s, obj %p\n", source, debugstr_guid(service), debugstr_guid(riid), obj);

    if (IsEqualGUID(service, &MF_RATE_CONTROL_SERVICE))
    {
        if (IsEqualIID(riid, &IID_IMFRateSupport))
        {
            IMFRateSupport_AddRef(&source->IMFRateSupport_iface);
            *obj = &source->IMFRateSupport_iface;
            return S_OK;
        }
        if (IsEqualIID(riid, &IID_IMFRateControl))
        {
            IMFRateControl_AddRef(&source->IMFRateControl_iface);
            *obj = &source->IMFRateControl_iface;
            return S_OK;
        }
    }

    FIXME("Unsupported service %s / riid %s\n", debugstr_guid(service), debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static const IMFGetServiceVtbl media_source_IMFGetService_vtbl =
{
    media_source_IMFGetService_QueryInterface,
    media_source_IMFGetService_AddRef,
    media_source_IMFGetService_Release,
    media_source_IMFGetService_GetService,
};

static struct media_source *media_source_from_IMFRateSupport(IMFRateSupport *iface)
{
    return CONTAINING_RECORD(iface, struct media_source, IMFRateSupport_iface);
}

static HRESULT WINAPI media_source_IMFRateSupport_QueryInterface(IMFRateSupport *iface, REFIID riid, void **obj)
{
    struct media_source *source = media_source_from_IMFRateSupport(iface);
    return IMFMediaSource_QueryInterface(&source->IMFMediaSource_iface, riid, obj);
}

static ULONG WINAPI media_source_IMFRateSupport_AddRef(IMFRateSupport *iface)
{
    struct media_source *source = media_source_from_IMFRateSupport(iface);
    return IMFMediaSource_AddRef(&source->IMFMediaSource_iface);
}

static ULONG WINAPI media_source_IMFRateSupport_Release(IMFRateSupport *iface)
{
    struct media_source *source = media_source_from_IMFRateSupport(iface);
    return IMFMediaSource_Release(&source->IMFMediaSource_iface);
}

static HRESULT WINAPI media_source_IMFRateSupport_GetSlowestRate(IMFRateSupport *iface,
        MFRATE_DIRECTION direction, BOOL thin, float *rate)
{
    struct media_source *source = media_source_from_IMFRateSupport(iface);
    TRACE("source %p, direction %d, thin %d, rate %p\n", source, direction, thin, rate);
    *rate = 0.0f;
    return S_OK;
}

static HRESULT WINAPI media_source_IMFRateSupport_GetFastestRate(IMFRateSupport *iface,
        MFRATE_DIRECTION direction, BOOL thin, float *rate)
{
    struct media_source *source = media_source_from_IMFRateSupport(iface);
    TRACE("source %p, direction %d, thin %d, rate %p\n", source, direction, thin, rate);
    *rate = direction == MFRATE_FORWARD ? 1e6f : -1e6f;
    return S_OK;
}

static HRESULT WINAPI media_source_IMFRateSupport_IsRateSupported(IMFRateSupport *iface, BOOL thin,
        float rate, float *nearest_rate)
{
    struct media_source *source = media_source_from_IMFRateSupport(iface);
    TRACE("source %p, thin %d, rate %f, nearest_rate %p\n", source, thin, rate, nearest_rate);
    if (nearest_rate) *nearest_rate = rate;
    return rate >= -1e6f && rate <= 1e6f ? S_OK : MF_E_UNSUPPORTED_RATE;
}

static const IMFRateSupportVtbl media_source_IMFRateSupport_vtbl =
{
    media_source_IMFRateSupport_QueryInterface,
    media_source_IMFRateSupport_AddRef,
    media_source_IMFRateSupport_Release,
    media_source_IMFRateSupport_GetSlowestRate,
    media_source_IMFRateSupport_GetFastestRate,
    media_source_IMFRateSupport_IsRateSupported,
};

static struct media_source *media_source_from_IMFRateControl(IMFRateControl *iface)
{
    return CONTAINING_RECORD(iface, struct media_source, IMFRateControl_iface);
}

static HRESULT WINAPI media_source_IMFRateControl_QueryInterface(IMFRateControl *iface, REFIID riid, void **obj)
{
    struct media_source *source = media_source_from_IMFRateControl(iface);
    return IMFMediaSource_QueryInterface(&source->IMFMediaSource_iface, riid, obj);
}

static ULONG WINAPI media_source_IMFRateControl_AddRef(IMFRateControl *iface)
{
    struct media_source *source = media_source_from_IMFRateControl(iface);
    return IMFMediaSource_AddRef(&source->IMFMediaSource_iface);
}

static ULONG WINAPI media_source_IMFRateControl_Release(IMFRateControl *iface)
{
    struct media_source *source = media_source_from_IMFRateControl(iface);
    return IMFMediaSource_Release(&source->IMFMediaSource_iface);
}

static HRESULT WINAPI media_source_IMFRateControl_SetRate(IMFRateControl *iface, BOOL thin, float rate)
{
    struct media_source *source = media_source_from_IMFRateControl(iface);
    HRESULT hr;

    FIXME("source %p, thin %d, rate %f, stub!\n", source, thin, rate);

    if (rate < 0.0f)
        return MF_E_REVERSE_UNSUPPORTED;
    if (thin)
        return MF_E_THINNING_UNSUPPORTED;

    if (FAILED(hr = IMFRateSupport_IsRateSupported(&source->IMFRateSupport_iface, thin, rate, NULL)))
        return hr;

    EnterCriticalSection(&source->cs);
    source->rate = rate;
    LeaveCriticalSection(&source->cs);

    return IMFMediaEventQueue_QueueEventParamVar(source->queue, MESourceRateChanged, &GUID_NULL, S_OK, NULL);
}

static HRESULT WINAPI media_source_IMFRateControl_GetRate(IMFRateControl *iface, BOOL *thin, float *rate)
{
    struct media_source *source = media_source_from_IMFRateControl(iface);

    TRACE("source %p, thin %p, rate %p\n", source, thin, rate);

    if (thin)
        *thin = FALSE;

    EnterCriticalSection(&source->cs);
    *rate = source->rate;
    LeaveCriticalSection(&source->cs);

    return S_OK;
}

static const IMFRateControlVtbl media_source_IMFRateControl_vtbl =
{
    media_source_IMFRateControl_QueryInterface,
    media_source_IMFRateControl_AddRef,
    media_source_IMFRateControl_Release,
    media_source_IMFRateControl_SetRate,
    media_source_IMFRateControl_GetRate,
};

static HRESULT WINAPI media_source_QueryInterface(IMFMediaSource *iface, REFIID riid, void **out)
{
    struct media_source *source = media_source_from_IMFMediaSource(iface);

    TRACE("source %p, riid %s, out %p\n", source, debugstr_guid(riid), out);

    if (IsEqualIID(riid, &IID_IUnknown)
            || IsEqualIID(riid, &IID_IMFMediaEventGenerator)
            || IsEqualIID(riid, &IID_IMFMediaSource))
    {
        IMFMediaSource_AddRef(&source->IMFMediaSource_iface);
        *out = &source->IMFMediaSource_iface;
        return S_OK;
    }

    if (IsEqualIID(riid, &IID_IMFGetService))
    {
        IMFGetService_AddRef(&source->IMFGetService_iface);
        *out = &source->IMFGetService_iface;
        return S_OK;
    }

    FIXME("Unsupported interface %s\n", debugstr_guid(riid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI media_source_AddRef(IMFMediaSource *iface)
{
    struct media_source *source = media_source_from_IMFMediaSource(iface);
    ULONG refcount = InterlockedIncrement(&source->refcount);
    TRACE("source %p, refcount %ld\n", source, refcount);
    return refcount;
}

static ULONG WINAPI media_source_Release(IMFMediaSource *iface)
{
    struct media_source *source = media_source_from_IMFMediaSource(iface);
    ULONG refcount = InterlockedDecrement(&source->refcount);

    TRACE("source %p, refcount %ld\n", source, refcount);

    if (!refcount)
    {
        IMFMediaSource_Shutdown(iface);

        winedmo_demuxer_destroy(&source->winedmo_demuxer);
        free(source->stream_map);
        free(source->streams);

        IMFMediaEventQueue_Release(source->queue);
        IMFByteStream_Release(source->stream);
        free(source->url);

        source->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&source->cs);

        free(source);
    }

    return refcount;
}

static HRESULT WINAPI media_source_GetEvent(IMFMediaSource *iface, DWORD flags, IMFMediaEvent **event)
{
    struct media_source *source = media_source_from_IMFMediaSource(iface);
    TRACE("source %p, flags %#lx, event %p\n", source, flags, event);
    return IMFMediaEventQueue_GetEvent(source->queue, flags, event);
}

static HRESULT WINAPI media_source_BeginGetEvent(IMFMediaSource *iface, IMFAsyncCallback *callback, IUnknown *state)
{
    struct media_source *source = media_source_from_IMFMediaSource(iface);
    TRACE("source %p, callback %p, state %p\n", source, callback, state);
    return IMFMediaEventQueue_BeginGetEvent(source->queue, callback, state);
}

static HRESULT WINAPI media_source_EndGetEvent(IMFMediaSource *iface, IMFAsyncResult *result,
        IMFMediaEvent **event)
{
    struct media_source *source = media_source_from_IMFMediaSource(iface);
    TRACE("source %p, result %p, event %p\n", source, result, event);
    return IMFMediaEventQueue_EndGetEvent(source->queue, result, event);
}

static HRESULT WINAPI media_source_QueueEvent(IMFMediaSource *iface, MediaEventType event_type,
        REFGUID ext_type, HRESULT hr, const PROPVARIANT *value)
{
    struct media_source *source = media_source_from_IMFMediaSource(iface);
    TRACE("source %p, event_type %#lx, ext_type %s, hr %#lx, value %p\n", source, event_type,
            debugstr_guid(ext_type), hr, debugstr_propvar(value));
    return IMFMediaEventQueue_QueueEventParamVar(source->queue, event_type, ext_type, hr, value);
}

static HRESULT WINAPI media_source_GetCharacteristics(IMFMediaSource *iface, DWORD *characteristics)
{
    struct media_source *source = media_source_from_IMFMediaSource(iface);
    HRESULT hr;

    TRACE("source %p, characteristics %p\n", source, characteristics);

    EnterCriticalSection(&source->cs);
    if (source->state == SOURCE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else
    {
        *characteristics = MFMEDIASOURCE_CAN_SEEK | MFMEDIASOURCE_CAN_PAUSE;
        hr = S_OK;
    }
    LeaveCriticalSection(&source->cs);

    return hr;
}

static HRESULT media_source_create_presentation_descriptor(struct media_source *source, IMFPresentationDescriptor **descriptor)
{
    IMFStreamDescriptor **descriptors;
    HRESULT hr;
    UINT i;

    if (!(descriptors = malloc(source->stream_count * sizeof(*descriptors))))
        return E_OUTOFMEMORY;
    for (i = 0; i < source->stream_count; ++i)
        descriptors[i] = source->streams[i]->descriptor;
    hr = MFCreatePresentationDescriptor(source->stream_count, descriptors, descriptor);
    free(descriptors);

    return hr;
}

static HRESULT WINAPI media_source_CreatePresentationDescriptor(IMFMediaSource *iface, IMFPresentationDescriptor **descriptor)
{
    struct media_source *source = media_source_from_IMFMediaSource(iface);
    HRESULT hr;
    UINT i;

    TRACE("source %p, descriptor %p\n", source, descriptor);

    EnterCriticalSection(&source->cs);

    if (source->state == SOURCE_SHUTDOWN)
        hr = MF_E_SHUTDOWN;
    else if (SUCCEEDED(hr = media_source_create_presentation_descriptor(source, descriptor)))
    {
        if (FAILED(hr = IMFPresentationDescriptor_SetString(*descriptor, &MF_PD_MIME_TYPE, source->mime_type)))
            WARN("Failed to set presentation descriptor MF_PD_MIME_TYPE, hr %#lx\n", hr);
        if (FAILED(hr = IMFPresentationDescriptor_SetUINT64(*descriptor, &MF_PD_TOTAL_FILE_SIZE, source->file_size)))
            WARN("Failed to set presentation descriptor MF_PD_TOTAL_FILE_SIZE, hr %#lx\n", hr);
        if (FAILED(hr = IMFPresentationDescriptor_SetUINT64(*descriptor, &MF_PD_DURATION, source->duration)))
            WARN("Failed to set presentation descriptor MF_PD_DURATION, hr %#lx\n", hr);

        for (i = 0; i < source->stream_count; ++i)
        {
            if (FAILED(hr = IMFPresentationDescriptor_SelectStream(*descriptor, i)))
                WARN("Failed to select stream %u, hr %#lx\n", i, hr);
        }

        hr = S_OK;
    }

    LeaveCriticalSection(&source->cs);

    return hr;
}

static HRESULT WINAPI media_source_Start(IMFMediaSource *iface, IMFPresentationDescriptor *descriptor, const GUID *format,
        const PROPVARIANT *position)
{
    struct media_source *source = media_source_from_IMFMediaSource(iface);
    FIXME("source %p, descriptor %p, format %s, position %s, stub!\n", source, descriptor,
            debugstr_guid(format), debugstr_propvar(position));
    return E_NOTIMPL;
}

static HRESULT WINAPI media_source_Stop(IMFMediaSource *iface)
{
    struct media_source *source = media_source_from_IMFMediaSource(iface);
    FIXME("source %p, stub!\n", source);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_source_Pause(IMFMediaSource *iface)
{
    struct media_source *source = media_source_from_IMFMediaSource(iface);
    FIXME("source %p, stub!\n", source);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_source_Shutdown(IMFMediaSource *iface)
{
    struct media_source *source = media_source_from_IMFMediaSource(iface);

    TRACE("source %p\n", source);

    EnterCriticalSection(&source->cs);

    if (source->state == SOURCE_SHUTDOWN)
    {
        LeaveCriticalSection(&source->cs);
        return MF_E_SHUTDOWN;
    }
    source->state = SOURCE_SHUTDOWN;

    IMFMediaEventQueue_Shutdown(source->queue);
    IMFByteStream_Close(source->stream);

    while (source->stream_count--)
    {
        struct media_stream *stream = source->streams[source->stream_count];
        IMFMediaEventQueue_Shutdown(stream->queue);
        IMFMediaStream_Release(&stream->IMFMediaStream_iface);
    }

    LeaveCriticalSection(&source->cs);

    return S_OK;
}

static const IMFMediaSourceVtbl media_source_vtbl =
{
    media_source_QueryInterface,
    media_source_AddRef,
    media_source_Release,
    media_source_GetEvent,
    media_source_BeginGetEvent,
    media_source_EndGetEvent,
    media_source_QueueEvent,
    media_source_GetCharacteristics,
    media_source_CreatePresentationDescriptor,
    media_source_Start,
    media_source_Stop,
    media_source_Pause,
    media_source_Shutdown,
};

static HRESULT media_type_from_mf_video_format( const MFVIDEOFORMAT *format, IMFMediaType **media_type )
{
    HRESULT hr;

    TRACE("format %p, media_type %p\n", format, media_type);

    if (FAILED(hr = MFCreateVideoMediaType( format, (IMFVideoMediaType **)media_type )) ||
        format->dwSize <= sizeof(*format))
        return hr;

    if (FAILED(IMFMediaType_GetItem(*media_type, &MF_MT_VIDEO_ROTATION, NULL)))
        IMFMediaType_SetUINT32(*media_type, &MF_MT_VIDEO_ROTATION, MFVideoRotationFormat_0);

    return hr;
}

static HRESULT media_type_from_winedmo_format( GUID major, union winedmo_format *format, IMFMediaType **media_type )
{
    TRACE("major %p, format %p, media_type %p\n", &major, format, media_type);

    if (IsEqualGUID( &major, &MFMediaType_Video ))
        return media_type_from_mf_video_format( &format->video, media_type );
    if (IsEqualGUID( &major, &MFMediaType_Audio ))
        return MFCreateAudioMediaType( &format->audio, (IMFAudioMediaType **)media_type );

    FIXME( "Unsupported major type %s\n", debugstr_guid( &major ) );
    return E_NOTIMPL;
}

static HRESULT get_stream_media_type(struct winedmo_demuxer demuxer, UINT index, GUID *major, IMFMediaType **media_type)
{
    union winedmo_format *format;
    NTSTATUS status;
    HRESULT hr;

    TRACE("demuxer %p, index %u, media_type %p\n", &demuxer, index, media_type);

    if ((status = winedmo_demuxer_stream_type(demuxer, index, major, &format)))
    {
        WARN("Failed to get stream %u type, status %#lx\n", index, status);
        return HRESULT_FROM_NT(status);
    }

    hr = media_type ? media_type_from_winedmo_format(*major, format, media_type) : S_OK;
    free(format);
    return hr;
}

static void media_source_init_stream_map(struct media_source *source, UINT stream_count)
{
    int i, n = 0;
    GUID major;

    TRACE("source %p, stream_count %d\n", source, stream_count);

    if (wcscmp(source->mime_type, L"video/mp4"))
    {
        for (i = stream_count - 1; i >= 0; i--)
        {
            TRACE("mapping source %p stream %u to demuxer stream %u\n", source, i, i);
            source->stream_map[i] = i;
        }
        return;
    }

    for (i = stream_count - 1; i >= 0; i--)
    {
        if (FAILED(get_stream_media_type(source->winedmo_demuxer, i, &major, NULL)))
            continue;
        if (IsEqualGUID(&major, &MFMediaType_Audio))
        {
            TRACE("mapping source %p stream %u to demuxer stream %u\n", source, n, i);
            source->stream_map[n++] = i;
        }
    }
    for (i = stream_count - 1; i >= 0; i--)
    {
        if (FAILED(get_stream_media_type(source->winedmo_demuxer, i, &major, NULL)))
            continue;
        if (IsEqualGUID(&major, &MFMediaType_Video))
        {
            TRACE("mapping source %p stream %u to demuxer stream %u\n", source, n, i);
            source->stream_map[n++] = i;
        }
    }
    for (i = stream_count - 1; i >= 0; i--)
    {
        if (FAILED(get_stream_media_type(source->winedmo_demuxer, i, &major, NULL)))
            continue;
        if (!IsEqualGUID(&major, &MFMediaType_Audio) && !IsEqualGUID(&major, &MFMediaType_Video))
        {
            TRACE("mapping source %p stream %u to demuxer stream %u\n", source, n, i);
            source->stream_map[n++] = i;
        }
    }
}

static HRESULT stream_descriptor_create(UINT32 id, IMFMediaType *media_type, IMFStreamDescriptor **out)
{
    IMFStreamDescriptor *descriptor;
    IMFMediaTypeHandler *handler;
    HRESULT hr;

    TRACE("id %d, media_type %p, out %p\n", id, media_type, out);

    *out = NULL;
    if (FAILED(hr = MFCreateStreamDescriptor(id, 1, &media_type, &descriptor)))
        return hr;

    if (FAILED(hr = IMFStreamDescriptor_GetMediaTypeHandler(descriptor, &handler)))
        IMFStreamDescriptor_Release(descriptor);
    else
    {
        if (SUCCEEDED(hr = IMFMediaTypeHandler_SetCurrentMediaType(handler, media_type)))
            *out = descriptor;
        IMFMediaTypeHandler_Release(handler);
    }

    return hr;
}

static NTSTATUS CDECL media_source_seek_cb( struct winedmo_stream *stream, UINT64 *pos )
{
    struct media_source *source = CONTAINING_RECORD(stream, struct media_source, winedmo_stream);
    TRACE("stream %p, pos %p\n", stream, pos);

    if (FAILED(IMFByteStream_Seek(source->stream, msoBegin, *pos, 0, pos)))
        return STATUS_UNSUCCESSFUL;
    return STATUS_SUCCESS;
}

static NTSTATUS CDECL media_source_read_cb(struct winedmo_stream *stream, BYTE *buffer, ULONG *size)
{
    struct media_source *source = CONTAINING_RECORD(stream, struct media_source, winedmo_stream);
    TRACE("stream %p, buffer %p, size %p\n", stream, buffer, size);

    if (FAILED(IMFByteStream_Read(source->stream, buffer, *size, size)))
        return STATUS_UNSUCCESSFUL;
    return STATUS_SUCCESS;
}

static HRESULT media_source_async_create(struct media_source *source, IMFAsyncResult *result)
{
    IUnknown *state = IMFAsyncResult_GetStateNoAddRef(result);
    UINT i, stream_count;
    NTSTATUS status;
    HRESULT hr;

    TRACE("source %p, result %p\n", source, result);

    if (FAILED(hr = IMFByteStream_GetLength(source->stream, &source->file_size)))
    {
        WARN("Failed to get byte stream length, hr %#lx\n", hr);
        source->file_size = -1;
    }
    if (FAILED(hr = IMFByteStream_SetCurrentPosition(source->stream, 0)))
    {
        WARN("Failed to set byte stream position, hr %#lx\n", hr);
        hr = S_OK;
    }

    source->winedmo_stream.p_seek = media_source_seek_cb;
    source->winedmo_stream.p_read = media_source_read_cb;

    if ((status = winedmo_demuxer_create(source->url, &source->winedmo_stream, source->file_size, &source->duration,
            &stream_count, source->mime_type, &source->winedmo_demuxer)))
    {
        WARN("Failed to create demuxer, status %#lx\n", status);
        hr = HRESULT_FROM_NT(status);
        goto done;
    }

    if (!(source->stream_map = calloc(stream_count, sizeof(*source->stream_map)))
            || !(source->streams = calloc(stream_count, sizeof(*source->streams))))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }

    media_source_init_stream_map(source, stream_count);

    for (i = 0; SUCCEEDED(hr) && i < stream_count; ++i)
    {
        IMFStreamDescriptor *descriptor;
        IMFMediaType *media_type;
        GUID major;

        if (FAILED(hr = get_stream_media_type(source->winedmo_demuxer, source->stream_map[i], &major, &media_type)))
            goto done;
        if (SUCCEEDED(hr = stream_descriptor_create(i + 1, media_type, &descriptor)))
        {
            if (SUCCEEDED(hr = media_stream_create(&source->IMFMediaSource_iface, descriptor, &source->streams[i])))
                source->stream_count++;
            IMFStreamDescriptor_Release(descriptor);
        }
        IMFMediaType_Release(media_type);
    }

done:
    IMFAsyncResult_SetStatus(result, hr);
    return MFInvokeCallback((IMFAsyncResult *)state);
}

DEFINE_MF_ASYNC_CALLBACK(media_source, async_create, IMFMediaSource_iface)

static WCHAR *get_byte_stream_url(IMFByteStream *stream, const WCHAR *url)
{
    IMFAttributes *attributes;
    WCHAR buffer[MAX_PATH];
    UINT32 size;
    HRESULT hr;

    TRACE("stream %p, url %s\n", stream, debugstr_w(url));

    if (SUCCEEDED(hr = IMFByteStream_QueryInterface(stream, &IID_IMFAttributes, (void **)&attributes)))
    {
        if (FAILED(hr = IMFAttributes_GetString(attributes, &MF_BYTESTREAM_ORIGIN_NAME,
                buffer, ARRAY_SIZE(buffer), &size)))
            WARN("Failed to get MF_BYTESTREAM_ORIGIN_NAME got size %#x, hr %#lx\n", size, hr);
        else
            url = buffer;
        IMFAttributes_Release(attributes);
    }

    return url ? wcsdup(url) : NULL;
}

static HRESULT media_source_create(const WCHAR *url, IMFByteStream *stream, IMFMediaSource **out)
{
    struct media_source *source;
    HRESULT hr;

    TRACE("url %s, stream %p, out %p\n", debugstr_w(url), stream, out);

    if (!(source = calloc(1, sizeof(*source))))
        return E_OUTOFMEMORY;
    source->IMFMediaSource_iface.lpVtbl = &media_source_vtbl;
    source->IMFGetService_iface.lpVtbl = &media_source_IMFGetService_vtbl;
    source->IMFRateSupport_iface.lpVtbl = &media_source_IMFRateSupport_vtbl;
    source->IMFRateControl_iface.lpVtbl = &media_source_IMFRateControl_vtbl;
    source->async_create_iface.lpVtbl = &media_source_async_create_vtbl;
    source->refcount = 1;

    if (FAILED(hr = MFCreateEventQueue(&source->queue)))
    {
        free(source);
        return hr;
    }

    source->url = get_byte_stream_url(stream, url);
    IMFByteStream_AddRef((source->stream = stream));

    source->rate = 1.0f;
    InitializeCriticalSectionEx(&source->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    source->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": cs");

    *out = &source->IMFMediaSource_iface;
    TRACE("created source %p\n", source);
    return S_OK;
}

struct byte_stream_handler
{
    IMFByteStreamHandler IMFByteStreamHandler_iface;
    LONG refcount;
};

static struct byte_stream_handler *byte_stream_handler_from_IMFByteStreamHandler(IMFByteStreamHandler *iface)
{
    return CONTAINING_RECORD(iface, struct byte_stream_handler, IMFByteStreamHandler_iface);
}

static HRESULT WINAPI byte_stream_handler_QueryInterface(IMFByteStreamHandler *iface, REFIID riid, void **out)
{
    struct byte_stream_handler *handler = byte_stream_handler_from_IMFByteStreamHandler(iface);

    TRACE("handler %p, riid %s, out %p\n", handler, debugstr_guid(riid), out);

    if (IsEqualIID(riid, &IID_IUnknown)
            || IsEqualIID(riid, &IID_IMFByteStreamHandler))
    {
        IMFByteStreamHandler_AddRef(&handler->IMFByteStreamHandler_iface);
        *out = &handler->IMFByteStreamHandler_iface;
        return S_OK;
    }

    WARN("Unsupported %s\n", debugstr_guid(riid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI byte_stream_handler_AddRef(IMFByteStreamHandler *iface)
{
    struct byte_stream_handler *handler = byte_stream_handler_from_IMFByteStreamHandler(iface);
    ULONG refcount = InterlockedIncrement(&handler->refcount);
    TRACE("handler %p, refcount %ld\n", handler, refcount);
    return refcount;
}

static ULONG WINAPI byte_stream_handler_Release(IMFByteStreamHandler *iface)
{
    struct byte_stream_handler *handler = byte_stream_handler_from_IMFByteStreamHandler(iface);
    ULONG refcount = InterlockedDecrement(&handler->refcount);
    TRACE("handler %p, refcount %ld\n", handler, refcount);
    if (!refcount)
        free(handler);
    return refcount;
}

static HRESULT WINAPI byte_stream_handler_BeginCreateObject(IMFByteStreamHandler *iface, IMFByteStream *stream, const WCHAR *url, DWORD flags,
        IPropertyStore *props, IUnknown **cookie, IMFAsyncCallback *callback, IUnknown *state)
{
    struct byte_stream_handler *handler = byte_stream_handler_from_IMFByteStreamHandler(iface);
    IMFAsyncResult *result;
    IMFMediaSource *source;
    HRESULT hr;
    DWORD caps;

    TRACE("handler %p, stream %p, url %s, flags %#lx, props %p, cookie %p, callback %p, state %p\n",
            handler, stream, debugstr_w(url), flags, props, cookie, callback, state);

    if (cookie)
        *cookie = NULL;
    if (!stream)
        return E_INVALIDARG;
    if (flags != MF_RESOLUTION_MEDIASOURCE)
        FIXME("Unimplemented flags %#lx\n", flags);

    if (FAILED(hr = IMFByteStream_GetCapabilities(stream, &caps)))
        return hr;
    if (!(caps & MFBYTESTREAM_IS_SEEKABLE))
    {
        FIXME("Non-seekable bytestreams not supported\n");
        return MF_E_BYTESTREAM_NOT_SEEKABLE;
    }

    if (FAILED(hr = media_source_create(url, stream, &source)))
        return hr;
    if (SUCCEEDED(hr = MFCreateAsyncResult((IUnknown *)source, callback, state, &result)))
    {
        struct media_source *media_source = media_source_from_IMFMediaSource(source);
        hr = MFPutWorkItem(MFASYNC_CALLBACK_QUEUE_IO, &media_source->async_create_iface, (IUnknown *)result);
        IMFAsyncResult_Release(result);
    }
    IMFMediaSource_Release(source);

    return hr;
}

static HRESULT WINAPI byte_stream_handler_EndCreateObject(IMFByteStreamHandler *iface, IMFAsyncResult *result,
        MF_OBJECT_TYPE *type, IUnknown **object)
{
    struct byte_stream_handler *handler = byte_stream_handler_from_IMFByteStreamHandler(iface);
    HRESULT hr;

    TRACE("handler %p, result %p, type %p, object %p\n", handler, result, type, object);

    *object = NULL;
    *type = MF_OBJECT_INVALID;

    if (SUCCEEDED(hr = IMFAsyncResult_GetStatus(result)))
    {
        hr = IMFAsyncResult_GetObject(result, object);
        *type = MF_OBJECT_MEDIASOURCE;
    }

    return hr;
}

static HRESULT WINAPI byte_stream_handler_CancelObjectCreation(IMFByteStreamHandler *iface, IUnknown *cookie)
{
    struct byte_stream_handler *handler = byte_stream_handler_from_IMFByteStreamHandler(iface);
    FIXME("handler %p, cookie %p, stub!\n", handler, cookie);
    return E_NOTIMPL;
}

static HRESULT WINAPI byte_stream_handler_GetMaxNumberOfBytesRequiredForResolution(IMFByteStreamHandler *iface, QWORD *bytes)
{
    struct byte_stream_handler *handler = byte_stream_handler_from_IMFByteStreamHandler(iface);
    FIXME("handler %p, bytes %p, stub!\n", handler, bytes);
    return E_NOTIMPL;
}

static const IMFByteStreamHandlerVtbl byte_stream_handler_vtbl =
{
    byte_stream_handler_QueryInterface,
    byte_stream_handler_AddRef,
    byte_stream_handler_Release,
    byte_stream_handler_BeginCreateObject,
    byte_stream_handler_EndCreateObject,
    byte_stream_handler_CancelObjectCreation,
    byte_stream_handler_GetMaxNumberOfBytesRequiredForResolution,
};

static HRESULT byte_stream_plugin_create(IUnknown *outer, REFIID riid, void **out)
{
    struct byte_stream_handler *handler;
    HRESULT hr;

    TRACE("outer %p, riid %s, out %p\n", outer, debugstr_guid(riid), out);

    if (outer)
        return CLASS_E_NOAGGREGATION;
    if (!(handler = calloc(1, sizeof(*handler))))
        return E_OUTOFMEMORY;
    handler->IMFByteStreamHandler_iface.lpVtbl = &byte_stream_handler_vtbl;
    handler->refcount = 1;
    TRACE("created %p\n", handler);

    hr = IMFByteStreamHandler_QueryInterface(&handler->IMFByteStreamHandler_iface, riid, out);
    IMFByteStreamHandler_Release(&handler->IMFByteStreamHandler_iface);
    return hr;
}

static BOOL use_gst_byte_stream_handler(void)
{
    BOOL result;
    DWORD size = sizeof(result);

    /* @@ Wine registry key: HKCU\Software\Wine\MediaFoundation */
    if (!RegGetValueW( HKEY_CURRENT_USER, L"Software\\Wine\\MediaFoundation", L"DisableGstByteStreamHandler",
                       RRF_RT_REG_DWORD, NULL, &result, &size ))
        return !result;

    return TRUE;
}

static HRESULT WINAPI asf_byte_stream_plugin_factory_CreateInstance(IClassFactory *iface,
        IUnknown *outer, REFIID riid, void **out)
{
    NTSTATUS status;

    if ((status = winedmo_demuxer_check("video/x-ms-asf")) || use_gst_byte_stream_handler())
    {
        static const GUID CLSID_GStreamerByteStreamHandler = {0x317df618,0x5e5a,0x468a,{0x9f,0x15,0xd8,0x27,0xa9,0xa0,0x81,0x62}};
        if (status) WARN("Unsupported demuxer, status %#lx.\n", status);
        return CoCreateInstance(&CLSID_GStreamerByteStreamHandler, outer, CLSCTX_INPROC_SERVER, riid, out);
    }

    return byte_stream_plugin_create(outer, riid, out);
}

static const IClassFactoryVtbl asf_byte_stream_plugin_factory_vtbl =
{
    class_factory_QueryInterface,
    class_factory_AddRef,
    class_factory_Release,
    asf_byte_stream_plugin_factory_CreateInstance,
    class_factory_LockServer,
};

IClassFactory asf_byte_stream_plugin_factory = {&asf_byte_stream_plugin_factory_vtbl};

static HRESULT WINAPI avi_byte_stream_plugin_factory_CreateInstance(IClassFactory *iface,
        IUnknown *outer, REFIID riid, void **out)
{
    NTSTATUS status;

    if ((status = winedmo_demuxer_check("video/avi")) || use_gst_byte_stream_handler())
    {
        static const GUID CLSID_GStreamerByteStreamHandler = {0x317df618,0x5e5a,0x468a,{0x9f,0x15,0xd8,0x27,0xa9,0xa0,0x81,0x62}};
        if (status) WARN("Unsupported demuxer, status %#lx.\n", status);
        return CoCreateInstance(&CLSID_GStreamerByteStreamHandler, outer, CLSCTX_INPROC_SERVER, riid, out);
    }

    return byte_stream_plugin_create(outer, riid, out);
}

static const IClassFactoryVtbl avi_byte_stream_plugin_factory_vtbl =
{
    class_factory_QueryInterface,
    class_factory_AddRef,
    class_factory_Release,
    avi_byte_stream_plugin_factory_CreateInstance,
    class_factory_LockServer,
};

IClassFactory avi_byte_stream_plugin_factory = {&avi_byte_stream_plugin_factory_vtbl};

static HRESULT WINAPI mpeg4_byte_stream_plugin_factory_CreateInstance(IClassFactory *iface,
        IUnknown *outer, REFIID riid, void **out)
{
    NTSTATUS status;

    if ((status = winedmo_demuxer_check("video/mp4")) || use_gst_byte_stream_handler())
    {
        static const GUID CLSID_GStreamerByteStreamHandler = {0x317df618,0x5e5a,0x468a,{0x9f,0x15,0xd8,0x27,0xa9,0xa0,0x81,0x62}};
        if (status) WARN("Unsupported demuxer, status %#lx.\n", status);
        return CoCreateInstance(&CLSID_GStreamerByteStreamHandler, outer, CLSCTX_INPROC_SERVER, riid, out);
    }

    return byte_stream_plugin_create(outer, riid, out);
}

static const IClassFactoryVtbl mpeg4_byte_stream_plugin_factory_vtbl =
{
    class_factory_QueryInterface,
    class_factory_AddRef,
    class_factory_Release,
    mpeg4_byte_stream_plugin_factory_CreateInstance,
    class_factory_LockServer,
};

IClassFactory mpeg4_byte_stream_plugin_factory = {&mpeg4_byte_stream_plugin_factory_vtbl};

static HRESULT WINAPI wav_byte_stream_plugin_factory_CreateInstance(IClassFactory *iface,
        IUnknown *outer, REFIID riid, void **out)
{
    NTSTATUS status;

    if ((status = winedmo_demuxer_check("audio/wav")) || use_gst_byte_stream_handler())
    {
        static const GUID CLSID_GStreamerByteStreamHandler = {0x317df618,0x5e5a,0x468a,{0x9f,0x15,0xd8,0x27,0xa9,0xa0,0x81,0x62}};
        if (status) WARN("Unsupported demuxer, status %#lx.\n", status);
        return CoCreateInstance(&CLSID_GStreamerByteStreamHandler, outer, CLSCTX_INPROC_SERVER, riid, out);
    }

    return byte_stream_plugin_create(outer, riid, out);
}

static const IClassFactoryVtbl wav_byte_stream_plugin_factory_vtbl =
{
    class_factory_QueryInterface,
    class_factory_AddRef,
    class_factory_Release,
    wav_byte_stream_plugin_factory_CreateInstance,
    class_factory_LockServer,
};

IClassFactory wav_byte_stream_plugin_factory = {&wav_byte_stream_plugin_factory_vtbl};
