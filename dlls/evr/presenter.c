/*
 * Copyright 2020 Nikolay Sivov
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

#define COBJMACROS

#include "evr.h"
#include "d3d9.h"
#include "mfapi.h"
#include "mferror.h"
#include "dxva2api.h"

#include "evr_classes.h"
#include "evr_private.h"

#include "wine/debug.h"
#include "wine/heap.h"

WINE_DEFAULT_DEBUG_CHANNEL(evr);

enum presenter_state
{
    PRESENTER_STATE_SHUT_DOWN = 0,
    PRESENTER_STATE_STARTED,
    PRESENTER_STATE_STOPPED,
    PRESENTER_STATE_PAUSED,
};

enum presenter_flags
{
    PRESENTER_MIXER_HAS_INPUT = 0x1,
};

enum streaming_thread_message
{
    EVRM_STOP = WM_USER,
};

struct streaming_thread
{
    HANDLE hthread;
    HANDLE ready_event;
    DWORD tid;
};

struct video_presenter
{
    IMFVideoPresenter IMFVideoPresenter_iface;
    IMFVideoDeviceID IMFVideoDeviceID_iface;
    IMFTopologyServiceLookupClient IMFTopologyServiceLookupClient_iface;
    IMFVideoDisplayControl IMFVideoDisplayControl_iface;
    IMFRateSupport IMFRateSupport_iface;
    IMFGetService IMFGetService_iface;
    IMFVideoPositionMapper IMFVideoPositionMapper_iface;
    IQualProp IQualProp_iface;
    IMFQualityAdvise IMFQualityAdvise_iface;
    IMFVideoSampleAllocatorNotify allocator_cb;
    IUnknown IUnknown_inner;
    IUnknown *outer_unk;
    LONG refcount;

    IMFTransform *mixer;
    IMFClock *clock;
    IMediaEventSink *event_sink;

    IDirect3DDeviceManager9 *device_manager;
    IDirect3DSwapChain9 *swapchain;
    HANDLE hdevice;

    IMFVideoSampleAllocator *allocator;
    struct streaming_thread thread;
    IMFMediaType *media_type;
    UINT reset_token;
    HWND video_window;
    MFVideoNormalizedRect src_rect;
    RECT dst_rect;
    DWORD rendering_prefs;
    SIZE native_size;
    SIZE native_ratio;
    unsigned int ar_mode;
    unsigned int state;
    unsigned int flags;
    CRITICAL_SECTION cs;
};

static struct video_presenter *impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct video_presenter, IUnknown_inner);
}

static struct video_presenter *impl_from_IMFVideoPresenter(IMFVideoPresenter *iface)
{
    return CONTAINING_RECORD(iface, struct video_presenter, IMFVideoPresenter_iface);
}

static struct video_presenter *impl_from_IMFVideoDeviceID(IMFVideoDeviceID *iface)
{
    return CONTAINING_RECORD(iface, struct video_presenter, IMFVideoDeviceID_iface);
}

static struct video_presenter *impl_from_IMFTopologyServiceLookupClient(IMFTopologyServiceLookupClient *iface)
{
    return CONTAINING_RECORD(iface, struct video_presenter, IMFTopologyServiceLookupClient_iface);
}

static struct video_presenter *impl_from_IMFVideoDisplayControl(IMFVideoDisplayControl *iface)
{
    return CONTAINING_RECORD(iface, struct video_presenter, IMFVideoDisplayControl_iface);
}

static struct video_presenter *impl_from_IMFRateSupport(IMFRateSupport *iface)
{
    return CONTAINING_RECORD(iface, struct video_presenter, IMFRateSupport_iface);
}

static struct video_presenter *impl_from_IMFGetService(IMFGetService *iface)
{
    return CONTAINING_RECORD(iface, struct video_presenter, IMFGetService_iface);
}

static struct video_presenter *impl_from_IMFVideoPositionMapper(IMFVideoPositionMapper *iface)
{
    return CONTAINING_RECORD(iface, struct video_presenter, IMFVideoPositionMapper_iface);
}

static struct video_presenter *impl_from_IMFVideoSampleAllocatorNotify(IMFVideoSampleAllocatorNotify *iface)
{
    return CONTAINING_RECORD(iface, struct video_presenter, allocator_cb);
}

static struct video_presenter *impl_from_IQualProp(IQualProp *iface)
{
    return CONTAINING_RECORD(iface, struct video_presenter, IQualProp_iface);
}

static struct video_presenter *impl_from_IMFQualityAdvise(IMFQualityAdvise *iface)
{
    return CONTAINING_RECORD(iface, struct video_presenter, IMFQualityAdvise_iface);
}

static void video_presenter_notify_renderer(struct video_presenter *presenter,
        LONG event, LONG_PTR param1, LONG_PTR param2)
{
    if (presenter->event_sink)
        IMediaEventSink_Notify(presenter->event_sink, event, param1, param2);
}

static unsigned int get_gcd(unsigned int a, unsigned int b)
{
    unsigned int m;

    while (b)
    {
        m = a % b;
        a = b;
        b = m;
    }

    return a;
}

static HRESULT video_presenter_get_device(struct video_presenter *presenter, IDirect3DDevice9 **device)
{
    HRESULT hr;

    if (!presenter->hdevice)
    {
        if (FAILED(hr = IDirect3DDeviceManager9_OpenDeviceHandle(presenter->device_manager, &presenter->hdevice)))
            return hr;
    }

    return IDirect3DDeviceManager9_LockDevice(presenter->device_manager, presenter->hdevice, device, TRUE);
}

static void video_presenter_get_native_video_size(struct video_presenter *presenter)
{
    IMFMediaType *media_type;
    UINT64 frame_size = 0;

    memset(&presenter->native_size, 0, sizeof(presenter->native_size));
    memset(&presenter->native_ratio, 0, sizeof(presenter->native_ratio));

    if (!presenter->mixer)
        return;

    if (FAILED(IMFTransform_GetInputCurrentType(presenter->mixer, 0, &media_type)))
        return;

    if (SUCCEEDED(IMFMediaType_GetUINT64(media_type, &MF_MT_FRAME_SIZE, &frame_size)))
    {
        unsigned int gcd;

        presenter->native_size.cx = frame_size >> 32;
        presenter->native_size.cy = frame_size;

        if ((gcd = get_gcd(presenter->native_size.cx, presenter->native_size.cy)))
        {
            presenter->native_ratio.cx = presenter->native_size.cx / gcd;
            presenter->native_ratio.cy = presenter->native_size.cy / gcd;
        }
    }

    IMFMediaType_Release(media_type);
}

/* It is important this is called to reset callback too to break circular referencing,
   when allocator keeps a reference of its container, that created it. */
