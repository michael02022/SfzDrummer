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
// onPreviewRequested: file explorer single click on a sample row - auditions
// the raw file via plugin.cpp's dedicated preview engine.
// onSavePreset/onLoadPreset/onSaveProfile/onLoadProfile: the persistent
// button row above everything else - each opens plugin.cpp's own zenity
// dialog and does the actual file I/O; failures report through the same
// guiState.listError toast as a bad sample/sfz load.
// onExplodeDrumKit: Sample tab's "Load regions as individual percussion"
// confirmation modal's Confirm button - explodes the selected drum's
// already-discovered Drum Kit Mode key groups into one separate drum per
// key (plugin.cpp's explodeSelectedDrumKit).
// onInitKit: persistent top row's "Init Kit" confirmation modal's Confirm
// button - wipes the whole kit (drum list + mpeEnabled/Bend Range) back to
// a freshly-loaded plugin's own defaults (plugin.cpp's initKit).
void drawEditorUI(SharedParams& params, EditorUIState& ui,
                  const std::function<void()>& onParamChanged,
                  const std::function<void(const std::string&)>& onLoadRequested,
                  const std::function<void(const std::string&)>& onPreviewRequested,
                  const std::function<void()>& onSavePreset,
                  const std::function<void()>& onLoadPreset,
                  const std::function<void()>& onSaveProfile,
                  const std::function<void()>& onLoadProfile,
                  const std::function<void()>& onExplodeDrumKit,
                  const std::function<void()>& onInitKit);
