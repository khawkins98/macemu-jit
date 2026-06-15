/*
 *  event_sched.cpp - host event scheduler (see event_sched.h for provenance).
 *  Ported from DingusPPC core/timermanager.cpp
 *  (https://github.com/dingusdev/dingusppc @ 92bb6d10549529f9f4031a85c2bc136149535bdc),
 *  GPL-3.0-or-later. [SS] adaptations: loguru -> fprintf(stderr) in cancel_all_timers;
 *  TimerManager -> EventScheduler; no singleton. Logic otherwise verbatim.
 */

#include "event_sched.h"

#include <cinttypes>
#include <memory>
#include <mutex>
#include <stdio.h>

uint32_t EventScheduler::add_absolute_timer(uint64_t timeout_ns, uint64_t interval, timer_cb cb)
{
    TimerInfo* ti = new TimerInfo;

    ti->id          = ++this->id;
    ti->timeout_ns  = timeout_ns;
    ti->interval_ns = interval;
    ti->cb          = cb;

    std::shared_ptr<TimerInfo> timer_desc(ti);

    // add new timer to the timer queue
    this->timer_queue.push(timer_desc);

    // notify listeners about changes in the timer queue
    if (!this->cb_active) {
        this->notify_timer_changes();
    }

    return ti->id;
}

uint32_t EventScheduler::add_oneshot_timer(uint64_t timeout, timer_cb cb)
{
    return EventScheduler::add_absolute_timer(this->get_time_now() + timeout, 0, cb);
}

uint32_t EventScheduler::add_immediate_timer(timer_cb cb)
{
    return EventScheduler::add_absolute_timer(0, 0, cb);
}

uint32_t EventScheduler::add_cyclic_timer(uint64_t interval, uint64_t delay, timer_cb cb)
{
    return EventScheduler::add_absolute_timer(this->get_time_now() + delay, interval, cb);
}

uint32_t EventScheduler::add_cyclic_timer(uint64_t interval, timer_cb cb)
{
    return this->add_cyclic_timer(interval, interval, cb);
}

void EventScheduler::cancel_timer(uint32_t id)
{
    this->timer_queue.remove_by_id(id);
    if (!this->cb_active) {
        this->notify_timer_changes();
    }
}

// SS_M18 S3 T3: pump-yield flag (NK supervisor owns scheduling). Pure module
// state — default false (byte-identical); flipped only by the gated glue path via
// EventSchedulerYield(). No gate symbol here so the standalone unit tests link
// without ppc-cpu.o.
static bool g_esched_yielded = false;
void EventSchedulerYield(bool on) { g_esched_yielded = on; }

uint64_t EventScheduler::process_timers()
{
    // SS_M18 S3 T3: under the NK supervisor the host pump is quiescent — fire no
    // callbacks, report the idle slice (0). The sched_pump caps a 0 slice to its
    // 10ms idle floor, so this is a quiet re-check, not a busy spin. Default OFF =>
    // byte-identical.
    if (g_esched_yielded)
        return 0ULL;

    std::shared_ptr<TimerInfo> cur_timer;
    uint64_t time_now = get_time_now();

{ // [ mtx scope
    std::lock_guard<std::recursive_mutex> lk(this->timer_queue.get_mtx());
    if (this->timer_queue.empty()) {
        return 0ULL;
    }

    // scan for expired timers
    cur_timer = this->timer_queue.top();
} // ] mtx scope
    while (cur_timer->timeout_ns <= time_now) {
        this->timer_queue.remove_by_id(cur_timer->id);
        uint64_t timeout_ns = cur_timer->timeout_ns;
        timer_cb cb = cur_timer->cb;

        // re-arm cyclic timers
        if (cur_timer->interval_ns) {
            std::lock_guard<std::recursive_mutex> lk(this->timer_queue.get_mtx());
            uint64_t timeout_ns_new = timeout_ns + cur_timer->interval_ns;
            if (timeout_ns_new <= time_now)
                timeout_ns_new = time_now + cur_timer->interval_ns;
            cur_timer->timeout_ns = timeout_ns_new;
            this->timer_queue.push(cur_timer);
        }

        this->cb_active = true;

        // invoke timer callback (queue mutex NOT held — M2 lock-order rule)
        cb();

        this->cb_active = false;

        // process next timer
{ // [ mtx scope
        std::lock_guard<std::recursive_mutex> lk(this->timer_queue.get_mtx());
        if (this->timer_queue.empty()) {
            return 0ULL;
        }

        cur_timer = this->timer_queue.top();
} // ] mtx scope
    }

    // return time slice in nanoseconds until next timer's expiry
    return cur_timer->timeout_ns - time_now;
}

void EventScheduler::cancel_all_timers()
{
    std::shared_ptr<TimerInfo> cur_timer;
    while (!this->timer_queue.empty()) {
        cur_timer = this->timer_queue.top();
        // [SS] was LOG_F(WARNING, ...)
        fprintf(stderr, "[ESCHED] Canceling timer id:%u ns:%llu\n",
                cur_timer->id, (unsigned long long)cur_timer->timeout_ns);
        this->timer_queue.pop();
    }
}
