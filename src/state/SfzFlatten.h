// Flattens a real multi-region .sfz instrument into ONE drum: every region
// (round robins, velocity layers, whatever keys it originally mapped to)
// collapses into a single blob, later wrapped under one <master key=...> by
// DrumSfzBuilder.cpp - the "just play the whole thing on one key" mode. See
// DrumKitFlatten.h for the alternative "one drum PER key found" mode
// (SfzParsePipeline.h/.cpp is the parser core both share).
#pragma once

#include <string>

struct FlattenedSfz {
    bool ok = false;
    std::string error;       // set iff !ok: missing file, circular include,
                              // zero regions found, etc.
    std::string regionsText; // concatenated "<region> key=val ...\n" blocks,
                              // cleaned to the basic opcode allowlist, with
                              // every sample= already rewritten relative to
                              // sfzdrummer's virtual "/" root - ready to
                              // splice into buildDrumSfzText verbatim.
    int regionCount = 0;
};

// GUI thread only (file dialog accept / XDND drop) - same threading
// convention as analyzeSampleFile() in SampleInfo.h.
FlattenedSfz flattenMultisampleSfz(const std::string& sfzPath);
