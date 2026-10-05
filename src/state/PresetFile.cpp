#include "PresetFile.h"

#include <algorithm>
#include <cstdint>
#include <fstream>

namespace {

constexpr uint32_t kDrumPresetMagic = 0x50524d44;  // "DMRP" (sfzDrummer Preset) LE bytes
constexpr uint32_t kDrumPresetVersion = 3; // v2: VelSW tab (velSwitchLayers/
                                           // velSwCrossfadeEnabled/velSwMainFloor,
                                           // identity-scoped like drumKitGroups)
                                           // v3: Kit Path (sourceIsKitRelative/
                                           // kitRelativeSubPath, plus the same pair
                                           // added to each VelSwitchLayer entry) -
                                           // NOT the kit path itself, which is a
                                           // global user preference never part of
                                           // this format, see shared.hpp's
                                           // GuiState::kitPath
constexpr uint32_t kDrumProfileMagic = 0x46524d44; // "DMRF" (sfzDrummer Profile) LE bytes
constexpr uint32_t kDrumProfileVersion = 1;
constexpr uint32_t kDrumPercMagic = 0x43524d44;    // "DMRC" (sfzDrummer perC) LE bytes
constexpr uint32_t kDrumPercVersion = 1;
// Deliberately five independent constants (not one shared magic + a "kind"
// byte, unlike SoloSampler's .sspreset/.ssprofile) - see PresetFile.h's
// file comment for why the three sfzdrummer formats don't share a shape.

template <typename T>
bool writeVal(std::ofstream& s, const T& v) {
    s.write(reinterpret_cast<const char*>(&v), sizeof(T));
    return static_cast<bool>(s);
}
template <typename T>
bool readVal(std::ifstream& s, T& v) {
    s.read(reinterpret_cast<char*>(&v), sizeof(T));
    return static_cast<bool>(s);
}

bool writeStr(std::ofstream& s, const std::string& str) {
    if (!writeVal(s, static_cast<uint32_t>(str.size()))) return false;
    if (str.empty()) return true;
    s.write(str.data(), static_cast<std::streamsize>(str.size()));
    return static_cast<bool>(s);
}
bool readStr(std::ifstream& s, std::string& str) {
    uint32_t len = 0;
    if (!readVal(s, len)) return false;
    str.resize(len);
    if (len == 0) return true;
    s.read(str.data(), static_cast<std::streamsize>(len));
    return static_cast<bool>(s);
}

bool writeBool(std::ofstream& s, bool b) { return writeVal(s, static_cast<uint8_t>(b ? 1 : 0)); }
bool readBool(std::ifstream& s, bool& b) {
    uint8_t v = 0;
    if (!readVal(s, v)) return false;
    b = v != 0;
    return true;
}

// Every DrumItem field from "Sample tab instrument-design" through
// Opcodes - shared verbatim between a .drmpreset entry (identity + this)
// and a whole .drmprofile file (just this, for one drum). Field order
// matches shared.hpp's DrumItem declaration order top to bottom - keep
// both in sync when adding a new instrument-design field (same discipline
// plugin.cpp's stateSave/stateLoad already follows for the CLAP host
// format).
bool writeDrumDesign(std::ofstream& s, const SharedParams::DrumItem& d) {
    bool ok = writeVal(s, d.volume) && writeVal(s, d.pan) && writeVal(s, d.panRandom) &&
             writeBool(s, d.panAlternate) && writeVal(s, d.width) && writeVal(s, d.quality) &&
             writeVal(s, d.polyphony) && writeVal(s, d.notePolyphony) &&
             writeBool(s, d.disableNoteSelfmask) && writeVal(s, d.loopModeIndex) &&
             writeBool(s, d.reverse) && writeVal(s, d.offset) && writeVal(s, d.vel2offset) &&
             writeBool(s, d.exclusiveClass) && writeVal(s, d.group) &&
             writeBool(s, d.offbyEnabled) && writeVal(s, d.offby) && writeVal(s, d.transpose) &&
             writeVal(s, d.tune);
    if (!ok) return false;

    ok = writeVal(s, static_cast<uint32_t>(d.ampVelCurve.size()));
    for (const auto& pt : d.ampVelCurve) {
        if (!ok) break;
        ok = writeVal(s, pt.velocity) && writeVal(s, pt.gain);
    }
    if (!ok) return false;

    ok = writeVal(s, d.ampVeltrack) && writeVal(s, d.ampRandom) &&
         writeVal(s, d.ampAttackTime) && writeVal(s, d.ampHoldTime) &&
         writeVal(s, d.ampDecayTime) && writeVal(s, d.ampDecayTimeExtra) &&
         writeVal(s, d.ampSustainLevel) && writeVal(s, d.ampReleaseTime) &&
         writeVal(s, d.ampAttackShape) && writeVal(s, d.ampDecayShape) &&
         writeVal(s, d.ampReleaseShape) && writeVal(s, d.ampVel2Attack) &&
         writeVal(s, d.ampVel2Sustain) && writeVal(s, d.ampVel2Volume) &&
         writeVal(s, d.filterTypeIndex) && writeVal(s, d.filterCutoff) &&
         writeVal(s, d.filterResonance) && writeVal(s, d.filterRandomCutoff) &&
         writeVal(s, d.filVeltrack) && writeVal(s, d.resoVeltrack) &&
         writeBool(s, d.filterEgEnabled) && writeVal(s, d.filDepth) &&
         writeVal(s, d.filEgStartLevel) && writeVal(s, d.filEgDelayTime) &&
         writeVal(s, d.filEgAttackTime) && writeVal(s, d.filEgAttackShape) &&
         writeVal(s, d.filEgHoldTime) && writeVal(s, d.filEgDecayTime) &&
         writeVal(s, d.filEgDecayTimeExtra) && writeVal(s, d.filEgDecayShape) &&
         writeVal(s, d.filEgSustainLevel) && writeVal(s, d.filEgReleaseTime) &&
         writeVal(s, d.filEgReleaseShape) && writeVal(s, d.pitchVeltrack) &&
         writeVal(s, d.pitchRandom) && writeBool(s, d.pitchEgEnabled) &&
         writeVal(s, d.pitchDepth) && writeVal(s, d.pitchVel2Depth) &&
         writeBool(s, d.pitchVel2Invert) && writeVal(s, d.pitchEgStartLevel) &&
         writeVal(s, d.pitchEgDelayTime) && writeVal(s, d.pitchEgAttackTime) &&
         writeVal(s, d.pitchEgAttackShape) && writeVal(s, d.pitchEgHoldTime) &&
         writeVal(s, d.pitchEgDecayTime) && writeVal(s, d.pitchEgDecayTimeExtra) &&
         writeVal(s, d.pitchEgDecayShape) && writeVal(s, d.pitchEgSustainLevel) &&
         writeVal(s, d.pitchEgReleaseTime) && writeVal(s, d.pitchEgReleaseShape) &&
         writeStr(s, d.customOpcodesText);
    return ok;
}

bool readDrumDesign(std::ifstream& s, SharedParams::DrumItem& d) {
    int volume = 0, pan = 0, panRandom = 0, width = 100, quality = 4, polyphony = 32,
        notePolyphony = 32, loopModeIndex = 1, offset = 0, vel2offset = 0, group = 0, offby = 0,
        transpose = 0, tune = 0;
    bool panAlternate = false, disableNoteSelfmask = false, reverse = false,
         exclusiveClass = false, offbyEnabled = false;
    bool ok = readVal(s, volume) && readVal(s, pan) && readVal(s, panRandom) &&
             readBool(s, panAlternate) && readVal(s, width) && readVal(s, quality) &&
             readVal(s, polyphony) && readVal(s, notePolyphony) &&
             readBool(s, disableNoteSelfmask) && readVal(s, loopModeIndex) &&
             readBool(s, reverse) && readVal(s, offset) && readVal(s, vel2offset) &&
             readBool(s, exclusiveClass) && readVal(s, group) && readBool(s, offbyEnabled) &&
             readVal(s, offby) && readVal(s, transpose) && readVal(s, tune);
    if (!ok) return false;
    d.volume = std::clamp(volume, -48, 48);
    d.pan = std::clamp(pan, -100, 100);
    d.panRandom = std::clamp(panRandom, -200, 200);
    d.panAlternate = panAlternate;
    d.width = std::clamp(width, 0, 100);
    d.quality = std::clamp(quality, 0, 9);
    d.polyphony = std::clamp(polyphony, 1, 256);
    d.notePolyphony = std::clamp(notePolyphony, 1, 256);
    d.disableNoteSelfmask = disableNoteSelfmask;
    d.loopModeIndex = std::clamp(loopModeIndex, 0, 4);
    d.reverse = reverse;
    d.offset = std::clamp(offset, 0, 4096);
    d.vel2offset = std::clamp(vel2offset, -4096, 0);
    d.exclusiveClass = exclusiveClass;
    d.group = std::clamp(group, 0, 99);
    d.offbyEnabled = offbyEnabled;
    d.offby = std::clamp(offby, 0, 99);
    d.transpose = std::clamp(transpose, -24, 24);
    d.tune = std::clamp(tune, -99, 99);

    uint32_t curveCount = 0;
    if (!readVal(s, curveCount)) return false;
    d.ampVelCurve.clear();
    d.ampVelCurve.reserve(curveCount);
    for (uint32_t i = 0; i < curveCount; ++i) {
        int velocity = 1;
        float gain = 1.f;
        if (!readVal(s, velocity) || !readVal(s, gain)) return false;
        d.ampVelCurve.push_back({std::clamp(velocity, 1, 127), std::clamp(gain, 0.f, 1.f)});
    }

    int ampVeltrack = 100, ampRandom = 0, ampVel2Volume = 0, filterTypeIndex = 1,
        filterCutoff = 20000, filterRandomCutoff = 0, filVeltrack = 0, resoVeltrack = 0,
        filDepth = 0, pitchVeltrack = 0, pitchRandom = 0, pitchDepth = 0, pitchVel2Depth = 0;
    float ampAttackTime = 0.00001f, ampHoldTime = 0.00001f, ampDecayTime = 0.00001f,
          ampDecayTimeExtra = 0.f, ampSustainLevel = 1.f, ampReleaseTime = 0.00001f,
          ampAttackShape = 0.00001f, ampDecayShape = -0.3616f, ampReleaseShape = -6.3616f,
          ampVel2Attack = 0.f, ampVel2Sustain = 0.f, filterResonance = 0.f,
          filEgStartLevel = 0.f, filEgDelayTime = 0.00001f, filEgAttackTime = 0.00001f,
          filEgAttackShape = 0.00001f, filEgHoldTime = 0.00001f, filEgDecayTime = 0.00001f,
          filEgDecayTimeExtra = 0.f, filEgDecayShape = 0.00001f, filEgSustainLevel = 1.f,
          filEgReleaseTime = 0.00001f, filEgReleaseShape = 0.00001f, pitchEgStartLevel = 0.f,
          pitchEgDelayTime = 0.00001f, pitchEgAttackTime = 0.00001f,
          pitchEgAttackShape = 0.00001f, pitchEgHoldTime = 0.00001f, pitchEgDecayTime = 0.00001f,
          pitchEgDecayTimeExtra = 0.f, pitchEgDecayShape = 0.00001f, pitchEgSustainLevel = 1.f,
          pitchEgReleaseTime = 0.00001f, pitchEgReleaseShape = 0.00001f;
    bool filterEgEnabled = false, pitchEgEnabled = false, pitchVel2Invert = false;

    ok = readVal(s, ampVeltrack) && readVal(s, ampRandom) && readVal(s, ampAttackTime) &&
         readVal(s, ampHoldTime) && readVal(s, ampDecayTime) && readVal(s, ampDecayTimeExtra) &&
         readVal(s, ampSustainLevel) && readVal(s, ampReleaseTime) &&
         readVal(s, ampAttackShape) && readVal(s, ampDecayShape) &&
         readVal(s, ampReleaseShape) && readVal(s, ampVel2Attack) &&
         readVal(s, ampVel2Sustain) && readVal(s, ampVel2Volume) &&
         readVal(s, filterTypeIndex) && readVal(s, filterCutoff) &&
         readVal(s, filterResonance) && readVal(s, filterRandomCutoff) &&
         readVal(s, filVeltrack) && readVal(s, resoVeltrack) && readBool(s, filterEgEnabled) &&
         readVal(s, filDepth) && readVal(s, filEgStartLevel) && readVal(s, filEgDelayTime) &&
         readVal(s, filEgAttackTime) && readVal(s, filEgAttackShape) &&
         readVal(s, filEgHoldTime) && readVal(s, filEgDecayTime) &&
         readVal(s, filEgDecayTimeExtra) && readVal(s, filEgDecayShape) &&
         readVal(s, filEgSustainLevel) && readVal(s, filEgReleaseTime) &&
         readVal(s, filEgReleaseShape) && readVal(s, pitchVeltrack) &&
         readVal(s, pitchRandom) && readBool(s, pitchEgEnabled) && readVal(s, pitchDepth) &&
         readVal(s, pitchVel2Depth) && readBool(s, pitchVel2Invert) &&
         readVal(s, pitchEgStartLevel) && readVal(s, pitchEgDelayTime) &&
         readVal(s, pitchEgAttackTime) && readVal(s, pitchEgAttackShape) &&
         readVal(s, pitchEgHoldTime) && readVal(s, pitchEgDecayTime) &&
         readVal(s, pitchEgDecayTimeExtra) && readVal(s, pitchEgDecayShape) &&
         readVal(s, pitchEgSustainLevel) && readVal(s, pitchEgReleaseTime) &&
         readVal(s, pitchEgReleaseShape) && readStr(s, d.customOpcodesText);
    if (!ok) return false;

    d.ampVeltrack = std::clamp(ampVeltrack, -100, 100);
    d.ampRandom = std::clamp(ampRandom, -24, 24);
    d.ampAttackTime = std::clamp(ampAttackTime, 0.00001f, 0.1f);
    d.ampHoldTime = std::clamp(ampHoldTime, 0.00001f, 0.1f);
    d.ampDecayTime = std::clamp(ampDecayTime, 0.00001f, 0.1f);
    d.ampDecayTimeExtra = std::clamp(ampDecayTimeExtra, 0.f, 12.f);
    d.ampSustainLevel = std::clamp(ampSustainLevel, 0.f, 1.f);
    d.ampReleaseTime = std::clamp(ampReleaseTime, 0.00001f, 12.f);
    d.ampAttackShape = std::clamp(ampAttackShape, -11.f, 11.f);
    d.ampDecayShape = std::clamp(ampDecayShape, -11.f, 11.f);
    d.ampReleaseShape = std::clamp(ampReleaseShape, -11.f, 11.f);
    d.ampVel2Attack = std::clamp(ampVel2Attack, -0.1f, 0.f);
    d.ampVel2Sustain = std::clamp(ampVel2Sustain, -1.f, 1.f);
    d.ampVel2Volume = std::clamp(ampVel2Volume, -24, 24);
    d.filterTypeIndex = std::clamp(filterTypeIndex, 0, 22);
    d.filterCutoff = std::clamp(filterCutoff, 1, 20000);
    d.filterResonance = std::clamp(filterResonance, -40.f, 40.f);
    d.filterRandomCutoff = std::clamp(filterRandomCutoff, 1, 20000);
    d.filVeltrack = std::clamp(filVeltrack, 0, 20000);
    d.resoVeltrack = std::clamp(resoVeltrack, -40, 40);
    d.filterEgEnabled = filterEgEnabled;
    d.filDepth = std::clamp(filDepth, 0, 20000);
    d.filEgStartLevel = std::clamp(filEgStartLevel, 0.f, 1.f);
    d.filEgDelayTime = std::clamp(filEgDelayTime, 0.00001f, 0.1f);
    d.filEgAttackTime = std::clamp(filEgAttackTime, 0.00001f, 0.1f);
    d.filEgAttackShape = std::clamp(filEgAttackShape, -11.f, 11.f);
    d.filEgHoldTime = std::clamp(filEgHoldTime, 0.00001f, 0.1f);
    d.filEgDecayTime = std::clamp(filEgDecayTime, 0.00001f, 0.1f);
    d.filEgDecayTimeExtra = std::clamp(filEgDecayTimeExtra, 0.f, 12.f);
    d.filEgDecayShape = std::clamp(filEgDecayShape, -11.f, 11.f);
    d.filEgSustainLevel = std::clamp(filEgSustainLevel, 0.f, 1.f);
    d.filEgReleaseTime = std::clamp(filEgReleaseTime, 0.00001f, 0.1f);
    d.filEgReleaseShape = std::clamp(filEgReleaseShape, -11.f, 11.f);
    d.pitchVeltrack = std::clamp(pitchVeltrack, -9600, 9600);
    d.pitchRandom = std::clamp(pitchRandom, 0, 9600);
    d.pitchEgEnabled = pitchEgEnabled;
    d.pitchDepth = std::clamp(pitchDepth, -9600, 9600);
    d.pitchVel2Depth = std::clamp(pitchVel2Depth, -9600, 9600);
    d.pitchVel2Invert = pitchVel2Invert;
    d.pitchEgStartLevel = std::clamp(pitchEgStartLevel, 0.f, 1.f);
    d.pitchEgDelayTime = std::clamp(pitchEgDelayTime, 0.00001f, 0.1f);
    d.pitchEgAttackTime = std::clamp(pitchEgAttackTime, 0.00001f, 0.1f);
    d.pitchEgAttackShape = std::clamp(pitchEgAttackShape, -11.f, 11.f);
    d.pitchEgHoldTime = std::clamp(pitchEgHoldTime, 0.00001f, 0.1f);
    d.pitchEgDecayTime = std::clamp(pitchEgDecayTime, 0.00001f, 0.1f);
    d.pitchEgDecayTimeExtra = std::clamp(pitchEgDecayTimeExtra, 0.f, 12.f);
    d.pitchEgDecayShape = std::clamp(pitchEgDecayShape, -11.f, 11.f);
    d.pitchEgSustainLevel = std::clamp(pitchEgSustainLevel, 0.f, 1.f);
    d.pitchEgReleaseTime = std::clamp(pitchEgReleaseTime, 0.00001f, 0.1f);
    d.pitchEgReleaseShape = std::clamp(pitchEgReleaseShape, -11.f, 11.f);
    return true;
}

// Identity-only fields (.drmpreset entries only - never written for a
// .drmprofile): which sample/sfz this drum plays, its Drum Kit Mode
// selection, its label/root note/output routing.
bool writeDrumIdentity(std::ofstream& s, const SharedParams::DrumItem& d) {
    bool ok = writeStr(s, d.label) && writeVal(s, d.rootNote) && writeVal(s, d.outputIndex) &&
             writeBool(s, d.hasSource) && writeBool(s, d.isSfz) && writeStr(s, d.sourcePath) &&
             writeStr(s, d.sampleRelativePath) && writeStr(s, d.regionsText) &&
             writeVal(s, d.regionCount) && writeBool(s, d.drumKitModeEnabled) &&
             writeVal(s, d.drumKitGroupIndex) &&
             writeVal(s, static_cast<uint32_t>(d.drumKitGroups.size()));
    for (const auto& g : d.drumKitGroups) {
        if (!ok) break;
        ok = writeVal(s, g.key) && writeStr(s, g.regionsText) && writeVal(s, g.regionCount);
    }
    if (!ok) return false;

    // v2: VelSW tab (velocity-switch layers) - identity-scoped, same
    // category as drumKitGroups above. v3 added isKitRelative/
    // kitRelativeSubPath to each layer entry's own shape below.
    ok = writeBool(s, d.velSwCrossfadeEnabled) && writeVal(s, d.velSwMainFloor) &&
         writeVal(s, static_cast<uint32_t>(d.velSwitchLayers.size()));
    for (const auto& layer : d.velSwitchLayers) {
        if (!ok) break;
        ok = writeStr(s, layer.sourcePath) && writeStr(s, layer.sampleRelativePath) &&
             writeVal(s, layer.hivel) && writeVal(s, layer.floor) &&
             writeBool(s, layer.isKitRelative) && writeStr(s, layer.kitRelativeSubPath);
    }
    if (!ok) return false;

    // v3: Kit Path (drum's own main source) - NOT the kit path itself,
    // which is a global user preference, never part of this format (see
    // shared.hpp's GuiState::kitPath).
    return writeBool(s, d.sourceIsKitRelative) && writeStr(s, d.kitRelativeSubPath);
}

bool readDrumIdentity(std::ifstream& s, SharedParams::DrumItem& d) {
    int rootNote = 60, outputIndex = 0, regionCount = 0, drumKitGroupIndex = 0;
    bool hasSource = false, isSfz = false, drumKitModeEnabled = false;
    uint32_t groupCount = 0;
    bool ok = readStr(s, d.label) && readVal(s, rootNote) && readVal(s, outputIndex) &&
             readBool(s, hasSource) && readBool(s, isSfz) && readStr(s, d.sourcePath) &&
             readStr(s, d.sampleRelativePath) && readStr(s, d.regionsText) &&
             readVal(s, regionCount) && readBool(s, drumKitModeEnabled) &&
             readVal(s, drumKitGroupIndex) && readVal(s, groupCount);
    if (!ok) return false;
    d.rootNote = std::clamp(rootNote, 0, 127);
    d.outputIndex = std::clamp(outputIndex, 0, kNumOutputs - 1);
    d.hasSource = hasSource;
    d.isSfz = isSfz;
    d.regionCount = regionCount;
    d.drumKitModeEnabled = drumKitModeEnabled;
    d.drumKitGroupIndex = drumKitGroupIndex;
    d.drumKitGroups.clear();
    d.drumKitGroups.reserve(groupCount);
    for (uint32_t i = 0; i < groupCount; ++i) {
        DrumKitKeyGroup group;
        if (!readVal(s, group.key) || !readStr(s, group.regionsText) ||
            !readVal(s, group.regionCount))
            return false;
        d.drumKitGroups.push_back(std::move(group));
    }

    // v2: VelSW tab.
    bool velSwCrossfadeEnabled = false;
    int velSwMainFloor = 64;
    uint32_t velSwLayerCount = 0;
    if (!readBool(s, velSwCrossfadeEnabled) || !readVal(s, velSwMainFloor) ||
        !readVal(s, velSwLayerCount))
        return false;
    d.velSwCrossfadeEnabled = velSwCrossfadeEnabled;
    d.velSwMainFloor = std::clamp(velSwMainFloor, 1, 126);
    d.velSwitchLayers.clear();
    d.velSwitchLayers.reserve(velSwLayerCount);
    for (uint32_t i = 0; i < velSwLayerCount; ++i) {
        VelSwitchLayer layer;
        int hivel = 64, floor = 64;
        bool isKitRelative = false;
        if (!readStr(s, layer.sourcePath) || !readStr(s, layer.sampleRelativePath) ||
            !readVal(s, hivel) || !readVal(s, floor) || !readBool(s, isKitRelative) ||
            !readStr(s, layer.kitRelativeSubPath))
            return false;
        layer.hivel = std::clamp(hivel, 1, 126);
        layer.floor = std::clamp(floor, 1, 126);
        layer.isKitRelative = isKitRelative;
        d.velSwitchLayers.push_back(std::move(layer));
    }

    // v3: Kit Path (drum's own main source).
    bool sourceIsKitRelative = false;
    if (!readBool(s, sourceIsKitRelative) || !readStr(s, d.kitRelativeSubPath)) return false;
    d.sourceIsKitRelative = sourceIsKitRelative;
    return true;
}

} // namespace

