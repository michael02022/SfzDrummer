// CLAP glue: entry point, factory, plugin, audio/note ports, and GUI.
// Structure ported from SoloSampler's plugin.cpp (sibling project), adapted
// for a multi-output drum machine: 8 stereo audio outputs instead of 1, and
// the "instrument" is a list of independent drum <master>s (see
// DrumSfzBuilder.h) instead of a single region/stack.
#include <clap/clap.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "gui/editor_ui.hpp"
#include "gui/gui_window.hpp"
#include "gui/zenity_dialog.h"
#include "sfizz/SfizzEngine.h"
#include "shared.hpp"
#include "state/DrumKitFlatten.h"
#include "state/DrumSfzBuilder.h"
#include "state/PresetFile.h"
#include "state/SampleInfo.h"
#include "state/SfzFlatten.h"

namespace {

constexpr uint32_t kDefaultW = 940, kDefaultH = 640;

struct Plugin {
    clap_plugin_t plug{};
    const clap_host_t* host = nullptr;
    std::unique_ptr<GuiWindow> window;
    SfizzEngine engine;        // main kit: all drums, routed to their own output=
    SfizzEngine previewEngine; // file explorer's sample-click audition (Out 1 only)
    SharedParams params;
    EditorUIState uiState;
    std::vector<float> previewBufL, previewBufR;
    // Set whenever the drum list's label/rootNote pairs might have changed;
    // drained by plugOnMainThread (see notifyNoteNamesChangedLater).
    std::atomic<bool> noteNamesChangedPending{false};

    explicit Plugin(const clap_host_t* h) : host(h) {}
};

// Sets guiState.listError together with its timestamp - editor_ui.cpp's
// drawErrorToast uses the timestamp to auto-dismiss the message after a few
// seconds. Caller must already hold guiState.mutex.
void setListError(SharedParams::GuiState& gs, std::string message) {
    gs.listError = std::move(message);
    gs.listErrorSetAt = std::chrono::steady_clock::now();
}

// Root-relative sample= value: the virtual sfz "path" sfizz is told about is
// always "/" (see regenerateAndLoadDrumSfz's loadSfzString call), so
// sample= must carry the file's full absolute path with the leading '/'
// stripped, not a path relative to some other fake root.
std::string sampleRelativePath(const std::string& absPath) {
    return (!absPath.empty() && absPath.front() == '/') ? absPath.substr(1) : absPath;
}

// Same note-naming as editor_ui.cpp's own noteName (duplicated rather than
// shared - it's a tiny, header-independent 5-liner, not worth a new shared
// header just to avoid one copy) - used by explodeSelectedDrumKit below to
// give each newly-created drum a descriptive default label instead of a
// generic "Drum N".
std::string noteName(int n) {
    static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F",
                                     "F#", "G", "G#", "A", "A#", "B"};
    int octave = n / 12 - 1;
    return std::string(kNames[n % 12]) + std::to_string(octave);
}

// clap_host_note_name_t::changed() is documented [main-thread]-only, but
// drum-list edits happen on the GUI thread (its own dedicated X11 thread,
// NOT the host's main thread - see gui_window.hpp) or on stateLoad's caller
// thread. request_callback() is [thread-safe] and asks the host to invoke
// plugin->on_main_thread() on its own main thread shortly after - that's
// where the actual changed() call happens (see plugOnMainThread below).
void notifyNoteNamesChangedLater(Plugin* p) {
    p->noteNamesChangedPending = true;
    if (p->host && p->host->request_callback) p->host->request_callback(p->host);
}

// Rebuilds the whole-kit SFZ text from the drum list and reloads it into the
// main engine. CT+OFF (see SfizzEngine.h) - call only from a non-audio
// thread (activation, or the GUI thread after a list edit). Every drum-list
// mutation funnels through here, so this is also the single place that
// tells the host the label/rootNote pairs it may have cached (CLAP
// clap.note-name, e.g. Reaper's piano roll drum names) might be stale.
void regenerateAndLoadDrumSfz(Plugin* p) {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        text = buildDrumSfzText(p->params.guiState.drums, p->params.bendUpCents.load(),
                                 p->params.bendDownCents.load());
    }
    p->engine.loadSfzString("/", text);
    notifyNoteNamesChangedLater(p);
}

// File explorer double-click (or XDND drop) - loads a sample or .sfz into
// whichever drum is currently selected. A plain sample becomes a bare
// region; a .sfz is flattened (resolves its own #define/#include/
// default_path, rewrites sample= relative to sfzdrummer's virtual "/" root)
// into its own region block, later key-stripped by DrumSfzBuilder so the
// drum's assigned root note (not whatever the source .sfz mapped it to)
// wins. If the drum list is completely empty, a first drum is created and
// selected automatically (so the very first import doesn't require pressing
// "+" first) - with a non-empty list, the normal rule still applies: a drum
// must already be selected, or the load is rejected via listError. Returns
// true iff a drum was actually updated - guiCreate's XDND handler uses that
// to decide whether to jump the file explorer to the dropped file's folder.
// GUI thread only.
bool loadIntoSelectedDrum(Plugin* p, const std::string& path) {
    const bool isSfz = isSfzFile(path);
    if (!isSfz && !isSupportedAudioFile(path)) {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        setListError(p->params.guiState, "Unsupported file: " + path);
        return false;
    }

    std::string regionsText;
    int regionCount = 0;
    std::vector<DrumKitKeyGroup> drumKitGroups;
    if (isSfz) {
        FlattenedSfz flat = flattenMultisampleSfz(path);
        if (!flat.ok) {
            std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
            setListError(p->params.guiState, flat.error);
            return false;
        }
        regionsText = flat.regionsText;
        regionCount = flat.regionCount;

        // Best-effort: populates the Sample tab's Drum Kit Mode key list
        // (see shared.hpp's DrumItem::drumKitGroups). A failure here (e.g.
        // no region in the file has a resolvable key mapping) is NOT fatal
        // to the load - it just means the checkbox stays disabled, the
        // whole-file blob above is still perfectly usable on its own.
        FlattenedDrumKit kit = flattenDrumKit(path);
        if (kit.ok) drumKitGroups = std::move(kit.keyGroups);
    }

    bool applied = false;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        auto& gs = p->params.guiState;
        if (gs.drums.empty()) {
            SharedParams::DrumItem d;
            d.id = gs.nextId++;
            d.label = "Drum " + std::to_string(d.id);
            gs.drums.push_back(d);
            gs.selectedId = d.id;
        }
        const int sel = gs.selectedId;
        if (sel < 0) {
            setListError(gs, "Select a percussion first");
        } else {
            for (auto& d : gs.drums) {
                if (d.id != sel) continue;
                d.hasSource = true;
                d.isSfz = isSfz;
                d.sourcePath = path;
                // A new source invalidates any previous Drum Kit Mode pick -
                // it referred to the OLD file's key layout.
                d.drumKitModeEnabled = false;
                d.drumKitGroupIndex = 0;
                if (isSfz) {
                    d.regionsText = regionsText;
                    d.regionCount = regionCount;
                    d.drumKitGroups = std::move(drumKitGroups);
                    d.sampleRelativePath.clear();
                } else {
                    d.sampleRelativePath = sampleRelativePath(path);
                    d.regionsText.clear();
                    d.regionCount = 1;
                    d.drumKitGroups.clear();
                }
                applied = true;
                break;
            }
            if (applied) gs.listError.clear();
        }
    }
    if (applied) regenerateAndLoadDrumSfz(p);
    return applied;
}

