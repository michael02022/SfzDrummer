// .drmpreset / .drmprofile file format - local-only save/recall of a
// sfzdrummer kit or single-drum design, independent of (and NOT wire-
// compatible with) the CLAP host state format in plugin.cpp's
// stateSave/stateLoad. Mirrors the MECHANISM SoloSampler uses for its own
// .sspreset/.ssprofile (sibling project) - a separate tiny binary
// serializer with its own magic/version - but NOT its exact shape:
// SoloSampler is a single instrument, so both its formats share one
// PresetFields struct gated by a "kind" byte (extra stack section or not).
// sfzdrummer's two formats are shaped completely differently instead:
//   .drmpreset  - the WHOLE kit: every drum, its identity (sample/sfz
//                 source, root note, output, label, Drum Kit Mode) AND its
//                 design (Sample tab's instrument-design section through
//                 Opcodes) together - same "baked regionsText, never
//                 re-flattened on load" property the CLAP state format
//                 already has for .sfz-sourced drums.
//   .drmprofile - ONE drum's design ONLY, no identity at all (not just the
//                 sample/sfz source - label/rootNote/outputIndex are left
//                 out too) - the whole point is reusing a sound design on
//                 a DIFFERENT drum/placement, so a Profile that silently
//                 renamed/relocated/rerouted its target would defeat that.
// Given the shapes differ this much, each format gets its own magic number
// instead of sharing one with a kind discriminator.
#pragma once

#include <string>
#include <vector>

#include "shared.hpp"

bool writeDrumPreset(const std::string& path,
                     const std::vector<SharedParams::DrumItem>& drums);

struct DrumPresetResult {
    bool ok = false;
    std::string error;
    std::vector<SharedParams::DrumItem> drums;
};
// A successful result's drums have id==0 (the caller reassigns stable ids
// on load, same as plugin.cpp's stateLoad does for CLAP host state).
DrumPresetResult readDrumPreset(const std::string& path);

bool writeDrumProfile(const std::string& path, const SharedParams::DrumItem& drum);

struct DrumProfileResult {
    bool ok = false;
    std::string error;
    // Only the design fields are meaningful here - identity fields
    // (id/label/rootNote/outputIndex/source/Drum Kit Mode) are left at
    // their SharedParams::DrumItem defaults, never read from the file.
    // Callers must copy design fields onto their target drum explicitly
    // (see plugin.cpp's applyDrumProfile) rather than assigning this whole
    // struct over an existing drum, so a future identity field added to
    // DrumItem can never silently leak a default value onto it.
    SharedParams::DrumItem fields;
};
DrumProfileResult readDrumProfile(const std::string& path);

// Copies ONLY the design fields (the same set writeDrumDesign/
// readDrumDesign operate on internally) from `source` onto `target`,
// leaving every identity field `target` already had (id/label/rootNote/
// outputIndex/hasSource/isSfz/sourcePath/sampleRelativePath/regionsText/
// regionCount/drumKitMode*) completely untouched - the sanctioned way to
// apply a DrumProfileResult::fields onto a real drum. Keep in sync with
// writeDrumDesign/readDrumDesign in PresetFile.cpp whenever a new
// instrument-design field is added (same discipline plugin.cpp's own
// versioned CLAP state read/write already follows for DrumItem as a
// whole).
void applyDrumProfileDesign(SharedParams::DrumItem& target, const SharedParams::DrumItem& source);