bool writeDrumPreset(const std::string& path,
                     const std::vector<SharedParams::DrumItem>& drums) {
    std::ofstream s(path, std::ios::binary | std::ios::trunc);
    if (!s) return false;

    bool ok = writeVal(s, kDrumPresetMagic) && writeVal(s, kDrumPresetVersion) &&
             writeVal(s, static_cast<uint32_t>(drums.size()));
    for (const auto& d : drums) {
        if (!ok) break;
        ok = writeDrumIdentity(s, d) && writeDrumDesign(s, d);
    }

    s.flush();
    return ok && static_cast<bool>(s);
}

DrumPresetResult readDrumPreset(const std::string& path) {
    DrumPresetResult result;
    std::ifstream s(path, std::ios::binary);
    if (!s) {
        result.error = "Could not open file: " + path;
        return result;
    }

    uint32_t magic = 0, version = 0, count = 0;
    if (!readVal(s, magic) || magic != kDrumPresetMagic) {
        result.error = "Not a sfzdrummer preset file: " + path;
        return result;
    }
    if (!readVal(s, version) || version != kDrumPresetVersion) {
        result.error = "Unsupported preset file version: " + path;
        return result;
    }
    if (!readVal(s, count)) {
        result.error = "Corrupt preset file: " + path;
        return result;
    }

    result.drums.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        SharedParams::DrumItem d;
        if (!readDrumIdentity(s, d) || !readDrumDesign(s, d)) {
            result.error = "Corrupt or truncated preset file: " + path;
            result.drums.clear();
            return result;
        }
        result.drums.push_back(std::move(d));
    }

    result.ok = true;
    return result;
}

