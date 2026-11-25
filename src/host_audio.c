/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host/host_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

/* The SDK declares the WASAPI IDs in headers but does not supply them in
 * every C toolchain's UUID library. Keep this translation unit self-contained. */
static const CLSID mm_device_enumerator_class =
    {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const IID mm_device_enumerator_interface =
    {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const IID audio_client_interface =
    {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
static const IID audio_render_client_interface =
    {0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
static const IID simple_audio_volume_interface =
    {0x87ce5498, 0x68d6, 0x44e5, {0x92, 0x15, 0x6d, 0xa4, 0x7e, 0xf8, 0x83, 0xd8}};

#define GCN_AUDIO_FRAME_BYTES 4u
#define GCN_AUDIO_RING_FRAMES 16384u
#define GCN_AUDIO_START_FRAMES 8192u
#define GCN_AUDIO_INPUT_BATCH_FRAMES 512u
#define GCN_AUDIO_INPUT_BATCH_BYTES \
    (GCN_AUDIO_INPUT_BATCH_FRAMES * GCN_AUDIO_FRAME_BYTES)

static u8 s_ring[GCN_AUDIO_RING_FRAMES * GCN_AUDIO_FRAME_BYTES];
static u8 s_input_batch[GCN_AUDIO_INPUT_BATCH_BYTES];
static u32 s_input_batch_bytes;
static u32 s_ring_read;
static u32 s_ring_write;
static u32 s_ring_count;
static u32 s_device_padding;

static CRITICAL_SECTION s_audio_lock;
static CONDITION_VARIABLE s_data_cv;
static CONDITION_VARIABLE s_space_cv;
static HANDLE s_worker;
static HANDLE s_ready_event;
static HANDLE s_stop_event;
static HANDLE s_audio_event;
static bool s_worker_stop;
static bool s_sync_ready;
static HRESULT s_start_hr;

static LARGE_INTEGER s_qpc_frequency;
static u64 s_submit_qpc;
static u64 s_render_qpc;
static u64 s_submit_calls;
static u64 s_underrun_frames;
static u64 s_first_underrun_at;
static u64 s_last_underrun_at;
#else
/* The portable null sink reports no device/timing activity. Keep the shared
 * shutdown diagnostics buildable without Windows-only types. */
static const struct { long long QuadPart; } s_qpc_frequency = {0};
static const u64 s_submit_qpc = 0, s_render_qpc = 0, s_submit_calls = 0;
static const u64 s_underrun_frames = 0, s_first_underrun_at = UINT64_MAX;
static const u64 s_last_underrun_at = 0;
#endif

static GcnHostAudioStats s_stats;
static FILE* s_capture;
static u64 s_capture_limit;

static void capture_u32(u8* out, u32 value) {
    for (u32 i = 0; i < 4; ++i) out[i] = (u8)(value >> (i * 8));
}

static bool capture_header(void) {
    u8 header[44] = {0};
    memcpy(header, "RIFF", 4);
    capture_u32(header + 4, 36u + (u32)s_stats.capture_frames * 4u);
    memcpy(header + 8, "WAVEfmt ", 8);
    capture_u32(header + 16, 16);
    header[20] = 1; header[22] = 2; /* PCM, stereo */
    capture_u32(header + 24, s_stats.sample_rate);
    capture_u32(header + 28, s_stats.sample_rate * 4u);
    header[32] = 4; header[34] = 16;
    memcpy(header + 36, "data", 4);
    capture_u32(header + 40, (u32)s_stats.capture_frames * 4u);
    return fseek(s_capture, 0, SEEK_SET) == 0 && fwrite(header, 1, 44, s_capture) == 44;
}

static void capture_close(void) {
    if (!s_capture) return;
    if (!capture_header()) s_stats.capture_failed = true;
    if (fclose(s_capture)) s_stats.capture_failed = true;
    s_capture = NULL;
}

static bool capture_start(u32 rate) {
    const char* path = getenv("GCN_AUDIO_CAPTURE");
    if (!path || !*path) return false;
    const char* limit = getenv("GCN_AUDIO_CAPTURE_FRAMES");
    s_capture_limit = limit && *limit ? strtoull(limit, NULL, 10) : (u64)rate * 30u;
    /* WAV uses 32-bit lengths. Bound diagnostics to at most ten minutes. */
    if (!s_capture_limit || s_capture_limit > (u64)rate * 600u) {
        s_stats.capture_failed = true;
        return false;
    }
    /* Captures are new evidence; never truncate an existing file or save. */
#ifdef _WIN32
    int descriptor = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    s_capture = descriptor < 0 ? NULL : _fdopen(descriptor, "wb");
    if (descriptor >= 0 && !s_capture) _close(descriptor);
#else
    s_capture = fopen(path, "wbx");
#endif
    if (!s_capture || !capture_header()) {
        s_stats.capture_failed = true;
        if (s_capture) { fclose(s_capture); s_capture = NULL; }
        fprintf(stderr, "gcn audio: could not create capture %s\n", path);
        return false;
    }
    return true;
}

static void capture_samples(const u8* source, u32 bytes) {
    u8 converted[1024];
    while (s_capture && bytes >= 4u && s_stats.capture_frames < s_capture_limit) {
        u32 frames = bytes / 4u;
        if (frames > sizeof(converted) / 4u) frames = sizeof(converted) / 4u;
        if (frames > s_capture_limit - s_stats.capture_frames)
            frames = (u32)(s_capture_limit - s_stats.capture_frames);
        gcn_host_audio_convert_be_s16(converted, source, frames * 4u);
        if (fwrite(converted, 4, frames, s_capture) != frames) {
            s_stats.capture_failed = true;
            capture_close();
            return;
        }
        for (u32 i = 0; i < frames; ++i) {
            bool signal = false;
            for (u32 channel = 0; channel < 2; ++channel) {
                const u8* p = source + i * 4u + channel * 2u;
                s16 sample = (s16)(((u16)p[0] << 8) | p[1]);
                u32 amplitude = sample < 0 ? (u32)(-(s32)sample) : (u32)sample;
                if (amplitude > s_stats.capture_peak) s_stats.capture_peak = amplitude;
                signal |= sample != 0;
            }
            s_stats.capture_signal_frames += signal;
        }
        s_stats.capture_frames += frames;
        source += frames * 4u;
        bytes -= frames * 4u;
    }
    if (s_capture && s_stats.capture_frames == s_capture_limit) capture_close();
}

u32 gcn_host_audio_convert_be_s16(u8* dst, const u8* src, u32 bytes) {
    if (!dst || !src)
        return 0;
    bytes -= bytes % 4u;
    for (u32 i = 0; i < bytes; i += 4u) {
        /* Dolphin Mixer::PushSamples: guest input is BE R/L, host output L/R. */
        dst[i] = src[i + 3u];
        dst[i + 1u] = src[i + 2u];
        dst[i + 2u] = src[i + 1u];
        dst[i + 3u] = src[i];
    }
    return bytes;
}

#ifdef _WIN32
static void note_signal(const u8* samples, u32 frames) {
    for (u32 i = 0; i < frames; ++i) {
        const u8* p = samples + i * GCN_AUDIO_FRAME_BYTES;
        u16 right = ((u16)p[0] << 8) | p[1];
        u16 left = ((u16)p[2] << 8) | p[3];
        if (left != 0 || right != 0)
            ++s_stats.audible_frames;
        s16 ls = (s16)left;
        s16 rs = (s16)right;
        u32 la = ls == (s16)0x8000 ? 32768u : (u32)(ls < 0 ? -ls : ls);
        u32 ra = rs == (s16)0x8000 ? 32768u : (u32)(rs < 0 ? -rs : rs);
        if (la > s_stats.peak) s_stats.peak = la;
        if (ra > s_stats.peak) s_stats.peak = ra;
    }
}

/* Called with s_audio_lock held. */
static u32 ring_read_frames(u8* dst, u32 frames) {
    u32 take = frames < s_ring_count ? frames : s_ring_count;
    u32 done = 0;
    while (done < take) {
        u32 chunk = take - done;
        u32 contiguous = GCN_AUDIO_RING_FRAMES - s_ring_read;
        if (chunk > contiguous)
            chunk = contiguous;
        memcpy(dst + done * GCN_AUDIO_FRAME_BYTES,
               s_ring + s_ring_read * GCN_AUDIO_FRAME_BYTES,
               chunk * GCN_AUDIO_FRAME_BYTES);
        s_ring_read = (s_ring_read + chunk) % GCN_AUDIO_RING_FRAMES;
        s_ring_count -= chunk;
        done += chunk;
    }
    return take;
}

static void wasapi_start_failed(HRESULT hr) {
    EnterCriticalSection(&s_audio_lock);
    s_start_hr = hr;
    s_stats.device_open = false;
    WakeAllConditionVariable(&s_space_cv);
    LeaveCriticalSection(&s_audio_lock);
    SetEvent(s_ready_event);
}

static DWORD WINAPI audio_worker_main(void* unused) {
    (void)unused;
    IMMDeviceEnumerator* enumerator = NULL;
    IMMDevice* device = NULL;
    IAudioClient* client = NULL;
    IAudioRenderClient* render = NULL;
    ISimpleAudioVolume* volume = NULL;
    bool com_ready = false;
    bool client_started = false;
    HANDLE mmcss = NULL;
    DWORD mmcss_task = 0;
    UINT32 buffer_frames = 0;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    if (SUCCEEDED(hr))
        com_ready = true;
    else if (hr != RPC_E_CHANGED_MODE) {
        wasapi_start_failed(hr);
        return 0;
    }

    hr = CoCreateInstance(&mm_device_enumerator_class, NULL, CLSCTX_ALL,
                          &mm_device_enumerator_interface, (void**)&enumerator);
    if (FAILED(hr)) goto init_failed;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
        enumerator, eRender, eConsole, &device);
    if (FAILED(hr)) goto init_failed;
    hr = IMMDevice_Activate(device, &audio_client_interface, CLSCTX_ALL, NULL,
                            (void**)&client);
    if (FAILED(hr)) goto init_failed;

    WAVEFORMATEX fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = s_stats.sample_rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = (WORD)(fmt.nChannels * fmt.wBitsPerSample / 8u);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                  AUDCLNT_STREAMFLAGS_NOPERSIST |
                  AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, flags,
                                 0, 0, &fmt, NULL);
    if (FAILED(hr)) goto init_failed;
    hr = IAudioClient_GetBufferSize(client, &buffer_frames);
    if (FAILED(hr)) goto init_failed;
    hr = IAudioClient_SetEventHandle(client, s_audio_event);
    if (FAILED(hr)) goto init_failed;
    hr = IAudioClient_GetService(client, &audio_render_client_interface,
                                 (void**)&render);
    if (FAILED(hr)) goto init_failed;
    /* Automated pacing QA must exercise the real device clock without sound.
     * Fail before submitting any samples if the session mute cannot be set. */
    hr = IAudioClient_GetService(client, &simple_audio_volume_interface, (void**)&volume);
    if (FAILED(hr)) goto init_failed;
    hr = ISimpleAudioVolume_SetMute(volume,s_stats.output_muted,NULL);
    if (FAILED(hr)) goto init_failed;

    EnterCriticalSection(&s_audio_lock);
    s_start_hr = S_OK;
    s_stats.device_open = true;
    LeaveCriticalSection(&s_audio_lock);
    SetEvent(s_ready_event);

    /* Do not start the endpoint on an empty queue. This is a one-time startup
     * cushion, not continuous host-time substitution or fabricated pacing. */
    EnterCriticalSection(&s_audio_lock);
    while (!s_worker_stop && s_ring_count < GCN_AUDIO_START_FRAMES)
        SleepConditionVariableCS(&s_data_cv, &s_audio_lock, INFINITE);
    bool stop = s_worker_stop;
    LeaveCriticalSection(&s_audio_lock);
    if (stop)
        goto done;

    BYTE* dst = NULL;
    hr = IAudioRenderClient_GetBuffer(render, buffer_frames, &dst);
    if (FAILED(hr)) goto stream_failed;
    EnterCriticalSection(&s_audio_lock);
    u32 copied = ring_read_frames(dst, buffer_frames);
    if (copied < buffer_frames)
        memset(dst + copied * GCN_AUDIO_FRAME_BYTES, 0,
               (buffer_frames - copied) * GCN_AUDIO_FRAME_BYTES);
    s_device_padding = buffer_frames;
    ++s_stats.buffers_submitted;
    WakeAllConditionVariable(&s_space_cv);
    LeaveCriticalSection(&s_audio_lock);
    hr = IAudioRenderClient_ReleaseBuffer(render, buffer_frames, 0);
    if (FAILED(hr)) goto stream_failed;
    /* WASAPI's event period is a real-time deadline.  MMCSS keeps unrelated
     * desktop work from delaying this small copy/submit worker. */
    mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task);
    hr = IAudioClient_Start(client);
    if (FAILED(hr)) goto stream_failed;
    client_started = true;

    HANDLE waits[2] = { s_stop_event, s_audio_event };
    for (;;) {
        DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wr == WAIT_OBJECT_0)
            break;
        if (wr != WAIT_OBJECT_0 + 1u) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            goto stream_failed;
        }

        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        UINT32 padding = 0;
        hr = IAudioClient_GetCurrentPadding(client, &padding);
        if (FAILED(hr)) goto stream_failed;
        UINT32 available = buffer_frames - padding;
        if (available == 0u)
            continue;
        hr = IAudioRenderClient_GetBuffer(render, available, &dst);
        if (FAILED(hr)) goto stream_failed;

        EnterCriticalSection(&s_audio_lock);
        copied = ring_read_frames(dst, available);
        if (copied < available) {
            memset(dst + copied * GCN_AUDIO_FRAME_BYTES, 0,
                   (available - copied) * GCN_AUDIO_FRAME_BYTES);
            ++s_stats.underruns;
            s_underrun_frames += available - copied;
            if (s_first_underrun_at == UINT64_MAX)
                s_first_underrun_at = s_stats.frames_received;
            s_last_underrun_at = s_stats.frames_received;
        }
        s_device_padding = padding + available;
        ++s_stats.buffers_submitted;
        WakeAllConditionVariable(&s_space_cv);
        LeaveCriticalSection(&s_audio_lock);

        hr = IAudioRenderClient_ReleaseBuffer(render, available, 0);
        if (FAILED(hr)) goto stream_failed;
        QueryPerformanceCounter(&t1);
        EnterCriticalSection(&s_audio_lock);
        s_render_qpc += (u64)(t1.QuadPart - t0.QuadPart);
        LeaveCriticalSection(&s_audio_lock);
    }
    goto done;

