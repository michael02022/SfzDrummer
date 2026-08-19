#include "SfizzEngine.h"

#include <sfizz.h>

#include <algorithm>

SfizzEngine::SfizzEngine() : synth(sfizz_create_synth()) {}

SfizzEngine::~SfizzEngine() { sfizz_free(synth); }

void SfizzEngine::prepare(double sampleRate, int maxBlockSize) {
    sfizz_set_sample_rate(synth, static_cast<float>(sampleRate));
    sfizz_set_samples_per_block(synth, maxBlockSize);
}

bool SfizzEngine::loadSfzString(const std::string& path, const std::string& sfzText) {
    std::lock_guard<std::mutex> lock(reloadMutex_);
    return sfizz_load_string(synth, path.c_str(), sfzText.c_str());
}

bool SfizzEngine::loadScalaFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(reloadMutex_);
    return sfizz_load_scala_file(synth, path.c_str());
}

bool SfizzEngine::loadScalaString(const std::string& text) {
    std::lock_guard<std::mutex> lock(reloadMutex_);
    return sfizz_load_scala_string(synth, text.c_str());
}

void SfizzEngine::renderBlock(const MidiEvent* events, int numEvents, float** channels,
                              int numChannels, int numFrames, bool mpeEnabled,
                              float tuningFrequency) {
    std::unique_lock<std::mutex> lock(reloadMutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        for (int c = 0; c < numChannels; ++c)
            if (channels[c]) std::fill(channels[c], channels[c] + numFrames, 0.0f);
        return;
    }

    if (mpeEnabled != mpeEnabledApplied_) {
        sfizz_set_mpe_enabled(synth, mpeEnabled);
        mpeEnabledApplied_ = mpeEnabled;
    }
    if (tuningFrequency != tuningFrequencyApplied_) {
        sfizz_set_tuning_frequency(synth, tuningFrequency);
        tuningFrequencyApplied_ = tuningFrequency;
    }

    for (int i = 0; i < numEvents; ++i) {
        const MidiEvent& e = events[i];
        switch (e.type) {
            case MidiEvent::Type::NoteOn:
                sfizz_send_note_on_channel(synth, e.delaySamples, e.channel, e.noteNumber,
                                          e.velocity);
                break;
            case MidiEvent::Type::NoteOff:
                sfizz_send_note_off_channel(synth, e.delaySamples, e.channel, e.noteNumber,
                                           e.velocity);
                break;
            case MidiEvent::Type::CC:
                sfizz_send_cc_channel(synth, e.delaySamples, e.channel, e.ccNumber, e.ccValue);
                break;
            case MidiEvent::Type::PitchWheel:
                sfizz_send_pitch_wheel_channel(synth, e.delaySamples, e.channel, e.pitch);
                break;
        }
    }

    sfizz_render_block(synth, channels, numChannels, numFrames);
    activeVoices_.store(sfizz_get_num_active_voices(synth), std::memory_order_relaxed);
}
