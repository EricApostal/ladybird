/*
 * Copyright (c) 2026, the Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Mutex.h>
#include <AK/ScopeGuard.h>
#include <AK/SourceLocation.h>
#include <AK/Vector.h>
#include <AudioServer/PlatformAudio.h>
#include <AudioServer/PlaybackStreamAAudio.h>
#include <LibCore/ThreadedPromise.h>

#include <aaudio/AAudio.h>

namespace Audio {

static void log_aaudio_error(aaudio_result_t error, SourceLocation location = SourceLocation::current());

#define AAUDIO_TRY(expression)                                                        \
    ({                                                                                \
        /* Ignore -Wshadow to allow nesting the macro. */                             \
        AK_IGNORE_DIAGNOSTIC("-Wshadow", auto&& _temporary_result = (expression));    \
        if (_temporary_result < 0) [[unlikely]] {                                     \
            log_aaudio_error(static_cast<aaudio_result_t>(_temporary_result));        \
            return Error::from_string_literal("AAudio call failed");                  \
        }                                                                             \
        _temporary_result;                                                            \
    })

struct AudioTask {
    enum class Type {
        Play,
        Pause,
        PauseAndDiscard,
        Volume,
    };

    void resolve()
    {
        promise->resolve();
    }

    void reject(Error error)
    {
        promise->reject(move(error));
    }

    Type type;
    NonnullRefPtr<Core::ThreadedPromise<void>> promise;
    Optional<double> data {};
};

// We never call AAudioStreamBuilder_setChannelMask(), so AAudioStream_getChannelMask() (which only reports back a
// mask that was explicitly requested) would never tell us anything useful here, and it additionally requires API
// level 32+ while this app supports a lower minimum. So for anything beyond mono/stereo we don't know the identity
// of each channel and report them as unknown positions instead of guessing.
static ChannelMap channel_map_from_channel_count(i32 channel_count)
{
    if (channel_count == 1)
        return ChannelMap::mono();
    if (channel_count == 2)
        return ChannelMap::stereo();

    Vector<Channel, ChannelMap::capacity()> channels;
    for (auto i = 0; i < channel_count && channels.size() < ChannelMap::capacity(); ++i)
        channels.unchecked_append(Channel::Unknown);

    return ChannelMap(channels);
}

class AudioState : public RefCounted<AudioState> {
public:
    static ErrorOr<NonnullRefPtr<AudioState>> create(PlaybackStream::AudioDataRequestCallback data_request_callback, OutputState initial_output_state)
    {
        auto state = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) AudioState(move(data_request_callback), initial_output_state)));

        AAudioStreamBuilder* builder = nullptr;
        AAUDIO_TRY(AAudio_createStreamBuilder(&builder));
        ScopeGuard delete_builder = [&] { AAudioStreamBuilder_delete(builder); };

        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setDataCallback(builder, &AudioState::on_aaudio_data_callback, state.ptr());
        AAudioStreamBuilder_setErrorCallback(builder, &AudioState::on_aaudio_error_callback, state.ptr());

        AAUDIO_TRY(AAudioStreamBuilder_openStream(builder, &state->m_stream));

        auto channel_count = AAudioStream_getChannelCount(state->m_stream);
        auto sample_rate = AAudioStream_getSampleRate(state->m_stream);
        state->m_sample_specification = SampleSpecification(static_cast<u32>(sample_rate), channel_map_from_channel_count(channel_count));

        AAUDIO_TRY(AAudioStream_requestStart(state->m_stream));

        return state;
    }

    ~AudioState()
    {
        if (m_stream != nullptr) {
            AAudioStream_requestStop(m_stream);
            AAudioStream_close(m_stream);
        }
    }

    void queue_task(AudioTask task)
    {
        MutexLocker lock(m_task_queue_mutex);
        m_task_queue.append(move(task));
        m_task_queue_is_empty = false;
    }

    void notify_data_available()
    {
        m_data_notified = true;
    }

    SampleSpecification const& sample_specification() const { return m_sample_specification; }