init_failed:
    wasapi_start_failed(hr);
    goto done;

stream_failed:
    fprintf(stderr, "gcn audio: WASAPI stream failed (HRESULT 0x%08lX)\n",
            (unsigned long)hr);
    EnterCriticalSection(&s_audio_lock);
    s_stats.device_open = false;
    WakeAllConditionVariable(&s_space_cv);
    LeaveCriticalSection(&s_audio_lock);

done:
    if (client_started)
        IAudioClient_Stop(client);
    if (mmcss)
        AvRevertMmThreadCharacteristics(mmcss);
    if (render) IAudioRenderClient_Release(render);
    if (volume) ISimpleAudioVolume_Release(volume);
    if (client) IAudioClient_Release(client);
    if (device) IMMDevice_Release(device);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    if (com_ready) CoUninitialize();
    return 0;
}

static void close_worker_handles(void) {
    if (s_worker) { CloseHandle(s_worker); s_worker = NULL; }
    if (s_audio_event) { CloseHandle(s_audio_event); s_audio_event = NULL; }
    if (s_stop_event) { CloseHandle(s_stop_event); s_stop_event = NULL; }
    if (s_ready_event) { CloseHandle(s_ready_event); s_ready_event = NULL; }
}
#endif

bool gcn_host_audio_start(u32 sample_rate) {
    capture_close();
    memset(&s_stats, 0, sizeof s_stats);
    const char* audio = getenv("GCN_AUDIO");
    const char* window = getenv("GCN_WINDOW");
    bool requested = audio && *audio ? *audio != '0' :
                     (window && *window && *window != '0');
    s_stats.enabled = requested;
    const char* mute = getenv("GCN_AUDIO_MUTE");
    s_stats.output_muted = mute && *mute && *mute != '0';
    s_stats.sample_rate = sample_rate;
    bool capturing = capture_start(sample_rate);
    if (!requested)
        return capturing;

#ifdef _WIN32
    memset(s_ring, 0, sizeof s_ring);
    memset(s_input_batch, 0, sizeof s_input_batch);
    s_input_batch_bytes = 0;
    s_ring_read = s_ring_write = s_ring_count = 0;
    s_device_padding = 0;
    s_worker = NULL;
    s_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_audio_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    s_worker_stop = false;
    s_sync_ready = false;
    s_start_hr = E_FAIL;
    s_submit_qpc = s_render_qpc = s_submit_calls = 0;
    s_underrun_frames = 0;
    s_first_underrun_at = UINT64_MAX;
    s_last_underrun_at = 0;
    QueryPerformanceFrequency(&s_qpc_frequency);

    if (!s_ready_event || !s_stop_event || !s_audio_event) {
        fprintf(stderr, "gcn audio: WASAPI event creation failed (%lu)\n",
                (unsigned long)GetLastError());
        close_worker_handles();
        return false;
    }
    InitializeCriticalSection(&s_audio_lock);
    InitializeConditionVariable(&s_data_cv);
    InitializeConditionVariable(&s_space_cv);
    s_sync_ready = true;
    s_worker = CreateThread(NULL, 0, audio_worker_main, NULL, 0, NULL);
    if (!s_worker) {
        fprintf(stderr, "gcn audio: WASAPI worker creation failed (%lu)\n",
                (unsigned long)GetLastError());
        DeleteCriticalSection(&s_audio_lock);
        s_sync_ready = false;
        close_worker_handles();
        return false;
    }
    if (WaitForSingleObject(s_ready_event, 5000u) != WAIT_OBJECT_0 ||
        !s_stats.device_open) {
        fprintf(stderr, "gcn audio: WASAPI open failed (HRESULT 0x%08lX)\n",
                (unsigned long)s_start_hr);
        gcn_host_audio_stop();
        return false;
    }
    fprintf(stdout,
            "gcn audio: WASAPI output ready (%u Hz, stereo s16, %u-frame ring)\n",
            sample_rate, GCN_AUDIO_RING_FRAMES);
    return true;
#else
    fprintf(stderr, "gcn audio: host output is not implemented on this platform\n");
    return false;
#endif
}

