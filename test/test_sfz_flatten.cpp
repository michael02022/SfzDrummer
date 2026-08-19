// Standalone regression test for the shared SFZ parsing pipeline
// (SfzParsePipeline.cpp) and its two flatteners: SfzFlatten.h's
// flattenMultisampleSfz (whole file as one drum) and DrumKitFlatten.h's
// flattenDrumKit (one drum per key found). Not wired into mini_host.c -
// these are plain C++ functions (std::string/std::vector-returning) with no
// CLAP-level entry point a C host could drive; this calls them directly
// instead, on a small hand-written drum-kit .sfz fixture.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "state/DrumKitFlatten.h"
#include "state/SfzFlatten.h"

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

int countOccurrences(const std::string& haystack, const std::string& needle) {
    int n = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

// Whole-line match ("opcode=value", one per line in the flatteners' output)
// rather than a plain substring search - "end=" as a bare substring would
// false-positive against the legitimately-allowed "loop_end=".
bool hasOpcodeLine(const std::string& text, const std::string& opcode) {
    const std::string needle = opcode + "=";
    size_t pos = 0;
    while (pos < text.size()) {
        if (text.compare(pos, needle.size(), needle) == 0) return true;
        size_t lineEnd = text.find('\n', pos);
        if (lineEnd == std::string::npos) break;
        pos = lineEnd + 1;
    }
    return false;
}

} // namespace

