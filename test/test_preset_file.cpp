// Standalone regression test for .drmpreset/.drmprofile round-tripping
// (state/PresetFile.cpp) - plain C++ against SharedParams::DrumItem
// directly, no CLAP-level entry point needed, same convention as
// test_sfz_flatten.cpp. Exercises every design field (two DIFFERENT non-
// default value sets, so a drum-to-drum field mixup in a multi-drum
// .drmpreset would be caught, not just "some value round-tripped"), the
// Profile format's identity-field exclusion (the single most important
// property to verify - a Profile must NEVER leak label/rootNote/output/
// sample identity onto whatever drum it's later applied to), and basic
// corrupt-file/wrong-format rejection.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "state/PresetFile.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    } else {
        printf("ok: %s\n", what.c_str());
    }
}

// Every design field set to a distinct, non-default value - "A" and "B"
// use different literals throughout so a field accidentally copied from
// the wrong drum (in the multi-drum preset test) reads as a clear
// mismatch rather than a coincidental match.
void fillDesignA(SharedParams::DrumItem& d) {
    d.volume = -12;
    d.pan = 30;
    d.panRandom = 80;
    d.panAlternate = true;
    d.width = 60;
    d.quality = 7;
    d.polyphony = 48;
    d.notePolyphony = 40;
    d.disableNoteSelfmask = true;
    d.loopModeIndex = 2;
    d.reverse = true;
    d.offset = 300;
    d.vel2offset = -150;
    d.exclusiveClass = true;
    d.group = 5;
    d.offbyEnabled = true;
    d.offby = 6;
    d.transpose = -3;
    d.tune = 20;
    d.ampVelCurve = {{20, 0.3f}, {100, 0.85f}};
    d.ampVeltrack = 70;
    d.ampRandom = -5;
    d.ampAttackTime = 0.02f;
    d.ampHoldTime = 0.03f;
    d.ampDecayTime = 0.04f;
    d.ampDecayTimeExtra = 1.5f;
    d.ampSustainLevel = 0.6f;
    d.ampReleaseTime = 0.5f;
    d.ampAttackShape = 2.0f;
    d.ampDecayShape = -1.0f;
    d.ampReleaseShape = 3.5f;
    d.ampVel2Attack = -0.02f;
    d.ampVel2Sustain = 0.4f;
    d.ampVel2Volume = -4;
    d.filterTypeIndex = 3;
    d.filterCutoff = 15000;
    d.filterResonance = 4.5f;
    d.filterRandomCutoff = 2000;
    d.filVeltrack = 3000;
    d.resoVeltrack = 8;
    d.filterEgEnabled = true;
    d.filDepth = 2500;
    d.filEgStartLevel = 0.1f;
    d.filEgDelayTime = 0.005f;
    d.filEgAttackTime = 0.006f;
    d.filEgAttackShape = 1.2f;
    d.filEgHoldTime = 0.007f;
    d.filEgDecayTime = 0.008f;
    d.filEgDecayTimeExtra = 0.9f;
    d.filEgDecayShape = -0.5f;
    d.filEgSustainLevel = 0.7f;
    d.filEgReleaseTime = 0.02f;
    d.filEgReleaseShape = 1.8f;
    d.pitchVeltrack = 1500;
    d.pitchRandom = 250;
    d.pitchEgEnabled = true;
    d.pitchDepth = 300;
    d.pitchVel2Depth = -120;
    d.pitchVel2Invert = true;
    d.pitchEgStartLevel = 0.05f;
    d.pitchEgDelayTime = 0.002f;
    d.pitchEgAttackTime = 0.003f;
    d.pitchEgAttackShape = 0.8f;
    d.pitchEgHoldTime = 0.004f;
    d.pitchEgDecayTime = 0.005f;
    d.pitchEgDecayTimeExtra = 0.6f;
    d.pitchEgDecayShape = -0.7f;
    d.pitchEgSustainLevel = 0.85f;
    d.pitchEgReleaseTime = 0.03f;
    d.pitchEgReleaseShape = 2.2f;
    d.customOpcodesText = "// drum A custom opcode\nkey=60";
}

