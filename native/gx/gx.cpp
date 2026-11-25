// SPDX-License-Identifier: MIT
#include "gcnllr/native_gx.h"
#include "gamecube_input.hpp"
#include "keyboard_mouse_input.hpp"
#include "memory.hpp"
#include "texture_encode.hpp"
#include "owned_texture.hpp"
#include "raster_viewport.hpp"
#include "pointer.hpp"
#include "gpu_bridge.hpp"
#include "present_trace.hpp"
#include <unordered_map>
#include "fifo_worker.hpp"
#include "ordered_effects.hpp"
#include "gfx/gxcore_draw.hpp"
#if GCNLLR_ASYNC_COPIES || GCNLLR_GPU_OWNED_TEXTURES
#include "gfx/texture.hpp"
#endif
#include "gfx/thread_cpu.hpp"
#include "webgpu/gpu.hpp"
#include "window.hpp"
#include <imgui.h>
#if GCNLLR_NATIVE_DRAW_SINK
#include "native_draw_sink.hpp"
#endif
#if GCNLLR_RETAIL_PIPELINE
#include <gxruntime/gx_recomp.h>
#include "native_retail.hpp"
#include "retail_texture_identity.hpp"
#include "retail_copy.hpp"
#include "gx/recomp.hpp"
#include "gx/gx.hpp"
extern "C" void gcnllr_aurora_retail_reset();
extern "C" void gcnllr_aurora_retail_overlay(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,float);
extern "C" void gcnllr_aurora_retail_overlay_split(float,float);
extern "C" void gcnllr_aurora_retail_view_framing(float,float,float);
#else
#include <gxruntime/aurora_recomp/retail_gx_frontend.hpp>
#include <gxruntime/gxcore/gxcore.hpp>
#endif
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/vi.h>
#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace aurora::webgpu {
void set_fixed_present_aspect(uint32_t width, uint32_t height) noexcept;
}

