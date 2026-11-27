/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Math.h>
#include <AK/Platform.h>
#include <Compositor/VSyncScheduler.h>
#include <LibCore/Timer.h>

#if defined(AK_OS_MACOS)
#    include <AK/Atomic.h>
#    include <AK/AtomicRefCounted.h>
#    include <AK/NonnullRefPtr.h>
#    include <AK/RefPtr.h>
#    include <CoreGraphics/CoreGraphics.h>
#    include <CoreVideo/CoreVideo.h>
#    include <LibCore/EventLoop.h>
#elif defined(AK_OS_ANDROID)
#    include <AK/Atomic.h>
#    include <AK/AtomicRefCounted.h>
#    include <AK/NonnullRefPtr.h>
#    include <LibCore/EventLoop.h>
#    include <android/choreographer.h>
#    include <android/looper.h>
#    include <pthread.h>
#endif

namespace Compositor {

class TimerVSyncScheduler final : public VSyncScheduler {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit TimerVSyncScheduler(Function<void(MonotonicTime)>&& tick_callback)
        : m_tick_callback(move(tick_callback))
        , m_timer(Core::Timer::create_single_shot(AK::Duration::from_milliseconds(0), [this] {
            fire();
        }))
    {
    }

    virtual ~TimerVSyncScheduler() override
    {
        m_timer->on_timeout = {};
        m_timer->stop();
    }

    virtual void schedule(double refresh_rate) override
    {
        VERIFY(refresh_rate == refresh_rate);
        VERIFY(refresh_rate > 0);
        VERIFY(refresh_rate < AK::Infinity<double>);

        auto refresh_rate_changed = m_refresh_rate != refresh_rate;
        m_refresh_rate = refresh_rate;
        auto now = MonotonicTime::now();
        if (!m_next_tick_time.has_value() || refresh_rate_changed)
            m_next_tick_time = now + frame_interval();
        else
            advance_next_tick_time_past(now);

        if (m_timer->is_active() && !refresh_rate_changed)
            return;

        arm_timer(now);
    }

    virtual Optional<MonotonicTime> most_recent_tick_time(MonotonicTime now, double refresh_rate) override
    {
        if (!m_next_tick_time.has_value() || m_refresh_rate != refresh_rate)
            return {};

        advance_next_tick_time_past(now);
        return *m_next_tick_time - frame_interval();
    }

private:
    void fire()
    {
        auto now = MonotonicTime::now();
        advance_next_tick_time_past(now);
        auto frame_time = *m_next_tick_time - frame_interval();
        m_tick_callback(frame_time);
    }