void fillDesignB(SharedParams::DrumItem& d) {
    d.volume = 8;
    d.pan = -45;
    d.panRandom = -60;
    d.panAlternate = false;
    d.width = 90;
    d.quality = 2;
    d.polyphony = 16;
    d.notePolyphony = 8;
    d.disableNoteSelfmask = false;
    d.loopModeIndex = 3;
    d.reverse = false;
    d.offset = 1000;
    d.vel2offset = -2000;
    d.exclusiveClass = false;
    d.group = 9;
    d.offbyEnabled = false;
    d.offby = 12;
    d.transpose = 7;
    d.tune = -40;
    d.ampVelCurve = {{10, 0.1f}, {60, 0.5f}, {127, 1.0f}};
    d.ampVeltrack = -10;
    d.ampRandom = 12;
    d.ampAttackTime = 0.09f;
    d.ampHoldTime = 0.01f;
    d.ampDecayTime = 0.07f;
    d.ampDecayTimeExtra = 6.0f;
    d.ampSustainLevel = 0.2f;
    d.ampReleaseTime = 3.0f;
    d.ampAttackShape = -4.0f;
    d.ampDecayShape = 5.0f;
    d.ampReleaseShape = -2.0f;
    d.ampVel2Attack = -0.08f;
    d.ampVel2Sustain = -0.6f;
    d.ampVel2Volume = 10;
    d.filterTypeIndex = 8;
    d.filterCutoff = 500;
    d.filterResonance = -20.0f;
    d.filterRandomCutoff = 18000;
    d.filVeltrack = 9000;
    d.resoVeltrack = -30;
    d.filterEgEnabled = false;
    d.filDepth = 100;
    d.filEgStartLevel = 0.9f;
    d.filEgDelayTime = 0.05f;
    d.filEgAttackTime = 0.08f;
    d.filEgAttackShape = -8.0f;
    d.filEgHoldTime = 0.02f;
    d.filEgDecayTime = 0.03f;
    d.filEgDecayTimeExtra = 11.0f;
    d.filEgDecayShape = 9.0f;
    d.filEgSustainLevel = 0.15f;
    d.filEgReleaseTime = 0.09f;
    d.filEgReleaseShape = -9.5f;
    d.pitchVeltrack = -8000;
    d.pitchRandom = 9000;
    d.pitchEgEnabled = false;
    d.pitchDepth = -9000;
    d.pitchVel2Depth = 8000;
    d.pitchVel2Invert = false;
    d.pitchEgStartLevel = 0.4f;
    d.pitchEgDelayTime = 0.01f;
    d.pitchEgAttackTime = 0.02f;
    d.pitchEgAttackShape = -3.0f;
    d.pitchEgHoldTime = 0.03f;
    d.pitchEgDecayTime = 0.04f;
    d.pitchEgDecayTimeExtra = 4.0f;
    d.pitchEgDecayShape = 6.5f;
    d.pitchEgSustainLevel = 0.33f;
    d.pitchEgReleaseTime = 0.06f;
    d.pitchEgReleaseShape = -1.5f;
    d.customOpcodesText = "// drum B custom opcode\nkey=62";
}

void fillIdentityA(SharedParams::DrumItem& d) {
    d.label = "Kick";
    d.rootNote = 36;
    d.outputIndex = 0;
    d.hasSource = true;
    d.isSfz = false;
    d.sourcePath = "/home/user/samples/kick.wav";
    d.sampleRelativePath = "home/user/samples/kick.wav";
    d.regionsText = "";
    d.regionCount = 1;
    d.drumKitModeEnabled = false;
    d.drumKitGroupIndex = 0;
}

