#include "SfzParsePipeline.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <utility>

// Faithful C++ port of proto/sfzflat.py's preprocess -> tokenize ->
// flatten_events -> clean_opcodes pipeline (see SoloSampler's identically-
// named proto/sfzflat.py, the Python reference this was ported from), plus
// the sfzdrummer-specific step of rewriting every sample= to be relative to
// the plugin's virtual "/" root (see DrumSfzBuilder.cpp/plugin.cpp's
// regenerateAndLoadDrumSfz for that convention - sfizz is always told the
// SFZ "path" is "/", so any real absolute path must have its leading '/'
// stripped before being written as sample=).
//
// Keep this file's structure/ordering aligned with proto/sfzflat.py's three
// phases so the two can be diffed against each other by eye.

namespace fs = std::filesystem;

namespace {

void dictSet(SfzOpcodeList& list, const std::string& key, const std::string& value) {
    for (auto& kv : list) {
        if (kv.first == key) {
            kv.second = value;
            return;
        }
    }
    list.emplace_back(key, value);
}

// Mutable lookup (needed for the sample= rewrite step below) - kept private,
// distinct from the header's read-only sfzOpcodeFind.
std::string* dictFindMut(SfzOpcodeList& list, const std::string& key) {
    for (auto& kv : list)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

std::string trim(const std::string& s) {
    static const char* kWhitespace = " \t\n\r\v\f";
    size_t start = s.find_first_not_of(kWhitespace);
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(kWhitespace);
    return s.substr(start, end - start + 1);
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ---------------------------------------------------------------------
// Phase 1: preprocess (#define, #include, comments)
// ---------------------------------------------------------------------

std::string readSfzText(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Could not open file: " + path);
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string text = ss.str();
    // SFZ always uses "/" as a path separator regardless of the OS it was
    // authored on - normalize up front, same as sfzflat.py's read_sfz_text.
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

// Strips a // comment to end-of-line, respecting "quoted" spans (so an
// #include "path//weird" isn't cut by accident).
std::string stripComment(const std::string& line) {
    bool inQuotes = false;
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            inQuotes = !inQuotes;
        } else if (!inQuotes && c == '/' && line[i + 1] == '/') {
            return line.substr(0, i);
        }
    }
    return line;
}

// Substitutes every $NAME with its define value, word-boundary aware (so
// $EXT doesn't match inside $EXTRA) - hand-rolled rather than
// std::regex_replace to avoid needing to escape a '$'-prefixed pattern and
// to avoid std::regex's replacement-string treating $1/$&/$$ specially if a
// define's own value happens to contain a literal '$' (e.g. nested defines
// like #define $SAMPLES $ROOT/samples).
std::string applyDefines(const std::string& line, const SfzOpcodeList& defines) {
    std::string result = line;
    for (const auto& [name, value] : defines) {
        std::string out;
        out.reserve(result.size());
        size_t pos = 0;
        while (pos < result.size()) {
            size_t found = result.find(name, pos);
            if (found == std::string::npos) {
                out.append(result, pos, std::string::npos);
                break;
            }
            size_t afterIdx = found + name.size();
            bool boundaryOk = afterIdx >= result.size() ||
                              !(std::isalnum(static_cast<unsigned char>(result[afterIdx])) ||
                                result[afterIdx] == '_');
            if (boundaryOk) {
                out.append(result, pos, found - pos);
                out += value;
                pos = afterIdx;
            } else {
                // Not a real match (e.g. $EXT inside $EXTRA) - keep scanning
                // one character further, same as a regex engine retrying the
                // next start position after a failed lookahead.
                out.append(result, pos, found - pos + 1);
                pos = found + 1;
            }
        }
        result = std::move(out);
    }
    return result;
}

const std::regex kDefineRe(R"(^#define\s+(\$\w+)\s+(.+?)\s*$)");
// Custom raw-string delimiter (re...re) - the pattern's own "([^"]+)" would
// otherwise collide with the default R"(...)" terminator at its first )".
const std::regex kIncludeRe(R"re(^#include\s+"([^"]+)"\s*$)re");

// Applies #define/#include recursively and returns the "pure" SFZ text
// (directives and comments gone). `defines` is shared by reference across
// the whole include tree, same single-pass-preprocessor semantics as
// sfzflat.py's preprocess(): a #define made before an #include is visible
// inside it, and one made inside stays visible after returning.
//
// `baseDir` is passed down UNCHANGED at every recursion level - every
// #include, no matter how deeply nested, resolves relative to the ORIGINAL
// top-level .sfz file's own directory, never to the immediately-including
// file's directory. This is NOT how a C preprocessor works, but it is
// exactly how the real sfizz engine behaves: its Parser::includeNewFile
// (library/src/sfizz/parser/Parser.cpp in the vendored fork, see project
// memory) sets `_originalDirectory` ONCE, from the very first file, and
// every subsequent include resolves against that same directory forever.
// Real-world kits are authored against that behavior (e.g. Big Rusty
// Drums' Programs/mappings/kick_24_map.sfz writes
// `#include "mappings/kick_24/k_kick.sfz"` - a path relative to Programs/,
// not to itself) - resolving file-relative like a C preprocessor (this
// function's original, incorrect behavior) breaks real files that do this.
//
// `includedPaths` mirrors sfizz's `_pathsIncluded`: a single set for the
// WHOLE parse (never erased, not scoped to the current ancestor chain) -
// re-including a path already seen ANYWHERE in the tree is a silent no-op,
// same as a `#pragma once` header guard. This also naturally terminates a
// genuine include cycle without needing a special error for it, exactly
// like sfizz - by the time the cycle would recurse back, that path is
// already in the set.
std::string preprocess(const std::string& text, const fs::path& baseDir, SfzOpcodeList& defines,
                       std::set<std::string>& includedPaths) {
    std::vector<std::string> outLines;
    std::istringstream stream(text);
    std::string rawLine;
    while (std::getline(stream, rawLine)) {
        if (!rawLine.empty() && rawLine.back() == '\r') rawLine.pop_back();

        std::string line = stripComment(rawLine);
        line = applyDefines(line, defines);
        std::string stripped = trim(line);

        std::smatch m;
        if (std::regex_match(stripped, m, kDefineRe)) {
            dictSet(defines, m[1].str(), trim(m[2].str()));
            continue;
        }
        if (std::regex_match(stripped, m, kIncludeRe)) {
            fs::path fullPath = fs::absolute(baseDir / m[1].str()).lexically_normal();
            std::string fullPathStr = fullPath.string();
            if (includedPaths.count(fullPathStr))
                continue; // already included somewhere in this tree - no-op, see above
            if (!fs::is_regular_file(fullPath))
                throw std::runtime_error("No se encontro el include: " + fullPathStr);

            includedPaths.insert(fullPathStr);
            std::string incText = readSfzText(fullPathStr);
            std::string incProcessed = preprocess(incText, baseDir, defines, includedPaths);
            outLines.push_back(std::move(incProcessed));
            continue;
        }
        outLines.push_back(std::move(line));
    }

    std::string result;
    for (size_t i = 0; i < outLines.size(); ++i) {
        if (i) result += "\n";
        result += outLines[i];
    }
    return result;
}

// ---------------------------------------------------------------------
// Phase 2: tokenize (headers + opcode=value, vertical or horizontal)
// ---------------------------------------------------------------------

struct Event {
    bool isHeader;
    std::string keyOrHeader; // lowercased
    std::string value;       // only meaningful when !isHeader
};

// An opcode is a key followed by "=". Its value is everything up to the
// next token (another opcode, a header, or end of text), regardless of
// newlines in between - lets a <region> span multiple lines or sit all on
// one line indifferently, and lets values (sample paths, labels) contain
// spaces without quoting.
const std::regex kTokenRe(R"(<(\w+)>|([A-Za-z_]\w*)=)");

std::vector<Event> tokenize(const std::string& text) {
    std::vector<std::smatch> matches(std::sregex_iterator(text.begin(), text.end(), kTokenRe),
                                     std::sregex_iterator());

    std::vector<Event> events;
    events.reserve(matches.size());
    for (size_t i = 0; i < matches.size(); ++i) {
        const auto& m = matches[i];
        size_t matchEnd = static_cast<size_t>(m.position(0) + m.length(0));
        size_t nextStart =
            (i + 1 < matches.size()) ? static_cast<size_t>(matches[i + 1].position(0)) : text.size();

        if (m[1].matched) {
            events.push_back({true, toLower(m[1].str()), ""});
        } else {
            std::string value = trim(text.substr(matchEnd, nextStart - matchEnd));
            events.push_back({false, toLower(m[2].str()), std::move(value)});
        }
    }
    return events;
}

// ---------------------------------------------------------------------
// Phase 3: flatten (global > master > group > region cascade + default_path)
// ---------------------------------------------------------------------

// Cascade rules (same as the real SFZ engine, see sfzflat.py's docstring):
//   <global>  resets master and group, becomes the new base.
//   <master>  resets group.
//   <group>   does NOT inherit the previous group, only master/global.
//   <region>  starts empty, merged with global+master+group on close; its
//             own opcodes win.
//   <control> not part of the cascade; only its default_path= matters,
//             replacing (not concatenating) a running prefix applied to
//             every region's sample= defined after it.
// Every other header (<curve>, <effect>, <midi>, ...) is simply dropped -
// this pipeline only ever needs <region> output.
std::vector<SfzOpcodeList> flattenEvents(const std::vector<Event>& events) {
    SfzOpcodeList cascadeGlobal, cascadeMaster, cascadeGroup;
    std::string currentHeader;
    std::string currentDefaultPath;
    bool inRegion = false;
    SfzOpcodeList regionOpcodes;
    std::vector<SfzOpcodeList> regions;

    auto closeRegion = [&]() {
        if (!inRegion) return;
        SfzOpcodeList merged;
        for (auto& kv : cascadeGlobal) dictSet(merged, kv.first, kv.second);
        for (auto& kv : cascadeMaster) dictSet(merged, kv.first, kv.second);
        for (auto& kv : cascadeGroup) dictSet(merged, kv.first, kv.second);
        for (auto& kv : regionOpcodes) dictSet(merged, kv.first, kv.second);

        if (!currentDefaultPath.empty()) {
            if (std::string* sample = dictFindMut(merged, "sample"))
                *sample = currentDefaultPath + *sample;
        }
        // sample= first, for readability - the rest keep their order.
        auto it = std::find_if(merged.begin(), merged.end(),
                               [](const auto& kv) { return kv.first == "sample"; });
        if (it != merged.end()) {
            auto samplePair = *it;
            merged.erase(it);
            merged.insert(merged.begin(), std::move(samplePair));
        }

        regions.push_back(std::move(merged));
        inRegion = false;
        regionOpcodes.clear();
    };

    for (const auto& ev : events) {
        if (ev.isHeader) {
            const std::string& name = ev.keyOrHeader;
            closeRegion();
            currentHeader = name;
            if (name == "global") {
                cascadeGlobal.clear();
                cascadeMaster.clear();
                cascadeGroup.clear();
            } else if (name == "master") {
                cascadeMaster.clear();
                cascadeGroup.clear();
            } else if (name == "group") {
                cascadeGroup.clear();
            } else if (name == "region") {
                inRegion = true;
                regionOpcodes.clear();
            }
            continue;
        }

        const std::string& key = ev.keyOrHeader;
        const std::string& value = ev.value;
        if (currentHeader == "control") {
            if (key == "default_path") currentDefaultPath = value;
        } else if (currentHeader == "region") {
            dictSet(regionOpcodes, key, value);
        } else if (currentHeader == "global") {
            dictSet(cascadeGlobal, key, value);
        } else if (currentHeader == "master") {
            dictSet(cascadeMaster, key, value);
        } else if (currentHeader == "group") {
            dictSet(cascadeGroup, key, value);
        }
        // Opcodes under any other header (<curve>, <effect>, <midi>, or
        // none yet) are silently dropped - not needed for region output.
    }
    closeRegion();
    return regions;
}

// ---------------------------------------------------------------------
// Opcode allowlist ("clean" step)
// ---------------------------------------------------------------------

// "pan"/"tune"/"loop_mode" are deliberately NOT in this list: they're
// per-drum controls the user sets in sfzdrummer's own UI (Sample tab), not
// something an imported .sfz should get to dictate - an imported kit's own
// pan=/tune=/loop_mode= would otherwise silently fight/override whatever
// the user configures.
const std::set<std::string> kAllowedOpcodes = {
    "sample", "pitch_keycenter", "lokey", "hikey", "lovel", "hivel", "key",
    "offset", "end", "volume",
    "seq_length", "seq_position", "lorand", "hirand",
    "loop_type", "direction", "loop_start", "loop_end", "loop_tune",
    "xfin_lokey", "xfin_hikey", "xfout_lokey", "xfout_hikey",
    "xfin_lovel", "xfin_hivel", "xfout_lovel", "xfout_hivel",
    "xf_keycurve", "xf_velcurve",
};

std::vector<SfzOpcodeList> cleanOpcodes(const std::vector<SfzOpcodeList>& regions) {
    std::vector<SfzOpcodeList> cleaned;
    cleaned.reserve(regions.size());
    for (const auto& region : regions) {
        SfzOpcodeList filtered;
        for (const auto& kv : region)
            if (kAllowedOpcodes.count(kv.first)) filtered.push_back(kv);
        cleaned.push_back(std::move(filtered));
    }
    return cleaned;
}

} // namespace

const std::string* sfzOpcodeFind(const SfzOpcodeList& list, const std::string& key) {
    for (auto& kv : list)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

ParsedSfzRegions parseAndFlattenSfz(const std::string& sfzPath) {
    ParsedSfzRegions result;
    try {
        fs::path absSfzPath = fs::absolute(sfzPath).lexically_normal();
        if (!fs::is_regular_file(absSfzPath)) {
            result.error = "File not found: " + sfzPath;
            return result;
        }
        fs::path baseDir = absSfzPath.parent_path();

        SfzOpcodeList defines;
        std::set<std::string> includedPaths;
        std::string raw = readSfzText(absSfzPath.string());
        std::string preprocessed = preprocess(raw, baseDir, defines, includedPaths);

        std::vector<Event> events = tokenize(preprocessed);
        std::vector<SfzOpcodeList> regions = cleanOpcodes(flattenEvents(events));

        if (regions.empty()) {
            result.error = "No <region> blocks found in SFZ file";
            return result;
        }

        // Rewrite every sample= to be relative to sfzdrummer's virtual "/"
        // root: prepend the .sfz file's own real absolute directory (leading
        // '/' stripped, plain string concatenation - matches the same
        // convention used for default_path above, and the leading-slash
        // stripping used for the single-sample case in plugin.cpp's
        // loadIntoSelectedDrum).
        std::string dirRelative = baseDir.string();
        if (!dirRelative.empty() && dirRelative.front() == '/')
            dirRelative = dirRelative.substr(1);

        for (auto& region : regions) {
            if (std::string* sample = dictFindMut(region, "sample"))
                *sample = dirRelative + "/" + *sample;
        }

        result.ok = true;
        result.regions = std::move(regions);
    } catch (const std::exception& e) {
        result.ok = false;
        result.error = e.what();
    }
    return result;
}
