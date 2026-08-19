// State shared between the audio thread and the GUI thread. Structure
// mirrors SoloSampler's shared.hpp (sibling project) but the "instrument" is
// a whole drum kit: a list of independent percussion masters instead of one
// region, and 8 stereo outputs instead of 1 - see DrumSfzBuilder.h for how
// the list becomes SFZ text.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "state/DrumKitFlatten.h"

// 8 stereo output busses (16 channels total) - each drum picks one via its
// outputIndex (0-7, "output=" opcode). See plugin.cpp's audioPortsGet/
// plugProcess.
inline constexpr int kNumOutputs = 8;

// Sample tab's loop_mode= combobox, indexed by DrumItem::loopModeIndex.
// Matches SoloSampler's kLoopModes exactly (same order/values - real SFZ
// opcode values, confirmed against sfizz's own Region.cpp/Opcode.cpp), just
// a different default index (1 = "one_shot", not SoloSampler's "none") since
// one-shot is the sensible default for a drum hit. "none" (index 4) is a
// sentinel, not a real SFZ value - DrumSfzBuilder.cpp skips writing
// loop_mode= entirely when this is selected, leaving sfizz to decide for
// itself instead of forcing a mode.
inline constexpr const char* kDrumLoopModes[] = {"no_loop", "one_shot", "loop_continuous",
                                                  "loop_sustain", "none"};

// Fil tab's filtype= combobox, indexed by DrumItem::filterTypeIndex. Ported
// verbatim from SoloSampler's kFilterTypes (sibling project) - same 23
// entries/order, real sfizz filtype= values.
inline constexpr const char* kFilterTypes[23] = {
    "lpf_1p", "lpf_2p", "lpf_2p_sv", "lpf_4p", "lpf_6p",
    "hpf_1p", "hpf_2p", "hpf_2p_sv", "hpf_4p", "hpf_6p",
    "bpf_1p", "bpf_2p", "bpf_2p_sv",
    "brf_1p", "brf_2p", "brf_2p_sv",
    "apf_1p", "pkf_2p",
    "comb", "pink", "lsh", "hsh", "peq"};

// Amp tab's amp_velcurve envelope: one sfz "amp_velcurve_<velocity>=<gain>"
// point per entry (confirmed against the vendored sfizz fork's Region.cpp -
// case hash("amp_velcurve_&"), parameters.back() is the input velocity,
// clamped 0..127 there but the UI restricts it to 1..127 per spec). Sparse
// and user-managed (add/drag/remove in the envelope editor, see
// editor_ui.cpp's drawAmpTab) - an empty ampVelCurve means no amp_velcurve_
// opcodes are written at all, leaving sfizz's own built-in default velocity
// curve in effect. Kept sorted by velocity ascending so drag clamping (never
// letting a point cross a neighbor) doesn't need a re-sort.
struct AmpVelCurvePoint {
    int velocity = 1;   // 1..127 (the "<velocity>" in amp_velcurve_<velocity>=)
    float gain = 1.f;   // 0.0..1.0
};

struct SharedParams {
    // Whole-instrument (not per-drum) MPE toggle, ported from SoloSampler's
    // own SharedParams::mpeEnabled (sibling project) - see
    // SfizzEngine::renderBlock's sfizz_set_mpe_enabled call (engine-level,
    // RT-thread-only, applied fresh every block, so flipping the flag alone
    // needs no SFZ regenerate). Persisted once in CLAP host state (not
    // per-drum, not part of .drmpreset/.drmprofile - those are per-drum/
    // per-kit design files, this is an engine-wide session setting, same
    // category as previewVolume/previewEnabled above which also aren't
    // preset-file material).
    std::atomic<bool> mpeEnabled{false};

    // Whole-instrument (not per-drum) pitch bend range in cents, same
    // pairing/defaults as SoloSampler's own bendUpCents/bendDownCents
    // (sibling project). Unlike mpeEnabled above, DrumSfzBuilder.cpp's
    // <global> bendup=/benddown= header is now ALWAYS written (not gated
    // on mpeEnabled) - these two values are what it writes. Checking
    // "Enable MPE" (editor_ui.cpp) forces both to +-4800 cents (MPE 1.0's
    // 48-semitone convention); the "Bend Range" combobox lets the user set
    // any of the same 4 choices SoloSampler offers regardless of MPE.
    // Persisted alongside mpeEnabled: once in CLAP host state, not part of
    // .drmpreset/.drmprofile.
    std::atomic<int> bendUpCents{2400};
    std::atomic<int> bendDownCents{-2400};

