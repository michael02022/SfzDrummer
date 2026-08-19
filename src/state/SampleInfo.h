#pragma once

#include <array>
#include <cstdint>
#include <string>

// Single source of truth for which extensions the file dialog, the XDND
// drop handler, and isSupportedAudioFile() below all accept.
inline constexpr std::array<const char*, 8> kSupportedAudioExtensions = {
    ".wav", ".wave", ".aif", ".aiff", ".flac", ".ogg", ".w64", ".caf"};

// What we can learn about an audio file up front: how many frames it has
// (drives the offset slider's max, per ExplicacionPlugin.txt's "cuenta el
// numero maximo de samples disponibles") and, if the file carries a WAV
// `smpl` chunk, its embedded root note, fine tune, and loop points.
struct SampleInfo {
    static constexpr int64_t defaultMaxOffset = 8192;

    bool isValid = false;
    int64_t numFrames = 0;
    double sampleRate = 0.0;

    // smpl chunk's MIDIUnityNote/MIDIPitchFraction fields (libsndfile's
    // SF_INSTRUMENT.basenote/detune); hasRootNote is false when the file has
    // no smpl chunk at all (distinct from having a chunk with no loops).
    bool hasRootNote = false;
    int rootNote = 60;
    float fineTuneCents = 0.0f;

    bool hasLoopPoints = false;
    int64_t loopStartFrame = 0;
    int64_t loopEndFrame = 0;

    int64_t maxOffset() const {
        return isValid && numFrames > 0 ? numFrames : defaultMaxOffset;
    }
};

// Safe to call from the message/GUI thread (file dialogs / drag-and-drop),
// never from the audio thread.
SampleInfo analyzeSampleFile(const std::string& path);

// Shared by the file dialog and the XDND drop handler so they agree on which
// extensions to accept.
bool isSupportedAudioFile(const std::string& path);

// Used by plugin.cpp's load dispatcher to route a picked/dropped file to the
// single-sample flow or the "Multisample SFZ" flow (see SfzFlatten.h).
bool isSfzFile(const std::string& path);
