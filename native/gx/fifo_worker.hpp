// SPDX-License-Identifier: GPL-3.0-or-later
// FIFO handoff/translation-worker design adapted from RecompCore GXRuntime
// backends/aurora/aurora_graphics.cpp, pinned revision recorded in THIRD_PARTY.md.
// The machine remains the sole owner of guest-visible device/memory effects.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace gcnllr {
class FifoWorker {
public:
    using Consumer=std::function<void(std::span<const uint8_t>,bool)>;
    struct Stats {uint64_t batches{},bytes{},barriers{},owner_calls{};};
    explicit FifoWorker(Consumer consumer):consume_(std::move(consumer)),thread_([this]{run();}) {}
    FifoWorker(const FifoWorker&)=delete;
    FifoWorker& operator=(const FifoWorker&)=delete;
    ~FifoWorker() {stop();}

    // Only the machine thread publishes, services effects, waits and stops.
    void append(std::span<const uint8_t> bytes) {
        service();
        if(bytes.empty())return;
        {
            std::lock_guard lock(mutex_);check();
            handoff_.insert(handoff_.end(),bytes.begin(),bytes.end());++appended_;
        }
        work_.notify_one();
    }
    void service() {
        std::function<void()> effect;
        {
            std::lock_guard lock(mutex_);check();
            effect=std::move(owner_effect_);
        }
        if(!effect)return;
        std::exception_ptr error;
        try {effect();}catch(...){error=std::current_exception();}
        {
            std::lock_guard lock(mutex_);
            if(error && !error_)error_=error;
            owner_done_=true;
        }
        changed_.notify_all();
        if(error)std::rethrow_exception(error);
    }
    // Owner-only: pending device completions need progress even after the
    // guest stops submitting FIFO bytes. Coalesce idle polls; never queue an
    // unbounded stream of empty batches while a consumer/effect is blocked.
    void poke() {
        {std::lock_guard lock(mutex_);check();if(appended_!=parsed_)return;++appended_;}
        work_.notify_one();
    }
    bool pending() {
        std::lock_guard lock(mutex_);check();
        return appended_!=parsed_ || bool(owner_effect_);
    }
    void wait(bool flush=false) {
        uint64_t target;
        {
            std::lock_guard lock(mutex_);check();
            if(flush){flush_=true;++appended_;++stats_.barriers;}
            target=appended_;
        }
        work_.notify_one();
        for(;;) {
            service();
            std::unique_lock lock(mutex_);check();
            if(parsed_>=target)return;
            changed_.wait(lock,[&]{return error_ || stopped_ || owner_effect_ || parsed_>=target;});
        }
    }
    // Worker-only: wait for an ordered effect on the machine thread. The
    // captured buffers remain alive until the machine has finished using them.
    void on_owner(std::function<void()> effect) {
        std::unique_lock lock(mutex_);check();
        owner_done_=false;owner_effect_=std::move(effect);++stats_.owner_calls;
        changed_.notify_all();
        changed_.wait(lock,[&]{return owner_done_ || error_ || stopped_;});check();
    }
    // Recording transitions run on the owner too. Never hold their lock while
    // waiting for an owner effect: the owner may be presenting when it arrives.
    // Restore ownership before either returning or unwinding the consumer.
    void on_owner(std::function<void()> effect,std::unique_lock<std::mutex>& recording) {
        recording.unlock();
        try {on_owner(std::move(effect));}
        catch(...) {recording.lock();throw;}
        recording.lock();
    }
    Stats stats() {std::lock_guard lock(mutex_);return stats_;}
    void stop() noexcept {
        {
            std::lock_guard lock(mutex_);stopped_=true;
        }
        work_.notify_all();changed_.notify_all();
        if(thread_.joinable())thread_.join();
        owner_effect_={};
    }
private:
    void check() const {
        if(error_)std::rethrow_exception(error_);
        if(stopped_)throw std::runtime_error("Native FIFO worker stopped");
    }
    void run() noexcept {
        // Swap capacities back into the handoff, as in the donor, rather than
        // allocate/free one buffer per batch. Parse outside the queue lock.
        std::vector<uint8_t> batch;
        try {
            for(;;) {
                bool flush;uint64_t target;
                {
                    std::unique_lock lock(mutex_);
                    work_.wait(lock,[&]{return stopped_ || appended_!=parsed_;});
                    if(stopped_)return;
                    batch.clear();batch.swap(handoff_);flush=flush_;flush_=false;target=appended_;
                }
                consume_(batch,flush);
                {
                    std::lock_guard lock(mutex_);
                    parsed_=target;++stats_.batches;stats_.bytes+=batch.size();
                }
                changed_.notify_all();
            }
        }catch(...) {
            std::lock_guard lock(mutex_);if(!error_)error_=std::current_exception();
            changed_.notify_all();
        }
    }
    Consumer consume_;
    std::mutex mutex_;
    std::condition_variable work_,changed_;
    std::vector<uint8_t> handoff_;
    std::function<void()> owner_effect_;
    uint64_t appended_{},parsed_{};
    bool flush_{},owner_done_{},stopped_{};
    std::exception_ptr error_;
    Stats stats_;
    // Must be last: run() can start before this constructor returns.
    std::thread thread_;
};
}