namespace aurora::gfx::gxcore {
bool submit_draw_plan(const gxruntime::gxcore::DrawPlan&);
void copy_efb_to_texture(const gxruntime::gxcore::EfbCopyCommand&);
std::vector<uint8_t> read_efb_copy(const gxruntime::gxcore::EfbCopyCommand&);
void reset_texture_cache();
void note_frame_presented();
void publish_efb_copy_memory(uint32_t,const void*,uint32_t,uint32_t,uint32_t,uint32_t);
void efb_copy_memory_stats(uint64_t*,uint64_t*);
}
namespace aurora::gx::fifo { void drain(); }
namespace {
constexpr uint32_t guest_gpu_address_mask=GC_WII_GX_WII ? 0x00ffffffu:0x001fffffu;
using Clock = std::chrono::steady_clock;
#if GCNLLR_RETAIL_PIPELINE
using RetailGxFrontend=gcnllr::NativeRetailFrontend;
bool retail_active{};
gcnllr::RetailTextureIdentity retail_texture_ids;
#else
using gxruntime::aurora_recomp::RetailGxFrontend;
#endif
#if !GCNLLR_RETAIL_PIPELINE
#if GCNLLR_NATIVE_DRAW_SINK
using GxCoreSink=gcnllr::NativeDrawSink;
#else
using gxruntime::gxcore::GxCoreSink;
#endif
#endif
void flush_display_copies();
GcnllrNativeGxConfig config{};
GcnllrNativeGxStatus state{};
std::atomic<bool> failed{};
std::atomic<uint64_t> readback_ns{};
std::atomic<uint64_t> resident_textures{},resident_texture_hits{},resident_texture_materializations{},resident_texture_incompatible{};
std::unique_ptr<gcnllr::FifoWorker> fifo_worker;
// Adapted donor recording ownership: a frame transition cannot close Aurora's
// packet beneath the translator. Queue waits always occur outside this lock.
std::mutex recording_mutex;
thread_local std::unique_lock<std::mutex>* worker_recording{};
struct RecordingBatch {
    std::unique_lock<std::mutex> lock{recording_mutex,std::defer_lock};
    RecordingBatch() {
        if(GCNLLR_GX_COOPERATIVE_VI)lock.lock();
        worker_recording=&lock;
    }
    ~RecordingBatch() {worker_recording=nullptr;}
};
void machine_effect(std::function<void()> effect);
void ordered_bp_effect(uint32_t reg,uint32_t value);
void retire_streams(bool wait);
uint64_t efb_generation=1,color_peek_generation{},depth_peek_generation{};
std::vector<uint8_t> color_peek;
std::vector<uint32_t> depth_peek;
uint32_t color_peek_width{},color_peek_height{},depth_peek_width{},depth_peek_height{};
std::unique_ptr<RetailGxFrontend> frontend;
#if GCNLLR_RETAIL_PIPELINE
// Aurora owns all command/state/vertex work. The machine retains only the BP
// registers needed for caller-visible copies and completion notifications.
class MachineSink {
public:
    std::array<uint32_t,256> bp{};
    std::array<bool,256> bp_seen{};
    void memory_boundary() {}
};
#else
#include "packet_renderer.inc"
#endif
std::unique_ptr<MachineSink> sink;
void flush_geometry() {
#if GCNLLR_RETAIL_PIPELINE
    gcnllr_aurora_retail_finish();
#else
    sink->core.flush_frame();
#endif
}
bool flush_commands() {
#if GCNLLR_RETAIL_PIPELINE
    return frontend->flush();
#else
    return frontend->flush(sink.get());
#endif
}
struct MemoryEpoch {uint32_t address,size;uint64_t epoch;bool valid;};
std::vector<MemoryEpoch> memory_epochs;
#if GCNLLR_RETAIL_PIPELINE
uint64_t array_read_scope{};
bool array_writers_stopped{};
// The synchronous FIFO owns MEM1 until it returns or publishes an effect.
// Worker translation and deferred materialization use their regular observers.
struct StoppedArrayWriters {
    StoppedArrayWriters() { array_writers_stopped=!config.pending_read && !config.memory_epoch; }
    ~StoppedArrayWriters() { array_writers_stopped=false; }
};
#endif
void invalidate_memory_observations() {
    memory_epochs.clear();
#if GCNLLR_RETAIL_PIPELINE
    if (++array_read_scope==0) { retail_texture_ids.clear(); ++array_read_scope; }
#endif
}
bool observe_pending_memory(uint32_t address,uint32_t size) {
    if(!config.pending_read || !config.pending_read(config.user,address,size))return true;
    bool ready=false;
    machine_effect([&]{ready=config.prepare_read && config.prepare_read(config.user,address,size);});
    // Cached host pointers remain valid addresses, but their bytes may just
    // have changed. Observe before an epoch hit or actual-content hash too.
    invalidate_memory_observations();sink->memory_boundary();
    return ready;
}
bool memory_epoch(uint32_t address,uint32_t size,uint64_t* epoch) {
    if(!observe_pending_memory(address,size))throw std::runtime_error("GPU memory observation failed");
    // The machine tracker already versions whole pages. Share the same query
    // for texture subranges on those pages during this synchronous entry.
    // A writer still ends the entry or explicitly clears this memo below.
    GcnllrNativeGxMemory memory{};
    if(size && gcnllr::gx_memory(config,address,size,memory)) {
        address=memory.physical_address;
        const uint32_t first=address&~4095u;
        const uint64_t end=(uint64_t(address)+size+4095u)&~uint64_t(4095u);
        if(end<=uint64_t(UINT32_MAX)+1u) {address=first;size=uint32_t(end-first);}
    }
    for (const auto& entry:memory_epochs) if (entry.address==address && entry.size==size) {
        ++state.epoch_cached;*epoch=entry.epoch;return entry.valid;
    }
    uint64_t value{};++state.epoch_queries;
    const bool valid=config.memory_epoch && config.memory_epoch(config.user,address,size,&value);
    if (memory_epochs.size()<64) memory_epochs.push_back({address,size,value,valid});
    *epoch=value;return valid;
}
DolGuestAddressResolver resolver{};
bool texture_epoch(uint32_t address,uint32_t size,uint64_t* epoch) {
    if(!config.texture_epoch)return memory_epoch(address,size,epoch);
    if(!observe_pending_memory(address,size))throw std::runtime_error("GPU texture observation failed");
    // Flush/device generations can change while the native CPU runs. Do not
    // carry a stopped-writer memo across repeated bindings in a worker batch.
    ++state.epoch_queries;
    return config.texture_epoch(config.user,address,size,epoch)!=0;
}
bool initialized{}, frame_open{};
std::array<uint8_t,64> fifo_tail{};
std::vector<uint8_t> xfb;
Clock::time_point last_events{};
SDL_Gamepad* gamepad{};
gcnllr::gamecube_input::ControllerPorts gamecube_pads;
gcnllr::keyboard_mouse::Input mouse_controls;
FILE* frame_times{};
std::atomic<uint64_t> completion_service_ns{};
uint64_t last_completion_service_ns{};
std::array<char,4u*1024u*1024u> frame_times_buffer;
bool profile{};
// Diagnostic-only, bounded distinct layouts; FIFO translation owns the set.
// This identifies why actual copied textures still require materialization.
std::array<std::array<uint32_t,10>,128> incompatible_views{};
size_t incompatible_view_count{};
struct Cost {uint64_t calls{},wall_ns{},cpu_cycles{};};
Cost fifo_cost,draw_cost,copy_cost,present_cost,status_cost,owner_effect_cost;
Clock::time_point last_profile{};
struct Measure {
    Cost& cost;Clock::time_point start;uint64_t cycles{};
    explicit Measure(Cost& value):cost(value) {
        if (!profile) return;
        start=Clock::now();
#ifdef _WIN32
        QueryThreadCycleTime(GetCurrentThread(),&cycles);
#endif
    }
    ~Measure() {
        if (!profile) return;
        ++cost.calls;cost.wall_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
#ifdef _WIN32
        uint64_t end{};QueryThreadCycleTime(GetCurrentThread(),&end);cost.cpu_cycles+=end-cycles;
#endif
    }
};
void machine_effect(std::function<void()> effect) {
    Measure measurement(owner_effect_cost);
    if(worker_recording && worker_recording->owns_lock())
        fifo_worker->on_owner(std::move(effect),*worker_recording);
    else if(worker_recording)fifo_worker->on_owner(std::move(effect));
    else effect();
}
void profile_report(bool force=false) {
    if(!profile || (!force && Clock::now()-last_profile<std::chrono::seconds(5)))return;
    last_profile=Clock::now();
    const Cost* costs[]={&fifo_cost,&draw_cost,&copy_cost,&present_cost,&status_cost,&owner_effect_cost};
    const char* names[]={"fifo-inclusive","draw","copy","present","status","owner-effect"};
    std::fprintf(stderr,"[native-gx-costs] time_ns=%llu",(unsigned long long)
        std::chrono::duration_cast<std::chrono::nanoseconds>(last_profile.time_since_epoch()).count());
    for(unsigned i=0;i<6;i++)std::fprintf(stderr," %s=%llu/%.3f",names[i],
        (unsigned long long)costs[i]->calls,double(costs[i]->wall_ns)/1e6);
    std::fprintf(stderr,"\n");
}

void fail(const char* message) {
    std::fprintf(stderr,"gcnllr native GX: %s\n",message);
    failed.store(true);
}
void log_message(AuroraLogLevel level,const char* module,const char* message,unsigned length) {
    if (level==LOG_INFO && !std::strcmp(module,"aurora::gfx") && length>=17 &&
        !std::strncmp(message,"Segmenting frame ",17)) return;
    std::fprintf(stderr,"[native-gx:%s] %.*s\n",module,int(length),message);
    if (level==LOG_FATAL) failed.store(true);
}
void place_playtest_window(SDL_Window* window) {
#ifdef _WIN32
    // Use the decorated frame and the selected monitor's work area. A taskbar
    // can occupy any edge; desktop (0,0) is not a valid launch policy.
    const auto hwnd=static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
    RECT frame{};MONITORINFO monitor{sizeof(MONITORINFO)};
    if(!hwnd || !GetWindowRect(hwnd,&frame) ||
       !GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor))return;
    constexpr LONG margin=12;
    const LONG left=monitor.rcWork.left+margin,top=monitor.rcWork.top+margin;
    const LONG right=std::max(left,monitor.rcWork.right-margin-(frame.right-frame.left));
    const LONG bottom=std::max(top,monitor.rcWork.bottom-margin-(frame.bottom-frame.top));
    const LONG x=std::clamp(frame.left,left,right),y=std::clamp(frame.top,top,bottom);
    if(x!=frame.left || y!=frame.top)
        SetWindowPos(hwnd,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
#else
    // Let the window manager place a normal decorated window on other hosts.
    (void)window;
#endif
}
bool resolve(void*,uint32_t address,uint32_t size,DolGuestAddressSpace space,
             DolGuestResourceKind resource,DolGuestResolvedRange* out) {
    GcnllrNativeGxMemory memory{};
    if(!gcnllr::gx_memory(config,address,size,memory))return false;
    // Texture/copy-destination resolution supplies only pointer and layout.
    // Actual RAM consumers still observe in the texture decoder, while a
    // qualified live GPU producer can be sampled without downloading it.
    if(resource!=DOL_GUEST_RESOURCE_TEXTURE && resource!=DOL_GUEST_RESOURCE_COPY_DESTINATION &&
       !observe_pending_memory(memory.physical_address,size))return false;
    *out={memory.bytes,address,size,memory.available,space,resource};
    return true;
}
void pump(bool force=false) {
    if (!initialized || (!force && Clock::now()-last_events<std::chrono::milliseconds(4))) return;
    last_events=Clock::now();
    for (const auto* event=aurora_update();event && event->type!=AURORA_NONE;++event)
        if (event->type==AURORA_EXIT) state.quit=1;
    auto& pad=state.pad;
    state.pointer={};
    if (config.hidden) {pad={};pad.stick_x=pad.stick_y=pad.substick_x=pad.substick_y=128;return;}
    auto* window=aurora::window::get_sdl_window();
    if(window && SDL_GetMouseFocus()==window && SDL_GetKeyboardFocus()==window) {
        float x,y;const auto buttons=SDL_GetMouseState(&x,&y);
        const auto size=aurora::window::get_window_size();
        state.pointer=gcnllr::image_pointer(x,y,
            ((buttons&SDL_BUTTON_LMASK)?1u:0u)|((buttons&SDL_BUTTON_RMASK)?2u:0u)|
            ((buttons&SDL_BUTTON_MMASK)?4u:0u),size.width,size.height,
            size.native_fb_width,size.native_fb_height,size.fb_width,size.fb_height);
    }
    const bool* keys=SDL_GetKeyboardState(nullptr);
    if (gamecube_pads.enabled()) {
        const bool* focused_keys = window && SDL_GetKeyboardFocus() == window ? keys : nullptr;
        gamecube_pads.poll(focused_keys);
        if (const int port = gamecube_pads.keyboard_port(); port >= 0)
            mouse_controls.poll(window, focused_keys, gamecube_pads.port_input(port));
        pad = gamecube_pads.input().ports[0];
        return;
    }
    pad={}; pad.stick_x=uint8_t(128+100*(int(keys[SDL_SCANCODE_RIGHT])-int(keys[SDL_SCANCODE_LEFT])));
    pad.stick_y=uint8_t(128+100*(int(keys[SDL_SCANCODE_UP])-int(keys[SDL_SCANCODE_DOWN])));
    pad.substick_x=uint8_t(128+100*(int(keys[SDL_SCANCODE_L])-int(keys[SDL_SCANCODE_J])));
    pad.substick_y=uint8_t(128+100*(int(keys[SDL_SCANCODE_I])-int(keys[SDL_SCANCODE_K])));
    if (keys[SDL_SCANCODE_X]) pad.buttons|=0x100;
    if (keys[SDL_SCANCODE_Z]) pad.buttons|=0x200;
    if (keys[SDL_SCANCODE_C]) pad.buttons|=0x400;
    if (keys[SDL_SCANCODE_V]) pad.buttons|=0x800;
    if (keys[SDL_SCANCODE_RETURN]) pad.buttons|=0x1000;
    if (keys[SDL_SCANCODE_A]) pad.buttons|=0x10; // Z
    if (keys[SDL_SCANCODE_S]) { pad.buttons|=0x40; pad.trigger_l=255; }
    if (keys[SDL_SCANCODE_D]) { pad.buttons|=0x20; pad.trigger_r=255; }
    if (keys[SDL_SCANCODE_KP_4]) pad.buttons|=1;
    if (keys[SDL_SCANCODE_KP_6]) pad.buttons|=2;
    if (keys[SDL_SCANCODE_KP_2]) pad.buttons|=4;
    if (keys[SDL_SCANCODE_KP_8]) pad.buttons|=8;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=nullptr; }
    if (!gamepad) {
        int count=0; auto* ids=SDL_GetGamepads(&count);
        if (ids && count) gamepad=SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    if (gamepad) {
        auto axis=[&](SDL_GamepadAxis a,bool invert=false) {
            int value=SDL_GetGamepadAxis(gamepad,a);
            if (value>-4096 && value<4096) return uint8_t(128);
            if (invert) value=-value;
            return uint8_t(std::clamp(128+value/256,0,255));
        };
        auto pressed=[&](SDL_GamepadButton b){return SDL_GetGamepadButton(gamepad,b);};
        auto merge_axis=[&](uint8_t& output,SDL_GamepadAxis a,bool invert=false) {
            const auto value=axis(a,invert);if (value!=128) output=value;
        };
        merge_axis(pad.stick_x,SDL_GAMEPAD_AXIS_LEFTX);merge_axis(pad.stick_y,SDL_GAMEPAD_AXIS_LEFTY,true);
        merge_axis(pad.substick_x,SDL_GAMEPAD_AXIS_RIGHTX);merge_axis(pad.substick_y,SDL_GAMEPAD_AXIS_RIGHTY,true);
        if (pressed(SDL_GAMEPAD_BUTTON_SOUTH)) pad.buttons|=0x100;
        if (pressed(SDL_GAMEPAD_BUTTON_EAST)) pad.buttons|=0x200;
        if (pressed(SDL_GAMEPAD_BUTTON_WEST)) pad.buttons|=0x400;
        if (pressed(SDL_GAMEPAD_BUTTON_NORTH)) pad.buttons|=0x800;
        if (pressed(SDL_GAMEPAD_BUTTON_START)) pad.buttons|=0x1000;
        if (pressed(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) pad.buttons|=0x10;
        if (pressed(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) pad.buttons|=0x40;
        if (pressed(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) pad.buttons|=1;
        if (pressed(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) pad.buttons|=2;
        if (pressed(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) pad.buttons|=4;
        if (pressed(SDL_GAMEPAD_BUTTON_DPAD_UP)) pad.buttons|=8;
        pad.trigger_l=std::max(pad.trigger_l,uint8_t(std::max(0,int(SDL_GetGamepadAxis(gamepad,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)))/128));
        pad.trigger_r=std::max(pad.trigger_r,uint8_t(std::max(0,int(SDL_GetGamepadAxis(gamepad,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)))/128));
        if (pad.trigger_l>=230) pad.buttons|=0x40;
        if (pad.trigger_r>=230) pad.buttons|=0x20;
    }
}
#if !GCNLLR_RETAIL_PIPELINE
#include "packet_draw.inc"
#endif
struct PendingDisplay {uint32_t destination,width,height,stride;float yscale;};
std::vector<PendingDisplay> pending_displays;
#if GCNLLR_ASYNC_COPIES
struct StreamedBytes {
    aurora::gfx::AsyncReadback ticket;
    aurora::gfx::AsyncReadback completion;
    gcnllr::gpu_bridge::EncodedImageHandle image;
    aurora::gfx::gxcore::OwnedTexture texture;
    std::mutex mutex;
    std::vector<uint8_t> bytes;
    std::exception_ptr error;
    bool completed{};
    const std::vector<uint8_t>& materialize() {
        std::lock_guard lock(mutex);
        if(!completed) {
            try {
                // Composed generations remain GPU-only until an actual
                // memory observer demands their immutable bytes. The source
                // is recorded before ownership publication. The batched
                // owner-thread provider submits it here before observation;
                // no call back to a waiting FIFO worker is required.
                if(!ticket)ticket=image ? gcnllr::gpu_bridge::queue_display_readback(image) :
                    aurora::gfx::queue_texture_readback(texture.texture);
                if(!aurora::gfx::async_readback_ready(ticket))aurora::gfx::poll_async_readbacks(true);
                gcnllr::gpu_bridge::validate_submission();
                bytes=aurora::gfx::take_async_readback(ticket);
                if(texture.texture)bytes=gcnllr::encode_native_copy(texture.format,texture.width,texture.height,
                    bytes,texture.texture->size.width,texture.texture->size.height);
            }catch(...){error=std::current_exception();}
            completed=true;
        }
        if(error)std::rethrow_exception(error);
        return bytes;
    }
};
gcnllr::OrderedEffects streamed_effects;
std::atomic<size_t> streamed_pending{};
size_t streamed_readbacks{};
size_t streamed_guest_effects{};
uint64_t streamed_retired{},streamed_polls{},streamed_barriers{};
void queue_stream(uint64_t token,uint32_t address,const std::shared_ptr<StreamedBytes>& stream) {
    const auto completion=stream->completion ? stream->completion:stream->ticket;
    // Recorded immutable generations have already captured their inputs. Queue
    // order supplies GPU consumers; RAM/EFB observers still materialize and wait.
    // Eager transfers retain the physical completion provider in either build.
    const bool owned=GCNLLR_OWNED_COMPLETION && GCNLLR_COMPOSED_SCANOUT &&
        !stream->ticket && (stream->image || stream->texture.texture);
    streamed_effects.push([completion,owned]{return owned || aurora::gfx::async_readback_ready(completion);},[token,address,stream,completion,owned] {
        // Preserve physical-ticket errors for eager/physical providers and
        // terminal submission errors even after the guest overwrites a token.
        // Owned completion does not claim the host GPU has finished executing.
        if(!owned)aurora::gfx::validate_async_readback(completion);
        gcnllr::gpu_bridge::validate_submission();
#if !GCNLLR_COMPOSED_SCANOUT
        machine_effect([token]{
            if(!config.retire_copy(config.user,token))throw std::runtime_error("Machine rejected ordered GPU completion");
        });
        // GX cache state is FIFO-thread-owned. Never update it from an early
        // CPU materialization racing translation. The owner handoff above
        // completes RAM publication before this optional matching-image hash.
        {std::lock_guard lock(stream->mutex);
         if(stream->completed && !stream->error && stream->texture.texture)
             aurora::gfx::gxcore::publish_owned_copy_memory(stream->texture,address,stream->bytes);}
#endif
        // GPU completion need not publish MEM1 in the composed group. Keep
        // the immutable generation for actual RAM observers. Completion
        // has no dependency on a transfer or mapping in this provider group.
        // A matching-RAM texture hash cannot be published before those stores.
        --streamed_readbacks;++streamed_retired;
        streamed_pending.fetch_sub(1,std::memory_order_release);
    });
    ++streamed_readbacks;streamed_pending.fetch_add(1,std::memory_order_release);
}
#endif
void retire_streams(bool wait) {
#if GCNLLR_ASYNC_COPIES
    if(!streamed_effects.size())return;
    const auto start=Clock::now();
    gcnllr::gpu_bridge::submit_copies();
    // A retained GPU generation permits asynchronous GPU execution, but not
    // successful PE retirement ahead of a failed host encoding operation.
    if(GCNLLR_OWNED_COMPLETION)gcnllr::gpu_bridge::acknowledge_submission();
    if(wait)++streamed_barriers;else ++streamed_polls;
    streamed_effects.drain(wait,[](bool block){aurora::gfx::poll_async_readbacks(block);});
    invalidate_memory_observations();sink->memory_boundary();
    completion_service_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count(),std::memory_order_relaxed);
#else
    (void)wait;
#endif
}
void service_streams() {
#if GCNLLR_COMPOSED_SCANOUT
    // Publication already submitted an immutable resource. CPU/DMA readers
    // can obtain its bytes independently, and native VI uses GPU queue order.
    // A small FIFO/device quantum is therefore not itself a host GPU fence.
    // Join only when an earned PE notification needs ordered retirement.
    // Explicit drain, shutdown and bounded-copy backpressure still join all.
    if(streamed_guest_effects)retire_streams(true);
#else
    retire_streams(true);
#endif
}
void ordered_bp_effect(uint32_t reg,uint32_t value) {
#if GCNLLR_ASYNC_COPIES
    streamed_effects.push({},[reg,value]{
        machine_effect([reg,value]{config.bp_write(config.user,reg,value);});
        --streamed_guest_effects;
        streamed_pending.fetch_sub(1,std::memory_order_release);
    });
    ++streamed_guest_effects;
    streamed_pending.fetch_add(1,std::memory_order_release);
#else
    machine_effect([reg,value]{config.bp_write(config.user,reg,value);});
#endif
}
struct RetainedCopy {
    GcnllrNativeGxCopy copy;
    gcnllr::gpu_bridge::EncodedImageHandle image;
    aurora::gfx::gxcore::OwnedTexture texture;
#if GCNLLR_GPU_OWNED_TEXTURES
    struct TextureView {
        uint32_t format,width,height;
        std::shared_ptr<aurora::gfx::TextureRef> texture;
    };
    std::vector<TextureView> views{};
    struct PaletteKey {
        const void* data{};
        uint32_t object{}, version{};
        GXTlutFmt format{};
        uint32_t entries{};
        bool operator==(const PaletteKey&) const = default;
    };
    PaletteKey palette_key{};
    std::shared_ptr<aurora::gfx::TextureRef> palette_view;
#endif
#if GCNLLR_ASYNC_COPIES
    std::shared_ptr<StreamedBytes> stream;
#endif
};
std::mutex retained_mutex;
std::unordered_map<uint64_t,RetainedCopy> retained_copies;
std::unordered_map<uint32_t,uint64_t> texture_producers;
uint64_t next_copy_token;
// Keep older handles for byte observers. A partially overwritten producer
// can no longer provide a decoded view of the complete guest texture.
void invalidate_overlapping_texture_producers(const GcnllrNativeGxCopy& copy) {
    const auto end=[](const GcnllrNativeGxCopy& region) {
        return uint64_t(region.address)+(region.rows-1)*uint64_t(region.stride)+region.row_bytes;
    };
    const auto copy_end=end(copy);
    std::erase_if(texture_producers,[&](const auto& producer) {
        const auto& older=retained_copies.at(producer.second).copy;
        return older.address<copy_end && copy.address<end(older);
    });
}
RetainedCopy retained_copy(uint64_t token) {
    std::lock_guard lock(retained_mutex);
    const auto found=retained_copies.find(token);
    return found==retained_copies.end()?RetainedCopy{}:found->second;
}
void release_copy(uint64_t token) {
    std::lock_guard lock(retained_mutex);
    const auto found=retained_copies.find(token);
    if(found==retained_copies.end())return;
    const auto current=texture_producers.find(found->second.copy.address);
    if(current!=texture_producers.end() && current->second==token)texture_producers.erase(current);
    retained_copies.erase(found);
}
// The caller holds retained_mutex. Only the newest writer of the exact guest
// base can supply a decoded or reinterpreted view.
auto find_texture_producer(uint32_t address) {
    const auto current = texture_producers.find(address & 0x1fffffffu);
    return current == texture_producers.end() ? retained_copies.end() : retained_copies.find(current->second);
}
bool is_intensity8_copy(uint32_t format) {
    // The converter replicates this single stored channel across RGBA.
    // Each format uses the same 8x4, one-byte-per-texel guest tile.
    switch (format) {
    case 1: case 0x11: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x39: case 0x3A:
        return true;
    default:
        return false;
    }
}
bool dense_texture_copy(const RetainedCopy& retained, uint32_t size, uint32_t width, uint32_t height) {
    const auto& t = retained.texture;
    const auto& c = retained.copy;
    return t.texture && t.width == width && t.height == height &&
        c.stride == c.row_bytes && uint64_t(c.row_bytes) * c.rows == size;
}
std::shared_ptr<aurora::gfx::TextureRef> owned_texture(uint32_t address,uint32_t size,
    uint32_t format,uint32_t width,uint32_t height) {
    std::lock_guard lock(retained_mutex);
    const auto found=find_texture_producer(address);
    if(found==retained_copies.end())return {};
    auto& retained=found->second;
    const auto& c=retained.copy;const auto& t=retained.texture;
    const bool dense=dense_texture_copy(retained,size,width,height);
    const bool scalar_alias=format==1u && t.format!=format && is_intensity8_copy(t.format);
#if GCNLLR_GPU_OWNED_TEXTURES
    // Viewing a copy-typed scalar as I8 replaces the guest byte decoder.
    // Keep its logical texel spacing when the producer retained subpixels.
    const bool logical_scalar=scalar_alias && dense &&
        (t.texture->size.width!=width || t.texture->size.height!=height);
#else
    constexpr bool logical_scalar=false;
#endif
    // Direct bindings preserve the converter's packed-byte decode. I4
    // keeps both nibbles, and Z16's [Zmid,Zhi] bytes decode as IA8.
    const bool compatible_format=
        ((format==0u || format==1u || format==4u || format==6u) && t.format==format) ||
        (scalar_alias && !logical_scalar) ||
        (format==3u && t.format==0x13u);
    if(compatible_format && dense) {
        resident_texture_hits.fetch_add(1,std::memory_order_relaxed);
        return t.texture;
    }
#if GCNLLR_GPU_OWNED_TEXTURES
    const bool padded_rgb565=format==4u && t.format==format && (t.width&1u) &&
        width==t.width+1u && height==t.height;
    const bool tiled_view=(format==0u || format==3u) && format!=t.format;
    // Views address the original dense tile payload, including its edge
    // padding. Stride gaps and interior bases remain explicit observers.
    if(t.texture && (padded_rgb565 || tiled_view || logical_scalar) &&
       c.stride==c.row_bytes && uint64_t(c.row_bytes)*c.rows==size) {
        const auto cached=std::find_if(retained.views.begin(),retained.views.end(),[&](const auto& view) {
            return view.format==format && view.width==width && view.height==height;
        });
        if(cached!=retained.views.end()) {
            resident_texture_hits.fetch_add(1,std::memory_order_relaxed);
            return cached->texture;
        }
        if(auto view=gcnllr::gpu_bridge::reinterpret_texture(t,format,width,height)) {
            retained.views.push_back({format,width,height,view});
            resident_texture_hits.fetch_add(1,std::memory_order_relaxed);
            return view;
        }
    }
#endif
    resident_texture_incompatible.fetch_add(1,std::memory_order_relaxed);
    if(profile) {
        const std::array<uint32_t,10> key{format,width,height,size,t.format,t.width,t.height,c.row_bytes,c.rows,c.stride};
        const auto end=incompatible_views.begin()+incompatible_view_count;
        if(incompatible_view_count<incompatible_views.size() && std::find(incompatible_views.begin(),end,key)==end) {
            incompatible_views[incompatible_view_count++]=key;
            std::fprintf(stderr,"[native-owned-incompatible] request=%u/%ux%u/%u copy=%u/%ux%u rows=%u/%u/%u texture=%u\n",
                format,width,height,size,t.format,t.width,t.height,c.row_bytes,c.rows,c.stride,unsigned(bool(t.texture)));
        }
    }
    return {};
}
int materialize_copy(uint64_t token) {
    if(!initialized || failed.load())return 0;
    const auto retained=retained_copy(token);
    if(!retained.image && !retained.texture.texture)return 0;
    const auto begin=Clock::now();
    try {
        const auto& c=retained.copy;
        const bool display=(c.flags&GCNLLR_NATIVE_GX_COPY_DISPLAY)!=0;
        std::vector<uint8_t> bytes;
#if GCNLLR_ASYNC_COPIES
        if(retained.stream)bytes=retained.stream->materialize();
        else
#endif
        if(display) {
            const auto image=gcnllr::gpu_bridge::materialize_display_copy(retained.image);
            if(image.width*2!=c.row_bytes || image.height!=c.rows)
                throw std::runtime_error("Resident XFB geometry changed");
            bytes=image.yuy2;
        } else {
            if(GCNLLR_GPU_FRAME_PACKETS)gcnllr::gpu_bridge::submit_copies();
            bytes=aurora::gfx::gxcore::materialize_owned_texture(retained.texture);
        }
        if(bytes.size()!=size_t(c.row_bytes)*c.rows)
            throw std::runtime_error("Resident copy byte geometry changed");
        // Called by the machine owner. No FIFO wait or owner round-trip is
        // allowed here: a publisher can be waiting for this materialization.
        const uint32_t flags=c.flags|GCNLLR_NATIVE_GX_COPY_MATERIALIZING;
        if(c.stride==c.row_bytes) {
            if(!config.write_copy(config.user,c.address,bytes.data(),uint32_t(bytes.size()),flags,
                    display ? c.row_bytes/2:0,display ? c.rows:0,display ? c.stride:0))
                throw std::runtime_error("Machine rejected observed GPU bytes");
        } else {
            for(uint32_t row=0;row<c.rows;row++)
                if(!config.write_copy(config.user,c.address+row*c.stride,
                        bytes.data()+size_t(row)*c.row_bytes,c.row_bytes,flags,
                        display ? c.row_bytes/2:0,display ? 1:0,display ? c.row_bytes:0))
                    throw std::runtime_error("Machine rejected observed GPU row");
        }
        ++state.resident_materializations;
        if(!display)resident_texture_materializations.fetch_add(1,std::memory_order_relaxed);
        readback_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin).count(),std::memory_order_relaxed);
        return 1;
    } catch(const std::exception& e) {fail(e.what());return 0;}
}
int materialize_copy_range(uint64_t token,uint32_t offset,uint32_t size) {
    if(!initialized || failed.load())return -1;
    const auto retained=retained_copy(token);
    const auto& copy=retained.copy;
    if(!retained.image || !size || copy.stride!=copy.row_bytes)return 0;
    const auto begin=Clock::now();
    try {
        const auto image=gcnllr::gpu_bridge::materialize_display_copy(retained.image,offset,size);
        if(!config.write_copy(config.user,copy.address+offset,image.yuy2.data(),size,
                GCNLLR_NATIVE_GX_COPY_MATERIALIZING,0,0,0))
            throw std::runtime_error("Machine rejected observed GPU byte range");
        ++state.resident_materializations;
        readback_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin).count(),std::memory_order_relaxed);
        return 1;
    } catch(const std::exception& e) {fail(e.what());return -1;}
}
void display_readback(const gxruntime::gxcore::EfbCopyCommand& copy) {
    if (pending_displays.size()>=16) flush_display_copies();
    const auto& bp=sink->bp;
    const uint32_t scale=bp[0x4E]&0xFFFFu;
    const bool invert=(bp[0x52]&(1u<<10))!=0;
    const float yscale=invert ? (scale ? 256.f/float(scale) : 1.f) : float(scale)/256.f;
    const uint32_t h=uint32_t(float(copy.height)*yscale),w=copy.width;
    const uint32_t stride=(bp[0x4D]&0x3FFu)<<5;
    if (!w || !h || (w&1) || w>640 || h>576 || stride<w*2 || stride>2048)
        throw std::runtime_error("Invalid guest XFB dimensions/stride");
    // Snapshot the live BP filter/clamp state with this immutable copy.
    // A programmed (or reset) all-zero filter is black, not identity.
    const gcnllr::gpu_bridge::DisplayCopyFilter filter{
        bp[0x53],bp[0x54],(bp[0x52]&1u)!=0,(bp[0x52]&2u)!=0};
    gcnllr::gpu_bridge::record_display_copy(copy,filter,!config.own_copy,h,yscale);
    pending_displays.push_back({(bp[0x4B]&guest_gpu_address_mask)<<5,w,h,stride,yscale});
    ++state.display_copies;
}
void flush_display_copies() {
    if (pending_displays.empty()) return;
    const bool resident=config.own_copy && (GCNLLR_GPU_OWNED_DISPLAY ||
        std::all_of(pending_displays.begin(),pending_displays.end(),
            [](const PendingDisplay& copy){return copy.yscale==1.f;}));
    if(resident) {
        auto images=gcnllr::gpu_bridge::retain_display_copies();
        if(images.size()!=pending_displays.size())throw std::runtime_error("Resident copy fence mismatch");
        for(size_t i=0;i<images.size();i++) {
            const auto& copy=pending_displays[i];
#if GCNLLR_ASYNC_COPIES
            if(streamed_readbacks>=16)retire_streams(true);
            auto stream=std::make_shared<StreamedBytes>();
#if GCNLLR_COMPOSED_SCANOUT
            stream->image=images[i];
            if(!GCNLLR_OWNED_COMPLETION)stream->completion=aurora::gfx::queue_gpu_completion();
#else
            stream->ticket=gcnllr::gpu_bridge::queue_display_readback(images[i]);
#endif
#endif
            if(next_copy_token==UINT64_MAX)throw std::runtime_error("GPU copy identity exhausted");
            const GcnllrNativeGxCopy descriptor{++next_copy_token,copy.destination,copy.width*2,copy.height,copy.stride,GCNLLR_NATIVE_GX_COPY_DISPLAY};
            {
                std::lock_guard lock(retained_mutex);
                invalidate_overlapping_texture_producers(descriptor);
                retained_copies.emplace(descriptor.token,RetainedCopy{descriptor,std::move(images[i]),{}});
#if GCNLLR_ASYNC_COPIES
                retained_copies.at(descriptor.token).stream=stream;
#endif
            }
            try {
                machine_effect([&]{
                    if(!config.own_copy(config.user,&descriptor))
                        throw std::runtime_error("Machine rejected GPU copy ownership");
                });
            } catch(...) {release_copy(descriptor.token);throw;}
#if GCNLLR_ASYNC_COPIES
            queue_stream(descriptor.token,descriptor.address,stream);
#endif
            ++state.resident_copies;
        }
        pending_displays.clear();invalidate_memory_observations();sink->memory_boundary();
        return;
    }
    retire_streams(true); // Unsupported resident row scaling uses eager floor.
    const auto start=Clock::now();
    auto images=gcnllr::gpu_bridge::take_display_copies();
    readback_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count(),std::memory_order_relaxed);
    if (images.size()!=pending_displays.size()) throw std::runtime_error("GPU display-copy fence mismatch");
    for (size_t i=0;i<images.size();i++) {
        const auto& image=images[i];const auto& copy=pending_displays[i];
        const auto w=copy.width,h=copy.height,stride=copy.stride;
        if (image.width!=w || !image.height || image.yuy2.size()!=size_t(w)*image.height*2)
            throw std::runtime_error("GPU display-copy byte geometry mismatch");
        xfb.resize(size_t(stride)*h);
        for (uint32_t y=0;y<h;y++) {
          const size_t sy=std::min(size_t(image.height-1),size_t(float(y)/copy.yscale+0.5f));
          std::memcpy(xfb.data()+size_t(y)*stride,image.yuy2.data()+sy*w*2,w*2);
        }
        machine_effect([&]{
            if (!config.write_copy(config.user,copy.destination,xfb.data(),uint32_t(xfb.size()),1,w,h,stride))
                throw std::runtime_error("Machine rejected completed GPU XFB write");
        });
        invalidate_memory_observations();
        sink->memory_boundary();
    }
    pending_displays.clear();
}
void copy(const gxruntime::gxcore::EfbCopyCommand& command,void*) {
    Measure measurement(copy_cost);
    if (command.clear) ++efb_generation;
    if (command.format==0xFu) { display_readback(command); return; }
    flush_display_copies();
    const auto tile=gcnllr::native_copy_layout(command.format);
    if(!tile.bytes || !command.destination_width || !command.destination_height)
        throw std::runtime_error("Native texture-copy format/size is unsupported");
    const uint32_t row_bytes=((command.destination_width+tile.width-1)/tile.width)*tile.bytes;
    const uint32_t rows=(command.destination_height+tile.height-1)/tile.height;
    const uint32_t stride=(sink->bp[0x4D]&0x3FFu)<<5;
    const uint64_t extent=uint64_t(rows-1)*stride+row_bytes;
    GcnllrNativeGxMemory destination{};
    if (!stride || extent>UINT32_MAX ||
        !gcnllr::gx_memory(config,command.dest_address,uint32_t(extent),destination))
        throw std::runtime_error("Invalid native texture-copy memory layout");
    if((!GCNLLR_GPU_OWNED_DISPLAY || GCNLLR_GPU_OWNED_TEXTURES) && config.own_copy && stride>=row_bytes) {
#if GCNLLR_ASYNC_COPIES
        if(streamed_readbacks>=16)retire_streams(true);
#endif
        auto texture=aurora::gfx::gxcore::capture_owned_efb_copy(command,!GCNLLR_GPU_FRAME_PACKETS);
        if(GCNLLR_GPU_FRAME_PACKETS)gcnllr::gpu_bridge::note_recorded_copy();
#if GCNLLR_ASYNC_COPIES
        auto stream=std::make_shared<StreamedBytes>();stream->texture=texture;
#if GCNLLR_COMPOSED_SCANOUT
        if(!GCNLLR_OWNED_COMPLETION)stream->completion=aurora::gfx::queue_gpu_completion();
#else
        stream->ticket=aurora::gfx::queue_texture_readback(texture.texture);
#endif
#endif
        if(next_copy_token==UINT64_MAX)throw std::runtime_error("GPU copy identity exhausted");
        const GcnllrNativeGxCopy descriptor{++next_copy_token,command.dest_address,row_bytes,rows,stride,0};
        {
            std::lock_guard lock(retained_mutex);
            invalidate_overlapping_texture_producers(descriptor);
            retained_copies.emplace(descriptor.token,RetainedCopy{descriptor,{},std::move(texture)});
            texture_producers[descriptor.address]=descriptor.token;
#if GCNLLR_ASYNC_COPIES
            retained_copies.at(descriptor.token).stream=stream;
#endif
        }
        try {
            machine_effect([&]{
                if(!config.own_copy(config.user,&descriptor))
                    throw std::runtime_error("Machine rejected GPU texture ownership");
            });
        } catch(...) {release_copy(descriptor.token);throw;}
#if GCNLLR_ASYNC_COPIES
        queue_stream(descriptor.token,descriptor.address,stream);
#endif
        resident_textures.fetch_add(1,std::memory_order_relaxed);
        ++state.texture_copies;
        invalidate_memory_observations();sink->memory_boundary();
        return;
    }
    retire_streams(true);
    aurora::gfx::gxcore::copy_efb_to_texture(command);
    const auto bytes=aurora::gfx::gxcore::read_efb_copy(command);
    if (bytes.size()!=size_t(row_bytes)*rows)
        throw std::runtime_error("Native texture-copy CPU materialization is unsupported");
    machine_effect([&]{
        for (uint32_t row=0;row<rows;row++)
            if (!config.write_copy(config.user,command.dest_address+row*stride,bytes.data()+size_t(row)*row_bytes,
                                   row_bytes,0,0,0,0))
                throw std::runtime_error("Machine rejected GPU texture-copy write");
    });
    if(stride==row_bytes)
        aurora::gfx::gxcore::publish_efb_copy_memory(command.dest_address,bytes.data(),uint32_t(bytes.size()),
            command.destination_width,command.destination_height,command.format);
    invalidate_memory_observations();
    sink->memory_boundary();
    ++state.texture_copies;
}
void stop() {
    if(fifo_worker) {
        try {fifo_worker->wait(true);}catch(const std::exception& e){fail(e.what());}
        fifo_worker->stop();
        const auto stats=fifo_worker->stats();
        std::fprintf(stderr,"[native-gx-worker] batches=%llu bytes=%llu barriers=%llu owner_calls=%llu cpu_us=%llu\n",
            (unsigned long long)stats.batches,(unsigned long long)stats.bytes,
            (unsigned long long)stats.barriers,(unsigned long long)stats.owner_calls,
            (unsigned long long)aurora::gfx::thread_cpu::cpu_us(aurora::gfx::thread_cpu::Role::GxWorker));
        fifo_worker.reset();
    }
    if (initialized) {try {flush_display_copies();retire_streams(true);} catch (const std::exception& e) {fail(e.what());}}
    if (initialized && profile) {
        profile_report(true);
        const Cost* costs[]={&fifo_cost,&draw_cost,&copy_cost,&present_cost,&status_cost,&owner_effect_cost};
        const char* names[]={"fifo-inclusive","draw","copy","present","status","owner-effect"};
        for (unsigned i=0;i<6;i++) std::fprintf(stderr,"[native-gx-cost] %s calls=%llu wall_ms=%.3f cpu_cycles=%llu\n",
            names[i],(unsigned long long)costs[i]->calls,double(costs[i]->wall_ns)/1e6,
            (unsigned long long)costs[i]->cpu_cycles);
    }
    if (initialized) std::fprintf(stderr,"[native-gx-summary] draws=%llu copies=%llu presents=%llu readback_ms=%.3f\n",
        (unsigned long long)state.draws,(unsigned long long)state.display_copies,
        (unsigned long long)state.presents,double(readback_ns.load(std::memory_order_relaxed))/1e6);
    if (initialized && frontend) std::fprintf(stderr,"[native-gx-fifo] bytes=%llu pending=%zu\n",
        (unsigned long long)state.fifo_bytes,frontend->pending_fifo_size());
#if GCNLLR_GX_ONE_COPY
    if(initialized && frontend)
        std::fprintf(stderr,"[native-gx-parser] borrowed=%llu copied-input=%llu\n",
            (unsigned long long)frontend->borrowed_bytes(),(unsigned long long)frontend->copied_bytes());
#endif
    if (initialized && std::getenv("GCNLLR_GPU_TRACE")) {
        std::fprintf(stderr,"[native-gx-fifo-tail] ");
        for(auto b:fifo_tail)std::fprintf(stderr,"%02X",b);
        std::fprintf(stderr,"\n");
    }
    const auto& cache=aurora::gfx::gxcore::texture_cache_stats();
    if (initialized) std::fprintf(stderr,"[native-gx-cache] generation_hits=%llu hashes=%llu epoch_queries=%llu epoch_cached=%llu\n",
        cache.generation_hits,cache.hashed_lookups,(unsigned long long)state.epoch_queries,(unsigned long long)state.epoch_cached);
    uint64_t copy_hits{},copy_misses{};aurora::gfx::gxcore::efb_copy_memory_stats(&copy_hits,&copy_misses);
    if(initialized)std::fprintf(stderr,"[native-gx-copy-cache] verified_hits=%llu invalidations=%llu\n",
        (unsigned long long)copy_hits,(unsigned long long)copy_misses);
#if !GCNLLR_RETAIL_PIPELINE
    if (initialized && sink) std::fprintf(stderr,"[native-gx-state] duplicate_draw_states=%llu duplicate_textures=%llu cache_placement_states=%llu\n",
        (unsigned long long)sink->duplicate_draw_states,(unsigned long long)sink->duplicate_textures,
        (unsigned long long)sink->cache_placement_states);
#endif
#if GCNLLR_ASYNC_COPIES
    std::fprintf(stderr,"[native-streamed-copies] retired=%llu polls=%llu barriers=%llu pending=%zu\n",
        (unsigned long long)streamed_retired,(unsigned long long)streamed_polls,
        (unsigned long long)streamed_barriers,streamed_effects.size());
    if(streamed_effects.size())fail("GPU stopped with unresolved ordered effects");
    streamed_effects={};streamed_pending.store(0);streamed_readbacks=0;streamed_guest_effects=0;
#endif
    if (initialized && frame_open) {
        try {gcnllr::gpu_bridge::finish();}
        catch(const std::exception& error) {fail(error.what());}
        catch(...) {fail("Native GPU completion failed during shutdown");}
        frame_open=false;
    }
    {
        std::lock_guard lock(retained_mutex);
        if(!retained_copies.empty())fail("GPU stopped with unresolved memory ownership");
        texture_producers.clear();retained_copies.clear();
    }
    aurora::gfx::gxcore::set_owned_texture_lookup(nullptr);
    frontend.reset(); sink.reset();
#if GCNLLR_RETAIL_PIPELINE
    std::fprintf(stderr,"[native-retail-textures] queries=%llu reused=%llu stable_array_hits=%llu\n",
        (unsigned long long)retail_texture_ids.queries,(unsigned long long)retail_texture_ids.reused,
        (unsigned long long)retail_texture_ids.scope_hits);
    retail_active=false;retail_texture_ids.clear();aurora::gx::recomp::clear_guest_address_resolver();
#endif
    if (gamepad) { SDL_CloseGamepad(gamepad);gamepad=nullptr; }
    mouse_controls.stop();
    gamecube_pads.stop();
    if (initialized) { gcnllr::gpu_bridge::release();aurora_shutdown();initialized=false; }
    gcnllr::present_trace::finish();
    if (frame_times) {std::fclose(frame_times);frame_times=nullptr;}
    frame_open=false;
}
int start(const GcnllrNativeGxConfig* input) {
    if (!input || input->abi!=GCNLLR_NATIVE_GX_ABI || initialized ||
        !input->bp_write || !input->write_copy || !input->data_directory || !input->cache_directory) return 0;
    if(bool(input->resolve_memory)!=bool(input->locate_memory))return 0;
    if(!input->resolve_memory && (!input->mem1 || input->mem1_size!=24u*1024u*1024u))return 0;
    const unsigned ownership=unsigned(input->own_copy!=nullptr)+unsigned(input->pending_read!=nullptr)+
        unsigned(input->prepare_read!=nullptr);
    if(ownership!=0 && ownership!=3)return 0;
#if GCNLLR_ASYNC_COPIES
    if(ownership!=3 || !input->retire_copy)return 0;
    streamed_retired=streamed_polls=streamed_barriers=0;
#endif
    config=*input;state={};failed.store(false);readback_ns.store(0);next_copy_token=0;
    resident_textures.store(0);resident_texture_hits.store(0);
    resident_texture_materializations.store(0);resident_texture_incompatible.store(0);
#if GCNLLR_GX_FIFO_WORKER
    if(config.memory_epoch) {fail("Concurrent FIFO worker cannot use stopped-writer memory epochs");return 0;}
#endif
    fifo_tail.fill(0);
    ++efb_generation;color_peek_generation=depth_peek_generation=0;
    color_peek.clear();depth_peek.clear();
    profile=std::getenv("GCNLLR_GX_PROFILE")!=nullptr;
    incompatible_view_count=0;
    fifo_cost={};draw_cost={};copy_cost={};present_cost={};status_cost={};owner_effect_cost={};last_profile=Clock::now();
    try {
        const char* controller_profile = std::getenv("GCNLLR_CONTROLLER_PROFILE");
        if (!config.hidden && controller_profile && *controller_profile &&
            !gamecube_pads.start(controller_profile))
            throw std::runtime_error("Unable to load the GameCube controller profile");
        if (const char* encoded = std::getenv("GCNLLR_KEYBOARD_MOUSE"); gamecube_pads.enabled() && encoded) {
            std::istringstream bindings(encoded);
            if (!mouse_controls.start(bindings, std::getenv("GCNLLR_MOUSE_SENSITIVITY_FILE")))
                throw std::runtime_error("Unable to load Mouse + Keyboard bindings");
        }
        SDL_SetMainReady();
        SDL_SetHint("GCNLLR_RENDER_HIDDEN",input->hidden ? "1" : "0");
        SDL_SetHint("GCNLLR_GX_FILL_WINDOW",std::getenv("GCNLLR_DISPLAY_PIXEL_ASPECT") ? "1" : "0");
        uint32_t present_width=0,present_height=0;
        if (const char* fixed=std::getenv("GCNLLR_DISPLAY_ASPECT")) {
            const std::string_view ratio(fixed);
            const auto separator=ratio.find(':');
            if (separator==ratio.npos)
                throw std::runtime_error("Fixed display aspect must be WIDTH:HEIGHT");
            const char* split=ratio.data()+separator;
            const char* end=ratio.data()+ratio.size();
            const auto x=std::from_chars(ratio.data(),split,present_width);
            const auto y=std::from_chars(split+1,end,present_height);
            if (x.ec!=std::errc{} || x.ptr!=split || y.ec!=std::errc{} || y.ptr!=end ||
                !present_width || !present_height || present_width>16384u || present_height>16384u)
                throw std::runtime_error("Fixed display aspect must be WIDTH:HEIGHT");
        }
        aurora::webgpu::set_fixed_present_aspect(present_width,present_height);
        SDL_SetHint("GCNLLR_VALIDATE_GPU",std::getenv("GCNLLR_GPU_VALIDATE") ? "1" : "0");
#ifdef _WIN32
        HMODULE module{};char path[32768]{};
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<const char*>(&config),&module) || !GetModuleFileNameA(module,path,sizeof(path)))
            throw std::runtime_error("Native graphics runtime directory is unavailable");
        std::string runtime_path(path);
        runtime_path.resize(runtime_path.find_last_of("\\/")+1);
        SDL_SetHint("GCNLLR_NATIVE_RUNTIME_DIR",runtime_path.c_str());
        const unsigned system_length=GetSystemDirectoryA(path,sizeof(path));
        if (!system_length || system_length+2>=sizeof(path)) throw std::runtime_error("System runtime directory is unavailable");
        path[system_length]='\\';path[system_length+1]=0;
        SDL_SetHint("GCNLLR_SYSTEM_RUNTIME_DIR",path);