static void video_presenter_set_allocator_callback(struct video_presenter *presenter,
        IMFVideoSampleAllocatorNotify *notify_cb)
{
    IMFVideoSampleAllocatorCallback *cb;

    IMFVideoSampleAllocator_QueryInterface(presenter->allocator, &IID_IMFVideoSampleAllocatorCallback, (void **)&cb);
    IMFVideoSampleAllocatorCallback_SetCallback(cb, notify_cb);
    IMFVideoSampleAllocatorCallback_Release(cb);
}

static void video_presenter_reset_media_type(struct video_presenter *presenter)
{
    if (presenter->media_type)
        IMFMediaType_Release(presenter->media_type);
    presenter->media_type = NULL;

    IMFVideoSampleAllocator_UninitializeSampleAllocator(presenter->allocator);
    video_presenter_set_allocator_callback(presenter, NULL);
}

static HRESULT video_presenter_set_media_type(struct video_presenter *presenter, IMFMediaType *media_type)
{
    unsigned int flags;
    HRESULT hr;

    if (!media_type)
    {
        video_presenter_reset_media_type(presenter);
        return S_OK;
    }

    if (presenter->media_type && IMFMediaType_IsEqual(presenter->media_type, media_type, &flags) == S_OK)
        return S_OK;

    video_presenter_reset_media_type(presenter);

    if (SUCCEEDED(hr = IMFVideoSampleAllocator_InitializeSampleAllocator(presenter->allocator, 3, media_type)))
    {
        presenter->media_type = media_type;
        IMFMediaType_AddRef(presenter->media_type);
    }
    else
        WARN("Failed to initialize sample allocator, hr %#x.\n", hr);

    return hr;
}

static HRESULT video_presenter_invalidate_media_type(struct video_presenter *presenter)
{
    IMFMediaType *media_type;
    unsigned int idx = 0;
    HRESULT hr;

    video_presenter_get_native_video_size(presenter);

    while (SUCCEEDED(hr = IMFTransform_GetOutputAvailableType(presenter->mixer, 0, idx++, &media_type)))
    {
        /* FIXME: check that d3d device supports this format */

        /* FIXME: potentially adjust frame size */

        hr = IMFTransform_SetOutputType(presenter->mixer, 0, media_type, MFT_SET_TYPE_TEST_ONLY);

        if (SUCCEEDED(hr))
            hr = video_presenter_set_media_type(presenter, media_type);

        if (SUCCEEDED(hr))
            hr = IMFTransform_SetOutputType(presenter->mixer, 0, media_type, 0);

        IMFMediaType_Release(media_type);

        if (SUCCEEDED(hr))
            break;
    }

    return hr;
}

static DWORD CALLBACK video_presenter_streaming_thread(void *arg)
{
    struct video_presenter *presenter = arg;
    BOOL stop_thread = FALSE;
    MSG msg;

    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);

    SetEvent(presenter->thread.ready_event);

    while (!stop_thread)
    {
        MsgWaitForMultipleObjects(0, NULL, FALSE, INFINITE, QS_POSTMESSAGE);

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            switch (msg.message)
            {
                case EVRM_STOP:
                    stop_thread = TRUE;
                    break;

                default:
                    ;
            }
        }
    }

    return 0;
}

static HRESULT video_presenter_start_streaming(struct video_presenter *presenter)
{
    if (presenter->thread.hthread)
        return S_OK;

    if (!(presenter->thread.ready_event = CreateEventW(NULL, FALSE, FALSE, NULL)))
        return HRESULT_FROM_WIN32(GetLastError());

    if (!(presenter->thread.hthread = CreateThread(NULL, 0, video_presenter_streaming_thread,
            presenter, 0, &presenter->thread.tid)))
    {
        WARN("Failed to create streaming thread.\n");
        CloseHandle(presenter->thread.ready_event);
        presenter->thread.ready_event = NULL;
        return E_FAIL;
    }

    video_presenter_set_allocator_callback(presenter, &presenter->allocator_cb);

    WaitForSingleObject(presenter->thread.ready_event, INFINITE);
    CloseHandle(presenter->thread.ready_event);
    presenter->thread.ready_event = NULL;

    TRACE("Started streaming thread, tid %#x.\n", presenter->thread.tid);

    return S_OK;
}

static HRESULT video_presenter_end_streaming(struct video_presenter *presenter)
{
    if (!presenter->thread.hthread)
        return S_OK;

    PostThreadMessageW(presenter->thread.tid, EVRM_STOP, 0, 0);

    WaitForSingleObject(presenter->thread.hthread, INFINITE);
    CloseHandle(presenter->thread.hthread);

    TRACE("Terminated streaming thread tid %#x.\n", presenter->thread.tid);

    memset(&presenter->thread, 0, sizeof(presenter->thread));
    video_presenter_set_allocator_callback(presenter, NULL);

    return S_OK;
}

static HRESULT video_presenter_process_input(struct video_presenter *presenter)
{
    MFT_OUTPUT_DATA_BUFFER buffer;
    HRESULT hr = S_OK;
    IMFSample *sample;
    DWORD status;

    if (!presenter->media_type)
        return S_OK;

    while (hr == S_OK)
    {
        LONGLONG mixing_started, mixing_finished;
        MFTIME systime;

        if (!(presenter->flags & PRESENTER_MIXER_HAS_INPUT))
            break;

        if (FAILED(hr = IMFVideoSampleAllocator_AllocateSample(presenter->allocator, &sample)))
        {
            WARN("Failed to allocate a sample, hr %#x.\n", hr);
            break;
        }

        memset(&buffer, 0, sizeof(buffer));
        buffer.pSample = sample;

        if (presenter->clock)
            IMFClock_GetCorrelatedTime(presenter->clock, 0, &mixing_started, &systime);

        if (FAILED(hr = IMFTransform_ProcessOutput(presenter->mixer, 0, 1, &buffer, &status)))
        {
            /* FIXME: failure path probably needs to handle some errors specifically */
            presenter->flags &= ~PRESENTER_MIXER_HAS_INPUT;
            IMFSample_Release(sample);
            break;
        }
        else
        {
            if (presenter->clock)
            {
                LONGLONG latency;

                IMFClock_GetCorrelatedTime(presenter->clock, 0, &mixing_finished, &systime);
                latency = mixing_finished - mixing_started;
                video_presenter_notify_renderer(presenter, EC_PROCESSING_LATENCY, (LONG_PTR)&latency, 0);
            }

            if (buffer.pEvents)
                IMFCollection_Release(buffer.pEvents);

            /* FIXME: for now drop output sample back to the pool */
            IMFSample_Release(sample);
        }
    }

    return S_OK;
}