// .drmperc - structurally just one .drmpreset entry (writeDrumIdentity+
// writeDrumDesign, same order/shape) with its own header instead of a
// count-prefixed list, since it's always exactly one drum. See
// PresetFile.h's own comment for why this exists alongside .drmpreset/
// .drmprofile.
bool writeDrumPerc(const std::string& path, const SharedParams::DrumItem& drum) {
    std::ofstream s(path, std::ios::binary | std::ios::trunc);
    if (!s) return false;

    bool ok = writeVal(s, kDrumPercMagic) && writeVal(s, kDrumPercVersion);
    if (ok) ok = writeDrumIdentity(s, drum) && writeDrumDesign(s, drum);

    s.flush();
    return ok && static_cast<bool>(s);
}

DrumPercResult readDrumPerc(const std::string& path) {
    DrumPercResult result;
    std::ifstream s(path, std::ios::binary);
    if (!s) {
        result.error = "Could not open file: " + path;
        return result;
    }

    uint32_t magic = 0, version = 0;
    if (!readVal(s, magic) || magic != kDrumPercMagic) {
        result.error = "Not a sfzdrummer perc file: " + path;
        return result;
    }
    if (!readVal(s, version) || version != kDrumPercVersion) {
        result.error = "Unsupported perc file version: " + path;
        return result;
    }

    if (!readDrumIdentity(s, result.drum) || !readDrumDesign(s, result.drum)) {
        result.error = "Corrupt or truncated perc file: " + path;
        result.drum = SharedParams::DrumItem{};
        return result;
    }

    result.ok = true;
    return result;
}

