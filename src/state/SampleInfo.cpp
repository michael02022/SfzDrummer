#include "SampleInfo.h"

#include <sndfile.h>

#include <algorithm>
#include <array>
#include <cctype>

SampleInfo analyzeSampleFile(const std::string& path) {
    SampleInfo info;

    SF_INFO sfInfo{};
    SNDFILE* file = sf_open(path.c_str(), SFM_READ, &sfInfo);
    if (!file) return info;

    info.isValid = true;
    info.numFrames = static_cast<int64_t>(sfInfo.frames);
    info.sampleRate = static_cast<double>(sfInfo.samplerate);

    SF_INSTRUMENT inst{};
    if (sf_command(file, SFC_GET_INSTRUMENT, &inst, sizeof(inst)) == SF_TRUE) {
        info.hasRootNote = true;
        info.rootNote = static_cast<unsigned char>(inst.basenote);
        // detune is the raw MIDI pitch fraction rescaled by libsndfile into
        // 0-99 cents above basenote (same quantity, same formula as the WAV
        // smpl chunk's dwMIDIPitchFraction / 2^32 * 100 that the old JUCE
        // metadata reader exposed as "MidiPitchFraction").
        info.fineTuneCents = static_cast<float>(static_cast<unsigned char>(inst.detune));

        if (inst.loop_count > 0) {
            info.hasLoopPoints = true;
            info.loopStartFrame = static_cast<int64_t>(inst.loops[0].start);
            info.loopEndFrame = static_cast<int64_t>(inst.loops[0].end);
        }
    }

    sf_close(file);
    return info;
}

bool isSupportedAudioFile(const std::string& path) {
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;

    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    return std::find(kSupportedAudioExtensions.begin(), kSupportedAudioExtensions.end(), ext) !=
          kSupportedAudioExtensions.end();
}

bool isSfzFile(const std::string& path) {
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;

    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    return ext == ".sfz";
}