    AK::Duration frame_interval() const
    {
        return AK::Duration::from_nanoseconds(static_cast<i64>((1'000'000'000.0 / m_refresh_rate) + 0.5));
    }

    void advance_next_tick_time_past(MonotonicTime now)
    {
        VERIFY(m_next_tick_time.has_value());
        if (*m_next_tick_time > now)
            return;

        auto interval = frame_interval();
        auto intervals_to_advance = (now - *m_next_tick_time).to_nanoseconds() / interval.to_nanoseconds() + 1;
        *m_next_tick_time += AK::Duration::from_nanoseconds(intervals_to_advance * interval.to_nanoseconds());
    }

    void arm_timer(MonotonicTime now)
    {
        VERIFY(m_next_tick_time.has_value());
        advance_next_tick_time_past(now);
        VERIFY(*m_next_tick_time > now);

        // NB: The timer is armed against a fixed display grid. Advancing by whole intervals keeps request and update
        //     latency from shifting later ticks.
        m_timer->restart(*m_next_tick_time - now);
    }

    Function<void(MonotonicTime)> m_tick_callback;
    double m_refresh_rate { 60.0 };
    RefPtr<Core::Timer> m_timer;
    Optional<MonotonicTime> m_next_tick_time;
};

#if defined(AK_OS_MACOS)

static constexpr int display_link_idle_stop_delay_ms = 250;

static CVReturn display_link_callback(CVDisplayLinkRef, CVTimeStamp const*, CVTimeStamp const*, CVOptionFlags, CVOptionFlags*, void* context);

class DisplayLinkState final : public AtomicRefCounted<DisplayLinkState> {
public:
    static RefPtr<DisplayLinkState> create(u64 display_id, Core::EventLoop& event_loop, Function<void(MonotonicTime)>&& tick_callback)
    {
        CVDisplayLinkRef display_link = nullptr;

#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
        auto result = CVDisplayLinkCreateWithCGDisplay(static_cast<CGDirectDisplayID>(display_id), &display_link);
#    pragma clang diagnostic pop

        if (result != kCVReturnSuccess || !display_link)
            return nullptr;

        auto state = adopt_ref(*new DisplayLinkState(event_loop, display_link));

#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
        result = CVDisplayLinkSetOutputCallback(display_link, display_link_callback, state.ptr());
#    pragma clang diagnostic pop

        if (result != kCVReturnSuccess)
            return nullptr;

        state->tick_callback = move(tick_callback);
        return state;
    }

    ~DisplayLinkState()
    {
        stop_display_link();
    }

    bool request_tick();
    void stop_display_link();
    void schedule_display_link_stop_if_idle();
    void stop_display_link_if_idle();
    bool consume_tick_request();
    bool is_valid() const;
    bool is_running() const;

    Core::EventLoop& event_loop;
    Function<void(MonotonicTime)> tick_callback;
    Atomic<bool> invalidated { false };
    Atomic<bool> tick_requested { false };
    Atomic<i64> most_recent_tick_time_nanoseconds { 0 };
    CVDisplayLinkRef display_link { nullptr };
    NonnullRefPtr<Core::Timer> idle_stop_timer;

private:
    DisplayLinkState(Core::EventLoop& event_loop, CVDisplayLinkRef display_link)
        : event_loop(event_loop)
        , display_link(display_link)
        , idle_stop_timer(Core::Timer::create_single_shot(AK::Duration::from_milliseconds(display_link_idle_stop_delay_ms), [this] {
            stop_display_link_if_idle();
        }))
    {
    }
};

bool DisplayLinkState::request_tick()
{
    if (invalidated.load() || !display_link)
        return false;

    tick_requested.store(true);
    idle_stop_timer->stop();

#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
    auto is_running = CVDisplayLinkIsRunning(display_link);
#    pragma clang diagnostic pop

    if (is_running)
        return true;

#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
    auto result = CVDisplayLinkStart(display_link);
#    pragma clang diagnostic pop

    if (result == kCVReturnSuccess || result == kCVReturnDisplayLinkAlreadyRunning)
        return true;

    tick_requested.store(false);
    return false;
}

void DisplayLinkState::stop_display_link()
{
    invalidated.store(true);
    tick_requested.store(false);
    idle_stop_timer->on_timeout = {};
    idle_stop_timer->stop();

    if (!display_link)
        return;

#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CVDisplayLinkStop(display_link);
#    pragma clang diagnostic pop
    CVDisplayLinkRelease(display_link);
    display_link = nullptr;
}

void DisplayLinkState::schedule_display_link_stop_if_idle()
{
    if (invalidated.load() || tick_requested.load() || !display_link)
        return;

    idle_stop_timer->restart(AK::Duration::from_milliseconds(display_link_idle_stop_delay_ms));
}

void DisplayLinkState::stop_display_link_if_idle()
{
    if (invalidated.load() || tick_requested.load() || !display_link)
        return;

#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CVDisplayLinkStop(display_link);
#    pragma clang diagnostic pop
}

bool DisplayLinkState::consume_tick_request()
{
    if (invalidated.load())
        return false;

    if (!tick_requested.exchange(false))
        return false;

    return true;
}

bool DisplayLinkState::is_valid() const
{
    return !invalidated.load();
}

bool DisplayLinkState::is_running() const
{
    if (invalidated.load() || !display_link)
        return false;

#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
    auto running = CVDisplayLinkIsRunning(display_link);
#    pragma clang diagnostic pop
    return running;
}

class CVDisplayLinkVSyncScheduler final : public VSyncScheduler {
public:
    AK_ALLOC_WITH_KMALLOC;

    static OwnPtr<CVDisplayLinkVSyncScheduler> try_create(u64 display_id, Function<void(MonotonicTime)>&& tick_callback)
    {
        auto state = DisplayLinkState::create(display_id, Core::EventLoop::current(), move(tick_callback));
        if (!state)
            return nullptr;
        return adopt_own(*new CVDisplayLinkVSyncScheduler(state.release_nonnull()));
    }

    explicit CVDisplayLinkVSyncScheduler(NonnullRefPtr<DisplayLinkState> state)
        : m_state(move(state))
    {
    }

    virtual ~CVDisplayLinkVSyncScheduler() override
    {
        m_state->stop_display_link();
    }

    virtual void schedule(double) override
    {
        m_state->request_tick();
    }

    virtual Optional<MonotonicTime> most_recent_tick_time(MonotonicTime now, double refresh_rate) override
    {
        if (!m_state->is_running())
            return {};

        auto tick_time_nanoseconds = m_state->most_recent_tick_time_nanoseconds.load();
        if (tick_time_nanoseconds == 0 || tick_time_nanoseconds > now.nanoseconds())
            return {};

        auto age_nanoseconds = now.nanoseconds() - tick_time_nanoseconds;
        auto frame_interval_nanoseconds = static_cast<i64>((1'000'000'000.0 / refresh_rate) + 0.5);
        if (age_nanoseconds >= frame_interval_nanoseconds)
            return {};
        return now - AK::Duration::from_nanoseconds(age_nanoseconds);
    }

private:
    NonnullRefPtr<DisplayLinkState> m_state;
};

static CVReturn display_link_callback(CVDisplayLinkRef, CVTimeStamp const*, CVTimeStamp const*, CVOptionFlags, CVOptionFlags*, void* context)
{
    auto state = NonnullRefPtr { *static_cast<DisplayLinkState*>(context) };
    auto frame_time = MonotonicTime::now();
    state->most_recent_tick_time_nanoseconds.store(frame_time.nanoseconds());
    if (!state->consume_tick_request())
        return kCVReturnSuccess;

    // NB: Sample the shared monotonic clock on the display-link thread so deferred delivery does not shift the
    //     frame timestamp to the Compositor event-loop wakeup time.
    state->event_loop.deferred_invoke([state = move(state), frame_time] {
        if (!state->is_valid())
            return;
        state->tick_callback(frame_time);
        state->schedule_display_link_stop_if_idle();
    });
    return kCVReturnSuccess;
}

OwnPtr<VSyncScheduler> create_vsync_scheduler(Optional<u64> display_id, Function<void(MonotonicTime)>&& tick_callback)
{
    if (display_id.has_value()) {
        if (auto scheduler = CVDisplayLinkVSyncScheduler::try_create(*display_id, move(tick_callback)); scheduler)
            return scheduler;
    }

    return make<TimerVSyncScheduler>(move(tick_callback));
}

#elif defined(AK_OS_ANDROID)

// How long the screen's refreshes are followed after a tick was last asked for. A tick asked for just after a refresh
// can still be given that one, so long as it was seen.
static constexpr i64 choreographer_idle_stop_delay_nanoseconds = 250'000'000;
// How long a tick may be overdue before a timer gives it instead.
static constexpr int choreographer_overdue_delay_ms = 100;
// How long the first refresh may take to arrive before the choreographer is given up on.
static constexpr int choreographer_first_tick_delay_ms = 500;
// No screen refreshes faster than this. An embedder that asks for more wants frames made at that rate, seen or not, as
// to measure how fast a page can be drawn.
static constexpr double fastest_screen_refresh_rate = 240.0;

// Android's choreographer, which reports the screen's refreshes to a looper, and so has a thread of its own to run one
// on. A frame made on a clock that is not the screen's cannot keep step with it: now and then two are made for one
// refresh and none for the next.
class ChoreographerThread final : public AtomicRefCounted<ChoreographerThread> {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ChoreographerThread(Core::EventLoop& event_loop)
        : m_event_loop(event_loop)
    {
    }

    // Called on the compositor's thread, as the rest of what is public is.
    bool start()
    {
        // The thread holds a reference of its own until it ends.
        ref();
        pthread_t thread;
        auto result = pthread_create(
            &thread, nullptr, [](void* self) -> void* {
                static_cast<ChoreographerThread*>(self)->run();
                static_cast<ChoreographerThread*>(self)->unref();
                return nullptr;
            },
            this);
        if (result != 0) {
            unref();
            return false;
        }
        pthread_detach(thread);
        return true;
    }

    void stop()
    {
        m_stopped.store(true);
        wake();
    }

    void request_tick()
    {
        m_last_request_time_nanoseconds.store(MonotonicTime::now().nanoseconds());
        m_tick_requested.store(true);
        wake();
    }

    i64 most_recent_tick_time_nanoseconds() const { return m_most_recent_tick_time_nanoseconds.load(); }

    Function<void(MonotonicTime)> on_tick;

private:
    // NB: There is no looper to wake before the thread has made one, and it looks for what was asked of it once it has.
    void wake()
    {
        if (auto* looper = m_looper.load())
            ALooper_wake(looper);
    }

    void run()
    {
        auto* looper = ALooper_prepare(0);
        m_choreographer = AChoreographer_getInstance();
        if (!m_choreographer) {
            dbgln("VSyncScheduler: this process has no choreographer");
            return;
        }
        // Its being there says that the screen's refreshes are what pace frames (`ps -T`).
        pthread_setname_np(pthread_self(), "VSyncChoreo");
        // Never released, so that it can be woken for as long as this object lives.
        ALooper_acquire(looper);
        m_looper.store(looper);

        while (!m_stopped.load()) {
            post_frame_callback_if_wanted();
            ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
        }
    }

    // On the choreographer's thread, which alone calls into the choreographer.
    void post_frame_callback_if_wanted()
    {
        if (m_frame_callback_posted)
            return;
        auto since_last_request = MonotonicTime::now().nanoseconds() - m_last_request_time_nanoseconds.load();
        if (!m_tick_requested.load() && since_last_request >= choreographer_idle_stop_delay_nanoseconds)
            return;
        m_frame_callback_posted = true;
        AChoreographer_postFrameCallback64(
            m_choreographer, [](int64_t, void* self) { static_cast<ChoreographerThread*>(self)->did_refresh(); }, this);
    }

    void did_refresh()
    {
        m_frame_callback_posted = false;
        // NB: Sampled here, so that delivery to the Compositor's event loop does not shift the frame's time.
        auto frame_time = MonotonicTime::now();
        m_most_recent_tick_time_nanoseconds.store(frame_time.nanoseconds());
        if (m_tick_requested.exchange(false)) {
            m_event_loop.deferred_invoke([self = NonnullRefPtr { *this }, frame_time] {
                if (self->on_tick)
                    self->on_tick(frame_time);
            });
        }
        post_frame_callback_if_wanted();
    }

    Core::EventLoop& m_event_loop;
    Atomic<ALooper*> m_looper { nullptr };
    Atomic<bool> m_stopped { false };
    Atomic<bool> m_tick_requested { false };
    Atomic<i64> m_last_request_time_nanoseconds { 0 };
    Atomic<i64> m_most_recent_tick_time_nanoseconds { 0 };

    // The choreographer's thread's own.
    AChoreographer* m_choreographer { nullptr };
    bool m_frame_callback_posted { false };
};

class ChoreographerVSyncScheduler final : public VSyncScheduler {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ChoreographerVSyncScheduler(Function<void(MonotonicTime)>&& tick_callback)
        : m_tick_callback(move(tick_callback))
        , m_overdue_timer(Core::Timer::create_single_shot(AK::Duration::from_milliseconds(choreographer_overdue_delay_ms), [this] {
            tick_is_overdue();
        }))
        , m_thread(adopt_ref(*new ChoreographerThread(Core::EventLoop::current())))
    {
        m_thread->on_tick = [this](MonotonicTime frame_time) {
            if (m_fallback)
                return;
            m_overdue_timer->stop();
            m_tick_callback(frame_time);
        };
        if (!m_thread->start())
            use_timer_instead();
    }

    virtual ~ChoreographerVSyncScheduler() override
    {
        m_overdue_timer->on_timeout = {};
        m_overdue_timer->stop();
        m_thread->on_tick = {};
        m_thread->stop();
    }

    virtual void schedule(double refresh_rate) override
    {
        m_refresh_rate = refresh_rate;
        if (!m_fallback && refresh_rate > fastest_screen_refresh_rate)
            use_timer_instead();
        if (m_fallback) {
            m_fallback->schedule(refresh_rate);
            return;
        }
        m_thread->request_tick();
        if (!m_overdue_timer->is_active()) {
            auto has_ticked = m_thread->most_recent_tick_time_nanoseconds() != 0;
            m_overdue_timer->restart(AK::Duration::from_milliseconds(has_ticked ? choreographer_overdue_delay_ms : choreographer_first_tick_delay_ms));
        }
    }

    virtual Optional<MonotonicTime> most_recent_tick_time(MonotonicTime now, double refresh_rate) override
    {
        if (m_fallback)
            return m_fallback->most_recent_tick_time(now, refresh_rate);

        auto tick_time_nanoseconds = m_thread->most_recent_tick_time_nanoseconds();
        if (tick_time_nanoseconds == 0 || tick_time_nanoseconds > now.nanoseconds())
            return {};

        auto age_nanoseconds = now.nanoseconds() - tick_time_nanoseconds;
        auto frame_interval_nanoseconds = static_cast<i64>((1'000'000'000.0 / refresh_rate) + 0.5);
        if (age_nanoseconds >= frame_interval_nanoseconds)
            return {};
        return now - AK::Duration::from_nanoseconds(age_nanoseconds);
    }

private:
    void tick_is_overdue()
    {
        if (m_thread->most_recent_tick_time_nanoseconds() == 0) {
            dbgln("VSyncScheduler: the choreographer reports no refreshes, so frames are paced by a timer");
            use_timer_instead();
            m_fallback->schedule(m_refresh_rate);
            return;
        }
        // The refreshes have stopped, as when the screen is turned off. What waits for a frame must not wait for good.
        m_tick_callback(MonotonicTime::now());
    }

    void use_timer_instead()
    {
        m_overdue_timer->stop();
        m_fallback = make<TimerVSyncScheduler>([this](MonotonicTime frame_time) {
            m_tick_callback(frame_time);
        });
        m_thread->stop();
    }

    Function<void(MonotonicTime)> m_tick_callback;
    double m_refresh_rate { 60.0 };
    NonnullRefPtr<Core::Timer> m_overdue_timer;
    NonnullRefPtr<ChoreographerThread> m_thread;
    OwnPtr<TimerVSyncScheduler> m_fallback;
};

OwnPtr<VSyncScheduler> create_vsync_scheduler(Optional<u64>, Function<void(MonotonicTime)>&& tick_callback)
{
    return make<ChoreographerVSyncScheduler>(move(tick_callback));
}

#else

OwnPtr<VSyncScheduler> create_vsync_scheduler(Optional<u64>, Function<void(MonotonicTime)>&& tick_callback)
{
    return make<TimerVSyncScheduler>(move(tick_callback));
}

#endif

}
