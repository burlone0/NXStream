#pragma once
#include <stratosphere.hpp>
#include <atomic>

namespace ams::mitm::applet {

    extern std::atomic<bool> g_audio_enabled;
    extern std::atomic<bool> g_audio_active;
    extern std::atomic<u32>  g_audio_state;

    void StartAudio();

    constexpr size_t AudioRingBytes = 64 * 1024;
    void AudioAttachRing(u8 *ring);
    void AudioDetachRing();
    size_t AudioTake(u8 *dst, size_t max);

    void AudioFlush();

    bool AudioOutWanted();
    void AudioOutFeed(const u8 *pcm, size_t n, u32 channels);

}