int main() {
    fs::path dir = fs::temp_directory_path() / "sfzdrummer_flatten_test";
    fs::create_directories(dir);
    fs::path sfzPath = dir / "kit.sfz";

    // A small drum kit: key 36 gets 2 round-robin regions (the first one
    // also carries pan=/tune=/loop_mode= - which must be stripped, see
    // kAllowedOpcodes' comment: pan/tune/loop_mode are per-drum controls the
    // user sets in sfzdrummer's own UI, an imported kit's own values for
    // them must not silently override it - and offset=/end=, which ARE
    // kept), key 38 gets 2 velocity-layered regions, key "cs2" (note-name
    // syntax, MIDI 37) gets 1 region, and key 40 is expressed as
    // lokey=hikey=pitch_keycenter=40 instead of the key= shorthand - all
    // four ways real kits express a single-note mapping. The tom region
    // also carries loop_start=/loop_end= (DIFFERENT, still-allowed opcodes)
    // to prove hasOpcodeLine's end=/loop_mode= checks below can't
    // false-positive against loop_end=/loop_start=.
    {
        std::ofstream f(sfzPath);
        f << "<region> sample=kick.wav key=36 seq_position=1 seq_length=2 pan=-30 tune=25 "
             "loop_mode=loop_continuous offset=100 end=48000\n"
             "<region> sample=kick2.wav key=36 seq_position=2 seq_length=2\n"
             "<region> sample=snare_soft.wav key=38 lovel=0 hivel=63\n"
             "<region> sample=snare_hard.wav key=38 lovel=64 hivel=127\n"
             "<region> sample=hihat.wav key=cs2\n"
             "<region> sample=tom.wav lokey=40 hikey=40 pitch_keycenter=40 "
             "loop_start=0 loop_end=44100\n";
    }

    // --- flattenMultisampleSfz: whole file as one drum - must behave
    // exactly as before the SfzParsePipeline extraction/refactor. ---
    FlattenedSfz single = flattenMultisampleSfz(sfzPath.string());
    check(single.ok, "flattenMultisampleSfz succeeds on the test kit");
    check(single.regionCount == 6, "flattenMultisampleSfz sees all 6 regions");
    check(countOccurrences(single.regionsText, "<region>") == 6,
         "flattenMultisampleSfz emits 6 <region> blocks");
    // sample= rewritten relative to the virtual "/" root: the sfz's own
    // directory (leading '/' stripped) prepended to each bare filename.
    std::string expectedDirPrefix = dir.string().substr(1) + "/kick.wav";
    check(single.regionsText.find(expectedDirPrefix) != std::string::npos,
         "flattenMultisampleSfz rewrites sample= relative to the virtual root");
    check(!hasOpcodeLine(single.regionsText, "pan"),
         "flattenMultisampleSfz strips pan= (a per-drum control, not the kit's to set)");
    check(!hasOpcodeLine(single.regionsText, "tune"),
         "flattenMultisampleSfz strips tune= (a per-drum control, not the kit's to set)");
    check(!hasOpcodeLine(single.regionsText, "loop_mode"),
         "flattenMultisampleSfz strips loop_mode= (a per-drum control, not the kit's to set)");
    check(hasOpcodeLine(single.regionsText, "offset"), "flattenMultisampleSfz keeps offset=");
    check(hasOpcodeLine(single.regionsText, "end"), "flattenMultisampleSfz keeps end=");
    check(hasOpcodeLine(single.regionsText, "loop_end"),
         "flattenMultisampleSfz keeps loop_end= too (a different opcode - proves the "
         "line-exact end= check above isn't a false positive against it)");
    check(hasOpcodeLine(single.regionsText, "loop_start"),
         "flattenMultisampleSfz keeps loop_start= too (not a false positive against loop_mode=)");

    // --- flattenDrumKit: one drum per key found ---
    FlattenedDrumKit kit = flattenDrumKit(sfzPath.string());
    check(kit.ok, "flattenDrumKit succeeds on the test kit");
    check(kit.keyGroups.size() == 4, "flattenDrumKit finds 4 distinct keys (36, 37, 38, 40)");
    if (kit.keyGroups.size() == 4) {
        check(kit.keyGroups[0].key == 36, "group 0 is key 36 (kick)");
        check(kit.keyGroups[0].regionCount == 2, "key 36 has both round-robin regions");
        check(!hasOpcodeLine(kit.keyGroups[0].regionsText, "pan"), "flattenDrumKit strips pan= too");
        check(!hasOpcodeLine(kit.keyGroups[0].regionsText, "tune"), "flattenDrumKit strips tune= too");
        check(!hasOpcodeLine(kit.keyGroups[0].regionsText, "loop_mode"),
             "flattenDrumKit strips loop_mode= too");
        check(hasOpcodeLine(kit.keyGroups[0].regionsText, "offset"), "flattenDrumKit keeps offset= too");
        check(hasOpcodeLine(kit.keyGroups[0].regionsText, "end"), "flattenDrumKit keeps end= too");
        check(kit.keyGroups[1].key == 37, "group 1 is key 37 (\"cs2\" note name parsed correctly)");
        check(kit.keyGroups[1].regionCount == 1, "key 37 has 1 region");
        check(kit.keyGroups[2].key == 38, "group 2 is key 38 (snare)");
        check(kit.keyGroups[2].regionCount == 2, "key 38 has both velocity-layer regions");
        check(kit.keyGroups[3].key == 40, "group 3 is key 40 (tom, lokey=hikey=pitch_keycenter)");
        check(hasOpcodeLine(kit.keyGroups[3].regionsText, "loop_end"),
             "flattenDrumKit keeps loop_end= (not a false positive against end=)");
        check(kit.keyGroups[3].regionCount == 1, "key 40 has 1 region");
        // groups are independent - one key's regionsText must not leak
        // another key's samples.
        check(kit.keyGroups[0].regionsText.find("snare") == std::string::npos,
             "key 36's group doesn't leak snare's regions");
        check(kit.keyGroups[2].regionsText.find("kick") == std::string::npos,
             "key 38's group doesn't leak kick's regions");
    }

    // --- #include resolution: every #include, no matter how deeply nested,
    // resolves relative to the TOP-LEVEL root file's own directory - never
    // to the immediately-including file's directory (NOT C-preprocessor
    // semantics). This matches the real sfizz engine's Parser::includeNewFile
    // (_originalDirectory set once, from the first file, never touched
    // again - see project memory) - real kits are authored against that
    // behavior (found via a real Big Rusty Drums .sfz that failed to load
    // before this fix: a file at Programs/mappings/kick_24_map.sfz included
    // "mappings/kick_24/k_kick.sfz", a path relative to Programs/, not to
    // itself). This fixture reproduces that exact shape at a smaller scale.
    fs::path incDir = fs::temp_directory_path() / "sfzdrummer_include_test";
    fs::create_directories(incDir / "sub" / "deeper");
    {
        std::ofstream(incDir / "root.sfz") << "#include \"sub/level1.sfz\"\n";
        // Written as if relative to incDir (the root), same as real kits -
        // if resolution were file-relative instead, this would incorrectly
        // look for incDir/sub/sub/deeper/level2.sfz (doubled "sub", exactly
        // the doubled "mappings/mappings" shape of the real bug).
        std::ofstream(incDir / "sub" / "level1.sfz")
            << "#include \"sub/deeper/level2.sfz\"\n<region> sample=one.wav key=60\n";
        std::ofstream(incDir / "sub" / "deeper" / "level2.sfz")
            << "<region> sample=two.wav key=61\n";
    }
    FlattenedSfz nested = flattenMultisampleSfz((incDir / "root.sfz").string());
    check(nested.ok, "nested #include with a root-relative (not file-relative) path resolves");
    check(nested.regionCount == 2, "both the including and the nested-included region show up");

    // --- #include dedup: the same path included from two different places
    // in the tree is only expanded ONCE, globally - matching sfizz's
    // `_pathsIncluded` (a `#pragma once`-style guard for the whole parse,
    // not just the current ancestor chain). Also proves a genuine include
    // cycle can no longer throw (it just naturally no-ops the same way).
    fs::path dedupDir = fs::temp_directory_path() / "sfzdrummer_dedup_test";
    fs::create_directories(dedupDir);
    {
        std::ofstream(dedupDir / "root.sfz") << "#include \"a.sfz\"\n#include \"b.sfz\"\n";
        std::ofstream(dedupDir / "a.sfz")
            << "#include \"shared.sfz\"\n<region> sample=a.wav key=70\n";
        std::ofstream(dedupDir / "b.sfz")
            << "#include \"shared.sfz\"\n<region> sample=b.wav key=71\n";
        std::ofstream(dedupDir / "shared.sfz") << "<region> sample=shared.wav key=72\n";
    }
    FlattenedSfz dedup = flattenMultisampleSfz((dedupDir / "root.sfz").string());
    check(dedup.ok, "diamond-shaped double-include resolves");
    check(dedup.regionCount == 3,
         "shared.sfz (included from both a.sfz and b.sfz) is only expanded once (3 regions "
         "total, not 4)");

    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(incDir, ec);
    fs::remove_all(dedupDir, ec);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
