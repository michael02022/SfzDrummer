// sfzdrummer's ImGui widget layer - draws the whole plugin window each
// frame: the drum list (left), the file explorer (middle), the per-drum
// config tabs (right, placeholders for now), and the root-note piano
// (bottom). Knows nothing about CLAP/sfizz directly: it reads/writes
// SharedParams (including the drum list, under its own mutex) and calls
// back into plugin.cpp for anything that needs engine access.
#pragma once

#include <functional>
#include <string>

#include "file_explorer.hpp"
#include "shared.hpp"

// GUI-thread-only interactive state - never touched by the audio thread.
struct EditorUIState {
    int heldPianoKey = -1;
    bool pianoScrolledOnce = false;
    FileExplorer fileExplorer;
    // VelSW tab's own embedded mini browser - a separate instance so
    // browsing there doesn't disturb the main explorer's own cwd/selection.
    // Kept in sync one-way (main -> here, never the reverse) by plugin.cpp's
    // loadIntoSelectedDrum every time it loads something into the selected
    // drum - see drawVelSwTab.
    FileExplorer velSwFileExplorer;

    // Amp tab's amp_velcurve envelope editor drag state (see
    // editor_ui.cpp's drawAmpEnvelope) - which point (index into the
    // selected drum's ampVelCurve) is currently being dragged, if any.
    int ampEnvDragIdx = -1;
    bool ampEnvDragging = false;

    // One-shot trigger for the Sample tab's "Load regions as individual
    // percussion" confirmation modal - set by the button click, consumed
    // (ImGui::OpenPopup + cleared) on the next draw, same deferred-open
    // pattern SoloSampler's own "Reset to Default" confirmation uses
    // (sibling project).
    bool explodeKitConfirmTrigger = false;
    // That modal's own "Add on top of existing percussion" checkbox state -
    // unchecked (the default) replaces the whole kit, per
    // plugin.cpp's explodeSelectedDrumKit; checked keeps every other drum
    // already in the list, for the exceptional case where the caller
    // actually wants to layer a second kit's pads alongside the first.
    // Persists across modal opens (not reset on confirm/cancel) since it's
    // a rarely-changed preference, not a one-shot trigger.
    bool explodeKitAddOnTop = false;

    // Same deferred-open trigger, for the persistent top row's "Init Kit"
    // confirmation modal.
    bool initKitConfirmTrigger = false;
};

// onParamChanged: call whenever a drum-list edit that feeds the SFZ text
// changes (output index, root note via piano) - plugin.cpp regenerates and
// reloads the whole kit.
// onLoadRequested: file explorer double-click (sample or .sfz) - loads into
// whichever drum is currently selected (guiState.selectedId); plugin.cpp
// reports "no drum selected"/"bad file" via guiState.listError.
// onVelSwLoadRequested: VelSW tab's own mini file explorer double-click (see
// drawVelSwTab) - adds the sample as a new, weaker layer under the selected
// drum's main sample (plugin.cpp's addVelSwitchLayer). Rejects .sfz and
// ineligible drums (isSfz/no source/no selection) via the same
// guiState.listError toast as onLoadRequested's own failures.
// onPreviewRequested: file explorer single click on a sample row - auditions
// the raw file via plugin.cpp's dedicated preview engine. Shared verbatim by
// both the main explorer and the VelSW mini explorer - one preview
// engine/control, never duplicated.
// onSavePreset/onLoadPreset/onSaveProfile/onLoadProfile: the persistent
// button row above everything else - each opens plugin.cpp's own zenity
// dialog and does the actual file I/O; failures report through the same
// guiState.listError toast as a bad sample/sfz load.
// onSavePerc/onLoadPerc: persistent row, same convention - save/restore ONE
// drum's full identity+design (source, label, note, output, VelSW layers,
// AND design) as its own .drmperc file, portable across kits. Unlike
// onSaveProfile/onLoadProfile (design-only), onLoadPerc always appends a
// new drum (plugin.cpp's loadPercFromPath) rather than overwriting the
// current selection.
// onSetKitPath: persistent row's "Set Kit Folder..." button - opens
// plugin.cpp's folder picker and updates guiState.kitPath (see its own
// comment in shared.hpp) plus re-resolves every already-loaded kit-relative
// source against the new folder.
// onExplodeDrumKit: Sample tab's "Load regions as individual percussion"
// confirmation modal's Confirm button - explodes the selected drum's
// already-discovered Drum Kit Mode key groups into one separate drum per
// key (plugin.cpp's explodeSelectedDrumKit). The bool is that modal's "Add
// on top of existing percussion" checkbox: false (default) replaces the
// whole kit, true keeps every other drum already in the list.
// onInitKit: persistent top row's "Init Kit" confirmation modal's Confirm
// button - wipes the whole kit (drum list + mpeEnabled/Bend Range) back to
// a freshly-loaded plugin's own defaults (plugin.cpp's initKit).
void drawEditorUI(SharedParams& params, EditorUIState& ui,
                  const std::function<void()>& onParamChanged,
                  const std::function<void(const std::string&)>& onLoadRequested,
                  const std::function<void(const std::string&)>& onVelSwLoadRequested,
                  const std::function<void(const std::string&)>& onPreviewRequested,
                  const std::function<void()>& onSavePreset,
                  const std::function<void()>& onLoadPreset,
                  const std::function<void()>& onSaveProfile,
                  const std::function<void()>& onLoadProfile,
                  const std::function<void()>& onSavePerc,
                  const std::function<void()>& onLoadPerc,
                  const std::function<void()>& onSetKitPath,
                  const std::function<void(bool)>& onExplodeDrumKit,
                  const std::function<void()>& onInitKit);
