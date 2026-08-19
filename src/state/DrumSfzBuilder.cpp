#include "DrumSfzBuilder.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace {

// Which regionsText a drum actually plays: Drum Kit Mode (Sample tab)
// swaps in one picked key group from the source .sfz's full mapping instead
// of the whole-file blob, see shared.hpp's DrumItem::drumKitModeEnabled.
// Falls back to the whole-file blob if drum kit mode is on but somehow has
// no groups (e.g. a stale/corrupt state load) - never silently emits
// nothing for a drum the user believes has a source loaded.
const std::string& activeRegionsText(const SharedParams::DrumItem& d) {
    if (d.drumKitModeEnabled && !d.drumKitGroups.empty()) {
        int idx = std::clamp(d.drumKitGroupIndex, 0, static_cast<int>(d.drumKitGroups.size()) - 1);
        return d.drumKitGroups[static_cast<size_t>(idx)].regionsText;
    }
    return d.regionsText;
}

// Drops any region-level key/lokey/hikey/pitch_keycenter line from a
// flattened .sfz's regions text, so the <master>'s key= (set from this
// drum's assigned root note) is always the one that actually wins - a
// source .sfz that already carries its own key mapping (e.g. re-imported
// from a full kit) must not silently override the root note the user just
// assigned on the piano. Lines come from SfzFlatten.cpp's own output, always
// exactly "opcode=value\n", one opcode per line, so a plain prefix check is
// enough (no need for a real SFZ tokenizer here).
std::string stripKeyOpcodes(const std::string& regionsText) {
    static const char* kStripPrefixes[] = {"key=", "lokey=", "hikey=",
                                           "pitch_keycenter="};
    std::istringstream in(regionsText);
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        bool strip = false;
        for (const char* prefix : kStripPrefixes) {
            size_t len = std::char_traits<char>::length(prefix);
            if (line.compare(0, len, prefix) == 0) {
                strip = true;
                break;
            }
        }
            if (!strip) out << line << "\n";
    }
    return out.str();
}