bool writeDrumProfile(const std::string& path, const SharedParams::DrumItem& drum) {
    std::ofstream s(path, std::ios::binary | std::ios::trunc);
    if (!s) return false;

    bool ok = writeVal(s, kDrumProfileMagic) && writeVal(s, kDrumProfileVersion);
    if (ok) ok = writeDrumDesign(s, drum);

    s.flush();
    return ok && static_cast<bool>(s);
}

DrumProfileResult readDrumProfile(const std::string& path) {
    DrumProfileResult result;
    std::ifstream s(path, std::ios::binary);
    if (!s) {
        result.error = "Could not open file: " + path;
        return result;
    }

    uint32_t magic = 0, version = 0;
    if (!readVal(s, magic) || magic != kDrumProfileMagic) {
        result.error = "Not a sfzdrummer profile file: " + path;
        return result;
    }
    if (!readVal(s, version) || version != kDrumProfileVersion) {
        result.error = "Unsupported profile file version: " + path;
        return result;
    }

    if (!readDrumDesign(s, result.fields)) {
        result.error = "Corrupt or truncated profile file: " + path;
        result.fields = SharedParams::DrumItem{};
        return result;
    }

    result.ok = true;
    return result;
}

void applyDrumProfileDesign(SharedParams::DrumItem& target, const SharedParams::DrumItem& source) {
    target.volume = source.volume;
    target.pan = source.pan;
    target.panRandom = source.panRandom;
    target.panAlternate = source.panAlternate;
    target.width = source.width;
    target.quality = source.quality;
    target.polyphony = source.polyphony;
    target.notePolyphony = source.notePolyphony;
    target.disableNoteSelfmask = source.disableNoteSelfmask;
    target.loopModeIndex = source.loopModeIndex;
    target.reverse = source.reverse;
    target.offset = source.offset;
    target.vel2offset = source.vel2offset;
    target.exclusiveClass = source.exclusiveClass;
    target.group = source.group;
    target.offbyEnabled = source.offbyEnabled;
    target.offby = source.offby;
    target.transpose = source.transpose;
    target.tune = source.tune;
    target.ampVelCurve = source.ampVelCurve;
    target.ampVeltrack = source.ampVeltrack;
    target.ampRandom = source.ampRandom;
    target.ampAttackTime = source.ampAttackTime;
    target.ampHoldTime = source.ampHoldTime;
    target.ampDecayTime = source.ampDecayTime;
    target.ampDecayTimeExtra = source.ampDecayTimeExtra;
    target.ampSustainLevel = source.ampSustainLevel;
    target.ampReleaseTime = source.ampReleaseTime;
    target.ampAttackShape = source.ampAttackShape;
    target.ampDecayShape = source.ampDecayShape;
    target.ampReleaseShape = source.ampReleaseShape;
    target.ampVel2Attack = source.ampVel2Attack;
    target.ampVel2Sustain = source.ampVel2Sustain;
    target.ampVel2Volume = source.ampVel2Volume;
    target.filterTypeIndex = source.filterTypeIndex;
    target.filterCutoff = source.filterCutoff;
    target.filterResonance = source.filterResonance;
    target.filterRandomCutoff = source.filterRandomCutoff;
    target.filVeltrack = source.filVeltrack;
    target.resoVeltrack = source.resoVeltrack;
    target.filterEgEnabled = source.filterEgEnabled;
    target.filDepth = source.filDepth;
    target.filEgStartLevel = source.filEgStartLevel;
    target.filEgDelayTime = source.filEgDelayTime;
    target.filEgAttackTime = source.filEgAttackTime;
    target.filEgAttackShape = source.filEgAttackShape;
    target.filEgHoldTime = source.filEgHoldTime;
    target.filEgDecayTime = source.filEgDecayTime;
    target.filEgDecayTimeExtra = source.filEgDecayTimeExtra;
    target.filEgDecayShape = source.filEgDecayShape;
    target.filEgSustainLevel = source.filEgSustainLevel;
    target.filEgReleaseTime = source.filEgReleaseTime;
    target.filEgReleaseShape = source.filEgReleaseShape;
    target.pitchVeltrack = source.pitchVeltrack;
    target.pitchRandom = source.pitchRandom;
    target.pitchEgEnabled = source.pitchEgEnabled;
    target.pitchDepth = source.pitchDepth;
    target.pitchVel2Depth = source.pitchVel2Depth;
    target.pitchVel2Invert = source.pitchVel2Invert;
    target.pitchEgStartLevel = source.pitchEgStartLevel;
    target.pitchEgDelayTime = source.pitchEgDelayTime;
    target.pitchEgAttackTime = source.pitchEgAttackTime;
    target.pitchEgAttackShape = source.pitchEgAttackShape;
    target.pitchEgHoldTime = source.pitchEgHoldTime;
    target.pitchEgDecayTime = source.pitchEgDecayTime;
    target.pitchEgDecayTimeExtra = source.pitchEgDecayTimeExtra;
    target.pitchEgDecayShape = source.pitchEgDecayShape;
    target.pitchEgSustainLevel = source.pitchEgSustainLevel;
    target.pitchEgReleaseTime = source.pitchEgReleaseTime;
    target.pitchEgReleaseShape = source.pitchEgReleaseShape;
    target.customOpcodesText = source.customOpcodesText;
}