    // File explorer's sample-click preview (plugin.cpp's dedicated
    // previewEngine, always mixed into Out 1 only - see plugProcess's own
    // comment for why not broadcast to every output). previewEnabled gates
    // BOTH whether a click starts a new preview and whether an already-
    // ringing one is currently audible, so switching it off is instant.
    std::atomic<float> previewVolume{0.8f}; // 0..1
    std::atomic<bool> previewEnabled{true};
    // GUI -> audio handoff: set true by a sample-row click (plugin.cpp's
    // previewFile), drained once per block by plugProcess, which retriggers
    // note 60 on previewEngine.
    std::atomic<bool> pendingFilePreviewTrigger{false};

    // GUI -> audio handoff: piano keyboard clicks, auditioned through the
    // MAIN engine (i.e. the real configured kit, whatever master(s) that
    // note happens to hit) - same one-held-key convention as SoloSampler.
    std::atomic<bool> pendingPreviewNoteOn{false};
    std::atomic<int> previewNoteOnNumber{0};
    std::atomic<int> previewNoteOnVelocity{0};
    std::atomic<bool> pendingPreviewNoteOff{false};
    std::atomic<int> previewNoteOffNumber{0};

    // Piano keyboard visualization - mirrors every note the main engine is
    // currently sounding (real incoming MIDI and GUI preview clicks alike).
    // Audio thread writes, GUI thread only reads.
    std::array<std::atomic<bool>, 128> noteActive{};
    std::array<std::atomic<float>, 128> noteVelocity01{};

    // Live main-engine voice count, shown under the piano. Audio thread
    // writes, GUI thread reads.
    std::atomic<int> activeVoiceCount{0};

    // One percussion/drum = one <master> block in the generated SFZ (see
    // DrumSfzBuilder.h). GUI-thread-only data (never touched by the audio
    // thread directly - the audio thread only ever sees the SFZ text already
    // baked into the engine), but state save/load runs on the CLAP host's
    // main thread, which can run concurrently with the GUI's own dedicated
    // thread - hence the mutex, same convention as SoloSampler's GuiState.
    struct DrumItem {
        int id = 0; // stable identity, survives root-note-driven re-sorting
                    // of the list's DISPLAY order (storage order is always
                    // insertion order - see editor_ui.cpp's sortedDrumIds).
        std::string label = "Drum";
        int rootNote = 60;   // 0-127, the master's key= opcode
        int outputIndex = 0; // 0..kNumOutputs-1, the master's output= opcode

        // Empty until a sample/SFZ is loaded (file explorer double-click) -
        // an empty drum contributes no <master>/<region> to the generated
        // SFZ text (silent placeholder, per spec).
        bool hasSource = false;
        bool isSfz = false;
        std::string sourcePath;         // absolute, display-only
        std::string sampleRelativePath; // !isSfz: root("/")-relative sample=
        std::string regionsText;        // isSfz, "whole file as one drum" mode
                                        // (flattenMultisampleSfz's output,
                                        // key-opcode-stripped by
                                        // DrumSfzBuilder.cpp) - used unless
                                        // drumKitModeEnabled below is set.
        int regionCount = 0;            // isSfz: label-only ("N layers")

        // Sample tab's "Drum Kit Mode": isSfz-only, lets the user pick ONE
        // key/drum out of a whole pre-mapped kit .sfz instead of treating
        // the entire file as a single drum (see DrumKitFlatten.h). Populated
        // alongside regionsText above whenever an .sfz is loaded (best-
        // effort - flattenDrumKit failing, e.g. no region has a resolvable
        // key, just leaves this empty and the checkbox disabled; the plain
        // regionsText path above still works either way). Reset to false/0
        // whenever a NEW source is loaded into this drum, since a previous
        // pick only made sense for the previous file's key layout.
        bool drumKitModeEnabled = false;
        int drumKitGroupIndex = 0; // index into drumKitGroups, clamped on use
        std::vector<DrumKitKeyGroup> drumKitGroups; // sorted by key ascending