// "Load regions as individual percussion" (Sample tab, confirmed via
// editor_ui.cpp's modal) - bulk alternative to picking one Drum Kit Mode
// key at a time: explodes EVERY key group already discovered on the
// selected drum (DrumItem::drumKitGroups, populated by loadIntoSelectedDrum
// above via DrumKitFlatten.h at import time) into its OWN separate drum in
// the list, one <master> per key. Reuses the SAME flattened data Drum Kit
// Mode's key slider already offers - this just does what picking every key
// one at a time (into N manually-added drums) would produce, in one shot.
//
// The selected drum itself is repurposed as key group 0 (equivalent to
// picking index 0 via Drum Kit Mode) rather than left as a "whole file"
// blob alongside the new drums - leaving it in whole-file mode would sound
// broken (its <master> would carry every key's regions at once, forced
// onto a single key= once DrumSfzBuilder strips the source key opcodes).
// Its own instrument-design fields (volume/pan/envelopes/etc, all
// orthogonal to which regions are loaded) are left untouched. Every drum
// gets a note-name label so the resulting list is immediately readable
// without cross-checking each rootNote by hand.
//
// GUI thread only, same convention as loadIntoSelectedDrum. Returns true
// iff the explode actually happened.
bool explodeSelectedDrumKit(Plugin* p) {
    std::vector<DrumKitKeyGroup> groups;
    int selId = -1;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        auto& gs = p->params.guiState;
        selId = gs.selectedId;
        SharedParams::DrumItem* d = nullptr;
        for (auto& item : gs.drums)
            if (item.id == selId) {
                d = &item;
                break;
            }
        if (!d) {
            setListError(gs, "Select a percussion first");
            return false;
        }
        if (!d->isSfz || d->drumKitGroups.empty()) {
            setListError(gs, "No independent keys found in this .sfz.");
            return false;
        }
        groups = d->drumKitGroups; // copy out before any structural mutation
                                    // below - inserting new drums can
                                    // reallocate gs.drums and invalidate d.
    }

    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        auto& gs = p->params.guiState;
        SharedParams::DrumItem* d = nullptr;
        for (auto& item : gs.drums)
            if (item.id == selId) {
                d = &item;
                break;
            }
        if (!d) {
            // Selection changed between the two locks above (e.g. deleted) -
            // bail rather than acting on stale data.
            setListError(gs, "Select a percussion first");
            return false;
        }

        const std::string sourcePath = d->sourcePath;
        d->rootNote = groups[0].key;
        d->label = noteName(groups[0].key);
        d->regionsText = groups[0].regionsText;
        d->regionCount = groups[0].regionCount;
        d->drumKitModeEnabled = false;
        d->drumKitGroupIndex = 0;
        d->drumKitGroups.clear();

        for (size_t i = 1; i < groups.size(); ++i) {
            SharedParams::DrumItem nd;
            nd.id = gs.nextId++;
            nd.label = noteName(groups[i].key);
            nd.rootNote = groups[i].key;
            nd.outputIndex = 0;
            nd.hasSource = true;
            nd.isSfz = true;
            nd.sourcePath = sourcePath;
            nd.regionsText = groups[i].regionsText;
            nd.regionCount = groups[i].regionCount;
            gs.drums.push_back(std::move(nd));
        }
        gs.listError.clear();
    }
    regenerateAndLoadDrumSfz(p);
    return true;
}

// "Init Kit" (persistent button row, confirmed via editor_ui.cpp's modal) -
// wipes the whole kit back to a freshly-constructed plugin's own defaults:
// the drum list (empty, id counter restarts at 1, nothing selected) plus
// the whole-instrument fields that feed the generated SFZ alongside it
// (mpeEnabled/bendUpCents/bendDownCents - see shared.hpp's SharedParams,
// same default values their own member initializers use). Deliberately
// does NOT touch previewVolume/previewEnabled - those are file-explorer
// audition settings, not part of the kit's own design. GUI thread only,
// same convention as loadIntoSelectedDrum/explodeSelectedDrumKit.
void initKit(Plugin* p) {
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        auto& gs = p->params.guiState;
        gs.drums.clear();
        gs.nextId = 1;
        gs.selectedId = -1;
        gs.listError.clear();
    }
    p->params.mpeEnabled = false;
    p->params.bendUpCents = 2400;
    p->params.bendDownCents = -2400;
    regenerateAndLoadDrumSfz(p);
}

// File explorer single-click on a sample row - loads a minimal one-region
// SFZ into the dedicated preview engine and flags plugProcess to (re)trigger
// note 60. GUI thread only (loadSfzString is CT+OFF, see SfizzEngine.h).
void previewFile(Plugin* p, const std::string& path) {
    if (!p->params.previewEnabled.load()) return;
    if (!isSupportedAudioFile(path)) return; // .sfz rows don't preview, per spec
    std::string text = "<region>\nsample=" + sampleRelativePath(path) + "\nkey=60\n";
    p->previewEngine.loadSfzString("/", text);
    p->params.pendingFilePreviewTrigger = true;
}

// ------------------------------------------------------------- presets

// ~/SfzdrummerPresets, matching SoloSampler's own ~/SoloSamplerPresets
// scheme exactly (sibling project) - $HOME + a fixed suffix, "." fallback
// if $HOME is somehow unset. GUI thread only.
std::string presetsDir() {
    const char* home = getenv("HOME");
    return (home ? std::string(home) : std::string(".")) + "/SfzdrummerPresets";
}

// Save Profile's default filename: the selected drum's own label (so
// saving "Kick"'s design proposes "Kick.drmprofile") - falls back to
// "Untitled" if somehow nothing is selected (handleSaveProfile already
// refuses to even open the dialog in that case, see below - this is just
// a safe fallback, never actually exercised through the button). Save
// Preset has no equivalent single "current name" to derive from (a preset
// is the WHOLE kit, not any one drum) - always "Untitled" there, matching
// SoloSampler's own stack-empty fallback.
std::string defaultProfileBaseName(Plugin* p) {
    std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
    for (auto& d : p->params.guiState.drums)
        if (d.id == p->params.guiState.selectedId) return d.label.empty() ? "Untitled" : d.label;
    return "Untitled";
}

// Actual save/load logic for all four buttons, factored out from the
// zenity-dialog-driven handlers below so SFZDRUMMER_DEBUG_* env hooks can
// exercise the exact same code path (mutex-protected gather/apply, real
// file I/O, regenerateAndLoadDrumSfz, listError) without needing a real
// zenity click (no synthetic mouse input in this sandbox, see project
// memory). GUI thread only, same as loadIntoSelectedDrum.

bool savePresetToPath(Plugin* p, const std::string& path) {
    std::vector<SharedParams::DrumItem> drums;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        drums = p->params.guiState.drums;
    }
    bool ok = writeDrumPreset(path, drums);
    std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
    if (ok) p->params.guiState.listError.clear();
    else setListError(p->params.guiState, "Could not save: " + path);
    return ok;
}

// Replaces the WHOLE drum list, same as CLAP host state restore - a
// preset is a whole-kit snapshot, not a merge.
bool loadPresetFromPath(Plugin* p, const std::string& path) {
    DrumPresetResult result = readDrumPreset(path);
    if (!result.ok) {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        setListError(p->params.guiState, result.error);
        return false;
    }

    int nextId = 1;
    for (auto& d : result.drums) d.id = nextId++;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        p->params.guiState.drums = std::move(result.drums);
        p->params.guiState.nextId = nextId;
        p->params.guiState.selectedId = -1;
        p->params.guiState.listError.clear();
    }
    regenerateAndLoadDrumSfz(p);
    return true;
}

bool saveProfileToPath(Plugin* p, const std::string& path) {
    SharedParams::DrumItem copy;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        auto& gs = p->params.guiState;
        SharedParams::DrumItem* d = nullptr;
        for (auto& item : gs.drums)
            if (item.id == gs.selectedId) {
                d = &item;
                break;
            }
        if (!d) {
            setListError(gs, "Select a percussion first");
            return false;
        }
        copy = *d;
    }
    bool ok = writeDrumProfile(path, copy);
    std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
    if (ok) p->params.guiState.listError.clear();
    else setListError(p->params.guiState, "Could not save: " + path);
    return ok;
}

