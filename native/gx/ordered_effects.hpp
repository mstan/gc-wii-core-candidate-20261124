// SPDX-License-Identifier: MIT
#pragma once
#include <deque>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <utility>

namespace gcnllr {
// FIFO-thread queue. Readbacks may finish out of order, but guest RAM effects
// and PE tokens/interrupts retire strictly in order. Owner-thread publication
// belongs in commit; a throwing commit remains queued and stops later effects.
class OrderedEffects {
    struct Entry {std::function<bool()> ready;std::function<void()> commit;};
    std::deque<Entry> entries_;
public:
    void push(std::function<bool()> ready,std::function<void()> commit) {
        if(!commit)throw std::invalid_argument("Missing ordered effect");
        entries_.push_back({std::move(ready),std::move(commit)});
    }
    std::size_t size() const {return entries_.size();}
    template<class Progress> void drain(bool wait,Progress progress) {
        if(entries_.empty())return;
        // Blocking progress submits and waits in one worker handoff. Avoid a
        // preliminary synchronous poll, and avoid any handoff for ready work.
        if(!wait && entries_.front().ready && !entries_.front().ready())progress(false);
        while(!entries_.empty()) {
            auto& e=entries_.front();
            if(e.ready && !e.ready()) {
                if(!wait)return;
                progress(true); // Provider owns the bounded completion wait.
                if(!e.ready())throw std::runtime_error("Ordered GPU effect did not complete");
            }
            e.commit();entries_.pop_front();
        }
    }
};
}