        // Sample tab (per-drum "instrument design" - independent of which
        // sample/SFZ is loaded, so swapping the source does NOT reset any of
        // these, same as a real sampler's channel strip). Written into this
        // drum's <master> block by DrumSfzBuilder.cpp; ranges/defaults and
        // opcode names all confirmed against the vendored sfizz fork's own
        // Region.cpp (see project memory) rather than assumed.
        int volume = 6;      // -48..48   (volume_oncc117=, <control>'s set_cc117=127
                              // makes this behave like a plain volume= with no
                              // external CC117 sent - same trick as SoloSampler)
        int pan = 0;          // -100..100 (pan=)

        // Pan Random/Alternate, ported verbatim from SoloSampler's Pan tab
        // (sibling project's SfzDocument.cpp/shared.hpp), minus its LFO
        // section and its "x2" checkbox (both dropped per spec). Same
        // CAVEAT as amp_random= vs the originally-spec'd volume_oncc135= a
        // few sessions back: nothing in SoloSampler OR sfzdrummer ever
        // sends a CC136/137 event, so pan_oncc136=/137= only has an
        // audible effect if the user routes a REAL external MIDI CC136 (or
        // 137, see panAlternate) to this drum from their DAW - it's a
        // modulation INPUT, not a self-contained per-note randomizer,
        // despite the "Random" name. Implemented faithfully as asked
        // rather than substituted for a native opcode this time, since
        // (unlike amp_random=) sfizz has no native pan-randomization
        // opcode to substitute in its place.
        int panRandom = 0;         // -200..200 (pan_oncc136=/137=)
        bool panAlternate = false; // false: pan_oncc136=, true: pan_oncc137=
                                    // (mutually exclusive, same panRandom value)

        int width = 100;      // 0..100    (width=)
        int quality = 4;      // 0..9      (sample_quality=)
        int polyphony = 32;   // 1..256    (polyphony=)
        int notePolyphony = 32; // 1..256  (note_polyphony=)
        // note_selfmask=off written only when true - sfizz's own default is
        // "on" (self-masking enabled), so leaving this false omits the
        // opcode entirely rather than writing a redundant note_selfmask=on.
        bool disableNoteSelfmask = false;
        int loopModeIndex = 1; // see kDrumLoopModes, default "one_shot"
        // direction=reverse written only when true - omitted (not
        // direction=forward) when false, matching sfizz's own default.
        bool reverse = false;
        int offset = 0;      // 0..4096   (offset_oncc118=, <control>'s
                              // set_cc118=127 - same static-value trick as volume)
        // "vel2offset": offset_oncc118 modulated by note velocity instead of
        // a real MIDI CC - plugin.cpp synthesizes a CC131 message equal to
        // each note's velocity right before its Note On so offset_oncc131=
        // can track it. Range is NEGATIVE (-4096..0): this vendored sfizz
        // build has no offset_curvecc support (verified against its
        // Region.cpp - offset modulation is plain linear, see
        // RegionStateful.cpp's sampleOffset: finalOffset = offset +
        // depth * ccValue), so a negative depth is how the "harder hit ->
        // less added offset" relationship the curve would have given is
        // achieved instead - at velocity 0 the modulation contributes
        // nothing (0 * depth), at velocity 127 it subtracts up to the full
        // depth from the base `offset` above.
        int vel2offset = 0;  // -4096..0  (offset_oncc131=)

        // Exclusive Class: group=/off_by= only written when this is true -
        // an unset group= would just be "not part of any exclusive class",
        // so gating avoids writing group=0 unconditionally (0 is otherwise a
        // perfectly valid, meaningful group number).
        bool exclusiveClass = false;
        int group = 0;        // 0..99 (group=), only when exclusiveClass
        // offby: only meaningful (and only shown/editable in the UI) when
        // exclusiveClass is also on.
        bool offbyEnabled = false;
        int offby = 0;         // 0..99 (off_by=, aliased "offby=" in sfz spec),
                                // only when exclusiveClass && offbyEnabled

        int transpose = 0;    // -24..24 (transpose=, semitones)
        int tune = 0;          // -99..99 (tune=, cents)

        // Amp tab: sparse amp_velcurve_<N>= points, see AmpVelCurvePoint
        // above. Kept sorted by velocity ascending.
        std::vector<AmpVelCurvePoint> ampVelCurve;