// Merges the file's design fields onto whichever drum is currently
// selected via applyDrumProfileDesign - that drum's identity (label/root
// note/output/sample source/Drum Kit Mode) is never touched, only its
// instrument design changes, per spec.
bool loadProfileFromPath(Plugin* p, const std::string& path) {
    DrumProfileResult result = readDrumProfile(path);
    if (!result.ok) {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        setListError(p->params.guiState, result.error);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        auto& gs = p->params.guiState;
        SharedParams::DrumItem* d = nullptr;
        for (auto& item : gs.drums)
            if (item.id == gs.selectedId) {
                d = &item;
                break;
            }
        if (!d) {
            setListError(gs, "Select a percussion first");
            return false;
        }
        applyDrumProfileDesign(*d, result.fields);
        gs.listError.clear();
    }
    regenerateAndLoadDrumSfz(p);
    return true;
}

// Save Preset/Save Profile button handlers: open zenity's save dialog
// (seeded with ~/SfzdrummerPresets/<default name>.drmXXX), append the
// right extension if the user didn't type one (zenity's own filter is
// display-only, it never forces this), then save. Cancelling zenity
// (empty path) is silent, not an error - same convention as SoloSampler.
void handleSavePreset(Plugin* p) {
    // Debug-only, opt-in: SFZDRUMMER_DEBUG_SAVE_PRESET_PATH=<path> calls
    // savePresetToPath directly, bypassing zenity, so this whole pipeline
    // (mutex-protected gather, real file write, listError) can be
    // screenshot/state-verified without a real click. Unset in normal
    // use - no effect on real hosts.
    if (const char* debugPath = getenv("SFZDRUMMER_DEBUG_SAVE_PRESET_PATH")) {
        savePresetToPath(p, debugPath);
        return;
    }
    std::filesystem::create_directories(presetsDir());
    const std::string defaultPath = presetsDir() + "/Untitled.drmpreset";
    std::string path =
        zenitySaveFile("Save Preset", defaultPath, {{"SfzDrummer Preset", "*.drmpreset"}});
    if (path.empty()) return;
    const std::string ext = ".drmpreset";
    if (path.size() < ext.size() || path.compare(path.size() - ext.size(), ext.size(), ext) != 0)
        path += ext;
    savePresetToPath(p, path);
}

void handleLoadPreset(Plugin* p) {
    if (const char* debugPath = getenv("SFZDRUMMER_DEBUG_LOAD_PRESET_PATH")) {
        loadPresetFromPath(p, debugPath);
        return;
    }
    std::string path = zenityOpenFile("Load Preset", presetsDir() + "/",
                                      {{"SfzDrummer Preset", "*.drmpreset"}});
    if (path.empty()) return;
    loadPresetFromPath(p, path);
}

void handleSaveProfile(Plugin* p) {
    if (const char* debugPath = getenv("SFZDRUMMER_DEBUG_SAVE_PROFILE_PATH")) {
        saveProfileToPath(p, debugPath);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        if (p->params.guiState.selectedId < 0) {
            setListError(p->params.guiState, "Select a percussion first");
            return;
        }
    }
    std::filesystem::create_directories(presetsDir());
    const std::string defaultPath = presetsDir() + "/" + defaultProfileBaseName(p) + ".drmprofile";
    std::string path =
        zenitySaveFile("Save Profile", defaultPath, {{"SfzDrummer Profile", "*.drmprofile"}});
    if (path.empty()) return;
    const std::string ext = ".drmprofile";
    if (path.size() < ext.size() || path.compare(path.size() - ext.size(), ext.size(), ext) != 0)
        path += ext;
    saveProfileToPath(p, path);
}

void handleLoadProfile(Plugin* p) {
    if (const char* debugPath = getenv("SFZDRUMMER_DEBUG_LOAD_PROFILE_PATH")) {
        loadProfileFromPath(p, debugPath);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        if (p->params.guiState.selectedId < 0) {
            setListError(p->params.guiState, "Select a percussion first");
            return;
        }
    }
    std::string path = zenityOpenFile("Load Profile", presetsDir() + "/",
                                      {{"SfzDrummer Profile", "*.drmprofile"}});
    if (path.empty()) return;
    loadProfileFromPath(p, path);
}

Plugin* self(const clap_plugin_t* p) { return static_cast<Plugin*>(p->plugin_data); }

const char* kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SAMPLER,
                           CLAP_PLUGIN_FEATURE_DRUM_MACHINE, CLAP_PLUGIN_FEATURE_STEREO,
                           nullptr};

const clap_plugin_descriptor_t kDesc = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "com.sfzlab.sfzdrummer",
    .name = "SfzDrummer",
    .vendor = "michael02022",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "0.1.0",
    .description = "Multi-output SFZ drum machine backed by sfizz.",
    .features = kFeatures,
};

// ------------------------------------------------------------- audio ports

uint32_t audioPortsCount(const clap_plugin_t*, bool is_input) {
    return is_input ? 0 : static_cast<uint32_t>(kNumOutputs);
}

