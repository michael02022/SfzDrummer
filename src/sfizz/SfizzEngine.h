#pragma once

#include <atomic>
#include <mutex>
#include <string>

struct sfizz_synth_t;

// Thin RAII wrapper around the sfizz C API. The SFZ text handed to
// loadSfzString() is generated in memory (never written to disk); the sfizz
// "path" argument only anchors the relative `sample=` opcodes to a root
// folder.
//
// Thread-safety: sfizz_load_string is CT+OFF (per sfizz.h - must run on a
// control thread, and no thread may be inside an RT-tagged call while it
// runs); sfizz_send_*/sfizz_render_block are RT. loadSfzString() takes a
// blocking lock; renderBlock() takes the same lock with try_lock and skips
// rendering (silence) on failure instead of blocking - so the audio thread
// never blocks, and no RT call ever overlaps a reload.
class SfizzEngine {
public:
    SfizzEngine();
    ~SfizzEngine();

    void prepare(double sampleRate, int maxBlockSize);

    // Call only from a non-audio thread.
    bool loadSfzString(const std::string& path, const std::string& sfzText);

    // Scala (.scl) tuning - CT+OFF per sfizz.h, same threading class as
    // loadSfzString (blocking lock, call only from a non-audio thread).
    // Tuning is a synth-level setting, not an SFZ opcode/part of the text
    // loadSfzString() loads - callers that want a previously-loaded scala
    // file/string to survive an SFZ reload must re-apply it themselves
    // afterward (see plugin.cpp's regenerateAndLoadSfz).
    bool loadScalaFile(const std::string& path);
    bool loadScalaString(const std::string& text);

    struct MidiEvent {
        enum class Type { NoteOn, NoteOff, CC, PitchWheel } type;
        int delaySamples = 0;
        int noteNumber = 0; // NoteOn/NoteOff, 0-127
        int velocity = 0;   // NoteOn/NoteOff, 0-127
        int ccNumber = 0;   // CC, 0-127
        int ccValue = 0;    // CC, 0-127
        int pitch = 0;      // PitchWheel, centered at 0 (-8192..8191)
        // MIDI channel, 0-15 (the low nibble of the status byte - always
        // present in raw MIDI, MPE or not). Always sent through to sfizz's
        // `_channel` API regardless of mpeEnabled below: per sfizz.h's MPE
        // group docs, sfizz itself collapses this to channel 0 internally
        // when MPE is disabled, so passing the real channel here is safe
        // either way.
        int channel = 0;
    };

    // Audio thread only. events must be ordered by delaySamples, matching
    // sfizz's own requirement. channels[i] is cleared to silence if the
    // engine can't acquire the reload lock this block. mpeEnabled is the
    // current desired engine-wide MPE flag (SharedParams::mpeEnabled) -
    // passed fresh every block since sfizz_set_mpe_enabled is RT-thread-only
    // (can't be called directly from the GUI thread); applied only when it
    // actually changes. tuningFrequency (SharedParams::tuningFrequency, Hz,
    // default 440) is the concert-pitch A4 reference - same
    // RT-only/apply-only-on-change convention as mpeEnabled, via
    // sfizz_set_tuning_frequency.
    void renderBlock(const MidiEvent* events, int numEvents, float** channels,
                     int numChannels, int numFrames, bool mpeEnabled,
                     float tuningFrequency);

    // Best-effort live voice count (sfizz_get_num_active_voices - its own
    // doc notes it "runs on the calling thread so voices may well start or
    // stop while checking", i.e. not strictly synchronized - fine for a UI
    // meter). Updated at the end of every successful renderBlock call; safe
    // to read from the GUI thread (plain atomic, same convention as
    // SharedParams::noteActive). Stale (last known value) if a block was
    // skipped because a reload was in progress.
    int activeVoiceCount() const { return activeVoices_.load(std::memory_order_relaxed); }

private:
    sfizz_synth_t* synth = nullptr;
    std::mutex reloadMutex_;
    bool mpeEnabledApplied_ = false;
    float tuningFrequencyApplied_ = 440.0f;
    std::atomic<int> activeVoices_{0};

    SfizzEngine(const SfizzEngine&) = delete;
    SfizzEngine& operator=(const SfizzEngine&) = delete;
};