// Per-drum "instrument design" opcodes (Sample + Amp tabs), written right
// after this drum's <master> key=/output= line so they cascade as defaults
// to every region under it (round robins/velocity layers included, whether
// from a plain sample or a picked Drum Kit Mode group). See shared.hpp's
// DrumItem fields for the opcode/range/default rationale.
void writeMasterOpcodes(std::ostringstream& sfz, const SharedParams::DrumItem& d) {
    sfz << "volume_oncc117=" << d.volume << "\n"
        << "pan=" << d.pan << "\n";

    // Pan Random/Alternate - verbatim port of SoloSampler's Pan tab (minus
    // LFO and its "x2" checkbox), see shared.hpp's comment on the
    // panRandom/panAlternate fields for the CC136/137
    // external-modulation-input caveat.
    if (d.panAlternate)
        sfz << "pan_oncc137=" << d.panRandom << "\n"
            << "pan_curvecc137=1\n";
    else
        sfz << "pan_oncc136=" << d.panRandom << "\n"
            << "pan_curvecc136=1\n";

    sfz << "width=" << d.width << "\n"
        << "sample_quality=" << d.quality << "\n"
        << "polyphony=" << d.polyphony << "\n"
        << "note_polyphony=" << d.notePolyphony << "\n";
    if (d.disableNoteSelfmask) sfz << "note_selfmask=off\n";
    // "none" (index 4 in kDrumLoopModes) is a sentinel, not a real SFZ
    // value - skip the opcode entirely so sfizz decides for itself (loop if
    // the sample has embedded loop points, one-shot otherwise).
    const char* loopMode = kDrumLoopModes[d.loopModeIndex];
    if (std::string_view(loopMode) != "none") sfz << "loop_mode=" << loopMode << "\n";
    if (d.reverse) sfz << "direction=reverse\n";
    sfz << "offset_oncc118=" << d.offset << "\n"
        // "vel2offset": CC131 is not a real controller - plugin.cpp sends it
        // as a synthetic CC matching each note's velocity right before that
        // note's Note On (see plugProcess), so this tracks velocity instead
        // of an actual MIDI CC. d.vel2offset is negative (-4096..0, see
        // shared.hpp), so harder hits SUBTRACT from the base offset above
        // instead of adding to it - no offset_curvecc needed for that
        // direction, since this vendored sfizz build's offset modulation is
        // plain linear (depth * ccValue) and a negative depth already gives
        // the inverse relationship on its own.
        << "offset_oncc131=" << d.vel2offset << "\n";
    if (d.exclusiveClass) {
        sfz << "group=" << d.group << "\n";
        if (d.offbyEnabled) sfz << "off_by=" << d.offby << "\n";
    }
    sfz << "transpose=" << d.transpose << "\n"
        << "tune=" << d.tune << "\n";

    // Amp tab: amp_veltrack=/amp_random= (native opcodes, see shared.hpp's
    // comment on why amp_random= replaces the originally-spec'd
    // volume_oncc135=) plus vel2volume riding the synthetic CC131=velocity
    // trick (same mechanism as vel2offset above).
    sfz << "amp_veltrack=" << d.ampVeltrack << "\n"
        << "amp_random=" << d.ampRandom << "\n"
        << "volume_oncc131=" << d.ampVel2Volume << "\n";

    // Amp envelope: fixed 6-breakpoint flexEG template, always routed to
    // amplitude (eg01_ampeg=100) with point 4 as the sustain point
    // (eg01_sustain=4) - see shared.hpp's DrumItem fields for the
    // breakpoint-by-breakpoint rationale (point 0/1 are fixed instant
    // anchors, point 1 replaces SoloSampler's user-editable Delay stage).
    // std::fixed/setprecision(5) matches SoloSampler's own float opcode
    // formatting (kept in effect for the rest of the stream too - harmless,
    // int-typed opcodes are unaffected by float formatting flags).
    sfz << std::fixed << std::setprecision(5)
        << "eg01_ampeg=100\n"
        << "eg01_sustain=4\n"
        << "eg01_level0=0 eg01_time0=-1\n"
        << "eg01_level1=0 eg01_time1=-1\n"
        << "eg01_level2=1 eg01_time2=" << d.ampAttackTime
        << " eg01_shape2=" << d.ampAttackShape
        << " eg01_time2_oncc131=" << d.ampVel2Attack << "\n"
        << "eg01_level3=1 eg01_time3=" << d.ampHoldTime << "\n"
        << "eg01_level4=" << d.ampSustainLevel << " eg01_time4="
        << (d.ampDecayTime + d.ampDecayTimeExtra)
        << " eg01_shape4=" << d.ampDecayShape
        << " eg01_level4_oncc131=" << d.ampVel2Sustain << "\n"
        << "eg01_level5=0 eg01_time5=" << d.ampReleaseTime
        << " eg01_shape5=" << d.ampReleaseShape << "\n";

    // Amp tab's envelope (shared.hpp's DrumItem::ampVelCurve) - one
    // amp_velcurve_<velocity>=<gain> line per point, already kept sorted by
    // velocity by the editor (see editor_ui.cpp's drawAmpTab); sorted again
    // here defensively since sfz doesn't care about order but a predictable
    // one makes the generated text easier to eyeball. Nothing is written at
    // all when the vector is empty, leaving sfizz's own default velocity
    // curve in effect.
    if (!d.ampVelCurve.empty()) {
        std::vector<AmpVelCurvePoint> pts = d.ampVelCurve;
        std::sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) {
            return a.velocity < b.velocity;
        });
        for (const auto& pt : pts)
            sfz << "amp_velcurve_" << pt.velocity << "=" << pt.gain << "\n";
    }

    // Fil tab - verbatim port of SoloSampler's Filter tab (minus
    // fil_keycenter=/fil_keytrack=/LFO), see shared.hpp's DrumItem fields
    // for the opcode/caveat rationale. filVeltrack's opcode NAME (not
    // value) depends on filterEgEnabled, matching SoloSampler exactly.
    sfz << "filtype=" << kFilterTypes[d.filterTypeIndex] << "\n"
        << "cutoff=" << d.filterCutoff << "\n"
        << "resonance=" << d.filterResonance << "\n"
        << "cutoff_oncc135=" << d.filterRandomCutoff << "\n"
        << (d.filterEgEnabled ? "eg02_cutoff_oncc131=" : "cutoff_oncc131=") << d.filVeltrack << "\n"
        << "resonance_oncc131=" << d.resoVeltrack << "\n";
    if (d.filterEgEnabled) {
        sfz << "eg02_cutoff=" << d.filDepth << "\n"
            << "eg02_sustain=4\n"
            << "eg02_level0=" << d.filEgStartLevel << " eg02_time0=-1\n"
            << "eg02_level1=" << d.filEgStartLevel << " eg02_time1=" << d.filEgDelayTime << "\n"
            << "eg02_level2=1 eg02_time2=" << d.filEgAttackTime
            << " eg02_shape2=" << d.filEgAttackShape << "\n"
            << "eg02_level3=1 eg02_time3=" << d.filEgHoldTime << "\n"
            << "eg02_level4=" << d.filEgSustainLevel << " eg02_time4="
            << (d.filEgDecayTime + d.filEgDecayTimeExtra)
            << " eg02_shape4=" << d.filEgDecayShape << "\n"
            << "eg02_level5=0 eg02_time5=" << d.filEgReleaseTime
            << " eg02_shape5=" << d.filEgReleaseShape << "\n";
    }

    // Pitch tab - verbatim port of SoloSampler's Pitch tab (minus
    // pitch_keytrack=/portamento/LFO), see shared.hpp's DrumItem fields
    // for the opcode/caveat rationale. eg03_pitch_oncc131= (vel2pitch) is
    // gated alongside eg03_pitch= under the same pitchEgEnabled checkbox -
    // both share the same EG-to-pitch routing, so neither means anything
    // without it.
    sfz << "pitch_veltrack=" << d.pitchVeltrack << "\n"
        << "pitch_random=" << d.pitchRandom << "\n";
    if (d.pitchEgEnabled) {
        sfz << "eg03_pitch=" << d.pitchDepth << "\n"
            << "eg03_pitch_oncc131=" << d.pitchVel2Depth << "\n";
        if (d.pitchVel2Invert) sfz << "eg03_pitch_curvecc131=2\n";
        sfz << "eg03_sustain=4\n"
            << "eg03_level0=" << d.pitchEgStartLevel << " eg03_time0=-1\n"
            << "eg03_level1=" << d.pitchEgStartLevel << " eg03_time1=" << d.pitchEgDelayTime << "\n"
            << "eg03_level2=1 eg03_time2=" << d.pitchEgAttackTime
            << " eg03_shape2=" << d.pitchEgAttackShape << "\n"
            << "eg03_level3=1 eg03_time3=" << d.pitchEgHoldTime << "\n"
            << "eg03_level4=" << d.pitchEgSustainLevel << " eg03_time4="
            << (d.pitchEgDecayTime + d.pitchEgDecayTimeExtra)
            << " eg03_shape4=" << d.pitchEgDecayShape << "\n"
            << "eg03_level5=0 eg03_time5=" << d.pitchEgReleaseTime
            << " eg03_shape5=" << d.pitchEgReleaseShape << "\n";
    }

    // Opcodes tab: raw user text, appended verbatim after every other
    // <master> opcode above and still inside THIS drum's own <master>
    // block (per spec - each drum's free-form opcodes are scoped to it
    // alone, unlike SoloSampler's single-instrument <global> equivalent).
    // Same mechanism as SoloSampler's own Opcodes tab otherwise: written
    // as-is, no validation - a typo here just becomes an unrecognized
    // opcode sfizz silently ignores, same as anywhere else in this file.
    if (!d.customOpcodesText.empty()) sfz << d.customOpcodesText << "\n";
}

} // namespace