        // Amp tab: amp_veltrack=/amp_random= (both real, native sfizz
        // opcodes - confirmed against Region.cpp/Defaults.cpp/
        // RegionStateful.cpp rather than the user's original spec of
        // volume_oncc135= for "Amp Random", which turned out to be a
        // non-functional opcode even in SoloSampler itself: nothing there
        // ever sends a CC135 event, so that modulation source always reads
        // 0 and contributes nothing. amp_random= is sfizz's OWN built-in
        // per-note gain randomization (RegionStateful.cpp's
        // `fast_real_distribution<float> volumeDistribution{0.0f,
        // region.ampRandom}`) - needs no CC at all, and its native range is
        // -24..24 same as requested, so it was used directly instead.
        int ampVeltrack = 100; // -100..100 (amp_veltrack=)
        int ampRandom = 0;     // -24..24   (amp_random=)

        // Amp envelope: fixed 6-breakpoint flexEG template (eg01_*), same
        // shape SoloSampler uses (see its SfzDocument.cpp) - eg01_ampeg=100
        // routes this generator to amplitude, eg01_sustain=4 designates
        // point 4 (the decay target) as the sustain point. Breakpoints 0
        // and 1 are both fixed instant anchors at level 0 (eg01_time0=
        // eg01_time1=-1, eg01_level0=eg01_level1=0, hardcoded directly in
        // DrumSfzBuilder.cpp - no Start Level or Delay Time control in
        // this UI, per spec).
        //
        // Attack/Hold/Decay floor is 0.00001 (SoloSampler's own "instant"
        // convention), NOT -1, despite the fixed anchor points above using
        // -1 - that -1 idiom only actually works for a stage whose target
        // level equals the stage before it (true for the two anchors, both
        // fixed at 0). For a REAL stage transition, a negative time
        // silently breaks the envelope: FlexEnvelope.cpp's process loop
        // only snaps `currentLevel_` to the stage's target when
        // `stageTime_ == 0` EXACTLY - a negative time takes that branch's
        // `else`, so the level is never actually updated and the envelope
        // gets stuck at the previous stage's level instead of reaching
        // this one's target (confirmed by reading the loop itself, not
        // assumed - this was tried with -1 first and was wrong). 0.00001s
        // is functionally instant for audio purposes while still being a
        // genuine positive duration, so the inner interpolation loop runs
        // and actually reaches the target level.
        float ampAttackTime = 0.00001f;  // 0.00001..0.1 (eg01_time2=)
        float ampHoldTime = 0.00001f;    // 0.00001..0.1 (eg01_time3=)
        float ampDecayTime = 0.00001f;   // 0.00001..0.1 (eg01_time4=, summed with
                                          // ampDecayTimeExtra below)
        // Decay Time (Extra): a second, coarser slider that ADDS onto
        // ampDecayTime above rather than replacing it - practical reason:
        // fine control near zero needs a small max (0.1) to stay usable on
        // a linear slider, but some drums genuinely want a multi-second
        // decay, hence this separate 0.0..12.0 range summed in at
        // DrumSfzBuilder.cpp's eg01_time4= (not a real opcode of its own).
        float ampDecayTimeExtra = 0.f;   // 0.0..12.0
        float ampSustainLevel = 1.f;     // 0.0..1.0    (eg01_level4=)
        float ampReleaseTime = 0.00001f; // 0.00001..12.0 (eg01_time5=)
        // Shape ranges/defaults kept identical to SoloSampler's Amp
        // envelope, per spec ("iguales que en SoloSampler").
        float ampAttackShape = 0.00001f;  // -11..11 (eg01_shape2=)
        float ampDecayShape = -0.3616f;   // -11..11 (eg01_shape4=)
        float ampReleaseShape = -6.3616f; // -11..11 (eg01_shape5=)

        // Velocity-modulation depths riding the SAME synthetic CC131
        // (=note velocity, sent by plugin.cpp's plugProcess right before
        // every Note On) already used by the Sample tab's vel2offset - no
        // new plumbing needed, just three more opcodes bound to that CC.
        // vel2attack is negative-range (like vel2offset): harder hits pull
        // Attack Time down toward/past zero (faster attack), same
        // "negative depth as inversion" idiom used elsewhere in this
        // project. At extreme settings (Attack Time near its 0.00001
        // floor plus a large negative vel2attack at high velocity) the
        // EFFECTIVE time can go negative, which on its own would hit the
        // same stuck-level bug the comment above describes - but Hold's
        // target level is also 1.0 (same as Attack's) with its own
        // guaranteed-positive floor, so Hold's own interpolation reaches
        // the correct level a few microseconds later regardless,
        // inaudibly. vel2sustain's range matches flexEGPointLevelMod
        // exactly (Defaults.cpp: {-1.0, 1.0}, kEnforceBounds).
        float ampVel2Attack = 0.f;  // -0.1..0.0 (eg01_time2_oncc131=)
        float ampVel2Sustain = 0.f; // -1.0..1.0 (eg01_level4_oncc131=)
        int ampVel2Volume = 0;      // -24..24   (volume_oncc131=)