#endif
        AuroraConfig settings{};
        settings.appName=input->title;settings.userPath=input->data_directory;settings.cachePath=input->cache_directory;
#if defined(GCNLLR_GPU_VULKAN)
        settings.desiredBackend=BACKEND_VULKAN;
#elif defined(_WIN32)
        settings.desiredBackend=BACKEND_D3D12;
#elif defined(__APPLE__)
        settings.desiredBackend=BACKEND_METAL;
#else
        settings.desiredBackend=BACKEND_VULKAN;
#endif
        settings.windowWidth=800;settings.windowHeight=600;
        settings.windowPosX=-1;settings.windowPosY=-1;
        settings.vsync=false;settings.startFullscreen=false;settings.pauseOnFocusLost=false;
        settings.allowJoystickBackgroundEvents=true;settings.logLevel=LOG_INFO;settings.logCallback=log_message;
        gcnllr::present_trace::start();
        auto info=aurora_initialize(0,nullptr,&settings);
        initialized=true;
        if (!info.window) throw std::runtime_error("Aurora did not create a native GPU window");
        if (mouse_controls.enabled())
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        if (info.backend==BACKEND_NULL) throw std::runtime_error("A real GPU backend is required");
        bool compatible=info.backend==settings.desiredBackend;
#if defined(_WIN32) && !defined(GCNLLR_GPU_VULKAN)
        // Dawn can open the hardware Vulkan adapter when D3D12 device
        // creation fails, including under Proton on Steam Deck.
        compatible=compatible || info.backend==BACKEND_VULKAN;
