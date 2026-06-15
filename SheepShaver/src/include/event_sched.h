/*
 *  event_sched.h - Machine Layer M2 host event scheduler (MACHINE-LAYER-PLAN §2c).
 *
 *  Ported from DingusPPC core/timermanager.h
 *  (https://github.com/dingusdev/dingusppc @ 92bb6d10549529f9f4031a85c2bc136149535bdc),
 *  GPL-3.0-or-later. Combined work is GPLv3 (DINGUSPPC-EVALUATION-PLAN.md).
 *  Adaptations for SheepShaver (marked [SS] below): class renamed TimerManager ->
 *  EventScheduler; singleton get_instance() removed (the machine layer owns the
 *  instance); loguru removed; unused NS_PER_* constants dropped with their
 *  values inlined into the USECS/MSECS_TO_NSECS macros. Queue, ordering, re-arm
 *  and callback semantics are preserved verbatim. Time unit: nanoseconds from
 *  the injected source.
 *
 *  DingusPPC - The Experimental PowerPC Macintosh emulator
 *  Copyright (C) 2018-26 The DingusPPC Development Team (see their CREDITS.MD)
 *  This program is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free Software
 *  Foundation, either version 3 of the License, or (at your option) any later
 *  version. Distributed WITHOUT ANY WARRANTY; see <https://www.gnu.org/licenses/>.
 */

#ifndef EVENT_SCHED_H
#define EVENT_SCHED_H

#include <atomic>
#include <algorithm>
#include <cinttypes>
#include <functional>
#include <memory>
#include <queue>
#include <vector>
#include <mutex>

#define USECS_TO_NSECS(us) (us) * 1000
#define MSECS_TO_NSECS(ms) (ms) * 1000000

typedef std::function<void()> timer_cb;
typedef std::function<void()> notify_changes_cb;

/** Extend std::priority_queue as suggested here:
    https://stackoverflow.com/a/36711682
    to be able to remove arbitrary elements.  (verbatim from the donor) */
template <typename T, class Container = std::vector<T>, class Compare = std::less<typename Container::value_type>>
class my_priority_queue : public std::priority_queue<T, Container, Compare> {
public:
    bool remove_by_id(const uint32_t id) {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        if (this->empty())
            return false;
        auto el = this->top();
        if (el->id == id) {
            std::priority_queue<T, Container, Compare>::pop();
            return true;
        }
        auto it = std::find_if(
            this->c.begin(), this->c.end(), [id](const T& el) { return el->id == id; });
        if (it != this->c.end()) {
            this->c.erase(it);
            std::make_heap(this->c.begin(), this->c.end(), this->comp);
            return true;
        }
        return false;
    }

    void push(T val) {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        std::priority_queue<T, Container, Compare>::push(val);
    }

    T pop() {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        T val = std::priority_queue<T, Container, Compare>::top();
        std::priority_queue<T, Container, Compare>::pop();
        return val;
    }

    std::recursive_mutex& get_mtx() { return mtx; }

private:
    std::recursive_mutex mtx;
};

typedef struct TimerInfo {
    uint32_t id;
    uint64_t timeout_ns;  // timer expiry
    uint64_t interval_ns; // 0 for one-shot timers
    timer_cb cb;          // timer callback
} TimerInfo;

// Custom comparator for sorting our timer queue in ascending order (verbatim)
class MyGtComparator {
public:
    bool operator()(const std::shared_ptr<TimerInfo>& l, const std::shared_ptr<TimerInfo>& r) {
        return l.get()->timeout_ns > r.get()->timeout_ns ||
            (l.get()->timeout_ns == r.get()->timeout_ns && l.get()->id > r.get()->id);
    }
};

class EventScheduler {   // [SS] was TimerManager; singleton removed
public:
    EventScheduler() {}

    // callback for retrieving current time
    void set_time_now_cb(const std::function<uint64_t()> &cb) { this->get_time_now = cb; }

    // callback for acknowledging changes in the timer queue (pump wake-up)
    void set_notify_changes_cb(const notify_changes_cb &cb) { this->notify_timer_changes = cb; }

    // return current virtual time in nanoseconds
    uint64_t current_time_ns() { return get_time_now(); }

    // creating and cancelling timers
    uint32_t add_absolute_timer(uint64_t timeout_ns, uint64_t interval, timer_cb cb);
    uint32_t add_oneshot_timer(uint64_t timeout, timer_cb cb);
    uint32_t add_immediate_timer(timer_cb cb);
    uint32_t add_cyclic_timer(uint64_t interval, timer_cb cb);
    uint32_t add_cyclic_timer(uint64_t interval, uint64_t delay, timer_cb cb);
    void cancel_timer(uint32_t id);
    void cancel_all_timers();

    // Fire all expired timers; return ns until the next expiry (0 = queue empty).
    // Contract (donor-preserved): the queue mutex is NEVER held while a callback
    // runs -> callbacks may take device/region locks (M2 lock-order rule).
    // Donor quirk (rev 2 finding S10, preserved): cancel_timer() does NOT
    // guarantee the callback won't run one last time (cancel can race the
    // top-of-queue snapshot here). Consumers must be generation-guarded - the
    // VIA and DEC consumers are; future consumers must not trust cancel alone.
    uint64_t process_timers();

private:
    my_priority_queue<std::shared_ptr<TimerInfo>, std::vector<std::shared_ptr<TimerInfo>>, MyGtComparator> timer_queue;

    std::function<uint64_t()>   get_time_now;
    std::function<void()>       notify_timer_changes;

    std::atomic<uint32_t> id{0};

    // FIXME: Do we need this? It gets written in main thread and read in audio thread.
    // [SS note, rev 2 finding S6: donor FIXME kept verbatim. Here it is written by
    // the pump thread and read by the CPU thread; the worst case is a skipped
    // notify (missed kick), bounded by the pump's 10ms cap. Not a correctness gate.]
    bool cb_active = false; // true if a timer callback is executing
};

// SS_M18 S3 T3 (Operation NewSheep): YIELD the host event-scheduler pump. When
// yielded, process_timers() runs no callbacks and returns the idle slice (0) — the
// NK's own DEC loop (0x50313200) is the live rescheduler. Default OFF (false) =>
// byte-identical. Set ONLY by the gated glue retirement path
// (NkSupervisorEnabled()); the pure module carries no gate symbol so the standalone
// event_sched / via6522 unit tests link without ppc-cpu.o.
extern void EventSchedulerYield(bool on);

#endif // EVENT_SCHED_H