        // Fil tab: ported verbatim from SoloSampler's Filter tab (sibling
        // project's shared.hpp/SfzDocument.cpp), minus fil_keycenter=/
        // fil_keytrack=/LFO per spec. filterTypeIndex indexes kFilterTypes
        // above.
        int filterTypeIndex = 1;      // "lpf_2p" (filtype=)
        int filterCutoff = 20000;     // 1..20000    (cutoff=)
        float filterResonance = 0.f;  // -40..40 dB  (resonance=)
        // filterRandomCutoff: cutoff_oncc135= - same external-modulation-
        // input caveat as amp_random=/pan_oncc136=/137= a couple sessions
        // back (CC135 is never sent by either plugin; SoloSampler's own
        // comment confirms it's a shared "random slot" meant to be driven
        // by a real external MIDI CC, not an internal RNG).
        int filterRandomCutoff = 0;   // 1..20000    (cutoff_oncc135=)
        // filVeltrack rides the REAL synthetic CC131=velocity trick (same
        // one used by vel2offset/vel2attack/etc) - genuinely functional,
        // unlike Random Cutoff above. Opcode NAME (not value) depends on
        // filterEgEnabled: cutoff_oncc131= when the EG is off (modulates
        // the static cutoff= directly), eg02_cutoff_oncc131= when it's on
        // (modulates the EG's overall depth instead) - see
        // DrumSfzBuilder.cpp, matches SoloSampler exactly.
        int filVeltrack = 0;          // 0..20000
        int resoVeltrack = 0;         // -40..40     (resonance_oncc131=)

        // Filter EG (eg02_*, same fixed 6-breakpoint flexEG template as
        // the Amp tab's eg01_*, routed to filter cutoff via eg02_cutoff=
        // instead of eg01_ampeg=100 - confirmed against Region.cpp's
        // `case hash("eg&_cutoff&")`, which - like the plain "cutoff="
        // opcode itself - accepts the trailing filter-index digit being
        // omitted, defaulting to filter 1) - gated behind filterEgEnabled
        // (checkbox, unlike the Amp EG which is always on), matching
        // SoloSampler exactly including its own already-correct 0.00001
        // "instant" floors (no -1-sentinel mistake to avoid porting here -
        // that bug was this project's own invention for the Amp tab, not
        // something SoloSampler's Filter tab ever had).
        bool filterEgEnabled = false;
        int filDepth = 0;                   // 0..20000      (eg02_cutoff=)
        float filEgStartLevel = 0.f;        // 0.0..1.0
        float filEgDelayTime = 0.00001f;    // 0.00001..0.1
        float filEgAttackTime = 0.00001f;   // 0.00001..0.1
        float filEgAttackShape = 0.00001f;  // -11..11
        float filEgHoldTime = 0.00001f;     // 0.00001..0.1
        float filEgDecayTime = 0.00001f;    // 0.00001..0.1 (eg02_time4=, summed
                                             // with filEgDecayTimeExtra below)
        float filEgDecayTimeExtra = 0.f;    // 0.0..12.0 - see ampDecayTimeExtra's
                                             // comment for the practical rationale
        float filEgDecayShape = 0.00001f;   // -11..11
        float filEgSustainLevel = 1.f;      // 0.0..1.0
        float filEgReleaseTime = 0.00001f;  // 0.00001..0.1
        float filEgReleaseShape = 0.00001f; // -11..11

        // Pitch tab: ported verbatim from SoloSampler's Pitch tab (sibling
        // project's shared.hpp/SfzDocument.cpp), minus pitch_keytrack= and
        // portamento/glide (eg04_*) per spec. Pitch EG (eg03_*) time ranges
        // match sfzdrummer's OWN Fil tab (0.00001..0.1, Decay 0.00001..1.0)
        // rather than SoloSampler's original 0.00001..4.0 across the
        // board - explicit instruction this round ("mismos rangos que el
        // de Filter de sfzdrummer").
        int pitchVeltrack = 0; // -9600..9600 (pitch_veltrack=)
        int pitchRandom = 0;   // 0..9600     (pitch_random=)

