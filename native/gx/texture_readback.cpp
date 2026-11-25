// SPDX-License-Identifier: GPL-3.0-or-later
// Ordered RGBA readback adapted from pinned RecompCore Aurora gfx/texture.cpp.
// Retains the caller's bytes/format contract; uses completion events and a
// staging buffer owned by Aurora's render worker instead of per-copy polling.
#include "gfx/common.hpp"
#include "gfx/texture.hpp"
#include "gfx/render_worker.hpp"
#include "webgpu/gpu.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <thread>

namespace aurora::gfx {
namespace {
struct ReadbackPool {
    wgpu::Buffer buffer;
    uint64_t capacity{},reads{},allocations{},polls{};
    ~ReadbackPool() {
        std::fprintf(stderr,"[native-gpu-readback] reads=%llu allocations=%llu fallback-polls=%llu capacity=%llu\n",
            (unsigned long long)reads,(unsigned long long)allocations,
            (unsigned long long)polls,(unsigned long long)capacity);
    }
};
struct ReadbackResult {
    std::vector<uint8_t> rgba;
    std::exception_ptr error;
    bool done{};
};
}

std::vector<uint8_t> read_texture_rgba8(const TextureHandle& texture) {
    if(!texture || render_worker::is_worker_thread())return {};
    return read_texture_rgba8(texture->texture,texture->size,texture->format);
}
std::vector<uint8_t> read_texture_rgba8(const wgpu::Texture& texture,wgpu::Extent3D size,
                                      wgpu::TextureFormat format) {
    if(!texture || render_worker::is_worker_thread() || !size.width || !size.height ||
       size.width>(UINT32_MAX-255u)/4u)return {};
    const bool bgra=format==wgpu::TextureFormat::BGRA8Unorm || format==wgpu::TextureFormat::BGRA8UnormSrgb;
    if(!bgra && format!=wgpu::TextureFormat::RGBA8Unorm && format!=wgpu::TextureFormat::RGBA8UnormSrgb)return {};
    auto result=std::make_shared<ReadbackResult>();
    render_worker::enqueue_work([texture,size,bgra,result] {
        // This worker exits (and destroys its TLS) before webgpu::shutdown.
        // Nothing is retained on the machine/FIFO thread across device teardown.
        static thread_local ReadbackPool pool;
        try {
            const uint32_t stride=(size.width*4u+255u)&~255u;
            const uint64_t bytes=uint64_t(stride)*size.height;
            if(!pool.buffer || pool.capacity<bytes) {
                const wgpu::BufferDescriptor descriptor{.label="GX ordered copy readback",
                    .usage=wgpu::BufferUsage::MapRead|wgpu::BufferUsage::CopyDst,.size=bytes};
                pool.buffer=webgpu::g_device.CreateBuffer(&descriptor);pool.capacity=bytes;++pool.allocations;
            }
            ++pool.reads;
            auto buffer=pool.buffer;
            auto encoder=webgpu::g_device.CreateCommandEncoder();
            if(!buffer || !encoder)return;
            const wgpu::TexelCopyTextureInfo source{.texture=texture};
            const wgpu::TexelCopyBufferInfo destination{
                .layout={.bytesPerRow=stride,.rowsPerImage=size.height},.buffer=buffer};
            const wgpu::Extent3D extent{.width=size.width,.height=size.height,.depthOrArrayLayers=1};
            encoder.CopyTextureToBuffer(&source,&destination,&extent);
            const auto commands=encoder.Finish();webgpu::g_queue.Submit(1,&commands);
            const auto future=buffer.MapAsync(wgpu::MapMode::Read,0,bytes,wgpu::CallbackMode::WaitAnyOnly,
                [buffer,bytes,stride,size,bgra,result](wgpu::MapAsyncStatus status,wgpu::StringView) {
                    if(status==wgpu::MapAsyncStatus::Success) {
                        try {
                            const auto* mapped=static_cast<const uint8_t*>(buffer.GetConstMappedRange(0,bytes));
                            if(mapped) {
                                result->rgba.resize(size_t(size.width)*size.height*4u);
                                for(uint32_t y=0;y<size.height;y++) {
                                    auto* row=result->rgba.data()+size_t(y)*size.width*4u;
                                    std::memcpy(row,mapped+size_t(y)*stride,size.width*4u);
                                    if(bgra)for(uint32_t x=0;x<size.width;x++)std::swap(row[x*4u],row[x*4u+2u]);
                                }
                            }
                        }catch(...) {result->error=std::current_exception();}
                        buffer.Unmap();
                    }
                    result->done=true;
                });
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            webgpu::g_instance.WaitAny(future,5000000000ull);
            // Hosts without timed WaitAny retain a bounded portable fallback.
            // WaitAnyOnly callbacks execute here; no asynchronous result race.
            while(!result->done && std::chrono::steady_clock::now()<deadline) {
                ++pool.polls;webgpu::g_instance.WaitAny(future,0);
                if(!result->done)std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            if(!result->done || result->rgba.empty()) {pool.buffer={};pool.capacity=0;}
        }catch(...) {result->error=std::current_exception();pool.buffer={};pool.capacity=0;}
    });
    render_worker::synchronize();
    if(result->error)std::rethrow_exception(result->error);
    return result->done?std::move(result->rgba):std::vector<uint8_t>{};
}
}

#if GCNLLR_ASYNC_COPIES
#include "async_readback.hpp"
#include <atomic>
#include <mutex>
#include <stdexcept>

namespace aurora::gfx {
struct AsyncReadbackResult {
    std::mutex mutex;
    std::atomic<bool> done{};
    bool cancelled{},consumed{};
    std::vector<uint8_t> bytes;
    std::exception_ptr error;
};
namespace {
void reject_async(const AsyncReadback& result,const char* message) {
    std::lock_guard lock(result->mutex);
    if(result->done.load(std::memory_order_relaxed))return;
    result->cancelled=true;
    result->error=std::make_exception_ptr(std::runtime_error(message));
    result->done.store(true,std::memory_order_release);
}
struct AsyncTransfer {
    AsyncReadback result;
    wgpu::Texture texture;
    wgpu::Buffer source,staging;
    uint64_t bytes{};
    uint32_t width{},height{},stride{};
    bool raw{},bgra{};
};
struct AsyncSlot {
    wgpu::Buffer buffer;
    uint64_t capacity{};
    std::weak_ptr<AsyncReadbackResult> lease;
};
struct AsyncMap {
    AsyncReadback result;
    wgpu::Buffer staging;
    wgpu::Future future;
};
struct AsyncFence {
    std::vector<AsyncReadback> results;
    wgpu::Future future;
};
class AsyncPool {
public:
    static constexpr uint64_t max_bytes=256ull*1024*1024;
    static constexpr size_t max_slots=32;
    ~AsyncPool() {
        for(auto& map:pending) {
            reject_async(map.result,"GPU readback worker stopped before completion");
            map.staging.Unmap();webgpu::g_instance.WaitAny(map.future,0);
        }
        for(auto& request:requests)reject_async(request.result,"GPU readback worker stopped before submission");
        for(auto& fence:fences) {
            for(auto& result:fence.results)reject_async(result,"GPU completion worker stopped before completion");
            webgpu::g_instance.WaitAny(fence.future,0);
        }
        for(auto& result:completion_requests)reject_async(result,"GPU completion worker stopped before submission");
        std::fprintf(stderr,"[native-copy-async] queued=%llu submissions=%llu maps=%llu waits=%llu allocations=%llu capacity=%llu pending=%zu\n",
            (unsigned long long)queued,(unsigned long long)submissions,
            (unsigned long long)maps,(unsigned long long)waits,
            (unsigned long long)allocations,(unsigned long long)capacity,
            requests.size()+pending.size());
        std::fprintf(stderr,"[native-copy-completion] queued=%llu events=%llu waits=%llu pending=%zu\n",
            (unsigned long long)completions,(unsigned long long)completion_events,
            (unsigned long long)completion_waits,fences.size()+completion_requests.size());
    }
    void stage_completion(AsyncReadback result) {
        completion_requests.push_back(std::move(result));++completions;
    }
    void submit_completions() {
        if(completion_requests.empty())return;
        auto batch=std::move(completion_requests);completion_requests.clear();
        try {
            // Called after preceding render-worker submissions. This observes
            // their GPU completion without copying any result to the host.
            auto future=webgpu::g_queue.OnSubmittedWorkDone(wgpu::CallbackMode::WaitAnyOnly,
                [batch](wgpu::QueueWorkDoneStatus status,wgpu::StringView) {
                    for(const auto& result:batch) {
                        if(status!=wgpu::QueueWorkDoneStatus::Success)
                            reject_async(result,"GPU submitted work failed");
                        else result->done.store(true,std::memory_order_release);
                    }
                });
            if(!future.id)throw std::runtime_error("GPU completion returned no future");
            fences.push_back({batch,future});++completion_events;
        }catch(...) {
            for(const auto& result:batch) {
                std::lock_guard lock(result->mutex);
                result->cancelled=true;result->error=std::current_exception();
                result->done.store(true,std::memory_order_release);
            }
        }
    }
    void stage(AsyncTransfer transfer) {
        const auto result=transfer.result;
        try {
            if(!transfer.bytes || transfer.bytes>max_bytes)
                throw std::runtime_error("Invalid asynchronous readback span");
            AsyncSlot* slot=nullptr;
            for(auto& candidate:slots)
                if(candidate.lease.expired() && candidate.capacity>=transfer.bytes &&
                   (!slot || candidate.capacity<slot->capacity))slot=&candidate;
            if(!slot) {
                // Bounded reuse: obsolete idle capacities do not accumulate
                // after window/texture size changes. Busy leases stay alive.
                for(auto& candidate:slots)if(candidate.lease.expired()) {
                    capacity-=candidate.capacity;candidate.buffer={};candidate.capacity=0;
                }
                for(auto& candidate:slots)if(!candidate.capacity) {slot=&candidate;break;}
                if(!slot) {
                    if(slots.size()>=max_slots)throw std::runtime_error("Asynchronous readback slots exhausted");
                    slots.emplace_back();slot=&slots.back();
                }
                if(capacity>max_bytes-transfer.bytes)
                    throw std::runtime_error("Asynchronous readback staging budget exceeded");
                const wgpu::BufferDescriptor descriptor{.label="GX batched copy readback",
                    .usage=wgpu::BufferUsage::MapRead|wgpu::BufferUsage::CopyDst,.size=transfer.bytes};
                slot->buffer=webgpu::g_device.CreateBuffer(&descriptor);
                if(!slot->buffer)throw std::runtime_error("Asynchronous staging allocation failed");
                slot->capacity=transfer.bytes;capacity+=transfer.bytes;++allocations;
            }
            slot->lease=transfer.result;transfer.staging=slot->buffer;
            requests.push_back(std::move(transfer));++queued;
        }catch(...) {
            std::lock_guard lock(result->mutex);
            result->cancelled=true;result->error=std::current_exception();
            result->done.store(true,std::memory_order_release);
        }
    }
    void submit() {
        if(requests.empty())return;
        auto batch=std::move(requests);requests.clear();
        try {
            auto encoder=webgpu::g_device.CreateCommandEncoder();
            if(!encoder)throw std::runtime_error("Asynchronous copy encoder failed");
            for(const auto& request:batch) {
                if(request.raw)encoder.CopyBufferToBuffer(request.source,0,request.staging,0,request.bytes);
                else {
                    const wgpu::TexelCopyTextureInfo source{.texture=request.texture};
                    const wgpu::TexelCopyBufferInfo destination{
                        .layout={.bytesPerRow=request.stride,.rowsPerImage=request.height},.buffer=request.staging};
                    const wgpu::Extent3D extent{.width=request.width,.height=request.height,.depthOrArrayLayers=1};
                    encoder.CopyTextureToBuffer(&source,&destination,&extent);
                }
            }
            const auto commands=encoder.Finish();webgpu::g_queue.Submit(1,&commands);++submissions;
            for(auto& request:batch) {
                auto result=request.result;auto buffer=request.staging;
                const auto future=buffer.MapAsync(wgpu::MapMode::Read,0,request.bytes,
                    wgpu::CallbackMode::WaitAnyOnly,
                    [request](wgpu::MapAsyncStatus status,wgpu::StringView) {
                        const auto& result=request.result;
                        std::lock_guard lock(result->mutex);
                        if(result->cancelled)return;
                        try {
                            if(status!=wgpu::MapAsyncStatus::Success)
                                throw std::runtime_error("Asynchronous GPU mapping failed");
                            const auto* mapped=static_cast<const uint8_t*>(
                                request.staging.GetConstMappedRange(0,request.bytes));
                            if(!mapped)throw std::runtime_error("Asynchronous GPU mapping has no bytes");
                            if(request.raw)result->bytes.assign(mapped,mapped+request.bytes);
                            else {
                                result->bytes.resize(size_t(request.width)*request.height*4u);
                                for(uint32_t y=0;y<request.height;y++) {
                                    auto* row=result->bytes.data()+size_t(y)*request.width*4u;
                                    std::memcpy(row,mapped+size_t(y)*request.stride,size_t(request.width)*4u);
                                    if(request.bgra)for(uint32_t x=0;x<request.width;x++)
                                        std::swap(row[x*4u],row[x*4u+2u]);
                                }
                            }
                        }catch(...) {result->error=std::current_exception();}
                        if(status==wgpu::MapAsyncStatus::Success)request.staging.Unmap();
                        result->done.store(true,std::memory_order_release);
                    });
                if(!future.id)throw std::runtime_error("Asynchronous GPU mapping returned no future");
                pending.push_back({result,buffer,future});++maps;
            }
        }catch(...) {
            const auto error=std::current_exception();
            for(auto& request:batch) {
                {std::lock_guard lock(request.result->mutex);
                 if(!request.result->done.load(std::memory_order_relaxed)) {
                     request.result->cancelled=true;request.result->error=error;
                     request.result->done.store(true,std::memory_order_release);
                 }}
                request.staging.Unmap();
            }
        }
    }
    void poll(bool wait) {
        submit();
        submit_completions();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        for(auto& fence:fences) {
            auto ready=[&]{return std::all_of(fence.results.begin(),fence.results.end(),
                [](const AsyncReadback& result){return result->done.load(std::memory_order_acquire);});};
            if(wait && !ready()) {
                ++completion_waits;
                const auto remaining=deadline-std::chrono::steady_clock::now();
                if(remaining>std::chrono::steady_clock::duration::zero())
                    webgpu::g_instance.WaitAny(fence.future,uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count()));
            } else webgpu::g_instance.WaitAny(fence.future,0);
            while(wait && !ready() && std::chrono::steady_clock::now()<deadline) {
                webgpu::g_instance.WaitAny(fence.future,0);
                if(!ready())std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            if(wait && !ready())for(auto& result:fence.results)
                reject_async(result,"GPU submitted work timed out");
        }
        std::erase_if(fences,[](const AsyncFence& fence){return std::all_of(fence.results.begin(),fence.results.end(),
            [](const AsyncReadback& result){return result->done.load(std::memory_order_acquire);});});
        for(auto& map:pending) {
            if(map.result->done.load(std::memory_order_acquire)) {
                webgpu::g_instance.WaitAny(map.future,0);continue;
            }
            if(wait) {
                ++waits;
                const auto remaining=deadline-std::chrono::steady_clock::now();
                if(remaining>std::chrono::steady_clock::duration::zero())
                    webgpu::g_instance.WaitAny(map.future,uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count()));
            }else webgpu::g_instance.WaitAny(map.future,0);
            // Retain the working portable fallback for hosts without timed
            // WaitAny. Every callback still executes on this worker only.
            while(wait && !map.result->done.load(std::memory_order_acquire) &&
                  std::chrono::steady_clock::now()<deadline) {
                webgpu::g_instance.WaitAny(map.future,0);
                if(!map.result->done.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            if(wait && !map.result->done.load(std::memory_order_acquire)) {
                reject_async(map.result,"Asynchronous GPU readback timed out");
                map.staging.Unmap();
                webgpu::g_instance.WaitAny(map.future,0);
            }
        }
        std::erase_if(pending,[](const AsyncMap& map){return map.result->done.load(std::memory_order_acquire);});
    }
private:
    std::vector<AsyncSlot> slots;
    std::vector<AsyncTransfer> requests;
    std::vector<AsyncMap> pending;
    std::vector<AsyncReadback> completion_requests;
    std::vector<AsyncFence> fences;
    uint64_t queued{},submissions{},maps{},waits{},allocations{},capacity{};
    uint64_t completions{},completion_events{},completion_waits{};
};
AsyncPool& async_pool() {static thread_local AsyncPool pool;return pool;}
}
AsyncReadback queue_texture_readback(const TextureHandle& texture) {
    auto result=std::make_shared<AsyncReadbackResult>();
    if(!texture || render_worker::is_worker_thread() || !texture->size.width || !texture->size.height ||
       texture->size.width>(UINT32_MAX-255u)/4u) {
        reject_async(result,"Invalid asynchronous texture readback");return result;
    }
    const auto size=texture->size;
    const auto format=texture->format;
    const bool bgra=format==wgpu::TextureFormat::BGRA8Unorm || format==wgpu::TextureFormat::BGRA8UnormSrgb;
    if(!bgra && format!=wgpu::TextureFormat::RGBA8Unorm && format!=wgpu::TextureFormat::RGBA8UnormSrgb) {
        reject_async(result,"Unsupported asynchronous texture format");return result;
    }
    const uint32_t stride=(size.width*4u+255u)&~255u;
    render_worker::enqueue_work([texture,size,stride,bgra,result] {
        AsyncTransfer request;request.result=result;request.texture=texture->texture;
        request.width=size.width;request.height=size.height;request.stride=stride;
        request.bytes=uint64_t(stride)*size.height;request.bgra=bgra;
        async_pool().stage(std::move(request));
    });
    return result;
}
AsyncReadback queue_buffer_readback(const wgpu::Buffer& source,uint64_t bytes) {
    auto result=std::make_shared<AsyncReadbackResult>();
    if(!source || !bytes || (bytes&3u) || render_worker::is_worker_thread()) {
        reject_async(result,"Invalid asynchronous buffer readback");return result;
    }
    render_worker::enqueue_work([source,bytes,result] {
        AsyncTransfer request;request.result=result;request.source=source;
        request.bytes=bytes;request.raw=true;async_pool().stage(std::move(request));
    });
    return result;
}
AsyncReadback queue_gpu_completion() {
    auto result=std::make_shared<AsyncReadbackResult>();
    if(render_worker::is_worker_thread()) {
        reject_async(result,"GPU completion reentered its worker");return result;
    }
    render_worker::enqueue_work([result]{async_pool().stage_completion(result);});
    return result;
}
void poll_async_readbacks(bool wait) {
    if(render_worker::is_worker_thread())throw std::runtime_error("Readback poll reentered its worker");
    auto error=std::make_shared<std::exception_ptr>();
    render_worker::enqueue_work([wait,error] {
        try {async_pool().poll(wait);}catch(...) {*error=std::current_exception();}
    });
    render_worker::synchronize();
    if(*error)std::rethrow_exception(*error);
}
bool async_readback_ready(const AsyncReadback& ticket) {
    return ticket && ticket->done.load(std::memory_order_acquire);
}
void validate_async_readback(const AsyncReadback& ticket) {
    if(!async_readback_ready(ticket))throw std::runtime_error("Asynchronous readback is not complete");
    std::lock_guard lock(ticket->mutex);
    if(ticket->error)std::rethrow_exception(ticket->error);
}
std::vector<uint8_t> take_async_readback(const AsyncReadback& ticket) {
    validate_async_readback(ticket);
    std::lock_guard lock(ticket->mutex);
    if(ticket->consumed)throw std::runtime_error("Asynchronous readback was already consumed");
    ticket->consumed=true;return std::move(ticket->bytes);
}
}
#endif