void fillIdentityB(SharedParams::DrumItem& d) {
    d.label = "HiHat";
    d.rootNote = 65;
    d.outputIndex = 7;
    d.hasSource = true;
    d.isSfz = true;
    d.sourcePath = "/home/user/kits/full_kit.sfz";
    d.sampleRelativePath = "";
    d.regionsText = "<region>\nsample=hh1.wav\n<region>\nsample=hh2.wav\n";
    d.regionCount = 2;
    d.drumKitModeEnabled = true;
    d.drumKitGroupIndex = 1;
    d.drumKitGroups = {{10, "<region>\nsample=decoy.wav\n", 1},
                       {20, "<region>\nsample=real.wav\n", 1}};
    d.velSwCrossfadeEnabled = true;
    d.velSwMainFloor = 90;
    d.velSwitchLayers = {
        {"/home/user/samples/hh_med.wav", "home/user/samples/hh_med.wav", 80, 70, true,
         "samples/hh_med.wav"},
        {"/home/user/samples/hh_soft.wav", "home/user/samples/hh_soft.wav", 40, 40, false, ""},
    };
    d.sourceIsKitRelative = true;
    d.kitRelativeSubPath = "kits/full_kit.sfz";
}

// Compares every design field between two drums - identity fields
// deliberately NOT compared here, see checkIdentityIsDefault below for
// that separate concern.
void checkDesignEqual(const SharedParams::DrumItem& a, const SharedParams::DrumItem& b,
                      const std::string& label) {
    check(a.volume == b.volume && a.pan == b.pan && a.panRandom == b.panRandom &&
             a.panAlternate == b.panAlternate && a.width == b.width && a.quality == b.quality &&
             a.polyphony == b.polyphony && a.notePolyphony == b.notePolyphony &&
             a.disableNoteSelfmask == b.disableNoteSelfmask &&
             a.loopModeIndex == b.loopModeIndex && a.reverse == b.reverse &&
             a.offset == b.offset && a.vel2offset == b.vel2offset &&
             a.exclusiveClass == b.exclusiveClass && a.group == b.group &&
             a.offbyEnabled == b.offbyEnabled && a.offby == b.offby &&
             a.transpose == b.transpose && a.tune == b.tune,
         label + ": Sample tab instrument-design fields match");

    bool curveOk = a.ampVelCurve.size() == b.ampVelCurve.size();
    if (curveOk)
        for (size_t i = 0; i < a.ampVelCurve.size(); ++i)
            curveOk = curveOk && a.ampVelCurve[i].velocity == b.ampVelCurve[i].velocity &&
                     a.ampVelCurve[i].gain == b.ampVelCurve[i].gain;
    check(curveOk, label + ": ampVelCurve points match");

    check(a.ampVeltrack == b.ampVeltrack && a.ampRandom == b.ampRandom &&
             a.ampAttackTime == b.ampAttackTime && a.ampHoldTime == b.ampHoldTime &&
             a.ampDecayTime == b.ampDecayTime && a.ampDecayTimeExtra == b.ampDecayTimeExtra &&
             a.ampSustainLevel == b.ampSustainLevel && a.ampReleaseTime == b.ampReleaseTime &&
             a.ampAttackShape == b.ampAttackShape && a.ampDecayShape == b.ampDecayShape &&
             a.ampReleaseShape == b.ampReleaseShape && a.ampVel2Attack == b.ampVel2Attack &&
             a.ampVel2Sustain == b.ampVel2Sustain && a.ampVel2Volume == b.ampVel2Volume,
         label + ": Amp tab fields match");

    check(a.filterTypeIndex == b.filterTypeIndex && a.filterCutoff == b.filterCutoff &&
             a.filterResonance == b.filterResonance &&
             a.filterRandomCutoff == b.filterRandomCutoff && a.filVeltrack == b.filVeltrack &&
             a.resoVeltrack == b.resoVeltrack && a.filterEgEnabled == b.filterEgEnabled &&
             a.filDepth == b.filDepth && a.filEgStartLevel == b.filEgStartLevel &&
             a.filEgDelayTime == b.filEgDelayTime && a.filEgAttackTime == b.filEgAttackTime &&
             a.filEgAttackShape == b.filEgAttackShape && a.filEgHoldTime == b.filEgHoldTime &&
             a.filEgDecayTime == b.filEgDecayTime &&
             a.filEgDecayTimeExtra == b.filEgDecayTimeExtra &&
             a.filEgDecayShape == b.filEgDecayShape &&
             a.filEgSustainLevel == b.filEgSustainLevel &&
             a.filEgReleaseTime == b.filEgReleaseTime &&
             a.filEgReleaseShape == b.filEgReleaseShape,
         label + ": Fil tab fields match");

    check(a.pitchVeltrack == b.pitchVeltrack && a.pitchRandom == b.pitchRandom &&
             a.pitchEgEnabled == b.pitchEgEnabled && a.pitchDepth == b.pitchDepth &&
             a.pitchVel2Depth == b.pitchVel2Depth && a.pitchVel2Invert == b.pitchVel2Invert &&
             a.pitchEgStartLevel == b.pitchEgStartLevel &&
             a.pitchEgDelayTime == b.pitchEgDelayTime &&
             a.pitchEgAttackTime == b.pitchEgAttackTime &&
             a.pitchEgAttackShape == b.pitchEgAttackShape &&
             a.pitchEgHoldTime == b.pitchEgHoldTime &&
             a.pitchEgDecayTime == b.pitchEgDecayTime &&
             a.pitchEgDecayTimeExtra == b.pitchEgDecayTimeExtra &&
             a.pitchEgDecayShape == b.pitchEgDecayShape &&
             a.pitchEgSustainLevel == b.pitchEgSustainLevel &&
             a.pitchEgReleaseTime == b.pitchEgReleaseTime &&
             a.pitchEgReleaseShape == b.pitchEgReleaseShape,
         label + ": Pitch tab fields match");

    check(a.customOpcodesText == b.customOpcodesText, label + ": Opcodes tab text matches");
}