/* Queue one producer-side batch. AID emits only eight frames per DMA block;
 * keeping the lock/QPC work here instead of in every tiny callback is the
 * difference between host audio being negligible and consuming several ms
 * per video frame. */
#ifdef _WIN32
static void submit_input_batch(const u8* samples, u32 bytes) {
    if (!s_sync_ready)
        return;
    LARGE_INTEGER call_t0, call_t1;
    QueryPerformanceCounter(&call_t0);
    u32 total_frames = bytes / GCN_AUDIO_FRAME_BYTES;
    u32 done = 0;

    EnterCriticalSection(&s_audio_lock);
    while (done < total_frames && s_stats.device_open) {
        while (s_ring_count == GCN_AUDIO_RING_FRAMES && s_stats.device_open) {
            LARGE_INTEGER wait_t0, wait_t1;
            QueryPerformanceCounter(&wait_t0);
            SleepConditionVariableCS(&s_space_cv, &s_audio_lock, INFINITE);
            QueryPerformanceCounter(&wait_t1);
            if (s_qpc_frequency.QuadPart)
                s_stats.wait_milliseconds += (u64)(
                    1000.0 * (double)(wait_t1.QuadPart - wait_t0.QuadPart) /
                    (double)s_qpc_frequency.QuadPart);
        }
        if (!s_stats.device_open)
            break;
        u32 chunk = total_frames - done;
        u32 free_frames = GCN_AUDIO_RING_FRAMES - s_ring_count;
        u32 contiguous = GCN_AUDIO_RING_FRAMES - s_ring_write;
        if (chunk > free_frames) chunk = free_frames;
        if (chunk > contiguous) chunk = contiguous;
        const u8* src = samples + done * GCN_AUDIO_FRAME_BYTES;
        gcn_host_audio_convert_be_s16(
            s_ring + s_ring_write * GCN_AUDIO_FRAME_BYTES, src,
            chunk * GCN_AUDIO_FRAME_BYTES);
        note_signal(src, chunk);
        s_ring_write = (s_ring_write + chunk) % GCN_AUDIO_RING_FRAMES;
        s_ring_count += chunk;
        s_stats.frames_received += chunk;
        done += chunk;
        WakeConditionVariable(&s_data_cv);
    }
    if (done < total_frames)
        s_stats.dropped_frames += total_frames - done;
    QueryPerformanceCounter(&call_t1);
    s_submit_qpc += (u64)(call_t1.QuadPart - call_t0.QuadPart);
    ++s_submit_calls;
    LeaveCriticalSection(&s_audio_lock);
}
#endif

