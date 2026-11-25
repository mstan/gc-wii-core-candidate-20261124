// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gcnllr::present_trace {
enum class Lane { render, source };
enum class Event {
    scanout_ready, scanout_queued, frame_slot_ready, present_selected,
    encoder_begin, callback_begin, acquire_begin, acquire_end, finish_begin,
    submit_begin, submit_end, swap_begin, swap_end, present_complete, callback_end,
};
struct Sample {
    std::uint64_t ns;
    std::uint64_t cycles;
    std::uint64_t frame;
    std::int64_t detail;
    Event event;
};
struct Ring {
    static constexpr std::size_t capacity = 131072;
    std::unique_ptr<Sample[]> samples;
    std::uint64_t count = 0;
    unsigned thread_id = 0;
};
// The VI producer and Aurora worker each write their own ring.
// GX start and stop run outside both lanes' recording lifetimes.
inline Ring render;
inline Ring source;
inline const char* output = nullptr;

using TextLine = std::array<char, 224>;
inline constexpr std::size_t text_capacity = 131072;
inline std::unique_ptr<TextLine[]> text_lines;
inline std::atomic<std::size_t> text_count{};

inline void start() noexcept {
    if (const auto* enabled = std::getenv("DOL_AURORA_PRESENT_LOG"); enabled && *enabled == '1')
        text_lines.reset(new (std::nothrow) TextLine[text_capacity]);

    output = std::getenv("GCNLLR_GX_HITCH_TRACE");
    if (!output || !*output)
        return;
    render.samples.reset(new (std::nothrow) Sample[Ring::capacity]);
    source.samples.reset(new (std::nothrow) Sample[Ring::capacity]);
    if (!render.samples || !source.samples) {
        render = {};
        source = {};
        output = nullptr;
    }
}

// stderr may be a console, pipe, or a file on a busy volume. Format the
// optional diagnostic lines in memory and write them only after GX stops.
template <typename... Args>
inline void record_text(const char* format, Args... args) noexcept {
    if (!text_lines)
        return;
    const auto index = text_count.fetch_add(1, std::memory_order_relaxed);
    if (index >= text_capacity)
        return;
    auto& line = text_lines[index];
    const int size = std::snprintf(line.data(), line.size(), format, args...);
    if (size < 0)
        line[0] = '\0';
    else if (static_cast<std::size_t>(size) >= line.size()) {
        line[line.size() - 2] = '\n';
        line.back() = '\0';
    }
}

inline void mark(Lane lane, Event event, std::uint64_t frame = 0, std::int64_t detail = 0) noexcept {
    auto& ring = lane == Lane::render ? render : source;
    if (!ring.samples)
        return;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    std::uint64_t cycles = 0;
#ifdef _WIN32
    if (!ring.thread_id)
        ring.thread_id = GetCurrentThreadId();
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
#endif
    ring.samples[ring.count++ % Ring::capacity] = {
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()),
        cycles, frame, detail, event};
}

inline void finish() noexcept {
    if (text_lines) {
        const auto recorded = text_count.exchange(0, std::memory_order_relaxed);
        for (std::size_t i = 0; i < recorded && i < text_capacity; ++i)
            std::fputs(text_lines[i].data(), stderr);
        if (recorded > text_capacity)
            std::fprintf(stderr, "[present-trace] dropped %zu text records\n", recorded - text_capacity);
        text_lines.reset();
    }
    if (output && *output) {
        if (auto* file = std::fopen(output, "w")) {
            std::fputs("lane,thread,event,ns,cycles,frame,detail\n", file);
            const auto write = [file](const char* lane, const Ring& ring) {
                const auto first = ring.count > Ring::capacity ? ring.count - Ring::capacity : 0;
                for (auto i = first; i < ring.count; ++i) {
                    const auto& s = ring.samples[i % Ring::capacity];
                    std::fprintf(file, "%s,%u,%u,%llu,%llu,%llu,%lld\n", lane, ring.thread_id,
                        static_cast<unsigned>(s.event), static_cast<unsigned long long>(s.ns),
                        static_cast<unsigned long long>(s.cycles), static_cast<unsigned long long>(s.frame),
                        static_cast<long long>(s.detail));
                }
            };
            write("render", render);
            write("source", source);
            std::fclose(file);
        }
    }
    render = {};
    source = {};
    output = nullptr;
}
}