void checkIdentityEqual(const SharedParams::DrumItem& a, const SharedParams::DrumItem& b,
                        const std::string& label) {
    check(a.label == b.label && a.rootNote == b.rootNote && a.outputIndex == b.outputIndex &&
             a.hasSource == b.hasSource && a.isSfz == b.isSfz && a.sourcePath == b.sourcePath &&
             a.sampleRelativePath == b.sampleRelativePath && a.regionsText == b.regionsText &&
             a.regionCount == b.regionCount && a.drumKitModeEnabled == b.drumKitModeEnabled &&
             a.drumKitGroupIndex == b.drumKitGroupIndex &&
             a.sourceIsKitRelative == b.sourceIsKitRelative &&
             a.kitRelativeSubPath == b.kitRelativeSubPath,
         label + ": identity fields match");

    bool groupsOk = a.drumKitGroups.size() == b.drumKitGroups.size();
    if (groupsOk)
        for (size_t i = 0; i < a.drumKitGroups.size(); ++i)
            groupsOk = groupsOk && a.drumKitGroups[i].key == b.drumKitGroups[i].key &&
                     a.drumKitGroups[i].regionsText == b.drumKitGroups[i].regionsText &&
                     a.drumKitGroups[i].regionCount == b.drumKitGroups[i].regionCount;
    check(groupsOk, label + ": drumKitGroups match");

    check(a.velSwCrossfadeEnabled == b.velSwCrossfadeEnabled &&
             a.velSwMainFloor == b.velSwMainFloor,
         label + ": VelSW crossfade/mainFloor match");
    bool layersOk = a.velSwitchLayers.size() == b.velSwitchLayers.size();
    if (layersOk)
        for (size_t i = 0; i < a.velSwitchLayers.size(); ++i)
            layersOk = layersOk &&
                     a.velSwitchLayers[i].sourcePath == b.velSwitchLayers[i].sourcePath &&
                     a.velSwitchLayers[i].sampleRelativePath ==
                         b.velSwitchLayers[i].sampleRelativePath &&
                     a.velSwitchLayers[i].hivel == b.velSwitchLayers[i].hivel &&
                     a.velSwitchLayers[i].floor == b.velSwitchLayers[i].floor &&
                     a.velSwitchLayers[i].isKitRelative == b.velSwitchLayers[i].isKitRelative &&
                     a.velSwitchLayers[i].kitRelativeSubPath ==
                         b.velSwitchLayers[i].kitRelativeSubPath;
    check(layersOk, label + ": velSwitchLayers match");
}