void gcn_host_audio_submit_be_s16(void* user, const u8* samples, u32 bytes) {
    (void)user;
    if (!samples || bytes < 4u)
        return;
    bytes -= bytes % 4u;
    s_stats.emulated_frames += bytes / 4u;
    capture_samples(samples, bytes);
    if (!s_stats.enabled) return;

#ifdef _WIN32
    while (bytes != 0u) {
        u32 room = GCN_AUDIO_INPUT_BATCH_BYTES - s_input_batch_bytes;
        u32 take = bytes < room ? bytes : room;
        memcpy(s_input_batch + s_input_batch_bytes, samples, take);
        s_input_batch_bytes += take;
        samples += take;
        bytes -= take;
        if (s_input_batch_bytes == GCN_AUDIO_INPUT_BATCH_BYTES) {
            submit_input_batch(s_input_batch, s_input_batch_bytes);
            s_input_batch_bytes = 0u;
        }
    }
#else
    s_stats.dropped_frames += bytes / 4u;
#endif
}

void gcn_host_audio_get_stats(GcnHostAudioStats* out) {
    if (!out)
        return;
#ifdef _WIN32
    if (s_sync_ready)
        EnterCriticalSection(&s_audio_lock);
#endif
    *out = s_stats;
#ifdef _WIN32
    if (s_stats.device_open)
        out->buffered_frames = s_ring_count + s_device_padding;
    if (s_sync_ready)
        LeaveCriticalSection(&s_audio_lock);
#endif
}