#endif
        if (!compatible) throw std::runtime_error("Requested native GPU backend is unavailable");
        if (input->hidden) SDL_HideWindow(info.window);
        else place_playtest_window(info.window);
        gcnllr::gpu_bridge::initialize();
        GXInit(nullptr,0);
        GXSetCopyClear(GXColor{0,0,0,255},0xFFFFFFu);
        GXRenderModeObj mode{};mode.viTVmode=VI_TVMODE_NTSC_PROG;
        mode.fbWidth=mode.viWidth=640;mode.efbHeight=mode.xfbHeight=mode.viHeight=480;
        VIConfigure(&mode);
        aurora_set_frame_buffer_scale(float(gcnllr::gpu_bridge::efb_scale(std::getenv("GCNLLR_EFB_SCALE"))));
        /* The machine owns a 640x480 EFB; resizing the presentation window
         * must not expand its raster grid to the host window's aspect. */
        aurora::window::set_frame_buffer_aspect_fit(true);
        aurora_set_frame_interpolation(false);
        aurora_set_bloom(false);aurora_set_far_blur(false);aurora_set_ambient_occlusion(false);
        /* Consume the host SDK's initialization commands before retail GX
         * owns the renderer. Delaying this until end_frame overwrites the
         * guest's copy-clear with GXInit's grey default after its first copy. */