// The single most important Profile property: loading one must NEVER
// leak identity (label/rootNote/output/sample source/Drum Kit Mode) from
// whichever drum it was originally saved from - readDrumProfile's result
// should carry a completely default-constructed identity, regardless of
// what the source drum's real identity was.
void checkIdentityIsDefault(const SharedParams::DrumItem& d, const std::string& label) {
    SharedParams::DrumItem def;
    check(d.label == def.label && d.rootNote == def.rootNote &&
             d.outputIndex == def.outputIndex && d.hasSource == def.hasSource &&
             d.isSfz == def.isSfz && d.sourcePath == def.sourcePath &&
             d.sampleRelativePath == def.sampleRelativePath && d.regionsText == def.regionsText &&
             d.regionCount == def.regionCount &&
             d.drumKitModeEnabled == def.drumKitModeEnabled &&
             d.drumKitGroupIndex == def.drumKitGroupIndex && d.drumKitGroups.empty() &&
             d.velSwCrossfadeEnabled == def.velSwCrossfadeEnabled &&
             d.velSwMainFloor == def.velSwMainFloor && d.velSwitchLayers.empty() &&
             d.sourceIsKitRelative == def.sourceIsKitRelative &&
             d.kitRelativeSubPath == def.kitRelativeSubPath,
         label + ": Profile load carries no leaked identity (all default)");
}

} // namespace