        bool pitchEgEnabled = false;
        int pitchDepth = 0; // -9600..9600 (eg03_pitch=)
        // vel2pitch: NOT in SoloSampler - new, added per explicit request
        // this round. Rides the same synthetic CC131=velocity trick as
        // vel2offset/vel2attack/etc (plugin.cpp's plugProcess), but targets
        // the EG's pitch-routing modulation depth itself
        // (eg03_pitch_oncc131= - confirmed real via Region.cpp's
        // `case_any_ccN("eg&_pitch")` -> EG_target_cc, Defaults.cpp's
        // pitchMod spec: native range -12000..12000 cents, permissive/
        // unclamped) rather than a per-breakpoint time/level modulation -
        // a SEPARATE depth term added alongside eg03_pitch= above, scaled
        // by velocity. Range mirrors pitchDepth's own -9600..9600 for UI
        // consistency (same unit/scale); bidirectional (unlike
        // vel2offset/vel2attack's negative-only range) since there's no
        // pre-existing direction here to invert via sign.
        int pitchVel2Depth = 0;               // -9600..9600 (eg03_pitch_oncc131=)
        // "Invert vel2pitch": writes eg03_pitch_curvecc131=2 when true.
        // Confirmed real and distinct from the earlier offset_curvecc
        // finding (which genuinely doesn't exist - RegionStateful.cpp's
        // sampleOffset() has no curve lookup at all) - this one goes
        // through the GENERIC CC-modulation connection framework instead
        // (Region.cpp's processGenericCc), which DOES support a per-
        // connection curve index: `switch (opcode.category) { case
        // kOpcodeCurveCcN: p.curve = opcode.read(Default::curveCC); ...}`
        // - same mechanism SoloSampler's pan_curvecc<N>=1 already relies
        // on (ported into this project's Pan Random). Default::curveCC's
        // own range is 0..255 (Defaults.cpp), so "2" is a valid, in-range
        // curve index, not a depth value.
        bool pitchVel2Invert = false;
        float pitchEgStartLevel = 0.f;        // 0.0..1.0
        float pitchEgDelayTime = 0.00001f;    // 0.00001..0.1
        float pitchEgAttackTime = 0.00001f;   // 0.00001..0.1
        float pitchEgAttackShape = 0.00001f;  // -11..11
        float pitchEgHoldTime = 0.00001f;     // 0.00001..0.1
        float pitchEgDecayTime = 0.00001f;    // 0.00001..0.1 (eg03_time4=, summed
                                               // with pitchEgDecayTimeExtra below)
        float pitchEgDecayTimeExtra = 0.f;    // 0.0..12.0 - see ampDecayTimeExtra's
                                               // comment for the practical rationale
        float pitchEgDecayShape = 0.00001f;   // -11..11
        float pitchEgSustainLevel = 1.f;      // 0.0..1.0
        float pitchEgReleaseTime = 0.00001f;  // 0.00001..0.1
        float pitchEgReleaseShape = 0.00001f; // -11..11

        // Opcodes tab: free-form raw SFZ text, copied verbatim into THIS
        // drum's own <master> block (after every other opcode above,
        // before its <region>/regionsText - see DrumSfzBuilder.cpp) - same
        // "written as-is, no validation" mechanism as SoloSampler's
        // Opcodes tab (sibling project), just scoped per-drum here instead
        // of once per whole instrument, since each drum already has its
        // own independent <master>.
        std::string customOpcodesText = "//custom opcodes here";
    };

    struct GuiState {
        std::mutex mutex;
        std::vector<DrumItem> drums;
        int nextId = 1;
        // -1 = nothing selected. The per-drum config tabs (Sample/Amp/Fil/
        // Pitch/Opcodes) and the piano's right-click root-note assignment
        // both target whichever drum this identifies.
        int selectedId = -1;

        // Last file-load/preview failure (unsupported extension, no drum
        // selected, a bad .sfz) - shown as a temporary corner toast (see
        // editor_ui.cpp's drawErrorToast) rather than inline, so the text
        // isn't cramped into the narrow file-explorer column. Cleared on the
        // next successful load, or by drawErrorToast once it's aged past its
        // display duration. listErrorSetAt is meaningless while listError is
        // empty - always set together (see plugin.cpp's setListError).
        std::string listError;
        std::chrono::steady_clock::time_point listErrorSetAt{};
    } guiState;
};