bool audioPortsGet(const clap_plugin_t*, uint32_t index, bool is_input,
                   clap_audio_port_info_t* info) {
    if (is_input || index >= static_cast<uint32_t>(kNumOutputs)) return false;
    info->id = index;
    snprintf(info->name, sizeof(info->name), "Out %u", index + 1);
    info->channel_count = 2;
    info->flags = (index == 0) ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t kExtAudioPorts = {
    .count = audioPortsCount,
    .get = audioPortsGet,
};

// -------------------------------------------------------------- note ports

uint32_t notePortsCount(const clap_plugin_t*, bool is_input) { return is_input ? 1 : 0; }

bool notePortsGet(const clap_plugin_t*, uint32_t index, bool is_input,
                  clap_note_port_info_t* info) {
    if (!is_input || index != 0) return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    snprintf(info->name, sizeof(info->name), "MIDI In");
    return true;
}

const clap_plugin_note_ports_t kExtNotePorts = {
    .count = notePortsCount,
    .get = notePortsGet,
};

// -------------------------------------------------------------- note names
//
// clap.note-name: lets a host (e.g. Reaper's piano roll/MIDI editor) show
// each drum's label instead of a raw note number. Read directly from
// guiState.drums rather than round-tripped through sfizz's own key-label
// query API (sfizz_get_key_label_text) - both ultimately come from the same
// data (see DrumSfzBuilder.cpp's label_key<N>= opcodes), and this avoids
// needing engine access for something the plugin already knows.

uint32_t noteNameCount(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
    uint32_t n = 0;
    for (const auto& d : p->params.guiState.drums)
        if (!d.label.empty()) ++n;
    return n;
}

bool noteNameGet(const clap_plugin_t* plugin, uint32_t index, clap_note_name_t* out) {
    Plugin* p = self(plugin);
    std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
    uint32_t i = 0;
    for (const auto& d : p->params.guiState.drums) {
        if (d.label.empty()) continue;
        if (i == index) {
            snprintf(out->name, sizeof(out->name), "%s", d.label.c_str());
            out->port = -1;
            out->key = static_cast<int16_t>(d.rootNote);
            out->channel = -1;
            return true;
        }
        ++i;
    }
    return false;
}

const clap_plugin_note_name_t kExtNoteName = {
    .count = noteNameCount,
    .get = noteNameGet,
};

// ------------------------------------------------------------------- state
//
// A small versioned binary blob (hand-rolled, no XML/JSON dependency) - the
// drum list plus whatever per-drum config tabs have grown fields worth
// persisting so far (see kStateVersion's per-version comment above).

constexpr uint32_t kStateMagic = 0x44465A53; // "SZFD" LE bytes
constexpr uint32_t kStateVersion = 14; // v3: Sample tab (volume/pan/width/quality/polyphony/
                                      // notePolyphony/disableNoteSelfmask/loopModeIndex/reverse/
                                      // offset/vel2offset/exclusiveClass/group/offbyEnabled/
                                      // offby/transpose/tune)
                                      // v4: Amp tab (ampVelCurve - sparse velocity/gain points)
                                      // v5: Amp tab (ampVeltrack/ampRandom/eg01_* envelope/
                                      // ampVel2Attack/ampVel2Sustain/ampVel2Volume)
                                      // v6: Amp tab - ampStartLevel removed (Start Level slider
                                      // dropped, eg01_level0/1 now hardcoded to 0)
                                      // v7: Sample tab - panRandom/panAlternate
                                      // v8: Fil tab (filterTypeIndex/filterCutoff/
                                      // filterResonance/filterRandomCutoff/filVeltrack/
                                      // resoVeltrack/filterEgEnabled/filDepth/eg02_* fields)
                                      // v9: Pitch tab (pitchVeltrack/pitchRandom/
                                      // pitchEgEnabled/pitchDepth/pitchVel2Depth/eg03_*
                                      // fields)
                                      // v10: Pitch tab - pitchVel2Invert (Invert vel2pitch)
                                      // v11: Amp/Fil/Pitch Decay Time max lowered to 0.1,
                                      // ampDecayTimeExtra/filEgDecayTimeExtra/
                                      // pitchEgDecayTimeExtra added (0.0..12.0, summed
                                      // onto the base Decay Time at DrumSfzBuilder.cpp,
                                      // not a real opcode of their own)
                                      // v12: Opcodes tab - customOpcodesText (free-form,
                                      // written verbatim into the drum's own <master>)
                                      // v13: whole-instrument mpeEnabled (Enable MPE checkbox,
                                      // NOT per-drum - written once, after the drum list)
                                      // v14: whole-instrument bendUpCents/bendDownCents (Bend
                                      // Range combobox, NOT per-drum - written once, right
                                      // after mpeEnabled)

bool streamWriteAll(const clap_ostream_t* stream, const void* data, uint64_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t written = 0;
    while (written < size) {
        int64_t n = stream->write(stream, p + written, size - written);
        if (n <= 0) return false;
        written += static_cast<uint64_t>(n);
    }
    return true;
}

bool streamReadAll(const clap_istream_t* stream, void* data, uint64_t size) {
    uint8_t* p = static_cast<uint8_t*>(data);
    uint64_t readTotal = 0;
    while (readTotal < size) {
        int64_t n = stream->read(stream, p + readTotal, size - readTotal);
        if (n <= 0) return false;
        readTotal += static_cast<uint64_t>(n);
    }
    return true;
}

template <typename T>
bool writeVal(const clap_ostream_t* s, const T& v) {
    return streamWriteAll(s, &v, sizeof(T));
}
template <typename T>
bool readVal(const clap_istream_t* s, T& v) {
    return streamReadAll(s, &v, sizeof(T));
}

bool writeString(const clap_ostream_t* s, const std::string& str) {
    if (!writeVal(s, static_cast<uint32_t>(str.size()))) return false;
    return str.empty() || streamWriteAll(s, str.data(), str.size());
}
bool readString(const clap_istream_t* s, std::string& out) {
    uint32_t len = 0;
    if (!readVal(s, len)) return false;
    out.resize(len);
    return len == 0 || streamReadAll(s, out.data(), len);
}

bool stateSave(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
    Plugin* p = self(plugin);
    std::vector<SharedParams::DrumItem> drums;
    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        drums = p->params.guiState.drums;
    }

    bool ok = writeVal(stream, kStateMagic) && writeVal(stream, kStateVersion) &&
             writeVal(stream, static_cast<uint32_t>(drums.size()));
    for (const auto& d : drums) {
        if (!ok) break;
        ok = writeVal(stream, static_cast<int32_t>(d.rootNote)) &&
             writeVal(stream, static_cast<int32_t>(d.outputIndex)) &&
             writeVal(stream, static_cast<uint8_t>(d.hasSource ? 1 : 0)) &&
             writeVal(stream, static_cast<uint8_t>(d.isSfz ? 1 : 0)) &&
             writeString(stream, d.label) && writeString(stream, d.sourcePath) &&
             writeString(stream, d.sampleRelativePath) &&
             writeString(stream, d.regionsText) &&
             writeVal(stream, static_cast<int32_t>(d.regionCount)) &&
             writeVal(stream, static_cast<uint8_t>(d.drumKitModeEnabled ? 1 : 0)) &&
             writeVal(stream, static_cast<int32_t>(d.drumKitGroupIndex)) &&
             writeVal(stream, static_cast<uint32_t>(d.drumKitGroups.size()));
        for (const auto& g : d.drumKitGroups) {
            if (!ok) break;
            ok = writeVal(stream, static_cast<int32_t>(g.key)) &&
                 writeString(stream, g.regionsText) &&
                 writeVal(stream, static_cast<int32_t>(g.regionCount));
        }
        // v3: Sample tab.
        if (!ok) break;
        ok = writeVal(stream, static_cast<int32_t>(d.volume)) &&
             writeVal(stream, static_cast<int32_t>(d.pan)) &&
             writeVal(stream, static_cast<int32_t>(d.width)) &&
             writeVal(stream, static_cast<int32_t>(d.quality)) &&
             writeVal(stream, static_cast<int32_t>(d.polyphony)) &&
             writeVal(stream, static_cast<int32_t>(d.notePolyphony)) &&
             writeVal(stream, static_cast<uint8_t>(d.disableNoteSelfmask ? 1 : 0)) &&
             writeVal(stream, static_cast<int32_t>(d.loopModeIndex)) &&
             writeVal(stream, static_cast<uint8_t>(d.reverse ? 1 : 0)) &&
             writeVal(stream, static_cast<int32_t>(d.offset)) &&
             writeVal(stream, static_cast<int32_t>(d.vel2offset)) &&
             writeVal(stream, static_cast<uint8_t>(d.exclusiveClass ? 1 : 0)) &&
             writeVal(stream, static_cast<int32_t>(d.group)) &&
             writeVal(stream, static_cast<uint8_t>(d.offbyEnabled ? 1 : 0)) &&
             writeVal(stream, static_cast<int32_t>(d.offby)) &&
             writeVal(stream, static_cast<int32_t>(d.transpose)) &&
             writeVal(stream, static_cast<int32_t>(d.tune));
        // v4: Amp tab.
        if (!ok) break;
        ok = writeVal(stream, static_cast<uint32_t>(d.ampVelCurve.size()));
        for (const auto& pt : d.ampVelCurve) {
            if (!ok) break;
            ok = writeVal(stream, static_cast<int32_t>(pt.velocity)) &&
                 writeVal(stream, pt.gain);
        }
        // v5: Amp tab (amp_veltrack/amp_random/eg01_* envelope/vel2*). v6
        // dropped ampStartLevel (Start Level slider removed) from this
        // group without renumbering the rest.
        if (!ok) break;
        ok = writeVal(stream, static_cast<int32_t>(d.ampVeltrack)) &&
             writeVal(stream, static_cast<int32_t>(d.ampRandom)) &&
             writeVal(stream, d.ampAttackTime) &&
             writeVal(stream, d.ampHoldTime) &&
             writeVal(stream, d.ampDecayTime) &&
             writeVal(stream, d.ampSustainLevel) &&
             writeVal(stream, d.ampReleaseTime) &&
             writeVal(stream, d.ampAttackShape) &&
             writeVal(stream, d.ampDecayShape) &&
             writeVal(stream, d.ampReleaseShape) &&
             writeVal(stream, d.ampVel2Attack) &&
             writeVal(stream, d.ampVel2Sustain) &&
             writeVal(stream, static_cast<int32_t>(d.ampVel2Volume));
        // v7: Sample tab (panRandom/panAlternate).
        if (!ok) break;
        ok = writeVal(stream, static_cast<int32_t>(d.panRandom)) &&
             writeVal(stream, static_cast<uint8_t>(d.panAlternate ? 1 : 0));
        // v8: Fil tab.
        if (!ok) break;
        ok = writeVal(stream, static_cast<int32_t>(d.filterTypeIndex)) &&
             writeVal(stream, static_cast<int32_t>(d.filterCutoff)) &&
             writeVal(stream, d.filterResonance) &&
             writeVal(stream, static_cast<int32_t>(d.filterRandomCutoff)) &&
             writeVal(stream, static_cast<int32_t>(d.filVeltrack)) &&
             writeVal(stream, static_cast<int32_t>(d.resoVeltrack)) &&
             writeVal(stream, static_cast<uint8_t>(d.filterEgEnabled ? 1 : 0)) &&
             writeVal(stream, static_cast<int32_t>(d.filDepth)) &&
             writeVal(stream, d.filEgStartLevel) &&
             writeVal(stream, d.filEgDelayTime) &&
             writeVal(stream, d.filEgAttackTime) &&
             writeVal(stream, d.filEgAttackShape) &&
             writeVal(stream, d.filEgHoldTime) &&
             writeVal(stream, d.filEgDecayTime) &&
             writeVal(stream, d.filEgDecayShape) &&
             writeVal(stream, d.filEgSustainLevel) &&
             writeVal(stream, d.filEgReleaseTime) &&
             writeVal(stream, d.filEgReleaseShape);
        // v9: Pitch tab.
        if (!ok) break;
        ok = writeVal(stream, static_cast<int32_t>(d.pitchVeltrack)) &&
             writeVal(stream, static_cast<int32_t>(d.pitchRandom)) &&
             writeVal(stream, static_cast<uint8_t>(d.pitchEgEnabled ? 1 : 0)) &&
             writeVal(stream, static_cast<int32_t>(d.pitchDepth)) &&
             writeVal(stream, static_cast<int32_t>(d.pitchVel2Depth)) &&
             writeVal(stream, d.pitchEgStartLevel) &&
             writeVal(stream, d.pitchEgDelayTime) &&
             writeVal(stream, d.pitchEgAttackTime) &&
             writeVal(stream, d.pitchEgAttackShape) &&
             writeVal(stream, d.pitchEgHoldTime) &&
             writeVal(stream, d.pitchEgDecayTime) &&
             writeVal(stream, d.pitchEgDecayShape) &&
             writeVal(stream, d.pitchEgSustainLevel) &&
             writeVal(stream, d.pitchEgReleaseTime) &&
             writeVal(stream, d.pitchEgReleaseShape);
        // v10: Pitch tab.
        if (!ok) break;
        ok = writeVal(stream, static_cast<uint8_t>(d.pitchVel2Invert ? 1 : 0));
        // v11: Amp/Fil/Pitch Decay Time (Extra).
        if (!ok) break;
        ok = writeVal(stream, d.ampDecayTimeExtra) && writeVal(stream, d.filEgDecayTimeExtra) &&
             writeVal(stream, d.pitchEgDecayTimeExtra);
        // v12: Opcodes tab.
        if (!ok) break;
        ok = writeString(stream, d.customOpcodesText);
    }
    // v13: whole-instrument mpeEnabled (NOT per-drum - written once, here,
    // after the drum list).
    if (ok) ok = writeVal(stream, static_cast<uint8_t>(p->params.mpeEnabled.load() ? 1 : 0));
    // v14: whole-instrument bendUpCents/bendDownCents (NOT per-drum).
    if (ok)
        ok = writeVal(stream, static_cast<int32_t>(p->params.bendUpCents.load())) &&
             writeVal(stream, static_cast<int32_t>(p->params.bendDownCents.load()));
    return ok;
}

