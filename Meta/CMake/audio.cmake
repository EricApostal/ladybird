include_guard()

# Audio backend -- how we output audio to the speakers.
if (ANDROID)
    # AAudio is the NDK's own low-latency output, and the only backend available to us on Android.
    set(LADYBIRD_AUDIO_BACKEND "AAUDIO")
    return()
elseif (APPLE AND NOT IOS)
    set(LADYBIRD_AUDIO_BACKEND "AUDIO_UNIT")
    return()
elseif (NOT WIN32)
    pkg_check_modules(PULSEAUDIO IMPORTED_TARGET libpulse)

    if (PULSEAUDIO_FOUND)
        set(LADYBIRD_AUDIO_BACKEND "PULSE")
        return()
    endif()
else()
    set(LADYBIRD_AUDIO_BACKEND "WASAPI")
    return()
endif()

message(WARNING "No audio backend available")