void gcn_host_audio_stop(void) {
    capture_close();
#ifdef _WIN32
    if (s_sync_ready) {
        if (s_input_batch_bytes != 0u) {
            submit_input_batch(s_input_batch, s_input_batch_bytes);
            s_input_batch_bytes = 0u;
        }
        EnterCriticalSection(&s_audio_lock);
        s_worker_stop = true;
        WakeAllConditionVariable(&s_data_cv);
        WakeAllConditionVariable(&s_space_cv);
        LeaveCriticalSection(&s_audio_lock);
        if (s_stop_event)
            SetEvent(s_stop_event);
        if (s_worker)
            WaitForSingleObject(s_worker, 5000u);
        close_worker_handles();
        DeleteCriticalSection(&s_audio_lock);
        s_sync_ready = false;
    }
#endif
    if (s_stats.enabled) {
        fprintf(stdout,
                "gcn audio: stopped (frames=%llu audible=%llu peak=%u packets=%llu "
                "underruns=%llu wait_ms=%llu dropped=%llu host_ms=%.3f "
                "render_ms=%.3f calls=%llu underrun_frames=%llu "
                "underrun_span=%llu..%llu)\n",
                (unsigned long long)s_stats.frames_received,
                (unsigned long long)s_stats.audible_frames, s_stats.peak,
                (unsigned long long)s_stats.buffers_submitted,
                (unsigned long long)s_stats.underruns,
                (unsigned long long)s_stats.wait_milliseconds,
                (unsigned long long)s_stats.dropped_frames,
                s_qpc_frequency.QuadPart ?
                    1000.0 * (double)s_submit_qpc /
                        (double)s_qpc_frequency.QuadPart : 0.0,
                s_qpc_frequency.QuadPart ?
                    1000.0 * (double)s_render_qpc /
                        (double)s_qpc_frequency.QuadPart : 0.0,
                (unsigned long long)s_submit_calls,
                (unsigned long long)s_underrun_frames,
                (unsigned long long)(s_first_underrun_at == UINT64_MAX ? 0u :
                                     s_first_underrun_at),
                (unsigned long long)s_last_underrun_at);
    }
    s_stats.device_open = false;
}