int main() {
    fs::path dir = fs::temp_directory_path() / "sfzdrummer_preset_test";
    fs::create_directories(dir);

    // --- .drmpreset: whole kit, 2 drums with deliberately different
    // identity AND design, to catch any drum-to-drum field mixup.
    {
        SharedParams::DrumItem a, b;
        fillIdentityA(a);
        fillDesignA(a);
        fillIdentityB(b);
        fillDesignB(b);
        std::vector<SharedParams::DrumItem> drums = {a, b};

        std::string path = (dir / "kit.drmpreset").string();
        check(writeDrumPreset(path, drums), "writeDrumPreset succeeds");

        DrumPresetResult result = readDrumPreset(path);
        check(result.ok, "readDrumPreset succeeds");
        check(result.drums.size() == 2, "readDrumPreset returns 2 drums");
        if (result.drums.size() == 2) {
            checkIdentityEqual(result.drums[0], a, "preset drum[0] (Kick)");
            checkDesignEqual(result.drums[0], a, "preset drum[0] (Kick)");
            checkIdentityEqual(result.drums[1], b, "preset drum[1] (HiHat)");
            checkDesignEqual(result.drums[1], b, "preset drum[1] (HiHat)");
        }
    }

    // --- .drmprofile: single drum, design-only. Source drum has REAL,
    // non-default identity (label "Kick" etc.) specifically so a leak
    // would be caught, not accidentally masked by comparing against
    // already-default values.
    {
        SharedParams::DrumItem source;
        fillIdentityA(source);
        fillDesignA(source);

        std::string path = (dir / "design.drmprofile").string();
        check(writeDrumProfile(path, source), "writeDrumProfile succeeds");

        DrumProfileResult result = readDrumProfile(path);
        check(result.ok, "readDrumProfile succeeds");
        checkDesignEqual(result.fields, source, "profile");
        checkIdentityIsDefault(result.fields, "profile");

        // applyDrumProfileDesign onto a DIFFERENT drum with its OWN real
        // identity (HiHat) must overwrite only design fields - the
        // target's identity must survive completely untouched, not reset
        // to default (checkIdentityIsDefault above) and not overwritten
        // by the profile's source identity (there isn't one to leak, but
        // this specifically checks the target keeps ITS OWN values).
        SharedParams::DrumItem target;
        fillIdentityB(target);
        fillDesignB(target); // starts as HiHat's own design, should be replaced by A's
        applyDrumProfileDesign(target, result.fields);
        checkDesignEqual(target, source, "applyDrumProfileDesign target");
        checkIdentityEqual(target, [] {
            SharedParams::DrumItem b;
            fillIdentityB(b);
            return b;
        }(),
                           "applyDrumProfileDesign target (identity preserved)");
    }

    // --- .drmperc: single drum, FULL identity+design together (unlike
    // .drmprofile) - so it must round-trip both, including a kit-relative
    // source and VelSW layers.
    {
        SharedParams::DrumItem source;
        fillIdentityB(source);
        fillDesignB(source);

        std::string path = (dir / "hihat.drmperc").string();
        check(writeDrumPerc(path, source), "writeDrumPerc succeeds");

        DrumPercResult result = readDrumPerc(path);
        check(result.ok, "readDrumPerc succeeds");
        checkIdentityEqual(result.drum, source, "perc");
        checkDesignEqual(result.drum, source, "perc");
    }

    // --- Empty-kit preset (0 drums) round-trips cleanly.
    {
        std::string path = (dir / "empty.drmpreset").string();
        check(writeDrumPreset(path, {}), "writeDrumPreset with 0 drums succeeds");
        DrumPresetResult result = readDrumPreset(path);
        check(result.ok && result.drums.empty(), "readDrumPreset with 0 drums round-trips empty");
    }

    // --- Corrupt/garbage file: both readers must fail gracefully (no
    // crash, ok=false, non-empty error), not throw or read garbage.
    {
        std::string path = (dir / "garbage.bin").string();
        std::ofstream(path, std::ios::binary) << "this is not a preset file, just plain text";

        DrumPresetResult presetResult = readDrumPreset(path);
        check(!presetResult.ok && !presetResult.error.empty(),
             "readDrumPreset rejects a garbage file cleanly");
        DrumProfileResult profileResult = readDrumProfile(path);
        check(!profileResult.ok && !profileResult.error.empty(),
             "readDrumProfile rejects a garbage file cleanly");
        DrumPercResult percResult = readDrumPerc(path);
        check(!percResult.ok && !percResult.error.empty(),
             "readDrumPerc rejects a garbage file cleanly");
    }

    // --- Cross-format rejection: a real .drmpreset/.drmprofile/.drmperc
    // file must not be silently accepted by either of the other two
    // readers (different magic) - each format's own file-type check must
    // actually gate this, not just happen to fail on field-count
    // mismatches.
    {
        std::vector<SharedParams::DrumItem> drums(1);
        std::string presetPath = (dir / "real.drmpreset").string();
        writeDrumPreset(presetPath, drums);
        check(!readDrumProfile(presetPath).ok,
             "readDrumProfile rejects a real .drmpreset file (wrong magic)");
        check(!readDrumPerc(presetPath).ok,
             "readDrumPerc rejects a real .drmpreset file (wrong magic)");

        std::string profilePath = (dir / "real.drmprofile").string();
        writeDrumProfile(profilePath, drums[0]);
        check(!readDrumPreset(profilePath).ok,
             "readDrumPreset rejects a real .drmprofile file (wrong magic)");
        check(!readDrumPerc(profilePath).ok,
             "readDrumPerc rejects a real .drmprofile file (wrong magic)");

        std::string percPath = (dir / "real.drmperc").string();
        writeDrumPerc(percPath, drums[0]);
        check(!readDrumPreset(percPath).ok,
             "readDrumPreset rejects a real .drmperc file (wrong magic)");
        check(!readDrumProfile(percPath).ok,
             "readDrumProfile rejects a real .drmperc file (wrong magic)");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
