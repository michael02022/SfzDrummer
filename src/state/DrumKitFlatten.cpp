#include "DrumKitFlatten.h"

#include <cctype>
#include <map>
#include <sstream>
#include <unordered_map>

#include "SfzParsePipeline.h"

namespace {

// Parses an SFZ key/note value: either a plain MIDI number ("60") or SFZ's
// note-name syntax ("c4", "cs3", "db-1", ...; '#'/'s' for sharp, 'b' for
// flat), using the standard c4=60 (middle C) convention sfizz itself
// defaults to. Real-world drum kits mix both conventions freely, so both
// need to resolve to the same grouping key. Returns -1 on anything
// unparseable or out of MIDI range.
int parseSfzNoteValue(const std::string& raw) {
    if (raw.empty()) return -1;

    if (std::isdigit(static_cast<unsigned char>(raw[0])) || raw[0] == '-') {
        try {
            size_t idx = 0;
            int v = std::stoi(raw, &idx);
            if (idx == raw.size() && v >= 0 && v <= 127) return v;
        } catch (...) {
        }
        return -1;
    }

    static const std::unordered_map<char, int> kBase = {
        {'c', 0}, {'d', 2}, {'e', 4}, {'f', 5}, {'g', 7}, {'a', 9}, {'b', 11},
    };
    auto it = kBase.find(static_cast<char>(std::tolower(static_cast<unsigned char>(raw[0]))));
    if (it == kBase.end()) return -1;
    int pitchClass = it->second;

    size_t i = 1;
    if (i < raw.size() && (raw[i] == '#' || std::tolower(static_cast<unsigned char>(raw[i])) == 's')) {
        pitchClass += 1;
        ++i;
    } else if (i < raw.size() && std::tolower(static_cast<unsigned char>(raw[i])) == 'b') {
        pitchClass -= 1;
        ++i;
    }
    if (i >= raw.size()) return -1;

    try {
        size_t idx = 0;
        int octave = std::stoi(raw.substr(i), &idx);
        if (i + idx != raw.size()) return -1;
        int midi = (octave + 1) * 12 + pitchClass;
        return (midi >= 0 && midi <= 127) ? midi : -1;
    } catch (...) {
        return -1;
    }
}

// A region's effective key, in priority order: key= (the SFZ shorthand for
// lokey=hikey=pitch_keycenter=), else lokey==hikey (a single-note range),
// else pitch_keycenter= alone, else lokey= alone as a last-resort fallback
// for a region with a real key RANGE (uncommon in a drum kit, where each
// piece normally owns exactly one key - see DrumKitFlatten.h). -1 if none of
// these resolve to a usable value.
int computeRegionKey(const SfzOpcodeList& region) {
    if (const std::string* v = sfzOpcodeFind(region, "key")) {
        int k = parseSfzNoteValue(*v);
        if (k >= 0) return k;
    }
    const std::string* lokey = sfzOpcodeFind(region, "lokey");
    const std::string* hikey = sfzOpcodeFind(region, "hikey");
    if (lokey && hikey) {
        int lo = parseSfzNoteValue(*lokey);
        int hi = parseSfzNoteValue(*hikey);
        if (lo >= 0 && lo == hi) return lo;
    }
    if (const std::string* v = sfzOpcodeFind(region, "pitch_keycenter")) {
        int k = parseSfzNoteValue(*v);
        if (k >= 0) return k;
    }
    if (lokey) {
        int lo = parseSfzNoteValue(*lokey);
        if (lo >= 0) return lo;
    }
    return -1;
}

} // namespace

FlattenedDrumKit flattenDrumKit(const std::string& sfzPath) {
    FlattenedDrumKit result;
    ParsedSfzRegions parsed = parseAndFlattenSfz(sfzPath);
    if (!parsed.ok) {
        result.error = parsed.error;
        return result;
    }

    // std::map keeps insertion in ascending key order for free, which is
    // exactly the order the Sample tab's key slider should present.
    std::map<int, std::vector<const SfzOpcodeList*>> byKey;
    for (const auto& region : parsed.regions) {
        int key = computeRegionKey(region);
        if (key < 0) continue; // no resolvable key mapping - can't slot into one drum
        byKey[key].push_back(&region);
    }
    if (byKey.empty()) {
        result.error = "No regions with a resolvable key mapping found";
        return result;
    }

    result.keyGroups.reserve(byKey.size());
    for (auto& [key, regionPtrs] : byKey) {
        DrumKitKeyGroup group;
        group.key = key;
        group.regionCount = static_cast<int>(regionPtrs.size());
        std::ostringstream out;
        for (const auto* region : regionPtrs) {
            out << "<region>\n";
            for (const auto& kv : *region) out << kv.first << "=" << kv.second << "\n";
        }
        group.regionsText = out.str();
        result.keyGroups.push_back(std::move(group));
    }
    result.ok = true;
    return result;
}