static HRESULT WINAPI video_presenter_inner_QueryInterface(IUnknown *iface, REFIID riid, void **obj)
{
    struct video_presenter *presenter = impl_from_IUnknown(iface);

    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
    }
    else if (IsEqualIID(riid, &IID_IMFClockStateSink)
            || IsEqualIID(riid, &IID_IMFVideoPresenter))
    {
        *obj = &presenter->IMFVideoPresenter_iface;
    }
    else if (IsEqualIID(riid, &IID_IMFVideoDeviceID))
    {
        *obj = &presenter->IMFVideoDeviceID_iface;
    }
    else if (IsEqualIID(riid, &IID_IMFTopologyServiceLookupClient))
    {
        *obj = &presenter->IMFTopologyServiceLookupClient_iface;
    }
    else if (IsEqualIID(riid, &IID_IMFVideoDisplayControl))
    {
        *obj = &presenter->IMFVideoDisplayControl_iface;
    }
    else if (IsEqualIID(riid, &IID_IMFRateSupport))
    {
        *obj = &presenter->IMFRateSupport_iface;
    }
    else if (IsEqualIID(riid, &IID_IMFGetService))
    {
        *obj = &presenter->IMFGetService_iface;
    }
    else if (IsEqualIID(riid, &IID_IMFVideoPositionMapper))
    {
        *obj = &presenter->IMFVideoPositionMapper_iface;
    }
    else if (IsEqualIID(riid, &IID_IQualProp))
    {
        *obj = &presenter->IQualProp_iface;
    }
    else if (IsEqualIID(riid, &IID_IMFQualityAdvise))
    {
        *obj = &presenter->IMFQualityAdvise_iface;
    }
    else
    {
        WARN("Unimplemented interface %s.\n", debugstr_guid(riid));
        *obj = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*obj);
    return S_OK;
}

static ULONG WINAPI video_presenter_inner_AddRef(IUnknown *iface)
{
    struct video_presenter *presenter = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedIncrement(&presenter->refcount);

    TRACE("%p, refcount %u.\n", iface, refcount);

    return refcount;
}

static void video_presenter_clear_container(struct video_presenter *presenter)
{
    if (presenter->clock)
        IMFClock_Release(presenter->clock);
    if (presenter->mixer)
        IMFTransform_Release(presenter->mixer);
    if (presenter->event_sink)
        IMediaEventSink_Release(presenter->event_sink);
    presenter->clock = NULL;
    presenter->mixer = NULL;
    presenter->event_sink = NULL;
}

static ULONG WINAPI video_presenter_inner_Release(IUnknown *iface)
{
    struct video_presenter *presenter = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedDecrement(&presenter->refcount);

    TRACE("%p, refcount %u.\n", iface, refcount);

    if (!refcount)
    {
        video_presenter_end_streaming(presenter);
        video_presenter_clear_container(presenter);
        video_presenter_reset_media_type(presenter);
        DeleteCriticalSection(&presenter->cs);
        if (presenter->swapchain)
            IDirect3DSwapChain9_Release(presenter->swapchain);
        if (presenter->device_manager)
        {
            IDirect3DDeviceManager9_CloseDeviceHandle(presenter->device_manager, presenter->hdevice);
            IDirect3DDeviceManager9_Release(presenter->device_manager);
        }
        if (presenter->allocator)
            IMFVideoSampleAllocator_Release(presenter->allocator);
        heap_free(presenter);
    }

    return refcount;
}

static const IUnknownVtbl video_presenter_inner_vtbl =
{
    video_presenter_inner_QueryInterface,
    video_presenter_inner_AddRef,
    video_presenter_inner_Release,
};

static HRESULT WINAPI video_presenter_QueryInterface(IMFVideoPresenter *iface, REFIID riid, void **obj)
{
    struct video_presenter *presenter = impl_from_IMFVideoPresenter(iface);
    return IUnknown_QueryInterface(presenter->outer_unk, riid, obj);
}

static ULONG WINAPI video_presenter_AddRef(IMFVideoPresenter *iface)
{
    struct video_presenter *presenter = impl_from_IMFVideoPresenter(iface);
    return IUnknown_AddRef(presenter->outer_unk);
}

static ULONG WINAPI video_presenter_Release(IMFVideoPresenter *iface)
{
    struct video_presenter *presenter = impl_from_IMFVideoPresenter(iface);
    return IUnknown_Release(presenter->outer_unk);
}

static HRESULT WINAPI video_presenter_OnClockStart(IMFVideoPresenter *iface, MFTIME systime, LONGLONG offset)
{
    struct video_presenter *presenter = impl_from_IMFVideoPresenter(iface);

    TRACE("%p, %s, %s.\n", iface, debugstr_time(systime), wine_dbgstr_longlong(offset));

    EnterCriticalSection(&presenter->cs);
    presenter->state = PRESENTER_STATE_STARTED;
    LeaveCriticalSection(&presenter->cs);

    return S_OK;
}

static HRESULT WINAPI video_presenter_OnClockStop(IMFVideoPresenter *iface, MFTIME systime)
{
    struct video_presenter *presenter = impl_from_IMFVideoPresenter(iface);

    TRACE("%p, %s.\n", iface, debugstr_time(systime));

    EnterCriticalSection(&presenter->cs);
    presenter->state = PRESENTER_STATE_STOPPED;
    LeaveCriticalSection(&presenter->cs);

    return S_OK;
}

static HRESULT WINAPI video_presenter_OnClockPause(IMFVideoPresenter *iface, MFTIME systime)
{
    struct video_presenter *presenter = impl_from_IMFVideoPresenter(iface);

    TRACE("%p, %s.\n", iface, debugstr_time(systime));

    EnterCriticalSection(&presenter->cs);
    presenter->state = PRESENTER_STATE_PAUSED;
    LeaveCriticalSection(&presenter->cs);

    return S_OK;
}

static HRESULT WINAPI video_presenter_OnClockRestart(IMFVideoPresenter *iface, MFTIME systime)
{
    struct video_presenter *presenter = impl_from_IMFVideoPresenter(iface);

    TRACE("%p, %s.\n", iface, debugstr_time(systime));

    EnterCriticalSection(&presenter->cs);
    presenter->state = PRESENTER_STATE_STARTED;
    LeaveCriticalSection(&presenter->cs);

    return S_OK;
}