bool stateLoad(const clap_plugin_t* plugin, const clap_istream_t* stream) {
    Plugin* p = self(plugin);

    uint32_t magic = 0, version = 0, count = 0;
    if (!readVal(stream, magic) || magic != kStateMagic) return false;
    if (!readVal(stream, version) || version != kStateVersion) return false;
    if (!readVal(stream, count)) return false;

    std::vector<SharedParams::DrumItem> drums;
    drums.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        SharedParams::DrumItem d;
        int32_t rootNote = 60, outputIndex = 0, regionCount = 0;
        uint8_t hasSource = 0, isSfz = 0, drumKitModeEnabled = 0;
        int32_t drumKitGroupIndex = 0;
        uint32_t groupCount = 0;
        if (!readVal(stream, rootNote) || !readVal(stream, outputIndex) ||
            !readVal(stream, hasSource) || !readVal(stream, isSfz) ||
            !readString(stream, d.label) || !readString(stream, d.sourcePath) ||
            !readString(stream, d.sampleRelativePath) || !readString(stream, d.regionsText) ||
            !readVal(stream, regionCount) || !readVal(stream, drumKitModeEnabled) ||
            !readVal(stream, drumKitGroupIndex) || !readVal(stream, groupCount))
            return false;
        d.rootNote = std::clamp(rootNote, 0, 127);
        d.outputIndex = std::clamp(outputIndex, 0, kNumOutputs - 1);
        d.hasSource = hasSource != 0;
        d.isSfz = isSfz != 0;
        d.regionCount = regionCount;
        d.drumKitModeEnabled = drumKitModeEnabled != 0;
        d.drumKitGroupIndex = drumKitGroupIndex;
        d.drumKitGroups.reserve(groupCount);
        for (uint32_t g = 0; g < groupCount; ++g) {
            DrumKitKeyGroup group;
            int32_t key = 0, regCount = 0;
            if (!readVal(stream, key) || !readString(stream, group.regionsText) ||
                !readVal(stream, regCount))
                return false;
            group.key = key;
            group.regionCount = regCount;
            d.drumKitGroups.push_back(std::move(group));
        }

        // v3: Sample tab.
        int32_t volume = 0, pan = 0, width = 100, quality = 4, polyphony = 32, notePolyphony = 32,
                loopModeIndex = 1, offset = 0, vel2offset = 0, group2 = 0, offby = 0,
                transpose = 0, tune = 0;
        uint8_t disableNoteSelfmask = 0, reverse = 0, exclusiveClass = 0, offbyEnabled = 0;
        if (!readVal(stream, volume) || !readVal(stream, pan) || !readVal(stream, width) ||
            !readVal(stream, quality) || !readVal(stream, polyphony) ||
            !readVal(stream, notePolyphony) || !readVal(stream, disableNoteSelfmask) ||
            !readVal(stream, loopModeIndex) || !readVal(stream, reverse) ||
            !readVal(stream, offset) || !readVal(stream, vel2offset) ||
            !readVal(stream, exclusiveClass) || !readVal(stream, group2) ||
            !readVal(stream, offbyEnabled) || !readVal(stream, offby) ||
            !readVal(stream, transpose) || !readVal(stream, tune))
            return false;
        d.volume = std::clamp(volume, -48, 48);
        d.pan = std::clamp(pan, -100, 100);
        d.width = std::clamp(width, 0, 100);
        d.quality = std::clamp(quality, 0, 9);
        d.polyphony = std::clamp(polyphony, 1, 256);
        d.notePolyphony = std::clamp(notePolyphony, 1, 256);
        d.disableNoteSelfmask = disableNoteSelfmask != 0;
        d.loopModeIndex = std::clamp(loopModeIndex, 0, 4);
        d.reverse = reverse != 0;
        d.offset = std::clamp(offset, 0, 4096);
        d.vel2offset = std::clamp(vel2offset, -4096, 0);
        d.exclusiveClass = exclusiveClass != 0;
        d.group = std::clamp(group2, 0, 99);
        d.offbyEnabled = offbyEnabled != 0;
        d.offby = std::clamp(offby, 0, 99);
        d.transpose = std::clamp(transpose, -24, 24);
        d.tune = std::clamp(tune, -99, 99);

        // v4: Amp tab.
        uint32_t ampPointCount = 0;
        if (!readVal(stream, ampPointCount)) return false;
        d.ampVelCurve.reserve(ampPointCount);
        for (uint32_t pi = 0; pi < ampPointCount; ++pi) {
            int32_t velocity = 1;
            float gain = 1.f;
            if (!readVal(stream, velocity) || !readVal(stream, gain)) return false;
            d.ampVelCurve.push_back(
                {std::clamp(velocity, 1, 127), std::clamp(gain, 0.f, 1.f)});
        }

        // v5: Amp tab (v6 dropped ampStartLevel, see kStateVersion's comment).
        int32_t ampVeltrack = 100, ampRandom = 0, ampVel2Volume = 0;
        float ampAttackTime = 0.00001f, ampHoldTime = 0.00001f, ampDecayTime = 0.00001f,
              ampSustainLevel = 1.f, ampReleaseTime = 0.00001f, ampAttackShape = 0.00001f,
              ampDecayShape = -0.3616f, ampReleaseShape = -6.3616f, ampVel2Attack = 0.f,
              ampVel2Sustain = 0.f;
        if (!readVal(stream, ampVeltrack) || !readVal(stream, ampRandom) ||
            !readVal(stream, ampAttackTime) ||
            !readVal(stream, ampHoldTime) || !readVal(stream, ampDecayTime) ||
            !readVal(stream, ampSustainLevel) || !readVal(stream, ampReleaseTime) ||
            !readVal(stream, ampAttackShape) || !readVal(stream, ampDecayShape) ||
            !readVal(stream, ampReleaseShape) || !readVal(stream, ampVel2Attack) ||
            !readVal(stream, ampVel2Sustain) || !readVal(stream, ampVel2Volume))
            return false;
        d.ampVeltrack = std::clamp(ampVeltrack, -100, 100);
        d.ampRandom = std::clamp(ampRandom, -24, 24);
        d.ampAttackTime = std::clamp(ampAttackTime, 0.00001f, 0.1f);
        d.ampHoldTime = std::clamp(ampHoldTime, 0.00001f, 0.1f);
        d.ampDecayTime = std::clamp(ampDecayTime, 0.00001f, 0.1f);
        d.ampSustainLevel = std::clamp(ampSustainLevel, 0.f, 1.f);
        d.ampReleaseTime = std::clamp(ampReleaseTime, 0.00001f, 12.f);
        d.ampAttackShape = std::clamp(ampAttackShape, -11.f, 11.f);
        d.ampDecayShape = std::clamp(ampDecayShape, -11.f, 11.f);
        d.ampReleaseShape = std::clamp(ampReleaseShape, -11.f, 11.f);
        d.ampVel2Attack = std::clamp(ampVel2Attack, -0.1f, 0.f);
        d.ampVel2Sustain = std::clamp(ampVel2Sustain, -1.f, 1.f);
        d.ampVel2Volume = std::clamp(ampVel2Volume, -24, 24);

        // v7: Sample tab (panRandom/panAlternate).
        uint8_t panAlternate = 0;
        int32_t panRandom = 0;
        if (!readVal(stream, panRandom) || !readVal(stream, panAlternate)) return false;
        d.panRandom = std::clamp(panRandom, -200, 200);
        d.panAlternate = panAlternate != 0;

        // v8: Fil tab.
        int32_t filterTypeIndex = 1, filterCutoff = 20000, filterRandomCutoff = 0, filVeltrack = 0,
                resoVeltrack = 0, filDepth = 0;
        uint8_t filterEgEnabled = 0;
        float filterResonance = 0.f, filEgStartLevel = 0.f, filEgDelayTime = 0.00001f,
              filEgAttackTime = 0.00001f, filEgAttackShape = 0.00001f, filEgHoldTime = 0.00001f,
              filEgDecayTime = 0.00001f, filEgDecayShape = 0.00001f, filEgSustainLevel = 1.f,
              filEgReleaseTime = 0.00001f, filEgReleaseShape = 0.00001f;
        if (!readVal(stream, filterTypeIndex) || !readVal(stream, filterCutoff) ||
            !readVal(stream, filterResonance) || !readVal(stream, filterRandomCutoff) ||
            !readVal(stream, filVeltrack) || !readVal(stream, resoVeltrack) ||
            !readVal(stream, filterEgEnabled) || !readVal(stream, filDepth) ||
            !readVal(stream, filEgStartLevel) || !readVal(stream, filEgDelayTime) ||
            !readVal(stream, filEgAttackTime) || !readVal(stream, filEgAttackShape) ||
            !readVal(stream, filEgHoldTime) || !readVal(stream, filEgDecayTime) ||
            !readVal(stream, filEgDecayShape) || !readVal(stream, filEgSustainLevel) ||
            !readVal(stream, filEgReleaseTime) || !readVal(stream, filEgReleaseShape))
            return false;
        d.filterTypeIndex = std::clamp(filterTypeIndex, 0, 22);
        d.filterCutoff = std::clamp(filterCutoff, 1, 20000);
        d.filterResonance = std::clamp(filterResonance, -40.f, 40.f);
        d.filterRandomCutoff = std::clamp(filterRandomCutoff, 1, 20000);
        d.filVeltrack = std::clamp(filVeltrack, 0, 20000);
        d.resoVeltrack = std::clamp(resoVeltrack, -40, 40);
        d.filterEgEnabled = filterEgEnabled != 0;
        d.filDepth = std::clamp(filDepth, 0, 20000);
        d.filEgStartLevel = std::clamp(filEgStartLevel, 0.f, 1.f);
        d.filEgDelayTime = std::clamp(filEgDelayTime, 0.00001f, 0.1f);
        d.filEgAttackTime = std::clamp(filEgAttackTime, 0.00001f, 0.1f);
        d.filEgAttackShape = std::clamp(filEgAttackShape, -11.f, 11.f);
        d.filEgHoldTime = std::clamp(filEgHoldTime, 0.00001f, 0.1f);
        d.filEgDecayTime = std::clamp(filEgDecayTime, 0.00001f, 0.1f);
        d.filEgDecayShape = std::clamp(filEgDecayShape, -11.f, 11.f);
        d.filEgSustainLevel = std::clamp(filEgSustainLevel, 0.f, 1.f);
        d.filEgReleaseTime = std::clamp(filEgReleaseTime, 0.00001f, 0.1f);
        d.filEgReleaseShape = std::clamp(filEgReleaseShape, -11.f, 11.f);

        // v9: Pitch tab.
        int32_t pitchVeltrack = 0, pitchRandom = 0, pitchDepth = 0, pitchVel2Depth = 0;
        uint8_t pitchEgEnabled = 0;
        float pitchEgStartLevel = 0.f, pitchEgDelayTime = 0.00001f, pitchEgAttackTime = 0.00001f,
              pitchEgAttackShape = 0.00001f, pitchEgHoldTime = 0.00001f,
              pitchEgDecayTime = 0.00001f, pitchEgDecayShape = 0.00001f,
              pitchEgSustainLevel = 1.f, pitchEgReleaseTime = 0.00001f,
              pitchEgReleaseShape = 0.00001f;
        if (!readVal(stream, pitchVeltrack) || !readVal(stream, pitchRandom) ||
            !readVal(stream, pitchEgEnabled) || !readVal(stream, pitchDepth) ||
            !readVal(stream, pitchVel2Depth) || !readVal(stream, pitchEgStartLevel) ||
            !readVal(stream, pitchEgDelayTime) || !readVal(stream, pitchEgAttackTime) ||
            !readVal(stream, pitchEgAttackShape) || !readVal(stream, pitchEgHoldTime) ||
            !readVal(stream, pitchEgDecayTime) || !readVal(stream, pitchEgDecayShape) ||
            !readVal(stream, pitchEgSustainLevel) || !readVal(stream, pitchEgReleaseTime) ||
            !readVal(stream, pitchEgReleaseShape))
            return false;
        d.pitchVeltrack = std::clamp(pitchVeltrack, -9600, 9600);
        d.pitchRandom = std::clamp(pitchRandom, 0, 9600);
        d.pitchEgEnabled = pitchEgEnabled != 0;
        d.pitchDepth = std::clamp(pitchDepth, -9600, 9600);
        d.pitchVel2Depth = std::clamp(pitchVel2Depth, -9600, 9600);
        d.pitchEgStartLevel = std::clamp(pitchEgStartLevel, 0.f, 1.f);
        d.pitchEgDelayTime = std::clamp(pitchEgDelayTime, 0.00001f, 0.1f);
        d.pitchEgAttackTime = std::clamp(pitchEgAttackTime, 0.00001f, 0.1f);
        d.pitchEgAttackShape = std::clamp(pitchEgAttackShape, -11.f, 11.f);
        d.pitchEgHoldTime = std::clamp(pitchEgHoldTime, 0.00001f, 0.1f);
        d.pitchEgDecayTime = std::clamp(pitchEgDecayTime, 0.00001f, 0.1f);
        d.pitchEgDecayShape = std::clamp(pitchEgDecayShape, -11.f, 11.f);
        d.pitchEgSustainLevel = std::clamp(pitchEgSustainLevel, 0.f, 1.f);
        d.pitchEgReleaseTime = std::clamp(pitchEgReleaseTime, 0.00001f, 0.1f);
        d.pitchEgReleaseShape = std::clamp(pitchEgReleaseShape, -11.f, 11.f);

        // v10: Pitch tab.
        uint8_t pitchVel2Invert = 0;
        if (!readVal(stream, pitchVel2Invert)) return false;
        d.pitchVel2Invert = pitchVel2Invert != 0;

        // v11: Amp/Fil/Pitch Decay Time (Extra).
        float ampDecayTimeExtra = 0.f, filEgDecayTimeExtra = 0.f, pitchEgDecayTimeExtra = 0.f;
        if (!readVal(stream, ampDecayTimeExtra) || !readVal(stream, filEgDecayTimeExtra) ||
            !readVal(stream, pitchEgDecayTimeExtra))
            return false;
        d.ampDecayTimeExtra = std::clamp(ampDecayTimeExtra, 0.f, 12.f);
        d.filEgDecayTimeExtra = std::clamp(filEgDecayTimeExtra, 0.f, 12.f);
        d.pitchEgDecayTimeExtra = std::clamp(pitchEgDecayTimeExtra, 0.f, 12.f);

        // v12: Opcodes tab.
        if (!readString(stream, d.customOpcodesText)) return false;

        drums.push_back(std::move(d));
    }

    // v13: whole-instrument mpeEnabled (NOT per-drum).
    uint8_t mpeEnabled = 0;
    if (!readVal(stream, mpeEnabled)) return false;

    // v14: whole-instrument bendUpCents/bendDownCents (NOT per-drum).
    int32_t bendUpCents = 2400, bendDownCents = -2400;
    if (!readVal(stream, bendUpCents) || !readVal(stream, bendDownCents)) return false;

    int nextId = 1;
    for (auto& d : drums) d.id = nextId++;

    {
        std::lock_guard<std::mutex> lock(p->params.guiState.mutex);
        p->params.guiState.drums = std::move(drums);
        p->params.guiState.nextId = nextId;
        p->params.guiState.selectedId = -1;
        p->params.guiState.listError.clear();
    }
    p->params.mpeEnabled = mpeEnabled != 0;
    p->params.bendUpCents = bendUpCents;
    p->params.bendDownCents = bendDownCents;
    regenerateAndLoadDrumSfz(p);
    return true;
}