#if !GCNLLR_RETAIL_PIPELINE
        dol_guest_address_resolver_init_callback(&resolver,resolve,nullptr);
#endif
#if GCNLLR_RETAIL_PIPELINE
        frontend=std::make_unique<RetailGxFrontend>();
#else
        frontend=std::make_unique<RetailGxFrontend>(resolver);
        frontend->set_packet_drain_enabled(true);
#endif
        sink=std::make_unique<MachineSink>();
#if !GCNLLR_RETAIL_PIPELINE
        sink->core.set_guest_resolver(&resolver);sink->core.set_plan_observer(plan,nullptr);sink->core.set_copy_observer(copy,nullptr);
#endif
        aurora::gfx::gxcore::reset_texture_cache();
        invalidate_memory_observations();
        aurora::gfx::gxcore::set_texture_dirty_epoch_observer(texture_epoch);
        aurora::gfx::gxcore::set_efb_copy_epoch_observer(memory_epoch);
        aurora::gfx::gxcore::set_owned_texture_lookup(config.own_copy ? owned_texture:nullptr);
        frame_open=aurora_begin_frame();
        if (!frame_open) throw std::runtime_error("Initial native GPU frame failed");
        aurora::gx::fifo::drain();
#if GCNLLR_RETAIL_PIPELINE
        aurora::gx::recomp::set_guest_address_resolver([](void*,uint32_t address,uint32_t size,
            aurora::gx::recomp::AddressSpace space,aurora::gx::recomp::ResourceKind resource,
            const void** data,uint32_t* available) {
            DolGuestResolvedRange range{};
            if(!resolve(nullptr,address,size,static_cast<DolGuestAddressSpace>(space),
                static_cast<DolGuestResourceKind>(resource),&range))return false;
            // Resolving a texture pointer does not consume its RAM bytes.
            // The selected native resolver first considers an exact live GPU
            // view, then observes memory only if conversion really needs it.
            *data=range.data;*available=range.available;return true;
        },nullptr);
        gcnllr_aurora_retail_reset();retail_texture_ids.clear();aurora::gx::clear_static_texture_cache();
        retail_active=true;
        std::fprintf(stderr,"[native-gx-pipeline] provider=aurora-retail packet_frontend=0 gxcore_state_owner=0\n");