std::string buildDrumSfzText(const std::vector<SharedParams::DrumItem>& drums, int bendUpCents,
                              int bendDownCents) {
    std::ostringstream sfz;

    // <control>, at the very start of the generated file: set_cc117/118's
    // default to 127 (full) so, with no external CC ever received,
    // volume_oncc117=/offset_oncc118= behave exactly like plain static
    // volume=/offset= opcodes - the same trick SoloSampler uses (see
    // ExplicacionPlugin.txt) - then label_key<N>=<name> for every drum, the
    // ARIA-standard way an SFZ names a note (sfizz itself parses and
    // exposes this, see sfizz_get_key_label_text). Reaper's piano roll
    // doesn't read label_key directly though; that's driven by sfzdrummer's
    // own CLAP clap.note-name extension (plugin.cpp), which mirrors the
    // same label/rootNote pairs - the opcode here is just the SFZ-side copy
    // of that same information, per spec.
    sfz << "<control>\n"
        << "set_cc117=127 // volume\n"
        << "set_cc118=127 // offset\n";
    for (const auto& d : drums) {
        if (d.label.empty()) continue;
        sfz << "label_key" << d.rootNote << "=" << d.label << "\n";
    }

    // <global>, before the <master> list per spec: bend range is always
    // written (same as SoloSampler's own always-present bendup=/benddown=,
    // sibling project) - the "Bend Range" combobox (editor_ui.cpp) picks
    // the value normal, non-MPE use should hear; checking "Enable MPE"
    // forces it to +-4800 cents (MPE 1.0's 48-semitone per-note pitch bend
    // convention) but leaves it there on uncheck, same as SoloSampler.
    sfz << "<global>\n"
        << "bendup=" << bendUpCents << "\n"
        << "benddown=" << bendDownCents << "\n";

    for (const auto& d : drums) {
        if (!d.hasSource) continue;

        sfz << "<master> key=" << d.rootNote << " output=" << d.outputIndex << "\n";
        writeMasterOpcodes(sfz, d);
        if (d.isSfz) {
            sfz << stripKeyOpcodes(activeRegionsText(d));
        } else {
            sfz << "<region>\n" << "sample=" << d.sampleRelativePath << "\n";
        }
    }
    return sfz.str();
}