const clap_plugin_state_t kExtState = {
    .save = stateSave,
    .load = stateLoad,
};

// --------------------------------------------------------------------- gui

bool guiIsApiSupported(const clap_plugin_t*, const char* api, bool is_floating) {
    return !is_floating && strcmp(api, CLAP_WINDOW_API_X11) == 0;
}

bool guiGetPreferredApi(const clap_plugin_t*, const char** api, bool* is_floating) {
    *api = CLAP_WINDOW_API_X11;
    *is_floating = false;
    return true;
}

bool guiCreate(const clap_plugin_t* plugin, const char* api, bool is_floating) {
    if (!guiIsApiSupported(plugin, api, is_floating)) return false;
    Plugin* p = self(plugin);
    if (p->window) return true;
    p->window = std::make_unique<GuiWindow>(
        [p] {
            drawEditorUI(
                p->params, p->uiState, [p] { regenerateAndLoadDrumSfz(p); },
                [p](const std::string& path) { loadIntoSelectedDrum(p, path); },
                [p](const std::string& path) { previewFile(p, path); },
                [p] { handleSavePreset(p); }, [p] { handleLoadPreset(p); },
                [p] { handleSaveProfile(p); }, [p] { handleLoadProfile(p); },
                [p] { explodeSelectedDrumKit(p); }, [p] { initKit(p); });
        },
        [p](const std::vector<std::string>& paths) {
            // XDND drop: same "load into whatever's selected" as a file
            // explorer double-click - a drum takes exactly one source, so
            // only the first dropped path is used if several were dragged
            // together. Unlike a double-click (the user is already browsing
            // that folder), a successful drop also jumps the embedded
            // explorer to the dropped file's own folder - the point being a
            // fast path from "wherever the DAW/system file manager's drag
            // came from" into the in-plugin browser, per user spec.
            if (paths.empty()) return;
            const std::string& path = paths.front();
            if (loadIntoSelectedDrum(p, path))
                p->uiState.fileExplorer.jumpTo(
                    std::filesystem::path(path).parent_path().string());
        });
    return p->window->create(kDefaultW, kDefaultH);
}