private:
    AudioState(PlaybackStream::AudioDataRequestCallback data_request_callback, OutputState initial_output_state)
        : m_paused(initial_output_state == OutputState::Playing ? Paused::No : Paused::Explicit)
        , m_data_request_callback(move(data_request_callback))
    {
    }

    Optional<AudioTask> dequeue_task()
    {
        // OPTIMIZATION: We can avoid taking a lock in the audio callback if there are no queued commands, which will
        //               be the case most of the time.
        if (m_task_queue_is_empty.load())
            return {};

        MutexLocker lock(m_task_queue_mutex);

        m_task_queue_is_empty = m_task_queue.size() == 1;
        return m_task_queue.take_first();
    }

    // When the frames this callback is handed will reach the speaker, so that clients (WebAudio's clock, the mixer's
    // drain timers) can time themselves against playback rather than against this call. AAudio reports the
    // presentation time of the frames it has already played; everything handed over since then, including the buffer
    // being filled now, plays after it.
    MonotonicTime buffer_starts_playing_at(AAudioStream* stream, i32 frames_in_this_buffer) const
    {
        i64 played_frame = 0;
        i64 played_time_ns = 0;
        if (AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &played_frame, &played_time_ns) != AAUDIO_OK)
            return MonotonicTime::now();

        // The frames AAudio has yet to play, counting the ones being buffered now.
        auto frames_ahead = m_frames_written - played_frame + frames_in_this_buffer;
        if (frames_ahead < 0)
            frames_ahead = 0;

        // AAudio reports the same clock AK's MonotonicTime reads, so the two are expressed against one another and
        // the frames still to play are added on top of that.
        auto now = MonotonicTime::now();
        auto played_at = now + AK::Duration::from_nanoseconds(played_time_ns - now.nanoseconds());
        return played_at + AK::Duration::from_time_units(frames_ahead, 1, m_sample_specification.sample_rate());
    }

    // This is invoked on a dedicated realtime audio thread that AAudio manages for us. It must not block, allocate,
    // or otherwise perform any work that could take an unbounded amount of time.
    static aaudio_data_callback_result_t on_aaudio_data_callback(AAudioStream* stream, void* user_data, void* audio_data, i32 num_frames)
    {
        auto& state = *static_cast<AudioState*>(user_data);
        VERIFY(state.m_sample_specification.is_valid());

        if (state.m_paused == Paused::Underrun && state.m_data_notified.exchange(false))
            state.m_paused = Paused::No;

        if (auto task = state.dequeue_task(); task.has_value()) {
            switch (task->type) {
            case AudioTask::Type::Play:
                state.m_paused = Paused::No;
                break;

            case AudioTask::Type::Pause:
            // FIXME: AAudio has no way to synchronously discard audio that has already been handed to the hardware
            //        while the stream keeps running, so the best we can do is stop feeding it new data.
            case AudioTask::Type::PauseAndDiscard:
                state.m_paused = Paused::Explicit;
                break;

            case AudioTask::Type::Volume:
                VERIFY(task->data.has_value());
                state.m_volume = *task->data;
                break;
            }

            task->resolve();
        }

        auto channel_count = state.m_sample_specification.channel_count();
        auto output_buffer = Span<float>(reinterpret_cast<float*>(audio_data), static_cast<size_t>(num_frames) * channel_count);

        if (state.m_paused == Paused::No) {
            auto starts_playing_at = state.buffer_starts_playing_at(stream, num_frames);
            auto written_buffer = state.m_data_request_callback(output_buffer, starts_playing_at);
            state.m_frames_written += static_cast<i64>(written_buffer.size() / channel_count);

            // AAudio has no per-stream volume on every API level we support, and software applies the user's volume
            // to what the page produced either way.
            if (state.m_volume != 1.0) {
                for (auto& sample : output_buffer.trim(written_buffer.size()))
                    sample = static_cast<float>(sample * state.m_volume);
            }

            if (written_buffer.is_empty())
                state.m_paused = Paused::Underrun;
        }

        if (state.m_paused != Paused::No)
            output_buffer.fill(0);

        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    static void on_aaudio_error_callback(AAudioStream*, void*, aaudio_result_t error)
    {
        // FIXME: This fires when the stream is disconnected, e.g. when an audio device is unplugged or the audio route
        //        otherwise changes. AAudio requires the stream to be stopped, closed, and reopened from a thread other
        //        than this one to recover, and doing that work here is not realtime-safe; for now we just stop
        //        producing sound.
        log_aaudio_error(error);
    }

    AAudioStream* m_stream { nullptr };
    SampleSpecification m_sample_specification;

    Mutex m_task_queue_mutex;
    Vector<AudioTask, 4> m_task_queue;
    Atomic<bool> m_task_queue_is_empty { true };

    enum class Paused : u8 {
        No,
        Explicit,
        Underrun,
    };
    Paused m_paused { Paused::Explicit };

    PlaybackStream::AudioDataRequestCallback m_data_request_callback;
    Atomic<bool> m_data_notified { false };
    double m_volume { 1.0 };
    i64 m_frames_written { 0 };
};

NonnullRefPtr<PlaybackStream::CreatePromise> create_platform_playback_stream(OutputState initial_output_state, u32 target_latency_ms, PlaybackStream::AudioDataRequestCallback data_request_callback)
{
    return PlaybackStreamAAudio::create(initial_output_state, target_latency_ms, move(data_request_callback));
}

NonnullRefPtr<PlaybackStream::CreatePromise> PlaybackStreamAAudio::create(OutputState initial_output_state, u32, AudioDataRequestCallback&& data_request_callback)
{
    auto promise = CreatePromise::construct();
    // FIXME: Create the AudioState off this thread. It sets up the audio output synchronously, which can take a
    //        noticeable amount of time under normal circumstances.
    auto state_or_error = AudioState::create(move(data_request_callback), initial_output_state);
    if (state_or_error.is_error()) {
        promise->reject(state_or_error.release_error());
        return promise;
    }
    auto state = state_or_error.release_value();
    auto stream = adopt_ref(*new PlaybackStreamAAudio(move(state)));
    promise->resolve(stream);
    return promise;
}

PlaybackStreamAAudio::PlaybackStreamAAudio(NonnullRefPtr<AudioState> impl)
    : m_state(move(impl))
{
}

PlaybackStreamAAudio::~PlaybackStreamAAudio() = default;

SampleSpecification PlaybackStreamAAudio::sample_specification() const
{
    return m_state->sample_specification();
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamAAudio::resume()
{
    auto promise = Core::ThreadedPromise<void>::create();
    m_state->queue_task({ AudioTask::Type::Play, promise });

    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamAAudio::drain_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    m_state->queue_task({ AudioTask::Type::Pause, promise });

    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamAAudio::discard_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    m_state->queue_task({ AudioTask::Type::PauseAndDiscard, promise });

    return promise;
}

void PlaybackStreamAAudio::notify_data_available()
{
    m_state->notify_data_available();
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamAAudio::set_volume(double volume)
{
    auto promise = Core::ThreadedPromise<void>::create();
    m_state->queue_task({ AudioTask::Type::Volume, promise, volume });

    return promise;
}

void log_aaudio_error([[maybe_unused]] aaudio_result_t error, [[maybe_unused]] SourceLocation location)
{
#if AUDIO_DEBUG
    warnln("{}: AAudio error {}: {}", location, static_cast<int>(error), StringView { AAudio_convertResultToText(error) });
#endif
}

}