static HRESULT WINAPI video_presenter_OnClockSetRate(IMFVideoPresenter *iface, MFTIME systime, float rate)
{
    FIXME("%p, %s, %f.\n", iface, debugstr_time(systime), rate);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_ProcessMessage(IMFVideoPresenter *iface, MFVP_MESSAGE_TYPE message, ULONG_PTR param)
{
    struct video_presenter *presenter = impl_from_IMFVideoPresenter(iface);
    HRESULT hr;

    TRACE("%p, %d, %lu.\n", iface, message, param);

    EnterCriticalSection(&presenter->cs);

    switch (message)
    {
        case MFVP_MESSAGE_INVALIDATEMEDIATYPE:
            hr = video_presenter_invalidate_media_type(presenter);
            break;
        case MFVP_MESSAGE_BEGINSTREAMING:
            hr = video_presenter_start_streaming(presenter);
            break;
        case MFVP_MESSAGE_ENDSTREAMING:
            hr = video_presenter_end_streaming(presenter);
            break;
        case MFVP_MESSAGE_PROCESSINPUTNOTIFY:
            presenter->flags |= PRESENTER_MIXER_HAS_INPUT;
            hr = video_presenter_process_input(presenter);
            break;
        default:
            FIXME("Unsupported message %u.\n", message);
            hr = E_NOTIMPL;
    }

    LeaveCriticalSection(&presenter->cs);

    return hr;
}

static HRESULT WINAPI video_presenter_GetCurrentMediaType(IMFVideoPresenter *iface,
        IMFVideoMediaType **media_type)
{
    struct video_presenter *presenter = impl_from_IMFVideoPresenter(iface);
    HRESULT hr;

    TRACE("%p, %p.\n", iface, media_type);

    EnterCriticalSection(&presenter->cs);

    if (presenter->state == PRESENTER_STATE_SHUT_DOWN)
        hr = MF_E_SHUTDOWN;
    else if (!presenter->media_type)
        hr = MF_E_NOT_INITIALIZED;
    else
    {
        hr = IMFMediaType_QueryInterface(presenter->media_type, &IID_IMFVideoMediaType,
                (void **)media_type);
    }

    LeaveCriticalSection(&presenter->cs);

    return hr;
}

static const IMFVideoPresenterVtbl video_presenter_vtbl =
{
    video_presenter_QueryInterface,
    video_presenter_AddRef,
    video_presenter_Release,
    video_presenter_OnClockStart,
    video_presenter_OnClockStop,
    video_presenter_OnClockPause,
    video_presenter_OnClockRestart,
    video_presenter_OnClockSetRate,
    video_presenter_ProcessMessage,
    video_presenter_GetCurrentMediaType,
};

static HRESULT WINAPI video_presenter_device_id_QueryInterface(IMFVideoDeviceID *iface, REFIID riid, void **obj)
{
    struct video_presenter *presenter = impl_from_IMFVideoDeviceID(iface);
    return IMFVideoPresenter_QueryInterface(&presenter->IMFVideoPresenter_iface, riid, obj);
}

static ULONG WINAPI video_presenter_device_id_AddRef(IMFVideoDeviceID *iface)
{
    struct video_presenter *presenter = impl_from_IMFVideoDeviceID(iface);
    return IMFVideoPresenter_AddRef(&presenter->IMFVideoPresenter_iface);
}

static ULONG WINAPI video_presenter_device_id_Release(IMFVideoDeviceID *iface)
{
    struct video_presenter *presenter = impl_from_IMFVideoDeviceID(iface);
    return IMFVideoPresenter_Release(&presenter->IMFVideoPresenter_iface);
}

static HRESULT WINAPI video_presenter_device_id_GetDeviceID(IMFVideoDeviceID *iface, IID *device_id)
{
    TRACE("%p, %p.\n", iface, device_id);

    if (!device_id)
        return E_POINTER;

    memcpy(device_id, &IID_IDirect3DDevice9, sizeof(*device_id));

    return S_OK;
}

static const IMFVideoDeviceIDVtbl video_presenter_device_id_vtbl =
{
    video_presenter_device_id_QueryInterface,
    video_presenter_device_id_AddRef,
    video_presenter_device_id_Release,
    video_presenter_device_id_GetDeviceID,
};

static HRESULT WINAPI video_presenter_service_client_QueryInterface(IMFTopologyServiceLookupClient *iface,
        REFIID riid, void **obj)
{
    struct video_presenter *presenter = impl_from_IMFTopologyServiceLookupClient(iface);
    return IMFVideoPresenter_QueryInterface(&presenter->IMFVideoPresenter_iface, riid, obj);
}

static ULONG WINAPI video_presenter_service_client_AddRef(IMFTopologyServiceLookupClient *iface)
{
    struct video_presenter *presenter = impl_from_IMFTopologyServiceLookupClient(iface);
    return IMFVideoPresenter_AddRef(&presenter->IMFVideoPresenter_iface);
}

static ULONG WINAPI video_presenter_service_client_Release(IMFTopologyServiceLookupClient *iface)
{
    struct video_presenter *presenter = impl_from_IMFTopologyServiceLookupClient(iface);
    return IMFVideoPresenter_Release(&presenter->IMFVideoPresenter_iface);
}

static void video_presenter_set_mixer_rect(struct video_presenter *presenter)
{
    IMFAttributes *attributes;
    HRESULT hr;

    if (!presenter->mixer)
        return;

    if (SUCCEEDED(IMFTransform_GetAttributes(presenter->mixer, &attributes)))
    {
        if (FAILED(hr = IMFAttributes_SetBlob(attributes, &VIDEO_ZOOM_RECT, (const UINT8 *)&presenter->src_rect,
                sizeof(presenter->src_rect))))
        {
            WARN("Failed to set zoom rectangle attribute, hr %#x.\n", hr);
        }
        IMFAttributes_Release(attributes);
    }
}

static HRESULT video_presenter_attach_mixer(struct video_presenter *presenter, IMFTopologyServiceLookup *service_lookup)
{
    IMFVideoDeviceID *device_id;
    unsigned int count;
    GUID id = { 0 };
    HRESULT hr;

    count = 1;
    if (FAILED(hr = IMFTopologyServiceLookup_LookupService(service_lookup, MF_SERVICE_LOOKUP_GLOBAL, 0,
            &MR_VIDEO_MIXER_SERVICE, &IID_IMFTransform, (void **)&presenter->mixer, &count)))
    {
        WARN("Failed to get mixer interface, hr %#x.\n", hr);
        return hr;
    }

    if (SUCCEEDED(hr = IMFTransform_QueryInterface(presenter->mixer, &IID_IMFVideoDeviceID, (void **)&device_id)))
    {
        if (SUCCEEDED(hr = IMFVideoDeviceID_GetDeviceID(device_id, &id)))
        {
            if (!IsEqualGUID(&id, &IID_IDirect3DDevice9))
                hr = MF_E_INVALIDREQUEST;
        }

        IMFVideoDeviceID_Release(device_id);
    }

    if (FAILED(hr))
    {
        IMFTransform_Release(presenter->mixer);
        presenter->mixer = NULL;
    }

    video_presenter_set_mixer_rect(presenter);
    video_presenter_get_native_video_size(presenter);

    return hr;
}

static HRESULT WINAPI video_presenter_service_client_InitServicePointers(IMFTopologyServiceLookupClient *iface,
        IMFTopologyServiceLookup *service_lookup)
{
    struct video_presenter *presenter = impl_from_IMFTopologyServiceLookupClient(iface);
    unsigned int count;
    HRESULT hr = S_OK;

    TRACE("%p, %p.\n", iface, service_lookup);

    if (!service_lookup)
        return E_POINTER;

    EnterCriticalSection(&presenter->cs);

    if (presenter->state == PRESENTER_STATE_STARTED ||
            presenter->state == PRESENTER_STATE_PAUSED)
    {
        hr = MF_E_INVALIDREQUEST;
    }
    else
    {
        video_presenter_clear_container(presenter);

        count = 1;
        IMFTopologyServiceLookup_LookupService(service_lookup, MF_SERVICE_LOOKUP_GLOBAL, 0,
                &MR_VIDEO_RENDER_SERVICE, &IID_IMFClock, (void **)&presenter->clock, &count);

        hr = video_presenter_attach_mixer(presenter, service_lookup);

        if (SUCCEEDED(hr))
        {
            count = 1;
            if (FAILED(hr = IMFTopologyServiceLookup_LookupService(service_lookup, MF_SERVICE_LOOKUP_GLOBAL, 0,
                    &MR_VIDEO_RENDER_SERVICE, &IID_IMediaEventSink, (void **)&presenter->event_sink, &count)))
            {
                WARN("Failed to get renderer event sink, hr %#x.\n", hr);
            }
        }

        if (SUCCEEDED(hr))
            presenter->state = PRESENTER_STATE_STOPPED;
    }

    LeaveCriticalSection(&presenter->cs);

    return hr;
}

static HRESULT WINAPI video_presenter_service_client_ReleaseServicePointers(IMFTopologyServiceLookupClient *iface)
{
    struct video_presenter *presenter = impl_from_IMFTopologyServiceLookupClient(iface);

    TRACE("%p.\n", iface);

    EnterCriticalSection(&presenter->cs);

    presenter->state = PRESENTER_STATE_SHUT_DOWN;
    video_presenter_clear_container(presenter);

    LeaveCriticalSection(&presenter->cs);

    return S_OK;
}

static const IMFTopologyServiceLookupClientVtbl video_presenter_service_client_vtbl =
{
    video_presenter_service_client_QueryInterface,
    video_presenter_service_client_AddRef,
    video_presenter_service_client_Release,
    video_presenter_service_client_InitServicePointers,
    video_presenter_service_client_ReleaseServicePointers,
};

static HRESULT WINAPI video_presenter_control_QueryInterface(IMFVideoDisplayControl *iface, REFIID riid, void **obj)
{
    struct video_presenter *presenter = impl_from_IMFVideoDisplayControl(iface);
    return IMFVideoPresenter_QueryInterface(&presenter->IMFVideoPresenter_iface, riid, obj);
}

static ULONG WINAPI video_presenter_control_AddRef(IMFVideoDisplayControl *iface)
{
    struct video_presenter *presenter = impl_from_IMFVideoDisplayControl(iface);
    return IMFVideoPresenter_AddRef(&presenter->IMFVideoPresenter_iface);
}

static ULONG WINAPI video_presenter_control_Release(IMFVideoDisplayControl *iface)
{
    struct video_presenter *presenter = impl_from_IMFVideoDisplayControl(iface);
    return IMFVideoPresenter_Release(&presenter->IMFVideoPresenter_iface);
}

static HRESULT WINAPI video_presenter_control_GetNativeVideoSize(IMFVideoDisplayControl *iface, SIZE *video_size,
        SIZE *aspect_ratio)
{
    struct video_presenter *presenter = impl_from_IMFVideoDisplayControl(iface);

    TRACE("%p, %p, %p.\n", iface, video_size, aspect_ratio);

    if (!video_size && !aspect_ratio)
        return E_POINTER;

    EnterCriticalSection(&presenter->cs);
    if (video_size)
        *video_size = presenter->native_size;
    if (aspect_ratio)
        *aspect_ratio = presenter->native_ratio;
    LeaveCriticalSection(&presenter->cs);

    return S_OK;
}

static HRESULT WINAPI video_presenter_control_GetIdealVideoSize(IMFVideoDisplayControl *iface, SIZE *min_size,
        SIZE *max_size)
{
    FIXME("%p, %p, %p.\n", iface, min_size, max_size);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_control_SetVideoPosition(IMFVideoDisplayControl *iface,
        const MFVideoNormalizedRect *src_rect, const RECT *dst_rect)
{
    struct video_presenter *presenter = impl_from_IMFVideoDisplayControl(iface);
    HRESULT hr = S_OK;

    TRACE("%p, %p, %s.\n", iface, src_rect, wine_dbgstr_rect(dst_rect));

    if (!src_rect && !dst_rect)
        return E_POINTER;

    if (src_rect && (src_rect->left < 0.0f || src_rect->top < 0.0f ||
            src_rect->right > 1.0f || src_rect->bottom > 1.0f ||
            src_rect->left > src_rect->right ||
            src_rect->top > src_rect->bottom))
    {
        return E_INVALIDARG;
    }

    if (dst_rect && (dst_rect->left > dst_rect->right ||
            dst_rect->top > dst_rect->bottom))
        return E_INVALIDARG;

    EnterCriticalSection(&presenter->cs);
    if (!presenter->video_window)
        hr = E_POINTER;
    else
    {
        if (src_rect)
        {
            if (memcmp(&presenter->src_rect, src_rect, sizeof(*src_rect)))
            {
                presenter->src_rect = *src_rect;
                video_presenter_set_mixer_rect(presenter);
            }
        }
        if (dst_rect)
            presenter->dst_rect = *dst_rect;
    }
    LeaveCriticalSection(&presenter->cs);

    return hr;
}

static HRESULT WINAPI video_presenter_control_GetVideoPosition(IMFVideoDisplayControl *iface, MFVideoNormalizedRect *src_rect,
        RECT *dst_rect)
{
    struct video_presenter *presenter = impl_from_IMFVideoDisplayControl(iface);

    TRACE("%p, %p, %p.\n", iface, src_rect, dst_rect);

    if (!src_rect || !dst_rect)
        return E_POINTER;

    EnterCriticalSection(&presenter->cs);
    *src_rect = presenter->src_rect;
    *dst_rect = presenter->dst_rect;
    LeaveCriticalSection(&presenter->cs);

    return S_OK;
}

static HRESULT WINAPI video_presenter_control_SetAspectRatioMode(IMFVideoDisplayControl *iface, DWORD mode)
{
    struct video_presenter *presenter = impl_from_IMFVideoDisplayControl(iface);

    TRACE("%p, %#x.\n", iface, mode);

    if (mode & ~MFVideoARMode_Mask)
        return E_INVALIDARG;

    EnterCriticalSection(&presenter->cs);
    presenter->ar_mode = mode;
    LeaveCriticalSection(&presenter->cs);

    return S_OK;
}

static HRESULT WINAPI video_presenter_control_GetAspectRatioMode(IMFVideoDisplayControl *iface, DWORD *mode)
{
    struct video_presenter *presenter = impl_from_IMFVideoDisplayControl(iface);

    TRACE("%p, %p.\n", iface, mode);

    if (!mode)
        return E_POINTER;

    EnterCriticalSection(&presenter->cs);
    *mode = presenter->ar_mode;
    LeaveCriticalSection(&presenter->cs);

    return S_OK;
}

static HRESULT video_presenter_create_swapchain(struct video_presenter *presenter)
{
    D3DPRESENT_PARAMETERS present_params = { 0 };
    IDirect3DDevice9 *d3d_device;
    HRESULT hr;

    if (SUCCEEDED(hr = video_presenter_get_device(presenter, &d3d_device)))
    {
        present_params.hDeviceWindow = presenter->video_window;
        present_params.Windowed = TRUE;
        present_params.SwapEffect = D3DSWAPEFFECT_COPY;
        present_params.Flags = D3DPRESENTFLAG_VIDEO;
        present_params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        hr = IDirect3DDevice9_CreateAdditionalSwapChain(d3d_device, &present_params, &presenter->swapchain);

        IDirect3DDevice9_Release(d3d_device);
        IDirect3DDeviceManager9_UnlockDevice(presenter->device_manager, presenter->hdevice, FALSE);
    }

    return hr;
}

static HRESULT WINAPI video_presenter_control_SetVideoWindow(IMFVideoDisplayControl *iface, HWND window)
{
    struct video_presenter *presenter = impl_from_IMFVideoDisplayControl(iface);
    HRESULT hr = S_OK;

    TRACE("%p, %p.\n", iface, window);

    if (!IsWindow(window))
        return E_INVALIDARG;

    EnterCriticalSection(&presenter->cs);
    if (presenter->video_window != window)
    {
        if (presenter->swapchain)
            IDirect3DSwapChain9_Release(presenter->swapchain);
        presenter->video_window = window;
        hr = video_presenter_create_swapchain(presenter);
    }
    LeaveCriticalSection(&presenter->cs);

    return hr;
}

static HRESULT WINAPI video_presenter_control_GetVideoWindow(IMFVideoDisplayControl *iface, HWND *window)
{
    struct video_presenter *presenter = impl_from_IMFVideoDisplayControl(iface);

    TRACE("%p, %p.\n", iface, window);

    if (!window)
        return E_POINTER;

    EnterCriticalSection(&presenter->cs);
    *window = presenter->video_window;
    LeaveCriticalSection(&presenter->cs);

    return S_OK;
}

static HRESULT WINAPI video_presenter_control_RepaintVideo(IMFVideoDisplayControl *iface)
{
    FIXME("%p.\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_control_GetCurrentImage(IMFVideoDisplayControl *iface, BITMAPINFOHEADER *header,
        BYTE **dib, DWORD *dib_size, LONGLONG *timestamp)
{
    FIXME("%p, %p, %p, %p, %p.\n", iface, header, dib, dib_size, timestamp);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_control_SetBorderColor(IMFVideoDisplayControl *iface, COLORREF color)
{
    FIXME("%p, %#x.\n", iface, color);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_control_GetBorderColor(IMFVideoDisplayControl *iface, COLORREF *color)
{
    FIXME("%p, %p.\n", iface, color);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_control_SetRenderingPrefs(IMFVideoDisplayControl *iface, DWORD flags)
{
    FIXME("%p, %#x.\n", iface, flags);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_control_GetRenderingPrefs(IMFVideoDisplayControl *iface, DWORD *flags)
{
    struct video_presenter *presenter = impl_from_IMFVideoDisplayControl(iface);

    TRACE("%p, %p.\n", iface, flags);

    if (!flags)
        return E_POINTER;

    EnterCriticalSection(&presenter->cs);
    *flags = presenter->rendering_prefs;
    LeaveCriticalSection(&presenter->cs);

    return S_OK;
}

static HRESULT WINAPI video_presenter_control_SetFullscreen(IMFVideoDisplayControl *iface, BOOL fullscreen)
{
    FIXME("%p, %d.\n", iface, fullscreen);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_control_GetFullscreen(IMFVideoDisplayControl *iface, BOOL *fullscreen)
{
    FIXME("%p, %p.\n", iface, fullscreen);

    return E_NOTIMPL;
}

static const IMFVideoDisplayControlVtbl video_presenter_control_vtbl =
{
    video_presenter_control_QueryInterface,
    video_presenter_control_AddRef,
    video_presenter_control_Release,
    video_presenter_control_GetNativeVideoSize,
    video_presenter_control_GetIdealVideoSize,
    video_presenter_control_SetVideoPosition,
    video_presenter_control_GetVideoPosition,
    video_presenter_control_SetAspectRatioMode,
    video_presenter_control_GetAspectRatioMode,
    video_presenter_control_SetVideoWindow,
    video_presenter_control_GetVideoWindow,
    video_presenter_control_RepaintVideo,
    video_presenter_control_GetCurrentImage,
    video_presenter_control_SetBorderColor,
    video_presenter_control_GetBorderColor,
    video_presenter_control_SetRenderingPrefs,
    video_presenter_control_GetRenderingPrefs,
    video_presenter_control_SetFullscreen,
    video_presenter_control_GetFullscreen,
};

static HRESULT WINAPI video_presenter_rate_support_QueryInterface(IMFRateSupport *iface, REFIID riid, void **obj)
{
    struct video_presenter *presenter = impl_from_IMFRateSupport(iface);
    return IMFVideoPresenter_QueryInterface(&presenter->IMFVideoPresenter_iface, riid, obj);
}

static ULONG WINAPI video_presenter_rate_support_AddRef(IMFRateSupport *iface)
{
    struct video_presenter *presenter = impl_from_IMFRateSupport(iface);
    return IMFVideoPresenter_AddRef(&presenter->IMFVideoPresenter_iface);
}

static ULONG WINAPI video_presenter_rate_support_Release(IMFRateSupport *iface)
{
    struct video_presenter *presenter = impl_from_IMFRateSupport(iface);
    return IMFVideoPresenter_Release(&presenter->IMFVideoPresenter_iface);
}

static HRESULT WINAPI video_presenter_rate_support_GetSlowestRate(IMFRateSupport *iface, MFRATE_DIRECTION direction,
        BOOL thin, float *rate)
{
    TRACE("%p, %d, %d, %p.\n", iface, direction, thin, rate);

    *rate = 0.0f;

    return S_OK;
}

static HRESULT WINAPI video_presenter_rate_support_GetFastestRate(IMFRateSupport *iface, MFRATE_DIRECTION direction,
        BOOL thin, float *rate)
{
    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_rate_support_IsRateSupported(IMFRateSupport *iface, BOOL thin, float rate,
        float *nearest_supported_rate)
{
    return E_NOTIMPL;
}

static const IMFRateSupportVtbl video_presenter_rate_support_vtbl =
{
    video_presenter_rate_support_QueryInterface,
    video_presenter_rate_support_AddRef,
    video_presenter_rate_support_Release,
    video_presenter_rate_support_GetSlowestRate,
    video_presenter_rate_support_GetFastestRate,
    video_presenter_rate_support_IsRateSupported,
};

static HRESULT WINAPI video_presenter_getservice_QueryInterface(IMFGetService *iface, REFIID riid, void **obj)
{
    struct video_presenter *presenter = impl_from_IMFGetService(iface);
    return IMFVideoPresenter_QueryInterface(&presenter->IMFVideoPresenter_iface, riid, obj);
}

static ULONG WINAPI video_presenter_getservice_AddRef(IMFGetService *iface)
{
    struct video_presenter *presenter = impl_from_IMFGetService(iface);
    return IMFVideoPresenter_AddRef(&presenter->IMFVideoPresenter_iface);
}

static ULONG WINAPI video_presenter_getservice_Release(IMFGetService *iface)
{
    struct video_presenter *presenter = impl_from_IMFGetService(iface);
    return IMFVideoPresenter_Release(&presenter->IMFVideoPresenter_iface);
}

static HRESULT WINAPI video_presenter_getservice_GetService(IMFGetService *iface, REFGUID service, REFIID riid, void **obj)
{
    struct video_presenter *presenter = impl_from_IMFGetService(iface);

    TRACE("%p, %s, %s, %p.\n", iface, debugstr_guid(service), debugstr_guid(riid), obj);

    if (IsEqualGUID(&MR_VIDEO_ACCELERATION_SERVICE, service))
        return IDirect3DDeviceManager9_QueryInterface(presenter->device_manager, riid, obj);

    if (IsEqualGUID(&MR_VIDEO_RENDER_SERVICE, service))
        return IMFVideoPresenter_QueryInterface(&presenter->IMFVideoPresenter_iface, riid, obj);

    FIXME("Unimplemented service %s.\n", debugstr_guid(service));

    return E_NOTIMPL;
}

static const IMFGetServiceVtbl video_presenter_getservice_vtbl =
{
    video_presenter_getservice_QueryInterface,
    video_presenter_getservice_AddRef,
    video_presenter_getservice_Release,
    video_presenter_getservice_GetService,
};

static HRESULT WINAPI video_presenter_position_mapper_QueryInterface(IMFVideoPositionMapper *iface, REFIID riid, void **obj)
{
    struct video_presenter *presenter = impl_from_IMFVideoPositionMapper(iface);
    return IMFVideoPresenter_QueryInterface(&presenter->IMFVideoPresenter_iface, riid, obj);
}

static ULONG WINAPI video_presenter_position_mapper_AddRef(IMFVideoPositionMapper *iface)
{
    struct video_presenter *presenter = impl_from_IMFVideoPositionMapper(iface);
    return IMFVideoPresenter_AddRef(&presenter->IMFVideoPresenter_iface);
}

static ULONG WINAPI video_presenter_position_mapper_Release(IMFVideoPositionMapper *iface)
{
    struct video_presenter *presenter = impl_from_IMFVideoPositionMapper(iface);
    return IMFVideoPresenter_Release(&presenter->IMFVideoPresenter_iface);
}

static HRESULT WINAPI video_presenter_position_mapper_MapOutputCoordinateToInputStream(IMFVideoPositionMapper *iface,
        float x_out, float y_out, DWORD output_stream, DWORD input_stream, float *x_in, float *y_in)
{
    FIXME("%p, %f, %f, %u, %u, %p, %p.\n", iface, x_out, y_out, output_stream, input_stream, x_in, y_in);

    return E_NOTIMPL;
}

static const IMFVideoPositionMapperVtbl video_presenter_position_mapper_vtbl =
{
    video_presenter_position_mapper_QueryInterface,
    video_presenter_position_mapper_AddRef,
    video_presenter_position_mapper_Release,
    video_presenter_position_mapper_MapOutputCoordinateToInputStream,
};

static HRESULT WINAPI video_presenter_allocator_cb_QueryInterface(IMFVideoSampleAllocatorNotify *iface,
        REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IMFVideoSampleAllocatorNotify) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IMFVideoSampleAllocatorNotify_AddRef(iface);
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI video_presenter_allocator_cb_AddRef(IMFVideoSampleAllocatorNotify *iface)
{
    struct video_presenter *presenter = impl_from_IMFVideoSampleAllocatorNotify(iface);
    return IMFVideoPresenter_AddRef(&presenter->IMFVideoPresenter_iface);
}

static ULONG WINAPI video_presenter_allocator_cb_Release(IMFVideoSampleAllocatorNotify *iface)
{
    struct video_presenter *presenter = impl_from_IMFVideoSampleAllocatorNotify(iface);
    return IMFVideoPresenter_Release(&presenter->IMFVideoPresenter_iface);
}

static HRESULT WINAPI video_presenter_allocator_cb_NotifyRelease(IMFVideoSampleAllocatorNotify *iface)
{
    return E_NOTIMPL;
}

static const IMFVideoSampleAllocatorNotifyVtbl video_presenter_allocator_cb_vtbl =
{
    video_presenter_allocator_cb_QueryInterface,
    video_presenter_allocator_cb_AddRef,
    video_presenter_allocator_cb_Release,
    video_presenter_allocator_cb_NotifyRelease,
};

static HRESULT WINAPI video_presenter_qualprop_QueryInterface(IQualProp *iface, REFIID riid, void **obj)
{
    struct video_presenter *presenter = impl_from_IQualProp(iface);
    return IMFVideoPresenter_QueryInterface(&presenter->IMFVideoPresenter_iface, riid, obj);
}

static ULONG WINAPI video_presenter_qualprop_AddRef(IQualProp *iface)
{
    struct video_presenter *presenter = impl_from_IQualProp(iface);
    return IMFVideoPresenter_AddRef(&presenter->IMFVideoPresenter_iface);
}

static ULONG WINAPI video_presenter_qualprop_Release(IQualProp *iface)
{
    struct video_presenter *presenter = impl_from_IQualProp(iface);
    return IMFVideoPresenter_Release(&presenter->IMFVideoPresenter_iface);
}

static HRESULT WINAPI video_presenter_qualprop_get_FramesDroppedInRenderer(IQualProp *iface, int *frames)
{
    FIXME("%p, %p stub.\n", iface, frames);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_qualprop_get_FramesDrawn(IQualProp *iface, int *frames)
{
    FIXME("%p, %p stub.\n", iface, frames);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_qualprop_get_AvgFrameRate(IQualProp *iface, int *avg_frame_rate)
{
    FIXME("%p, %p stub.\n", iface, avg_frame_rate);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_qualprop_get_Jitter(IQualProp *iface, int *jitter)
{
    FIXME("%p, %p stub.\n", iface, jitter);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_qualprop_get_AvgSyncOffset(IQualProp *iface, int *offset)
{
    FIXME("%p, %p stub.\n", iface, offset);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_qualprop_get_DevSyncOffset(IQualProp *iface, int *devoffset)
{
    FIXME("%p, %p stub.\n", iface, devoffset);

    return E_NOTIMPL;
}

static const IQualPropVtbl video_presenter_qualprop_vtbl =
{
    video_presenter_qualprop_QueryInterface,
    video_presenter_qualprop_AddRef,
    video_presenter_qualprop_Release,
    video_presenter_qualprop_get_FramesDroppedInRenderer,
    video_presenter_qualprop_get_FramesDrawn,
    video_presenter_qualprop_get_AvgFrameRate,
    video_presenter_qualprop_get_Jitter,
    video_presenter_qualprop_get_AvgSyncOffset,
    video_presenter_qualprop_get_DevSyncOffset,
};

static HRESULT WINAPI video_presenter_quality_advise_QueryInterface(IMFQualityAdvise *iface, REFIID riid, void **out)
{
    struct video_presenter *presenter = impl_from_IMFQualityAdvise(iface);
    return IMFVideoPresenter_QueryInterface(&presenter->IMFVideoPresenter_iface, riid, out);
}

static ULONG WINAPI video_presenter_quality_advise_AddRef(IMFQualityAdvise *iface)
{
    struct video_presenter *presenter = impl_from_IMFQualityAdvise(iface);
    return IMFVideoPresenter_AddRef(&presenter->IMFVideoPresenter_iface);
}

static ULONG WINAPI video_presenter_quality_advise_Release(IMFQualityAdvise *iface)
{
    struct video_presenter *presenter = impl_from_IMFQualityAdvise(iface);
    return IMFVideoPresenter_Release(&presenter->IMFVideoPresenter_iface);
}

static HRESULT WINAPI video_presenter_quality_advise_SetDropMode(IMFQualityAdvise *iface,
        MF_QUALITY_DROP_MODE mode)
{
    FIXME("%p, %u.\n", iface, mode);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_quality_advise_SetQualityLevel(IMFQualityAdvise *iface,
        MF_QUALITY_LEVEL level)
{
    FIXME("%p, %u.\n", iface, level);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_quality_advise_GetDropMode(IMFQualityAdvise *iface,
        MF_QUALITY_DROP_MODE *mode)
{
    FIXME("%p, %p.\n", iface, mode);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_quality_advise_GetQualityLevel(IMFQualityAdvise *iface,
        MF_QUALITY_LEVEL *level)
{
    FIXME("%p, %p.\n", iface, level);

    return E_NOTIMPL;
}

static HRESULT WINAPI video_presenter_quality_advise_DropTime(IMFQualityAdvise *iface, LONGLONG interval)
{
    FIXME("%p, %s.\n", iface, wine_dbgstr_longlong(interval));

    return E_NOTIMPL;
}

static const IMFQualityAdviseVtbl video_presenter_quality_advise_vtbl =
{
    video_presenter_quality_advise_QueryInterface,
    video_presenter_quality_advise_AddRef,
    video_presenter_quality_advise_Release,
    video_presenter_quality_advise_SetDropMode,
    video_presenter_quality_advise_SetQualityLevel,
    video_presenter_quality_advise_GetDropMode,
    video_presenter_quality_advise_GetQualityLevel,
    video_presenter_quality_advise_DropTime,
};

HRESULT WINAPI MFCreateVideoPresenter(IUnknown *owner, REFIID riid_device, REFIID riid, void **obj)
{
    TRACE("%p, %s, %s, %p.\n", owner, debugstr_guid(riid_device), debugstr_guid(riid), obj);

    *obj = NULL;

    if (!IsEqualIID(riid_device, &IID_IDirect3DDevice9))
        return E_INVALIDARG;

    return CoCreateInstance(&CLSID_MFVideoPresenter9, owner, CLSCTX_INPROC_SERVER, riid, obj);
}

static HRESULT video_presenter_init_d3d(struct video_presenter *presenter)
{
    D3DPRESENT_PARAMETERS present_params = { 0 };
    IDirect3DDevice9 *device;
    IDirect3D9 *d3d;
    HRESULT hr;

    d3d = Direct3DCreate9(D3D_SDK_VERSION);

    present_params.BackBufferCount = 1;
    present_params.SwapEffect = D3DSWAPEFFECT_COPY;
    present_params.hDeviceWindow = GetDesktopWindow();
    present_params.Windowed = TRUE;
    present_params.Flags = D3DPRESENTFLAG_VIDEO;
    present_params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, GetDesktopWindow(),
            0, &present_params, &device);

    IDirect3D9_Release(d3d);

    if (FAILED(hr))
    {
        WARN("Failed to create d3d device, hr %#x.\n", hr);
        return hr;
    }

    hr = IDirect3DDeviceManager9_ResetDevice(presenter->device_manager, device, presenter->reset_token);
    IDirect3DDevice9_Release(device);
    if (FAILED(hr))
        WARN("Failed to set new device for the manager, hr %#x.\n", hr);

    if (SUCCEEDED(hr = MFCreateVideoSampleAllocator(&IID_IMFVideoSampleAllocator, (void **)&presenter->allocator)))
    {
        hr = IMFVideoSampleAllocator_SetDirectXManager(presenter->allocator, (IUnknown *)presenter->device_manager);
    }

    return hr;
}

HRESULT evr_presenter_create(IUnknown *outer, void **out)
{
    struct video_presenter *object;
    HRESULT hr;

    *out = NULL;

    if (!(object = heap_alloc_zero(sizeof(*object))))
        return E_OUTOFMEMORY;

    object->IMFVideoPresenter_iface.lpVtbl = &video_presenter_vtbl;
    object->IMFVideoDeviceID_iface.lpVtbl = &video_presenter_device_id_vtbl;
    object->IMFTopologyServiceLookupClient_iface.lpVtbl = &video_presenter_service_client_vtbl;
    object->IMFVideoDisplayControl_iface.lpVtbl = &video_presenter_control_vtbl;
    object->IMFRateSupport_iface.lpVtbl = &video_presenter_rate_support_vtbl;
    object->IMFGetService_iface.lpVtbl = &video_presenter_getservice_vtbl;
    object->IMFVideoPositionMapper_iface.lpVtbl = &video_presenter_position_mapper_vtbl;
    object->IQualProp_iface.lpVtbl = &video_presenter_qualprop_vtbl;
    object->IMFQualityAdvise_iface.lpVtbl = &video_presenter_quality_advise_vtbl;
    object->allocator_cb.lpVtbl = &video_presenter_allocator_cb_vtbl;
    object->IUnknown_inner.lpVtbl = &video_presenter_inner_vtbl;
    object->outer_unk = outer ? outer : &object->IUnknown_inner;
    object->refcount = 1;
    object->src_rect.right = object->src_rect.bottom = 1.0f;
    object->ar_mode = MFVideoARMode_PreservePicture | MFVideoARMode_PreservePixel;
    InitializeCriticalSection(&object->cs);

    if (FAILED(hr = DXVA2CreateDirect3DDeviceManager9(&object->reset_token, &object->device_manager)))
        goto failed;

    if (FAILED(hr = video_presenter_init_d3d(object)))
        goto failed;

    *out = &object->IUnknown_inner;

    return S_OK;

failed:

    IUnknown_Release(&object->IUnknown_inner);

    return hr;
}