void guiDestroy(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    if (p->window) {
        p->window->destroy();
        p->window.reset();
    }
}

bool guiSetScale(const clap_plugin_t*, double) { return true; }

bool guiGetSize(const clap_plugin_t* plugin, uint32_t* w, uint32_t* h) {
    Plugin* p = self(plugin);
    if (p->window) p->window->getSize(*w, *h);
    else { *w = kDefaultW; *h = kDefaultH; }
    return true;
}

bool guiCanResize(const clap_plugin_t*) { return true; }

bool guiGetResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t* hints) {
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = false;
    return true;
}

bool guiAdjustSize(const clap_plugin_t*, uint32_t* w, uint32_t* h) {
    if (*w < 640) *w = 640;
    if (*h < 420) *h = 420;
    return true;
}

bool guiSetSize(const clap_plugin_t* plugin, uint32_t w, uint32_t h) {
    Plugin* p = self(plugin);
    if (p->window) p->window->setSize(w, h);
    return true;
}

bool guiSetParent(const clap_plugin_t* plugin, const clap_window_t* win) {
    Plugin* p = self(plugin);
    if (!p->window || !win) return false;
    p->window->setParent((unsigned long)win->x11);
    return true;
}

bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return true; }

void guiSuggestTitle(const clap_plugin_t*, const char*) {}

bool guiShow(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    if (!p->window) return false;
    p->window->setVisible(true);
    return true;
}

bool guiHide(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    if (!p->window) return false;
    p->window->setVisible(false);
    return true;
}

const clap_plugin_gui_t kExtGui = {
    .is_api_supported = guiIsApiSupported,
    .get_preferred_api = guiGetPreferredApi,
    .create = guiCreate,
    .destroy = guiDestroy,
    .set_scale = guiSetScale,
    .get_size = guiGetSize,
    .can_resize = guiCanResize,
    .get_resize_hints = guiGetResizeHints,
    .adjust_size = guiAdjustSize,
    .set_size = guiSetSize,
    .set_parent = guiSetParent,
    .set_transient = guiSetTransient,
    .suggest_title = guiSuggestTitle,
    .show = guiShow,
    .hide = guiHide,
};

// ------------------------------------------------------------------ plugin

bool plugInit(const clap_plugin_t*) { return true; }

void plugDestroy(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    guiDestroy(plugin);
    delete p;
}

bool plugActivate(const clap_plugin_t* plugin, double sampleRate, uint32_t,
                  uint32_t maxFrames) {
    Plugin* p = self(plugin);
    p->engine.prepare(sampleRate, static_cast<int>(maxFrames));
    p->previewEngine.prepare(sampleRate, static_cast<int>(maxFrames));
    p->previewBufL.assign(maxFrames, 0.0f);
    p->previewBufR.assign(maxFrames, 0.0f);
    // Loads whatever the drum list currently holds (empty on first
    // activation - silence is correct until drums are added/loaded).
    regenerateAndLoadDrumSfz(p);
    return true;
}

void plugDeactivate(const clap_plugin_t*) {}

bool plugStartProcessing(const clap_plugin_t*) { return true; }

void plugStopProcessing(const clap_plugin_t*) {}

void plugReset(const clap_plugin_t*) {}

