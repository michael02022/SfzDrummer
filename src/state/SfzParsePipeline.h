// Shared preprocess -> tokenize -> flatten -> clean-opcodes -> rewrite-
// sample= pipeline behind both SfzFlatten.h's flattenMultisampleSfz (a
// picked .sfz becomes ONE drum, every region merged into a single blob) and
// DrumKitFlatten.h's flattenDrumKit (a picked .sfz becomes ONE drum PER key
// found in the file) - both need the exact same parser, they only differ in
// how the resulting regions get grouped/formatted afterward.
#pragma once

#include <string>
#include <utility>
#include <vector>

// Ordered "dict" of opcode=value pairs (insertion order preserved, matching
// Python dict.update() semantics) - see SfzParsePipeline.cpp for why this
// ordering matters (cascade merge order, "sample first").
using SfzOpcodeList = std::vector<std::pair<std::string, std::string>>;

// Looks up `key` in an SfzOpcodeList; nullptr if absent. Read-only - callers
// that need to grade a value must copy it out.
const std::string* sfzOpcodeFind(const SfzOpcodeList& list, const std::string& key);

struct ParsedSfzRegions {
    bool ok = false;
    std::string error;                  // set iff !ok
    std::vector<SfzOpcodeList> regions; // cleaned to the basic opcode allowlist,
                                        // sample= already rewritten relative to
                                        // sfzdrummer's virtual "/" root
};

// GUI thread only (file dialog accept / XDND drop), same convention as
// flattenMultisampleSfz/flattenDrumKit.
ParsedSfzRegions parseAndFlattenSfz(const std::string& sfzPath);