#endif
        if (const auto* path=std::getenv("GCNLLR_FRAME_TIMES")) {
            frame_times=std::fopen(path,"wx");
            if (!frame_times) throw std::runtime_error("Could not create new frame-time evidence");
            // Keep diagnostic disk writes out of a normal qualification run.
            // The bounded buffer holds over ten minutes and flushes on close.
            if(std::setvbuf(frame_times,frame_times_buffer.data(),_IOFBF,frame_times_buffer.size()))
                throw std::runtime_error("Could not buffer frame-time evidence");
            completion_service_ns=last_completion_service_ns=0;
            std::fprintf(frame_times,"frame,time_ns,present_call_ns,copy_readback_ns,main_cpu_cycles,completion_service_ns\n");
        }
        pump(true);
        if (!gcnllr::gpu_bridge::settle_framebuffer())
            throw std::runtime_error("Native framebuffer initialization did not settle");
#if GCNLLR_GX_FIFO_WORKER
        fifo_worker=std::make_unique<gcnllr::FifoWorker>([](std::span<const uint8_t> bytes,bool flush){
            static thread_local const bool registered=[] {
                aurora::gfx::thread_cpu::register_current(aurora::gfx::thread_cpu::Role::GxWorker);return true;
            }();
            (void)registered;
            RecordingBatch recording;
            Measure measurement(fifo_cost);
            invalidate_memory_observations();sink->memory_boundary();
            // The pinned donor's drain mode emits trace events at command
            // boundaries before its bounded ring fills. Keep the handoff as
            // one stream, including partial-command carry, instead of forcing
            // a parse/erase/packet handoff for every 1 KiB of guest commands.
            if(!bytes.empty() && (!frontend->write_fifo(bytes) || !flush_commands()))
                throw std::runtime_error(frontend->last_error() ? frontend->last_error() : "Native FIFO resource validation failed");
            if(flush) {
                if(!flush_commands())throw std::runtime_error("Native FIFO barrier failed");
                flush_geometry();flush_display_copies();
            }
#if GCNLLR_ASYNC_COPIES
            else {
                // A copy without a later PE finish must still execute. The
                // resolved copy already flushes preceding draw assembly in
                // GXCore. Encode those display copies without breaking draw
                // fusion at every unrelated CPU-to-worker handoff.
                flush_display_copies();
            }
#endif
#if GCNLLR_ASYNC_COPIES
            // Wait once for this completed batch on the worker. The next
            // earned device service joins this work before guest execution
            // continues; host latency must not extend guest PE wait time.
            retire_streams(true);
#else
            retire_streams(flush);
#endif
            if(failed.load())throw std::runtime_error("Native FIFO worker failed");
        });