clap_process_status plugProcess(const clap_plugin_t* plugin, const clap_process_t* process) {
    Plugin* p = self(plugin);
    if (process->audio_outputs_count < static_cast<uint32_t>(kNumOutputs))
        return CLAP_PROCESS_CONTINUE;

    float* channels[kNumOutputs * 2];
    for (int i = 0; i < kNumOutputs; ++i) {
        const auto& ob = process->audio_outputs[i];
        if (!ob.data32 || !ob.data32[0] || !ob.data32[1]) return CLAP_PROCESS_CONTINUE;
        channels[i * 2 + 0] = ob.data32[0];
        channels[i * 2 + 1] = ob.data32[1];
    }

    constexpr uint32_t kMaxEvents = 256;
    SfizzEngine::MidiEvent events[kMaxEvents];
    uint32_t numEvents = 0;

    auto noteOnVisual = [p](int note, int velocity) {
        if (note < 0 || note > 127) return;
        p->params.noteActive[static_cast<size_t>(note)] = true;
        p->params.noteVelocity01[static_cast<size_t>(note)] = velocity / 127.0f;
    };
    auto noteOffVisual = [p](int note) {
        if (note < 0 || note > 127) return;
        p->params.noteActive[static_cast<size_t>(note)] = false;
    };

    // Every Note On is preceded by a synthetic CC131 = velocity, on the same
    // channel/delay: CC131 isn't a real controller, it's how the Sample
    // tab's "vel2offset" (offset_oncc131=, see DrumSfzBuilder.cpp) tracks
    // note velocity - sfizz has no native velocity->offset opcode, so the
    // plugin fabricates the CC itself, ordered strictly before the Note On
    // so the voice sees the right value the instant it triggers.
    auto pushNoteOn = [&](int delay, int channel, int note, int velocity) {
        if (numEvents + 1 >= kMaxEvents) return; // need room for both events
        // {type, delaySamples, noteNumber, velocity, ccNumber, ccValue, pitch, channel}
        events[numEvents++] = {SfizzEngine::MidiEvent::Type::CC, delay, 0, 0, 131, velocity, 0,
                               channel};
        events[numEvents++] = {SfizzEngine::MidiEvent::Type::NoteOn, delay, note, velocity, 0, 0,
                               0, channel};
        noteOnVisual(note, velocity);
    };

    // GUI -> audio handoff: piano preview clicks, drained once per block,
    // injected ahead of anything parsed from in_events.
    if (p->params.pendingPreviewNoteOn.exchange(false)) {
        int note = p->params.previewNoteOnNumber.load();
        int vel = p->params.previewNoteOnVelocity.load();
        pushNoteOn(0, 0, note, vel);
    }
    if (numEvents < kMaxEvents && p->params.pendingPreviewNoteOff.exchange(false)) {
        int note = p->params.previewNoteOffNumber.load();
        events[numEvents++] = {SfizzEngine::MidiEvent::Type::NoteOff, 0, note, 0, 0, 0, 0};
        noteOffVisual(note);
    }

    const uint32_t inCount = process->in_events->size(process->in_events);
    for (uint32_t i = 0; i < inCount && numEvents < kMaxEvents; ++i) {
        const clap_event_header_t* hdr = process->in_events->get(process->in_events, i);
        if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID || hdr->type != CLAP_EVENT_MIDI)
            continue;
        const auto* midi = reinterpret_cast<const clap_event_midi_t*>(hdr);
        const int delay = static_cast<int>(hdr->time);
        const uint8_t status = midi->data[0] & 0xF0;
        const int d1 = midi->data[1], d2 = midi->data[2];
        const int channel = midi->data[0] & 0x0F;

        if (status == 0x90 && d2 > 0) {
            pushNoteOn(delay, channel, d1, d2);
            continue;
        }

        SfizzEngine::MidiEvent ev{};
        ev.delaySamples = delay;
        ev.channel = channel;
        if (status == 0x80 || (status == 0x90 && d2 == 0)) {
            ev.type = SfizzEngine::MidiEvent::Type::NoteOff;
            ev.noteNumber = d1;
            ev.velocity = d2;
            noteOffVisual(d1);
        } else if (status == 0xB0) {
            ev.type = SfizzEngine::MidiEvent::Type::CC;
            ev.ccNumber = d1;
            ev.ccValue = d2;
        } else if (status == 0xE0) {
            ev.type = SfizzEngine::MidiEvent::Type::PitchWheel;
            ev.pitch = ((d2 << 7) | d1) - 8192;
        } else {
            continue; // aftertouch/program-change/etc. not handled
        }
        events[numEvents++] = ev;
    }

    p->engine.renderBlock(events, static_cast<int>(numEvents), channels, kNumOutputs * 2,
                          static_cast<int>(process->frames_count),
                          p->params.mpeEnabled.load(), 440.0f);
    p->params.activeVoiceCount = p->engine.activeVoiceCount();

    // File explorer's sample-click preview - always mixed into Out 1 only,
    // never broadcast to every output: with 8 outputs likely feeding 8
    // separate DAW tracks/stems, injecting an audition click into every one
    // of them would pollute stems the user isn't even monitoring.
    {
        SfizzEngine::MidiEvent previewEvents[2];
        int numPreview = 0;
        if (p->params.pendingFilePreviewTrigger.exchange(false)) {
            previewEvents[numPreview++] = {SfizzEngine::MidiEvent::Type::NoteOff, 0, 60, 0, 0, 0, 0};
            previewEvents[numPreview++] = {SfizzEngine::MidiEvent::Type::NoteOn, 0, 60, 100, 0, 0, 0};
        }
        float* previewChannels[2] = {p->previewBufL.data(), p->previewBufR.data()};
        p->previewEngine.renderBlock(previewEvents, numPreview, previewChannels, 2,
                                     static_cast<int>(process->frames_count), false, 440.0f);
        const float vol =
            p->params.previewEnabled.load() ? p->params.previewVolume.load() : 0.0f;
        if (vol > 0.0f) {
            for (uint32_t i = 0; i < process->frames_count; ++i) {
                channels[0][i] += p->previewBufL[i] * vol;
                channels[1][i] += p->previewBufR[i] * vol;
            }
        }
    }

    return CLAP_PROCESS_CONTINUE;
}

const void* plugGetExtension(const clap_plugin_t*, const char* id) {
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kExtAudioPorts;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kExtNotePorts;
    if (!strcmp(id, CLAP_EXT_NOTE_NAME)) return &kExtNoteName;
    if (!strcmp(id, CLAP_EXT_GUI)) return &kExtGui;
    if (!strcmp(id, CLAP_EXT_STATE)) return &kExtState;
    return nullptr;
}

void plugOnMainThread(const clap_plugin_t* plugin) {
    Plugin* p = self(plugin);
    if (!p->noteNamesChangedPending.exchange(false)) return;
    if (!p->host) return;
    const auto* hostNoteName = static_cast<const clap_host_note_name_t*>(
        p->host->get_extension(p->host, CLAP_EXT_NOTE_NAME));
    if (hostNoteName && hostNoteName->changed) hostNoteName->changed(p->host);
}

// ----------------------------------------------------------------- factory

const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host,
                                   const char* plugin_id) {
    if (strcmp(plugin_id, kDesc.id) != 0) return nullptr;
    Plugin* p = new (std::nothrow) Plugin(host);
    if (!p) return nullptr;
    p->plug.desc = &kDesc;
    p->plug.plugin_data = p;
    p->plug.init = plugInit;
    p->plug.destroy = plugDestroy;
    p->plug.activate = plugActivate;
    p->plug.deactivate = plugDeactivate;
    p->plug.start_processing = plugStartProcessing;
    p->plug.stop_processing = plugStopProcessing;
    p->plug.reset = plugReset;
    p->plug.process = plugProcess;
    p->plug.get_extension = plugGetExtension;
    p->plug.on_main_thread = plugOnMainThread;
    return &p->plug;
}

uint32_t factoryCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* factoryDesc(const clap_plugin_factory_t*, uint32_t index) {
    return index == 0 ? &kDesc : nullptr;
}

const clap_plugin_factory_t kFactory = {
    .get_plugin_count = factoryCount,
    .get_plugin_descriptor = factoryDesc,
    .create_plugin = factoryCreate,
};

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryGetFactory(const char* id) {
    if (!strcmp(id, CLAP_PLUGIN_FACTORY_ID)) return &kFactory;
    return nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = entryInit,
    .deinit = entryDeinit,
    .get_factory = entryGetFactory,
};