#endif
        std::fprintf(stderr,"[native-gx] translation=%s vi=%s\n",fifo_worker ? "worker" : "synchronous",
            GCNLLR_GX_COOPERATIVE_VI ? "completed-scanout/recording-lock" : "full-barrier");
        return 1;
    } catch (const std::exception& e) { fail(e.what());stop();return 0; }
}
int fifo(const uint8_t* bytes,uint32_t size) {
    if (!initialized || failed.load() || (!bytes && size)) return 0;
    if(std::getenv("GCNLLR_GPU_TRACE")) {
        const auto count=std::min<size_t>(size,fifo_tail.size());
        std::memmove(fifo_tail.data(),fifo_tail.data()+count,fifo_tail.size()-count);
        if(count)std::memcpy(fifo_tail.data()+fifo_tail.size()-count,bytes+size-count,count);
    }
    try {
        if(fifo_worker)fifo_worker->append({bytes,size});
        else {
#if GCNLLR_RETAIL_PIPELINE
            StoppedArrayWriters stopped;
#endif
            Measure measurement(fifo_cost);
            invalidate_memory_observations();sink->memory_boundary();
            if (!frontend->write_fifo({bytes,size}) || !flush_commands())
                throw std::runtime_error(frontend->last_error() ? frontend->last_error() : "Native FIFO resource validation failed");
#if GCNLLR_ASYNC_COPIES
            // Owner-thread translation eliminates a condition-variable
            // round-trip for each small emulated GPU quantum. Actual GPU
            // encoding remains on Aurora's render worker. Composed ownership
            // joins for PE effects rather than every small FIFO quantum.
            flush_display_copies();service_streams();
#endif
        }
        state.fifo_bytes+=size;
        return !failed.load();
    } catch (const std::exception& e) { fail(e.what());return 0; }
}
int drain() {
    if (!initialized || failed.load()) return 0;
    try {
        gcnllr::gpu_bridge::validate_submission();
        if(fifo_worker)fifo_worker->wait(true);
        else {
            invalidate_memory_observations();sink->memory_boundary();
            if (!flush_commands())
                throw std::runtime_error(frontend->last_error() ? frontend->last_error() : "Native FIFO resource validation failed");
            flush_geometry();flush_display_copies();retire_streams(true);
        }
        return !failed.load();
    } catch (const std::exception& e) { fail(e.what());return 0; }
}
void status(GcnllrNativeGxStatus* output) {
    if (!output) return;
    Measure measurement(status_cost);
    try {
        if(initialized)gcnllr::gpu_bridge::validate_submission();
        if(fifo_worker) {
            if(GCNLLR_GX_COOPERATIVE_VI)fifo_worker->service();
            else fifo_worker->wait();
        }
    }catch(const std::exception& e){fail(e.what());}
    std::unique_lock recording(recording_mutex,std::defer_lock);
    if(GCNLLR_GX_COOPERATIVE_VI)recording.lock();
    if (initialized) pump();
    const auto& cache=aurora::gfx::gxcore::texture_cache_stats();
    state.texture_generation_hits=cache.generation_hits;state.texture_hashes=cache.hashed_lookups;
    aurora::gfx::gxcore::efb_copy_memory_stats(&state.verified_copy_hits,&state.verified_copy_misses);
    profile_report();
    *output=state;output->failed=failed.load();output->readback_ns=readback_ns.load(std::memory_order_relaxed);
    output->resident_textures=resident_textures.load(std::memory_order_relaxed);
    output->resident_texture_hits=resident_texture_hits.load(std::memory_order_relaxed);
    output->resident_texture_materializations=resident_texture_materializations.load(std::memory_order_relaxed);
    output->resident_texture_incompatible=resident_texture_incompatible.load(std::memory_order_relaxed);
}
int service() {
    if(!initialized || failed.load())return 0;
    try {
        gcnllr::gpu_bridge::validate_submission();
        if(fifo_worker) {
            fifo_worker->service();
#if GCNLLR_ASYNC_COPIES
            // This service is an earned guest-device deadline/observation.
            // Host GPU latency may stall the host here, but must not consume
            // more guest time while the CPU spins on a late PE completion.
            // Keep the copies batched; retire the submitted batch together.
            if(fifo_worker->pending())fifo_worker->wait();
            // Defensive completion for effects produced outside a normal
            // handoff. Ordinary batches retire before their parsed marker.
            if(streamed_pending.load(std::memory_order_acquire))fifo_worker->wait(true);
#endif
        }
#if GCNLLR_ASYNC_COPIES
        else service_streams();
#elif GCNLLR_GPU_OWNED_DISPLAY
        else flush_display_copies();
#endif
        return !failed.load();
    }
    catch(const std::exception& e){fail(e.what());return 0;}
}
int peek(uint32_t depth,uint16_t x,uint16_t y,uint32_t* value) {
    if (!value || !drain()) return 0;
    std::unique_lock recording(recording_mutex,std::defer_lock);
    if(GCNLLR_GX_COOPERATIVE_VI)recording.lock();
    if (depth==2) {
        try {return gcnllr::gpu_bridge::peek_scanout(x,y,value);}
        catch(const std::exception& e) {fail(e.what());return 0;}
    }
    // This renderer's raster grid is 640x480. Reads must observe the current
    // ordered EFB, including before the first VI presentation. Reuse a CPU
    // download only until an accepted draw, clearing copy or new frame.
    if (x>=640 || y>=480 || depth>1) return 0;
    try {
        if (depth) {
            if (depth_peek_generation!=efb_generation) {
                depth_peek=gcnllr::gpu_bridge::read_depth(depth_peek_width,depth_peek_height);
                depth_peek_generation=efb_generation;
            }
            const auto sx=std::min(depth_peek_width-1,uint32_t((uint64_t(x)*2+1)*depth_peek_width/1280));
            const auto sy=std::min(depth_peek_height-1,uint32_t((uint64_t(y)*2+1)*depth_peek_height/960));
            *value=depth_peek[size_t(sy)*depth_peek_width+sx];
        } else {
            if (color_peek_generation!=efb_generation) {
                color_peek=gcnllr::gpu_bridge::read_efb(color_peek_width,color_peek_height);
                color_peek_generation=efb_generation;
            }
            const auto sx=std::min(color_peek_width-1,uint32_t((uint64_t(x)*2+1)*color_peek_width/1280));
            const auto sy=std::min(color_peek_height-1,uint32_t((uint64_t(y)*2+1)*color_peek_height/960));
            const auto* p=color_peek.data()+(size_t(sy)*color_peek_width+sx)*4;
            *value=(uint32_t(p[3])<<24)|(uint32_t(p[0])<<16)|(uint32_t(p[1])<<8)|p[2];
#if GCNLLR_RETAIL_PIPELINE
            if((sink->bp[0x43]&7u)!=1u)*value|=0xff000000u;
#endif
        }
        return 1;
    } catch (const std::exception& e) {fail(e.what());return 0;}
}
int present_impl(const uint8_t* yuy2,uint32_t width,uint32_t height,uint32_t stride,
                 const gcnllr::gpu_bridge::EncodedImageHandle& resident={},
                 const std::vector<gcnllr::gpu_bridge::DisplaySpan>& spans={}) {
    Measure measurement(present_cost);
    if (!initialized || failed.load()) return 0;
    if(GCNLLR_GX_COOPERATIVE_VI) {
        if(!service())return 0;
    } else if(!drain())return 0;
    const auto start=Clock::now();
    try {
        std::unique_lock recording(recording_mutex,std::defer_lock);
        if(GCNLLR_GX_COOPERATIVE_VI) {
            recording.lock();
            // Preserve pending geometry before closing its recording packet.
            // Owner effects release this lock only after earlier assembly was
            // flushed for their copy/token, so this cannot re-enter a draw.
            flush_geometry();
        }
        const bool shown=!spans.empty() ? gcnllr::gpu_bridge::present_display_spans(spans,width,height) :
            resident ? gcnllr::gpu_bridge::present_display_copy(resident,width,height) :
            gcnllr::gpu_bridge::present(yuy2,width,height,stride);
        if (!shown)
            throw std::runtime_error("VI frame could not be presented");
        aurora::gfx::gxcore::note_frame_presented();
        ++state.presents;
        if(resident || !spans.empty())++state.resident_scanouts;
        ++efb_generation;
        const auto now=Clock::now();
        if (frame_times) {
            const auto completion_ns=completion_service_ns.load(std::memory_order_relaxed);
            uint64_t cycles{};
#ifdef _WIN32
            QueryThreadCycleTime(GetCurrentThread(),&cycles);
#endif
            std::fprintf(frame_times,"%llu,%llu,%llu,%llu,%llu,%llu\n",(unsigned long long)state.presents,
                (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(),
                (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(now-start).count(),
                (unsigned long long)readback_ns.load(std::memory_order_relaxed),(unsigned long long)cycles,
                (unsigned long long)(completion_ns-last_completion_service_ns));
            last_completion_service_ns=completion_ns;
        }
        pump(true);return 1;
    } catch (const std::exception& e) {fail(e.what());return 0;}
}
int present(const uint8_t* yuy2,uint32_t width,uint32_t height,uint32_t stride) {
    return present_impl(yuy2,width,height,stride);
}
int present_copy(uint64_t token,uint32_t width,uint32_t height,uint32_t stride) {
    if(!initialized || failed.load())return -1;
    const auto retained=retained_copy(token);
    if(!(retained.copy.flags&GCNLLR_NATIVE_GX_COPY_DISPLAY) || !retained.image ||
       width!=retained.copy.row_bytes/2 || height!=retained.copy.rows ||
       stride!=retained.copy.stride)return 0;
    return present_impl(nullptr,width,height,stride,retained.image) ? 1:-1;
}
int present_spans(const GcnllrNativeGxScanoutSpan* plan,uint32_t count,uint32_t width,uint32_t height) {
    if(!initialized || failed.load())return -1;
    if(!plan || !count || count>576 || !width || !height || width>640 || height>576)return 0;
    try {
        std::vector<gcnllr::gpu_bridge::DisplaySpan> spans;spans.reserve(count);
        uint64_t end=0,total=uint64_t(width)*height*2;
        for(uint32_t n=0;n<count;n++) {
            const auto& p=plan[n];
            if(!p.size || ((p.source_offset|p.destination_offset|p.size)&3u) ||
               p.destination_offset!=end ||
               uint64_t(p.destination_offset)+p.size>total)return 0;
            if(p.token) {
                const auto retained=retained_copy(p.token);
                if(!(retained.copy.flags&GCNLLR_NATIVE_GX_COPY_DISPLAY) || !retained.image ||
                   uint64_t(p.source_offset)+p.size>uint64_t(retained.copy.row_bytes)*retained.copy.rows)return 0;
                spans.push_back({retained.image,p.source_offset,p.destination_offset,p.size,{}});
            } else {
                GcnllrNativeGxMemory memory{};
                if(!gcnllr::gx_memory(config,p.source_offset,p.size,memory))return 0;
                if(config.pending_read && config.pending_read(config.user,p.source_offset,p.size) &&
                   (!config.prepare_read || !config.prepare_read(config.user,p.source_offset,p.size)))
                    throw std::runtime_error("CPU scanout observation failed");
                spans.push_back({{},0,p.destination_offset,p.size,
                    std::vector<uint8_t>(memory.bytes,memory.bytes+p.size)});
            }
            end+=p.size;
        }
        if(end!=total)return 0;
        return present_impl(nullptr,width,height,width*2,{},spans)?1:-1;
    } catch(const std::exception& e){fail(e.what());return -1;}
}
#if !GCNLLR_RETAIL_PIPELINE && !GCNLLR_NATIVE_DRAW_SINK
#include "checkpoint.inc"
#endif
const GcnllrNativeGxApi api={GCNLLR_NATIVE_GX_ABI,start,fifo,drain,status,peek,stop,present,service,
    GCNLLR_NATIVE_GX_RESIDENT_COPY|GCNLLR_NATIVE_GX_SLICED_TEXTURE_COPIES
        |(GCNLLR_GX_COOPERATIVE_VI ? GCNLLR_NATIVE_GX_COMPLETED_SCANOUT : 0u)
        |(GCNLLR_ASYNC_COPIES ? GCNLLR_NATIVE_GX_STREAMED_COPY : 0u)
        |(GCNLLR_COMPOSED_SCANOUT ? GCNLLR_NATIVE_GX_DEFERRED_RETIREMENT : 0u)
        |(GCNLLR_GPU_OWNED_DISPLAY ? GCNLLR_NATIVE_GX_DISPLAY_COLOR|GCNLLR_NATIVE_GX_COPY_RANGE : 0u)
        |(GCNLLR_GPU_OWNED_TEXTURES ? GCNLLR_NATIVE_GX_TEXTURE_VIEWS : 0u)
        |(GCNLLR_GX_COOPERATIVE_VI && !GCNLLR_GPU_OWNED_DISPLAY ? GCNLLR_NATIVE_GX_COMPOSED_SCANOUT : 0u),
    materialize_copy,release_copy,present_copy,present_spans,materialize_copy_range};
}
#ifdef _WIN32
#define GCNLLR_EXPORT __declspec(dllexport)
#else
#define GCNLLR_EXPORT __attribute__((visibility("default")))
#endif
#if GCNLLR_RETAIL_PIPELINE
#if GCNLLR_PERSISTENT_GEOMETRY
extern "C" bool gcnllr_aurora_geometry_epoch(const void* data,uint32_t size,uint32_t* address,uint64_t* epoch) {
    if(!retail_active || !config.texture_epoch || !data || !size ||
       !gcnllr::gx_address(config,data,size,*address))return false;
    return texture_epoch(*address,size,epoch);
}
#endif
extern "C" uint64_t gcnllr_aurora_retail_array(const void* data,uint32_t bytes) {
    if(!retail_active || !data || !bytes)return 0;
    uint32_t address;
    if(!gcnllr::gx_address(config,data,bytes,address))
        throw std::runtime_error("Native retail array exceeds consumer memory");
    const auto* begin=static_cast<const uint8_t*>(data);
    const auto identity=array_writers_stopped
        ? retail_texture_ids.array_prefix(address,begin,bytes,array_read_scope)
        : retail_texture_ids.identify({address,bytes,0,0,UINT32_MAX,0},begin,
              [&](uint64_t* epoch){return memory_epoch(address,bytes,epoch);});
    return (uint64_t(identity.first)<<32)|identity.second;
}
extern "C" bool gcnllr_aurora_retail_texture(const void* data,uint32_t bytes,uint32_t width,
    uint32_t height,uint32_t format,uint32_t mips,uint32_t* id,uint32_t* version) {
    if(!retail_active || !bytes || !data)return false;
    uint32_t address;
    if(!gcnllr::gx_address(config,data,bytes,address))return false;
    const auto identity=retail_texture_ids.identify({address,bytes,width,height,format,mips},
        static_cast<const uint8_t*>(data),[&](uint64_t* epoch){return texture_epoch(address,bytes,epoch);});
    *id=identity.first;*version=identity.second;return true;
}
extern "C" void gcnllr_aurora_retail_draw() {
    if(retail_active){++state.draws;++efb_generation;}
}
extern "C" void gcnllr_aurora_retail_observe(const void* data,uint32_t size) {
    if(!retail_active || !size || !data)return;
    uint32_t address;
    if(!gcnllr::gx_address(config,data,0,address))return; /* donor-owned storage */
    if(!gcnllr::gx_address(config,data,size,address) || !observe_pending_memory(address,size))
        throw std::runtime_error("Native retail resource observation failed");
}
// The exact live machine producer is the authority, including CPU writes and
// overlap retirement. On a miss the normal native texture decoder observes RAM.
std::shared_ptr<aurora::gfx::TextureRef> gcnllr_aurora_retail_owned(
    const void* data,uint32_t bytes,uint32_t width,uint32_t height,uint32_t format,uint32_t mips) {
    if(!retail_active || !config.own_copy || !data || !bytes || mips>1)return {};
    uint32_t address;
    if(!gcnllr::gx_address(config,data,bytes,address))return {};
    return owned_texture(address,bytes,format,width,height);
}
std::shared_ptr<aurora::gfx::TextureRef> gcnllr_aurora_retail_owned_ci8(
    [[maybe_unused]] const GXTexObj_& input, [[maybe_unused]] const GXTlutObj_& tlut) {
#if GCNLLR_GPU_OWNED_TEXTURES
    if (!retail_active || !config.own_copy || !input.data || input.mip_count() != 1 ||
        !tlut.data || !tlut.numEntries) return {};
    const auto size = aurora::gx::recomp::texture_byte_size(input);
    uint32_t address;
    if (!size || !gcnllr::gx_address(config, input.data, size, address)) return {};
    std::unique_lock lock(retained_mutex);
    auto found = find_texture_producer(address);
    if (found == retained_copies.end() || !is_intensity8_copy(found->second.texture.format) ||
        !dense_texture_copy(found->second, size, input.width(), input.height())) return {};
    const RetainedCopy::PaletteKey key{
        tlut.data, tlut.tlutObjId, tlut.tlutDataVersion, tlut.format, tlut.numEntries};
    const bool cacheable = tlut.tlutObjId && !tlut.no_cache();
    if (cacheable && found->second.palette_view && found->second.palette_key == key) {
        resident_texture_hits.fetch_add(1, std::memory_order_relaxed);
        return found->second.palette_view;
    }
    const uint64_t token = found->first;
    lock.unlock();
    // Palette bytes remain guest memory and the read may itself have to wait
    // on an owned copy. Observe them without re-entering the retained mutex.
    gcnllr_aurora_retail_observe(tlut.data, std::min<uint32_t>(tlut.numEntries, 256) * 2);
    lock.lock();
    found = find_texture_producer(address);
    if (found == retained_copies.end() || found->first != token) return {};
    auto view = gcnllr::gpu_bridge::colorize_ci8(found->second.texture.texture, tlut);
    if (cacheable) {
        found->second.palette_key = key;
        found->second.palette_view = view;
    }
    resident_texture_hits.fetch_add(1, std::memory_order_relaxed);
    return view;
#else
    return {};
#endif
}
extern "C" bool gcnllr_aurora_retail_bp(uint32_t reg,uint32_t value) {
    if(!retail_active)return false;
    sink->bp[reg]=value;sink->bp_seen[reg]=true;
    if(reg==0x45 || reg==0x47 || reg==0x48) {
        gcnllr_aurora_retail_finish();
        flush_display_copies();ordered_bp_effect(reg,value);return true;
    }
    if(reg!=0x52)return false;
    gcnllr_aurora_retail_finish();
    const auto& bp=sink->bp;
    const uint32_t width=(bp[0x4a]&1023u)+1,height=((bp[0x4a]>>10)&1023u)+1;
    const bool display=(value&(1u<<14))!=0;
    const bool half=!display && (value&(1u<<9));
    const uint32_t dst_width=half?(width+1)/2:width,dst_height=half?(height+1)/2:height;
    const uint32_t format=display?0xf:gcnllr::retail_copy_format(value,(bp[0x43]&7u)==3u);
    const auto tile=gcnllr::native_copy_layout(format);
    if(!display && !tile.bytes)throw std::runtime_error("Native retail copy format is unsupported");
    const uint32_t bytes=display?0:((dst_width+tile.width-1)/tile.width)*
        ((dst_height+tile.height-1)/tile.height)*tile.bytes;
    const auto c0=bp[0x4f],c1=bp[0x50],cz=bp[0x51];
    const gxruntime::gxcore::EfbCopyCommand command{
        .dest_address=(bp[0x4b]&guest_gpu_address_mask)<<5,.byte_size=bytes,.format=format,
        .src_x=bp[0x49]&1023u,.src_y=(bp[0x49]>>10)&1023u,.width=width,.height=height,
        .destination_width=dst_width,.destination_height=dst_height,.clear=(value&(1u<<11))!=0,
        .clear_r=c0&255,.clear_g=(c1>>8)&255,.clear_b=c1&255,.clear_a=(c0>>8)&255,.clear_z=cz&0xffffff,
        .color_update=!sink->bp_seen[0x41] || (bp[0x41]&8)!=0,
        .alpha_update=!sink->bp_seen[0x41] || (bp[0x41]&16)!=0,
        .depth_update=!sink->bp_seen[0x40] || (bp[0x40]&16)!=0,
        .efb_has_alpha=!sink->bp_seen[0x43] || (bp[0x43]&7)==1};
    copy(command,nullptr);
    if(!display) {
        aurora::gx::g_gxState.stateDirty=true;
        for(auto& binding:aurora::gx::g_gxState.textures)binding.texObj.texObjId=0;
    }
    return true;
}
#endif
extern "C" GCNLLR_EXPORT const GcnllrNativeGxApi* gcnllr_native_gx_get_api(uint32_t abi) {
    return abi==GCNLLR_NATIVE_GX_ABI ? &api : nullptr;
}

void gcnllr::draw_mouse_settings() {
    if (!mouse_controls.settings_open() || !ImGui::GetCurrentContext())
        return;
    namespace mk = gcnllr::keyboard_mouse;
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({io.DisplaySize.x * .5f, io.DisplaySize.y * .5f},
                            ImGuiCond_Always, {.5f,.5f});
    ImGui::SetNextWindowSize({std::min(380.f, io.DisplaySize.x * .9f), 0.f});
    bool open = true;
    if (ImGui::Begin("Mouse + Keyboard", &open,
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        int percent = mouse_controls.sensitivity();
        if (ImGui::SliderInt("Aim sensitivity", &percent, mk::min_sensitivity, mk::max_sensitivity, "%d%%"))
            mouse_controls.set_sensitivity(percent);
        ImGui::SetItemDefaultFocus();
        if (mouse_controls.save_failed())
            ImGui::TextUnformatted("Sensitivity could not be saved.");
        if (ImGui::Button("Resume"))
            open = false;
    }
    ImGui::End();
    if (!open)
        mouse_controls.close_settings();
}
extern "C" GCNLLR_EXPORT const GcnllrNativeGxCheckpointApi* gcnllr_native_gx_get_checkpoint_api(uint32_t abi) {
#if !GCNLLR_RETAIL_PIPELINE && !GCNLLR_NATIVE_DRAW_SINK
    if(abi==GCNLLR_NATIVE_GX_CHECKPOINT_ABI && checkpoint_supported())return &checkpoint_api;
#else
    (void)abi;
#endif
    return nullptr;
}
extern "C" GCNLLR_EXPORT int gcnllr_native_gx_gamecube_input(uint32_t abi, GcnllrGameCubeInput* output) {
    if (abi != GCNLLR_GAMECUBE_INPUT_ABI || !output || !gamecube_pads.enabled())
        return 0;
    *output = gamecube_pads.input();
    return 1;
}

extern "C" GCNLLR_EXPORT int gcnllr_native_gx_keyboard_mouse(uint32_t port, uint32_t consume,
                                                           GcnllrKeyboardMouseInput* output) {
    const int keyboard_port = gamecube_pads.keyboard_port();
    return output && keyboard_port >= 0 && port == static_cast<uint32_t>(keyboard_port) &&
        mouse_controls.read(consume, *output);
}
extern "C" GCNLLR_EXPORT void gcnllr_native_gx_scanout_view(uint32_t x,uint32_t y,uint32_t columns,uint32_t rows,
                                                           uint32_t aspect_width,uint32_t aspect_height,
                                                           uint32_t trim_lines) {
    gcnllr::gpu_bridge::set_scanout_view(x,y,columns,rows,aspect_width,aspect_height,trim_lines);
}

extern "C" GCNLLR_EXPORT void gcnllr_native_gx_scanout_timing(uint64_t native_ns) {
    gcnllr::gpu_bridge::set_scanout_timing(native_ns);
}

extern "C" GCNLLR_EXPORT int gcnllr_native_gx_overlay_layout(uint32_t columns,uint32_t rows,uint32_t tiles,
                                                            uint32_t width,uint32_t height,float aspect) {
#if GCNLLR_RETAIL_PIPELINE
    if(!retail_active || !frontend)return 0;
    if(tiles && (!columns || columns>2 || rows!=2 || !width || !height || width>640 || height>480))return 0;
    if(!flush_commands())return 0;
    flush_geometry();
    gcnllr_aurora_retail_overlay(columns,rows,tiles,width,height,aspect);
    return 1;
#else
    (void)columns;(void)rows;(void)tiles;(void)width;(void)height;
    return 0;
#endif
}

extern "C" GCNLLR_EXPORT int gcnllr_native_gx_overlay_split(float split,float shift) {
#if GCNLLR_RETAIL_PIPELINE
    if(!retail_active || !frontend || !flush_commands())return 0;
    flush_geometry();
    gcnllr_aurora_retail_overlay_split(split,shift);
    return 1;
#else
    (void)split;(void)shift;
    return 0;
#endif
}

extern "C" GCNLLR_EXPORT int gcnllr_native_gx_view_framing(float scale,float anchor_x,float anchor_y) {
#if GCNLLR_RETAIL_PIPELINE
    if(!retail_active || !frontend || !flush_commands())return 0;
    flush_geometry();
    gcnllr_aurora_retail_view_framing(scale,anchor_x,anchor_y);
    return 1;
#else
    (void)scale;(void)anchor_x;(void)anchor_y;
    return 0;
#endif
}

extern "C" GCNLLR_EXPORT int gcnllr_native_gx_output_size(uint32_t* width,uint32_t* height) {
    int w=0,h=0;
    if(!initialized || !width || !height ||
       !SDL_GetWindowSizeInPixels(aurora::window::get_sdl_window(),&w,&h) || w<=0 || h<=0)
        return 0;
    const auto viewport=aurora::webgpu::calculate_present_viewport(w,h,w,h);
    *width=uint32_t(viewport.width);
    *height=uint32_t(viewport.height);
    return *width && *height;
}
extern "C" GCNLLR_EXPORT int gcnllr_native_gx_resize_window(uint32_t width,uint32_t height) {
    auto* window=initialized ? aurora::window::get_sdl_window():nullptr;
    return window && width>=64 && height>=64 && width<=16384 && height<=16384 &&
        SDL_SetWindowSize(window,int(width),int(height));
}

namespace aurora::gfx::efb_readback { bool request_surface() noexcept; }

extern "C" GCNLLR_EXPORT int gcnllr_native_gx_request_present(void) {
    return initialized && aurora::gfx::efb_readback::request_surface();
}

extern "C" GCNLLR_EXPORT int gcnllr_native_gx_take_present(const uint8_t** rgba,uint32_t* w,uint32_t* h) {
    return initialized && aurora_take_framebuffer_readback(rgba,w,h);
}

extern "C" GCNLLR_EXPORT int gcnllr_native_gx_fullscreen(int enabled) {
    auto* window=initialized ? aurora::window::get_sdl_window():nullptr;
    return window && SDL_SetWindowFullscreen(window,enabled!=0);
}
