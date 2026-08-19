#include "editor_ui.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h" // ImGuiInputTextState - see drawOpcodesTab's context menu

namespace {

std::string noteName(int n) {
    static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F",
                                     "F#", "G", "G#", "A", "A#", "B"};
    int octave = n / 12 - 1;
    return std::string(kNames[n % 12]) + std::to_string(octave);
}

// ------------------------------------------------------------- piano

// Ported from SoloSampler's drawPiano (sibling project) - same geometry/
// velocity-from-click-height/scroll-to-root behavior, but the root-note
// marker and the right-click target are parameters instead of a single
// global SharedParams field, since here they depend on whichever drum is
// currently selected (see drawEditorUI). hasDrum[n] additionally grays out
// any key with no loaded percussion assigned to it, so the user can see at a
// glance which notes actually make sound instead of guessing/remembering.
void drawPiano(SharedParams& params, EditorUIState& ui, float width, int rootMarker,
               const std::array<bool, 128>& hasDrum,
               const std::function<void(int)>& onSetRootNote) {
    const float pianoH = 88.f;
    const float whiteW = 13.f;
    static const bool kBlackMap[12] = {false, true, false, true, false, false,
                                       true, false, true, false, true, false};
    const float contentW = 76 * whiteW; // 128 MIDI notes: 75 white + margin

    ImGui::BeginChild("##piano",
                      ImVec2(width, pianoH + ImGui::GetStyle().ScrollbarSize + 4),
                      false, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##pianokeys", ImVec2(contentW, pianoH),
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    ImVec2 mouse = ImGui::GetIO().MousePos;

    struct Key { float x0, x1; bool black; };
    static Key keys[128];
    static bool geomDone = false;
    if (!geomDone) {
        geomDone = true;
        int whiteIdx = 0;
        for (int n = 0; n < 128; ++n) {
            if (!kBlackMap[n % 12]) {
                keys[n] = {whiteIdx * whiteW, (whiteIdx + 1) * whiteW, false};
                ++whiteIdx;
            } else {
                float cx = whiteIdx * whiteW;
                keys[n] = {cx - whiteW * 0.32f, cx + whiteW * 0.32f, true};
            }
        }
    }

    int hitNote = -1;
    float hitRelY = 0.f;
    if (hovered) {
        float mx = mouse.x - p0.x, my = mouse.y - p0.y;
        for (int n = 0; n < 128 && hitNote < 0; ++n)
            if (keys[n].black && my < pianoH * 0.62f && mx >= keys[n].x0 &&
                mx < keys[n].x1) {
                hitNote = n;
                hitRelY = my / (pianoH * 0.62f);
            }
        for (int n = 0; n < 128 && hitNote < 0; ++n)
            if (!keys[n].black && mx >= keys[n].x0 && mx < keys[n].x1) {
                hitNote = n;
                hitRelY = my / pianoH;
            }
    }

    // Press: velocity from vertical click position (bottom = loud, top =
    // soft). Auditions the MAIN engine - the real configured kit, whatever
    // master(s) that note happens to hit.
    if (hitNote >= 0 && ui.heldPianoKey < 0 && ImGui::IsMouseClicked(0)) {
        int velocity = 1 + static_cast<int>(
                               std::lround(std::clamp(hitRelY, 0.f, 1.f) * 126.0f));
        ui.heldPianoKey = hitNote;
        params.previewNoteOnNumber = hitNote;
        params.previewNoteOnVelocity = velocity;
        params.pendingPreviewNoteOn = true;
    }
    if (hitNote >= 0 && ImGui::IsMouseClicked(1) && onSetRootNote) onSetRootNote(hitNote);

    if (ui.heldPianoKey >= 0 && !ImGui::IsMouseDown(0)) {
        params.previewNoteOffNumber = ui.heldPianoKey;
        params.pendingPreviewNoteOff = true;
        ui.heldPianoKey = -1;
    }

    for (int pass = 0; pass < 2; ++pass) {
        for (int n = 0; n < 128; ++n) {
            const Key& k = keys[n];
            if (k.black != (pass == 1)) continue;
            float x0 = p0.x + k.x0, x1 = p0.x + k.x1;
            float y0 = p0.y, y1 = p0.y + (k.black ? pianoH * 0.62f : pianoH);
            bool active = params.noteActive[static_cast<size_t>(n)].load();
            bool defined = hasDrum[static_cast<size_t>(n)];
            ImU32 col;
            if (n == rootMarker)
                col = IM_COL32(240, 150, 40, 255); // selected drum's root note
            else if (active)
                col = IM_COL32(120, 190, 255, 255);
            else if (n == hitNote)
                col = k.black ? IM_COL32(90, 90, 100, 255)
                              : IM_COL32(230, 230, 235, 255);
            else if (defined)
                col = k.black ? IM_COL32(25, 25, 30, 255)
                              : IM_COL32(245, 245, 248, 255);
            else // no percussion loaded on this key - grayed out
                col = k.black ? IM_COL32(60, 60, 63, 255)
                              : IM_COL32(130, 130, 133, 255);
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
            if (!k.black)
                dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(90, 90, 95, 255));
            if (active) {
                float v = params.noteVelocity01[static_cast<size_t>(n)].load();
                float barTop = y0 + (y1 - y0) * (1.0f - v);
                dl->AddRectFilled(ImVec2(x0, barTop), ImVec2(x1, y1),
                                  IM_COL32(220, 60, 60, 140));
            }
            if (n % 12 == 0 && !k.black)
                dl->AddText(ImVec2(x0 + 1, p0.y + pianoH - 16),
                            IM_COL32(90, 90, 95, 255), noteName(n).c_str());
        }
    }

    if (hitNote >= 0) {
        ImGui::SetTooltip("%s (%d)%s", noteName(hitNote).c_str(), hitNote,
                          hitNote == rootMarker ? " - root note" : "");
    }

    if (!ui.pianoScrolledOnce) {
        ui.pianoScrolledOnce = true;
        int scrollTarget = rootMarker >= 0 ? rootMarker : 60;
        ImGui::SetScrollX(std::max(0.f, keys[scrollTarget].x0 - 300.f));
    }

    ImGui::EndChild();
}

// ------------------------------------------------------------- drum list

void drawDrumList(SharedParams& params, const std::function<void()>& onParamChanged) {
    if (ImGui::Button("+")) {
        {
            std::lock_guard<std::mutex> lock(params.guiState.mutex);
            SharedParams::DrumItem d;
            d.id = params.guiState.nextId++;
            d.label = "Drum " + std::to_string(d.id);
            params.guiState.drums.push_back(d);
            params.guiState.selectedId = d.id;
        }
        if (onParamChanged) onParamChanged();
    }
    ImGui::SameLine();
    bool hasSelection;
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        hasSelection = params.guiState.selectedId >= 0;
    }
    ImGui::BeginDisabled(!hasSelection);
    if (ImGui::Button("-")) {
        {
            std::lock_guard<std::mutex> lock(params.guiState.mutex);
            int sel = params.guiState.selectedId;
            auto& drums = params.guiState.drums;
            drums.erase(std::remove_if(drums.begin(), drums.end(),
                                       [&](const SharedParams::DrumItem& d) {
                                           return d.id == sel;
                                       }),
                       drums.end());
            params.guiState.selectedId = -1;
        }
        if (onParamChanged) onParamChanged();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Note  Label                Out");

    bool sfzChanged = false;
    if (ImGui::BeginChild("##listrows", ImVec2(0, 0), false)) {
        // Display order only - storage stays insertion order (see
        // shared.hpp's DrumItem::id comment); re-derived every frame so a
        // root-note edit re-sorts the list immediately.
        std::vector<std::pair<int, size_t>> order;
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        auto& drums = params.guiState.drums;
        order.reserve(drums.size());
        for (size_t i = 0; i < drums.size(); ++i) order.emplace_back(drums[i].rootNote, i);
        std::stable_sort(order.begin(), order.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });

        for (auto& [rootNote, idx] : order) {
            SharedParams::DrumItem& d = drums[idx];
            ImGui::PushID(d.id);
            bool selected = d.id == params.guiState.selectedId;
            float rowH = ImGui::GetFrameHeight();
            if (ImGui::Selectable("##sel", selected, ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0, rowH)))
                params.guiState.selectedId = d.id;
            ImGui::SameLine();
            ImGui::TextUnformatted(std::to_string(d.rootNote).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", noteName(d.rootNote).c_str());
            ImGui::SameLine(50);
            char labelBuf[64];
            snprintf(labelBuf, sizeof(labelBuf), "%s", d.label.c_str());
            ImGui::SetNextItemWidth(140);
            if (ImGui::InputText("##label", labelBuf, sizeof(labelBuf))) {
                // The label also feeds the generated SFZ's label_key= opcode
                // and the CLAP note-name extension (see plugin.cpp), so an
                // edit here needs the same regenerate as a rootNote/output
                // change, not just a local field update.
                d.label = labelBuf;
                sfzChanged = true;
            }
            ImGui::SameLine();
            int outDisp = d.outputIndex + 1;
            ImGui::SetNextItemWidth(40);
            if (ImGui::InputInt("##out", &outDisp, 0, 0)) {
                outDisp = std::clamp(outDisp, 1, kNumOutputs);
                d.outputIndex = outDisp - 1;
                sfzChanged = true;
            }
            if (!d.hasSource) {
                ImGui::SameLine();
                ImGui::TextDisabled("(empty)");
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (sfzChanged && onParamChanged) onParamChanged();
}

// ------------------------------------------------------------- edit zone

// Extra interactions layered on every slider in this tab, ported from
// SoloSampler's own sliderExtrasInt: middle-click opens a popup to type an
// exact value, right-click resets to defaultValue. Must be called
// immediately after the Slider* widget so IsItemClicked/IsItemHovered still
// refer to it. popupId must be unique per slider.
bool sliderExtrasInt(const char* popupId, int* value, int defaultValue, int lo, int hi) {
    bool changed = false;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Middle)) ImGui::OpenPopup(popupId);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        *value = defaultValue;
        changed = true;
    }
    if (ImGui::BeginPopup(popupId)) {
        ImGui::SetNextItemWidth(120.f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::InputInt("##edit", value, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
            *value = std::clamp(*value, lo, hi);
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return changed;
}

// Label above a full-width slider (this tab's column is narrow, so an
// inline label would crowd the drag area) - see sliderExtrasInt for the
// middle/right-click extras every one of these gets for free.
bool drawIntSlider(const char* label, int* value, int lo, int hi, int defaultValue,
                   const char* fmt = "%d") {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    std::string id = std::string("##") + label;
    bool changed = ImGui::SliderInt(id.c_str(), value, lo, hi, fmt);
    std::string popupId = id + "popup";
    changed |= sliderExtrasInt(popupId.c_str(), value, defaultValue, lo, hi);
    return changed;
}

// Float counterpart of sliderExtrasInt/drawIntSlider - same middle-click-to-
// type/right-click-to-reset extras, used by the Amp tab's envelope controls
// (times/levels/shapes need sub-integer precision, unlike every Sample tab
// slider so far).
bool sliderExtrasFloat(const char* popupId, float* value, float defaultValue, float lo, float hi) {
    bool changed = false;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Middle)) ImGui::OpenPopup(popupId);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        *value = defaultValue;
        changed = true;
    }
    if (ImGui::BeginPopup(popupId)) {
        ImGui::SetNextItemWidth(120.f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::InputFloat("##edit", value, 0.f, 0.f, "%.5f",
                              ImGuiInputTextFlags_EnterReturnsTrue)) {
            *value = std::clamp(*value, lo, hi);
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return changed;
}

bool drawFloatSlider(const char* label, float* value, float lo, float hi, float defaultValue,
                     const char* fmt = "%.5f") {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    std::string id = std::string("##") + label;
    bool changed = ImGui::SliderFloat(id.c_str(), value, lo, hi, fmt);
    std::string popupId = id + "popup";
    changed |= sliderExtrasFloat(popupId.c_str(), value, defaultValue, lo, hi);
    return changed;
}

bool drawCombo(const char* label, int* index, const char* const* items, int count) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    bool changed = false;
    std::string id = std::string("##") + label;
    if (ImGui::BeginCombo(id.c_str(), items[std::clamp(*index, 0, count - 1)])) {
        for (int i = 0; i < count; ++i) {
            bool selected = i == *index;
            if (ImGui::Selectable(items[i], selected)) {
                *index = i;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Sample tab's per-drum "instrument design" controls: volume/pan/width/
// quality/polyphony through tune=, per spec - everything here feeds
// DrumSfzBuilder.cpp's writeMasterOpcodes, applying to the whole drum
// (round robins/velocity layers included) since it's all written at the
// <master> level. Mutates `d` directly (GUI thread) and reports back via
// `changed`, same convention as drawDrumKitModeControls.
void drawInstrumentDesignControls(SharedParams::DrumItem& d, bool& changed) {
    changed |= drawIntSlider("Volume (dB)", &d.volume, -48, 48, 6);
    changed |= drawIntSlider("Pan", &d.pan, -100, 100, 0);

    // Pan Random/Alternate, ported verbatim from SoloSampler's Pan tab
    // (minus its LFO section and its "x2" checkbox) - see shared.hpp's
    // DrumItem fields for the opcode rationale.
    changed |= drawIntSlider("Pan Random", &d.panRandom, -200, 200, 0);
    if (ImGui::Checkbox("Alternate", &d.panAlternate)) changed = true;

    changed |= drawIntSlider("Width", &d.width, 0, 100, 100);

    static const char* kQualityItems[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    changed |= drawCombo("Quality", &d.quality, kQualityItems, 10);

    changed |= drawIntSlider("Polyphony", &d.polyphony, 1, 256, 32);
    changed |= drawIntSlider("Note Polyphony", &d.notePolyphony, 1, 256, 32);

    if (ImGui::Checkbox("Disable note selfmask", &d.disableNoteSelfmask)) changed = true;

    static const char* kLoopModeItems[] = {"No Loop", "One Shot", "Loop Continuous",
                                           "Loop Sustain", "Auto (from sample)"};
    changed |= drawCombo("Loop Mode", &d.loopModeIndex, kLoopModeItems, 5);

    if (ImGui::Checkbox("Reverse", &d.reverse)) changed = true;

    changed |= drawIntSlider("Offset", &d.offset, 0, 4096, 0);
    changed |= drawIntSlider("vel2offset", &d.vel2offset, -4096, 0, 0);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Subtracts up to this many sample frames from the Offset above, "
            "scaled by note velocity (harder hits start closer to the "
            "sample's true beginning; velocity 0 leaves Offset unchanged).");

    ImGui::Separator();
    if (ImGui::Checkbox("Exclusive Class", &d.exclusiveClass)) changed = true;
    if (!d.exclusiveClass) ImGui::BeginDisabled();
    {
        ImGui::TextUnformatted("Group");
        ImGui::SetNextItemWidth(-1);
        int group = d.group;
        if (ImGui::InputInt("##group", &group, 0, 0)) {
            d.group = std::clamp(group, 0, 99);
            changed = true;
        }
        if (ImGui::Checkbox("offby", &d.offbyEnabled)) changed = true;
        if (!d.offbyEnabled) ImGui::BeginDisabled();
        {
            ImGui::TextUnformatted("Off By");
            ImGui::SetNextItemWidth(-1);
            int offby = d.offby;
            if (ImGui::InputInt("##offby", &offby, 0, 0)) {
                d.offby = std::clamp(offby, 0, 99);
                changed = true;
            }
        }
        if (!d.offbyEnabled) ImGui::EndDisabled();
    }
    if (!d.exclusiveClass) ImGui::EndDisabled();
    ImGui::Separator();

    changed |= drawIntSlider("Transpose (semitones)", &d.transpose, -24, 24, 0);
    changed |= drawIntSlider("Tune (cents)", &d.tune, -99, 99, 0);
}

// Sample tab's "Drum Kit Mode": lets the user pick ONE key/drum out of a
// whole pre-mapped kit .sfz (see DrumKitFlatten.h/shared.hpp's
// DrumItem::drumKitGroups) instead of treating the whole file as a single
// drum. Only usable once a .sfz (not a plain sample) with at least one
// resolvable key is loaded on the selected drum - the checkbox/slider stay
// disabled otherwise. Mutates the drum directly (GUI thread, same
// convention as drawDrumList) and reports back via `changed` whether
// onParamChanged needs to run once the caller's lock is released.
void drawDrumKitModeControls(SharedParams::DrumItem& d, bool& changed) {
    const bool canUse = d.isSfz && !d.drumKitGroups.empty();
    if (!canUse) ImGui::BeginDisabled();

    bool enabled = d.drumKitModeEnabled;
    if (ImGui::Checkbox("Drum Kit Mode", &enabled)) {
        d.drumKitModeEnabled = enabled;
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Pick one key out of a full kit .sfz's mapping instead of "
            "playing the whole file - round robins, velocity layers and "
            "crossfades within that key all come along.");

    if (canUse && enabled) {
        int idx = std::clamp(d.drumKitGroupIndex, 0, static_cast<int>(d.drumKitGroups.size()) - 1);
        ImGui::SetNextItemWidth(220);
        if (ImGui::SliderInt("##dkindex", &idx, 0, static_cast<int>(d.drumKitGroups.size()) - 1,
                             "")) {
            d.drumKitGroupIndex = idx;
            changed = true;
        }
        const auto& group = d.drumKitGroups[static_cast<size_t>(idx)];
        ImGui::SameLine();
        ImGui::Text("Key %d (%s), %d region(s)", group.key, noteName(group.key).c_str(),
                   group.regionCount);
    } else if (!canUse) {
        ImGui::TextDisabled(d.isSfz ? "No independent keys found in this .sfz."
                                    : "Load a .sfz (not a plain sample) to use this.");
    }

    if (!canUse) ImGui::EndDisabled();
}

void drawSampleTab(SharedParams& params, EditorUIState& ui,
                   const std::function<void()>& onParamChanged,
                   const std::function<void()>& onExplodeDrumKit) {
    bool changed = false;
    bool hasDrum = false;
    bool canExplode = false;
    size_t explodeGroupCount = 0;
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        SharedParams::DrumItem* d = nullptr;
        if (params.guiState.selectedId >= 0)
            for (auto& item : params.guiState.drums)
                if (item.id == params.guiState.selectedId) {
                    d = &item;
                    break;
                }

        if (!d) {
            ImGui::TextDisabled("No percussion selected");
        } else {
            hasDrum = true;
            drawDrumKitModeControls(*d, changed);
            canExplode = d->isSfz && !d->drumKitGroups.empty();
            explodeGroupCount = d->drumKitGroups.size();
        }
    }

    // "Load regions as individual percussion": bulk alternative to picking
    // one Drum Kit Mode key at a time above - explodes EVERY key group
    // already discovered on this drum (same d.drumKitGroups DrumKitFlatten.h
    // populated at import time) into its own separate drum in the list, one
    // <master> per key, root note taken from that key (see plugin.cpp's
    // explodeSelectedDrumKit). Repurposes THIS drum's own content rather
    // than just adding alongside it, so it's gated behind a confirmation
    // modal - same deferred-open trigger pattern as SoloSampler's "Reset to
    // Default" (sibling project). Drawn OUTSIDE the lock above (and the one
    // below): the modal's Confirm button calls back into plugin.cpp, which
    // re-locks the same (non-recursive) guiState mutex. Placed right after
    // Drum Kit Mode, before the rest of the Sample tab's controls, since
    // it's really just a bulk version of that same feature.
    if (hasDrum) {
        if (!canExplode) ImGui::BeginDisabled();
        if (ImGui::Button("Load regions as individual percussion"))
            ui.explodeKitConfirmTrigger = true;
        if (!canExplode) ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && !canExplode)
            ImGui::SetTooltip(
                "Load a .sfz drum kit (not a plain sample) with at least one "
                "resolvable key first.");

        if (ui.explodeKitConfirmTrigger) {
            ImGui::OpenPopup("Load regions as individual percussion?");
            ui.explodeKitConfirmTrigger = false;
        }
        if (ImGui::BeginPopupModal("Load regions as individual percussion?", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Create %d individual percussion, one per key found in this kit?",
                       static_cast<int>(explodeGroupCount));
            ImGui::TextUnformatted("Each new percussion's root note is taken from its key.");
            ImGui::TextUnformatted(
                "This replaces this drum's own kit content with key group 1.");
            if (ImGui::Button("Create", ImVec2(140, 0))) {
                if (onExplodeDrumKit) onExplodeDrumKit();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::Separator();
    }

    if (hasDrum) {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        SharedParams::DrumItem* d = nullptr;
        for (auto& item : params.guiState.drums)
            if (item.id == params.guiState.selectedId) {
                d = &item;
                break;
            }
        if (d) drawInstrumentDesignControls(*d, changed);
    }

    if (changed && onParamChanged) onParamChanged();
}

// ------------------------------------------------------------- amp tab

// amp_velcurve envelope editor (Amp tab). X = MIDI velocity 1..127
// (integer), Y = gain 0.0..1.0 (float) - see shared.hpp's AmpVelCurvePoint.
// Interaction ported from NeoLooper's waveform envelope overlay (sibling
// project, src/gui/waveform_view.cpp): a single InvisibleButton spanning the
// plot rect, manual hit-testing instead of per-point widgets, right-click
// empty space to add (and immediately start dragging) a point, right-click
// an existing point to delete it, left-click-drag to move one. NeoLooper's
// points live on a continuous double (sample frame) axis where the
// neighbor-clamp in its movePoint alone is enough to keep them from ever
// landing on the same X; here the axis is only 127 discrete slots, so the
// same clamp additionally reserves one integer of headroom on each side
// (prevVelocity+1 .. nextVelocity-1) to keep two points from colliding on
// the same velocity after rounding.
void drawAmpEnvelope(SharedParams::DrumItem& d, EditorUIState& ui, bool& changed) {
    constexpr float kHeight = 220.f;
    constexpr float kHitRadiusSq = 64.f; // 8px
    constexpr float kPointRadius = 4.f;
    const ImU32 kColGrid = IM_COL32(90, 90, 90, 120);
    const ImU32 kColCurve = IM_COL32(240, 150, 80, 255);
    const ImU32 kColPoint = IM_COL32(255, 255, 255, 255);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 rectMin = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    ImVec2 rectMax(rectMin.x + width, rectMin.y + kHeight);

    dl->AddRectFilled(rectMin, rectMax, IM_COL32(30, 30, 30, 255));
    dl->AddRect(rectMin, rectMax, kColGrid);
    for (int i = 1; i < 4; ++i) {
        float y = rectMin.y + kHeight * static_cast<float>(i) / 4.f;
        dl->AddLine(ImVec2(rectMin.x, y), ImVec2(rectMax.x, y), kColGrid);
    }

    auto veloToX = [&](int v) {
        return rectMin.x + (static_cast<float>(v) - 1.f) / 126.f * width;
    };
    auto xToVelo = [&](float x) {
        float t = (x - rectMin.x) / width;
        return std::clamp(static_cast<int>(std::lround(1.f + t * 126.f)), 1, 127);
    };
    auto gainToY = [&](float g) { return rectMax.y - std::clamp(g, 0.f, 1.f) * kHeight; };
    auto yToGain = [&](float y) {
        return std::clamp((rectMax.y - y) / kHeight, 0.f, 1.f);
    };

    ImGui::InvisibleButton("##ampenv", ImVec2(width, kHeight),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    ImVec2 mouse = ImGui::GetIO().MousePos;

    auto& pts = d.ampVelCurve;

    int hit = -1;
    if (hovered) {
        for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
            float dx = mouse.x - veloToX(pts[static_cast<size_t>(i)].velocity);
            float dy = mouse.y - gainToY(pts[static_cast<size_t>(i)].gain);
            if (dx * dx + dy * dy < kHitRadiusSq) {
                hit = i;
                break;
            }
        }
    }

    // Start (or finish, via delete) an interaction. Left-click on a point
    // begins a drag; right-click on a point deletes it; right-click on empty
    // space inserts a new point (clamped between its future neighbors) and
    // immediately begins dragging it, so place-then-position is one motion.
    if (hovered && ImGui::IsMouseClicked(0) && hit >= 0) {
        ui.ampEnvDragIdx = hit;
        ui.ampEnvDragging = true;
    } else if (hovered && ImGui::IsMouseClicked(1)) {
        if (hit >= 0) {
            pts.erase(pts.begin() + hit);
            changed = true;
        } else {
            int v = xToVelo(mouse.x);
            float g = yToGain(mouse.y);
            size_t insertAt = 0;
            while (insertAt < pts.size() && pts[insertAt].velocity < v) ++insertAt;
            int lo = insertAt == 0 ? 1 : pts[insertAt - 1].velocity + 1;
            int hi = insertAt == pts.size() ? 127 : pts[insertAt].velocity - 1;
            if (lo <= hi) {
                v = std::clamp(v, lo, hi);
                pts.insert(pts.begin() + static_cast<long>(insertAt), {v, g});
                ui.ampEnvDragIdx = static_cast<int>(insertAt);
                ui.ampEnvDragging = true;
                changed = true;
            }
        }
    }

    if (ui.ampEnvDragging && ui.ampEnvDragIdx >= 0 &&
        ui.ampEnvDragIdx < static_cast<int>(pts.size())) {
        if (ImGui::IsMouseDown(0) || ImGui::IsMouseDown(1)) {
            size_t idx = static_cast<size_t>(ui.ampEnvDragIdx);
            int lo = idx == 0 ? 1 : pts[idx - 1].velocity + 1;
            int hi = idx + 1 == pts.size() ? 127 : pts[idx + 1].velocity - 1;
            int v = std::clamp(xToVelo(mouse.x), lo, hi);
            float g = yToGain(mouse.y);
            if (pts[idx].velocity != v || pts[idx].gain != g) {
                pts[idx].velocity = v;
                pts[idx].gain = g;
                changed = true;
            }
            ImGui::SetTooltip("vel %d, gain %.3f", v, g);
        } else {
            ui.ampEnvDragging = false;
            ui.ampEnvDragIdx = -1;
        }
    }

    // Curve preview: sfizz anchors velocity 0 at gain 0.0 and velocity 127
    // at gain 1.0 whenever the user hasn't placed an explicit point there
    // (Curve::buildFromVelcurvePoints), interpolating linearly between
    // whatever points exist - so the drawn line includes those two implicit
    // endpoints (clipped to the plot rect) for an accurate preview, not just
    // a "connect the dots" of the user's own points.
    if (!pts.empty()) {
        dl->PushClipRect(rectMin, rectMax, true);
        ImVec2 prev(veloToX(0), gainToY(0.f));
        for (const auto& pt : pts) {
            ImVec2 cur(veloToX(pt.velocity), gainToY(pt.gain));
            dl->AddLine(prev, cur, kColCurve, 2.f);
            prev = cur;
        }
        dl->AddLine(prev, ImVec2(veloToX(127), gainToY(1.f)), kColCurve, 2.f);
        dl->PopClipRect();
    }
    for (const auto& pt : pts) {
        ImVec2 c(veloToX(pt.velocity), gainToY(pt.gain));
        dl->AddCircleFilled(c, kPointRadius, kColPoint);
        dl->AddCircle(c, kPointRadius, kColCurve);
    }
}

// amp_veltrack=/amp_random=/vel2volume, then the eg01_* amp envelope (fixed
// 6-breakpoint flexEG template - see shared.hpp's DrumItem fields for the
// breakpoint rationale, including why there's no Delay Time control here
// unlike SoloSampler's own Amp tab). vel2attack/vel2sustain sit right next
// to the time/level slider they modulate, matching the Sample tab's
// Offset/vel2offset layout.
void drawAmpEnvelopeControls(SharedParams::DrumItem& d, bool& changed) {
    changed |= drawIntSlider("Amp Veltrack", &d.ampVeltrack, -100, 100, 100);
    changed |= drawIntSlider("Amp Random", &d.ampRandom, -24, 24, 0);
    changed |= drawIntSlider("vel2volume", &d.ampVel2Volume, -24, 24, 0);

    ImGui::Spacing();
    ImGui::TextDisabled("Envelope (eg01_*)");
    changed |= drawFloatSlider("Attack Time", &d.ampAttackTime, 0.00001f, 0.1f, 0.00001f);
    changed |= drawFloatSlider("Attack Shape", &d.ampAttackShape, -11.f, 11.f, 0.00001f);
    changed |= drawFloatSlider("vel2attack", &d.ampVel2Attack, -0.1f, 0.f, 0.f);
    ImGui::Separator();
    changed |= drawFloatSlider("Hold Time", &d.ampHoldTime, 0.00001f, 0.1f, 0.00001f);
    ImGui::Separator();
    changed |= drawFloatSlider("Decay Time", &d.ampDecayTime, 0.00001f, 0.1f, 0.00001f);
    changed |= drawFloatSlider("Decay Time (Extra)", &d.ampDecayTimeExtra, 0.f, 12.f, 0.f);
    changed |= drawFloatSlider("Decay Shape", &d.ampDecayShape, -11.f, 11.f, -0.3616f);
    ImGui::Separator();
    changed |= drawFloatSlider("Sustain Level", &d.ampSustainLevel, 0.f, 1.f, 1.f);
    changed |= drawFloatSlider("vel2sustain", &d.ampVel2Sustain, -1.f, 1.f, 0.f);
    ImGui::Separator();
    changed |= drawFloatSlider("Release Time", &d.ampReleaseTime, 0.00001f, 12.f, 0.00001f);
    changed |= drawFloatSlider("Release Shape", &d.ampReleaseShape, -11.f, 11.f, -6.3616f);
}

void drawAmpTab(SharedParams& params, EditorUIState& ui, const std::function<void()>& onParamChanged) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        SharedParams::DrumItem* d = nullptr;
        if (params.guiState.selectedId >= 0)
            for (auto& item : params.guiState.drums)
                if (item.id == params.guiState.selectedId) {
                    d = &item;
                    break;
                }

        if (!d) {
            ImGui::TextDisabled("No percussion selected");
        } else {
            ImGui::TextDisabled("Velocity Curve (amp_velcurve_*)");
            ImGui::TextDisabled(
                "Right-click empty space to add a point, right-click a point to "
                "remove it, drag a point to move it.");
            drawAmpEnvelope(*d, ui, changed);
            if (ImGui::Button("Clear all points") && !d->ampVelCurve.empty()) {
                d->ampVelCurve.clear();
                changed = true;
            }
            ImGui::Separator();

            drawAmpEnvelopeControls(*d, changed);
        }
    }
    if (changed && onParamChanged) onParamChanged();
}

// ----------------------------------------------------------------- fil tab

// Fil tab: verbatim port of SoloSampler's Filter tab, minus fil_keycenter=/
// fil_keytrack=/LFO per spec - see shared.hpp's DrumItem fields for the
// opcode/caveat rationale.
void drawFilterControls(SharedParams::DrumItem& d, bool& changed) {
    changed |= drawCombo("Filter Type", &d.filterTypeIndex, kFilterTypes, 23);
    changed |= drawIntSlider("Cutoff", &d.filterCutoff, 1, 20000, 20000);
    changed |= drawFloatSlider("Resonance (dB)", &d.filterResonance, -40.f, 40.f, 0.f, "%.2f");
    changed |= drawIntSlider("Random Cutoff", &d.filterRandomCutoff, 1, 20000, 0);

    changed |= drawIntSlider("Fil Veltrack", &d.filVeltrack, 0, 20000, 0);
    ImGui::TextDisabled(d.filterEgEnabled ? "(eg02_cutoff_oncc131)" : "(cutoff_oncc131)");
    changed |= drawIntSlider("Reso Veltrack", &d.resoVeltrack, -40, 40, 0);

    ImGui::Spacing();
    if (ImGui::Checkbox("Filter EG", &d.filterEgEnabled)) changed = true;
    // Not gated on filterEgEnabled - same "don't gate speculatively"
    // convention used throughout this UI. Whatever's here gets picked up
    // when the checkbox is on.
    ImGui::TextDisabled("Envelope (eg02_*)");
    changed |= drawIntSlider("Fil Depth", &d.filDepth, 0, 20000, 0);
    changed |= drawFloatSlider("Start Level", &d.filEgStartLevel, 0.f, 1.f, 0.f);
    changed |= drawFloatSlider("Delay Time", &d.filEgDelayTime, 0.00001f, 0.1f, 0.00001f);
    changed |= drawFloatSlider("Attack Time", &d.filEgAttackTime, 0.00001f, 0.1f, 0.00001f);
    changed |= drawFloatSlider("Hold Time", &d.filEgHoldTime, 0.00001f, 0.1f, 0.00001f);
    changed |= drawFloatSlider("Decay Time", &d.filEgDecayTime, 0.00001f, 0.1f, 0.00001f);
    changed |= drawFloatSlider("Decay Time (Extra)", &d.filEgDecayTimeExtra, 0.f, 12.f, 0.f);
    changed |= drawFloatSlider("Sustain Level", &d.filEgSustainLevel, 0.f, 1.f, 1.f);
    changed |= drawFloatSlider("Release Time", &d.filEgReleaseTime, 0.00001f, 0.1f, 0.00001f);
    ImGui::Separator();

    changed |= drawFloatSlider("Attack Shape", &d.filEgAttackShape, -11.f, 11.f, 0.00001f);
    changed |= drawFloatSlider("Decay Shape", &d.filEgDecayShape, -11.f, 11.f, 0.00001f);
    changed |= drawFloatSlider("Release Shape", &d.filEgReleaseShape, -11.f, 11.f, 0.00001f);
}

void drawFilTab(SharedParams& params, const std::function<void()>& onParamChanged) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        SharedParams::DrumItem* d = nullptr;
        if (params.guiState.selectedId >= 0)
            for (auto& item : params.guiState.drums)
                if (item.id == params.guiState.selectedId) {
                    d = &item;
                    break;
                }

        if (!d) ImGui::TextDisabled("No percussion selected");
        else drawFilterControls(*d, changed);
    }
    if (changed && onParamChanged) onParamChanged();
}

// --------------------------------------------------------------- pitch tab

// Pitch tab: verbatim port of SoloSampler's Pitch tab, minus
// pitch_keytrack=/portamento/LFO per spec - see shared.hpp's DrumItem
// fields for the opcode/caveat rationale. vel2pitch is new (not in
// SoloSampler), placed right after Pitch Depth per the established
// "vel2X right after X" convention (Sample tab's Offset/vel2offset, Amp
// tab's Attack Time/vel2attack, etc).
void drawPitchControls(SharedParams::DrumItem& d, bool& changed) {
    changed |= drawIntSlider("Pitch Veltrack", &d.pitchVeltrack, -9600, 9600, 0);
    changed |= drawIntSlider("Pitch Random", &d.pitchRandom, 0, 9600, 0);

    ImGui::Spacing();
    if (ImGui::Checkbox("Pitch EG", &d.pitchEgEnabled)) changed = true;
    // Not gated on pitchEgEnabled - same "don't gate speculatively"
    // convention used throughout this UI. Whatever's here gets picked up
    // when the checkbox is on.
    ImGui::TextDisabled("Envelope (eg03_*)");
    changed |= drawIntSlider("Pitch Depth", &d.pitchDepth, -9600, 9600, 0);
    changed |= drawIntSlider("vel2pitch", &d.pitchVel2Depth, -9600, 9600, 0);
    if (ImGui::Checkbox("Invert vel2pitch", &d.pitchVel2Invert)) changed = true;
    changed |= drawFloatSlider("Start Level", &d.pitchEgStartLevel, 0.f, 1.f, 0.f);
    changed |= drawFloatSlider("Delay Time", &d.pitchEgDelayTime, 0.00001f, 0.1f, 0.00001f);
    changed |= drawFloatSlider("Attack Time", &d.pitchEgAttackTime, 0.00001f, 0.1f, 0.00001f);
    changed |= drawFloatSlider("Hold Time", &d.pitchEgHoldTime, 0.00001f, 0.1f, 0.00001f);
    changed |= drawFloatSlider("Decay Time", &d.pitchEgDecayTime, 0.00001f, 0.1f, 0.00001f);
    changed |= drawFloatSlider("Decay Time (Extra)", &d.pitchEgDecayTimeExtra, 0.f, 12.f, 0.f);
    changed |= drawFloatSlider("Sustain Level", &d.pitchEgSustainLevel, 0.f, 1.f, 1.f);
    changed |= drawFloatSlider("Release Time", &d.pitchEgReleaseTime, 0.00001f, 0.1f, 0.00001f);
    ImGui::Separator();

    changed |= drawFloatSlider("Attack Shape", &d.pitchEgAttackShape, -11.f, 11.f, 0.00001f);
    changed |= drawFloatSlider("Decay Shape", &d.pitchEgDecayShape, -11.f, 11.f, 0.00001f);
    changed |= drawFloatSlider("Release Shape", &d.pitchEgReleaseShape, -11.f, 11.f, 0.00001f);
}

void drawPitchTab(SharedParams& params, const std::function<void()>& onParamChanged) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        SharedParams::DrumItem* d = nullptr;
        if (params.guiState.selectedId >= 0)
            for (auto& item : params.guiState.drums)
                if (item.id == params.guiState.selectedId) {
                    d = &item;
                    break;
                }

        if (!d) ImGui::TextDisabled("No percussion selected");
        else drawPitchControls(*d, changed);
    }
    if (changed && onParamChanged) onParamChanged();
}

// ------------------------------------------------------------ opcodes tab

// Splices out [b,e) (any existing selection, order-independent) and inserts
// insertText at that point, then flags the widget to resync from the now-
// externally-edited buffer with the cursor collapsed right after the
// insertion. Ported verbatim from SoloSampler's own helper of the same
// name (sibling project) - see its comment for why the Reload* fields are
// set directly instead of going through ImGui's own
// ReloadUserBufAndKeepSelection (it derives the target selection from
// state *before* our edit, which would restore the now-stale pre-edit
// range).
void spliceAndReloadCursor(std::string* text, ImGuiInputTextState* state, int b, int e,
                           const char* insertText = "") {
    if (b > e) std::swap(b, e);
    text->erase(static_cast<size_t>(b), static_cast<size_t>(e - b));
    text->insert(static_cast<size_t>(b), insertText);
    state->WantReloadUserBuf = true;
    state->ReloadSelectionStart = state->ReloadSelectionEnd = b + static_cast<int>(strlen(insertText));
}

// ImGui::InputTextMultiline only takes a fixed char buffer - imgui_stdlib.h
// (the official std::string adapter) isn't vendored in this project, so
// this reimplements its resize-callback trick directly, same as
// SoloSampler: ImGui is given str's own buffer (capacity()+1 bytes) and,
// whenever the user types past that capacity,
// ImGuiInputTextFlags_CallbackResize fires and the callback grows str
// before ImGui writes further.
bool inputTextMultilineStdString(const char* label, std::string* str, const ImVec2& size) {
    auto callback = [](ImGuiInputTextCallbackData* data) -> int {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* s = static_cast<std::string*>(data->UserData);
            s->resize(static_cast<size_t>(data->BufTextLen));
            data->Buf = s->data();
        }
        return 0;
    };
    return ImGui::InputTextMultiline(label, str->data(), str->capacity() + 1, size,
                                     ImGuiInputTextFlags_CallbackResize, callback, str);
}

// Free-form text editor: whatever the user types here is written verbatim
// into THIS drum's own <master> block, after every other opcode (see
// DrumSfzBuilder.cpp's writeMasterOpcodes) - per spec, scoped to the
// selected percussion alone, unlike SoloSampler's single-instrument
// <global> equivalent. Mechanism otherwise ported verbatim: Ctrl+X/C/V/A
// and Delete already work via ImGui's own InputText handling (round-trips
// with the real X11 clipboard via gui_window.cpp); this adds the mouse-
// driven equivalent via a right-click context menu, reaching into ImGui's
// internal input-text state (GetInputTextState) so it operates on the live
// selection/cursor exactly like the keyboard shortcuts do.
void drawOpcodesControls(std::string& text, bool& changed) {
    changed |= inputTextMultilineStdString("##customopcodes", &text, ImVec2(-1, -1));

    // Only non-null once the field has actually been focused at least once
    // (ImGui tracks a single active InputText at a time). Cut/Copy/Delete/
    // Select All need it; Paste falls back to appending when the field has
    // never been focused, since there's no cursor to speak of.
    ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetItemID());
    bool hasSelection = state && state->HasSelection();

    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, hasSelection)) {
            int b = state->GetSelectionStart(), e = state->GetSelectionEnd();
            ImGui::SetClipboardText(text.substr(static_cast<size_t>(std::min(b, e)),
                                                static_cast<size_t>(std::abs(e - b)))
                                        .c_str());
            spliceAndReloadCursor(&text, state, b, e);
            changed = true;
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, hasSelection)) {
            int b = state->GetSelectionStart(), e = state->GetSelectionEnd();
            ImGui::SetClipboardText(text.substr(static_cast<size_t>(std::min(b, e)),
                                                static_cast<size_t>(std::abs(e - b)))
                                        .c_str());
        }
        if (ImGui::MenuItem("Paste", "Ctrl+V")) {
            if (const char* clip = ImGui::GetClipboardText()) {
                if (state) {
                    int b = state->HasSelection() ? state->GetSelectionStart() : state->GetCursorPos();
                    int e = state->HasSelection() ? state->GetSelectionEnd() : state->GetCursorPos();
                    spliceAndReloadCursor(&text, state, b, e, clip);
                } else {
                    text.append(clip);
                }
                changed = true;
            }
        }
        if (ImGui::MenuItem("Delete", "Del", false, hasSelection)) {
            spliceAndReloadCursor(&text, state, state->GetSelectionStart(), state->GetSelectionEnd());
            changed = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", "Ctrl+A", false, state != nullptr)) state->SelectAll();
        ImGui::EndPopup();
    }
}

void drawOpcodesTab(SharedParams& params, const std::function<void()>& onParamChanged) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        SharedParams::DrumItem* d = nullptr;
        if (params.guiState.selectedId >= 0)
            for (auto& item : params.guiState.drums)
                if (item.id == params.guiState.selectedId) {
                    d = &item;
                    break;
                }

        if (!d) {
            ImGui::TextDisabled("No percussion selected");
        } else {
            ImGui::TextDisabled(
                "Custom opcodes (written into this drum's own <master>, after "
                "everything else):");
            drawOpcodesControls(d->customOpcodesText, changed);
        }
    }
    if (changed && onParamChanged) onParamChanged();
}

void drawEditZone(SharedParams& params, EditorUIState& ui,
                  const std::function<void()>& onParamChanged,
                  const std::function<void()>& onExplodeDrumKit) {
    std::string selLabel;
    bool hasSel;
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        hasSel = params.guiState.selectedId >= 0;
        if (hasSel)
            for (auto& d : params.guiState.drums)
                if (d.id == params.guiState.selectedId) {
                    selLabel = d.label;
                    break;
                }
    }
    if (hasSel) ImGui::Text("Editing: %s", selLabel.c_str());
    else ImGui::TextDisabled("No percussion selected");
    ImGui::Separator();

    // Debug-only, opt-in: SFZDRUMMER_DEBUG_TAB=<Name> forces that tab open on
    // the first frame, purely so test/mini_host's screenshot technique (no
    // synthetic mouse input in this sandbox) can verify tabs other than the
    // first one. Unset in normal use - no effect on real hosts. Same
    // convention as SoloSampler's SOLOSAMPLER_DEBUG_TAB (sibling project).
    static const char* forcedTab = getenv("SFZDRUMMER_DEBUG_TAB");
    auto tabFlags = [](const char* name) {
        return (forcedTab && !strcmp(forcedTab, name)) ? ImGuiTabItemFlags_SetSelected : 0;
    };

    if (ImGui::BeginTabBar("##edittabs")) {
        if (ImGui::BeginTabItem("Sample", nullptr, tabFlags("Sample"))) {
            drawSampleTab(params, ui, onParamChanged, onExplodeDrumKit);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Amp", nullptr, tabFlags("Amp"))) {
            drawAmpTab(params, ui, onParamChanged);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Fil", nullptr, tabFlags("Fil"))) {
            drawFilTab(params, onParamChanged);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Pitch", nullptr, tabFlags("Pitch"))) {
            drawPitchTab(params, onParamChanged);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Opcodes", nullptr, tabFlags("Opcodes"))) {
            drawOpcodesTab(params, onParamChanged);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ------------------------------------------------------------ error toast

// A load/preview failure (guiState.listError) shown as a floating, non-
// interactive notification in the window's top-right corner, fading out and
// clearing itself after a few seconds - per user spec, this reads far
// better than cramming it inline into the narrow file-explorer column, and
// it doesn't linger once it's been seen. Own top-level ImGui window (not
// nested inside "##root"), drawn last so it floats above everything else.
void drawErrorToast(SharedParams& params) {
    constexpr double kVisibleSeconds = 4.0;
    constexpr double kFadeSeconds = 0.6;

    std::string message;
    double elapsed = 0.0;
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        if (!params.guiState.listError.empty()) {
            elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                     params.guiState.listErrorSetAt)
                         .count();
            if (elapsed >= kVisibleSeconds)
                params.guiState.listError.clear(); // aged out - dismiss for good
            else
                message = params.guiState.listError;
        }
    }
    if (message.empty()) return;

    float alpha = 1.0f;
    if (elapsed > kVisibleSeconds - kFadeSeconds)
        alpha = static_cast<float>(
            std::clamp((kVisibleSeconds - elapsed) / kFadeSeconds, 0.0, 1.0));

    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kPad = 12.0f;
    constexpr float kWidth = 320.0f; // fixed - long messages wrap instead of
                                     // growing the window over the rest of the UI
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kPad, kPad), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(kWidth, 0), ImVec2(kWidth, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::Begin("##errortoast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.55f, 1.0f));
    ImGui::TextWrapped("%s", message.c_str());
    ImGui::PopStyleColor();
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace

void drawEditorUI(SharedParams& params, EditorUIState& ui,
                  const std::function<void()>& onParamChanged,
                  const std::function<void(const std::string&)>& onLoadRequested,
                  const std::function<void(const std::string&)>& onPreviewRequested,
                  const std::function<void()>& onSavePreset,
                  const std::function<void()>& onLoadPreset,
                  const std::function<void()>& onSaveProfile,
                  const std::function<void()>& onLoadProfile,
                  const std::function<void()>& onExplodeDrumKit,
                  const std::function<void()>& onInitKit) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);

    // Persistent Preset/Profile row - always visible above everything
    // else regardless of which tab is open, since these save/restore
    // either the WHOLE kit (.drmpreset) or one drum's design
    // (.drmprofile), not anything tab-specific. Same "pinned outside the
    // tab body" convention SoloSampler uses for its own equivalent row.
    // Each button just invokes a callback - plugin.cpp owns the actual
    // zenity dialog and file I/O; failures surface through the same
    // guiState.listError toast a bad sample/sfz load already uses (see
    // drawErrorToast below).
    if (ImGui::Button("Save Preset") && onSavePreset) onSavePreset();
    ImGui::SameLine();
    if (ImGui::Button("Load Preset") && onLoadPreset) onLoadPreset();
    ImGui::SameLine();
    if (ImGui::Button("Save Profile") && onSaveProfile) onSaveProfile();
    ImGui::SameLine();
    if (ImGui::Button("Load Profile") && onLoadProfile) onLoadProfile();
    ImGui::SameLine();

    // "Init Kit" - destructive (wipes every percussion plus mpeEnabled/Bend
    // Range back to a freshly-loaded plugin's own defaults, see plugin.cpp's
    // initKit), so gated behind a confirmation modal rather than firing
    // straight from the click - same deferred-open trigger pattern as
    // SoloSampler's own "Reset to Default" (sibling project).
    if (ImGui::Button("Init Kit")) ui.initKitConfirmTrigger = true;
    ImGui::SameLine();

    if (ui.initKitConfirmTrigger) {
        ImGui::OpenPopup("Init Kit?");
        ui.initKitConfirmTrigger = false;
    }
    if (ImGui::BeginPopupModal("Init Kit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Remove every percussion and reset MPE/Bend Range to default?");
        ImGui::TextUnformatted("This cannot be undone.");
        if (ImGui::Button("Init Kit", ImVec2(120, 0))) {
            if (onInitKit) onInitKit();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Bend Range combobox - same 4 choices/default (2400) as SoloSampler's
    // own always-present control (sibling project). The value it picks is
    // what DrumSfzBuilder.cpp's <global> bendup=/benddown= header writes
    // (now unconditional, see shared.hpp's bendUpCents/bendDownCents
    // comment), so every change here is an SFZ opcode change and needs a
    // regenerate.
    static constexpr int kBendChoices[4] = {200, 1200, 2400, 4800};
    int bendUp = params.bendUpCents.load();
    int bendIdx = 2; // default 2400
    for (int i = 0; i < 4; ++i)
        if (kBendChoices[i] == bendUp) { bendIdx = i; break; }
    char bendLabel[8];
    std::snprintf(bendLabel, sizeof(bendLabel), "%d", kBendChoices[bendIdx]);
    ImGui::SetNextItemWidth(90);
    if (ImGui::BeginCombo("Bend Range", bendLabel)) {
        for (int i = 0; i < 4; ++i) {
            bool sel = (i == bendIdx);
            char lbl[8];
            std::snprintf(lbl, sizeof(lbl), "%d", kBendChoices[i]);
            if (ImGui::Selectable(lbl, sel)) {
                params.bendUpCents = kBendChoices[i];
                params.bendDownCents = -kBendChoices[i];
                if (onParamChanged) onParamChanged();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();

    // Engine-level toggle (sfizz_set_mpe_enabled, applied on the audio
    // thread - see SfizzEngine::renderBlock), same method as SoloSampler's
    // own "Enable MPE" checkbox (sibling project). The flag itself isn't
    // an SFZ opcode, so flipping it alone needs no regenerate - it DOES
    // also force Bend Range above to 4800 cents (MPE 1.0's 48-semitone
    // per-note pitch bend convention), which IS an opcode change and does
    // need onParamChanged; unchecking leaves Bend Range as the user left
    // it, same as SoloSampler.
    bool mpeEnabled = params.mpeEnabled.load();
    if (ImGui::Checkbox("Enable MPE", &mpeEnabled)) {
        params.mpeEnabled = mpeEnabled;
        if (mpeEnabled) {
            params.bendUpCents = 4800;
            params.bendDownCents = -4800;
            if (onParamChanged) onParamChanged();
        }
    }
    ImGui::Separator();

    // Debug-only, opt-in: SFZDRUMMER_DEBUG_SELECT=<label> force-selects that
    // drum on the first frame it's found, purely so test/mini_host's
    // screenshot technique (no synthetic mouse input in this sandbox, see
    // project memory) can verify tab content that otherwise requires a real
    // click to reach. Same convention as SoloSampler's SOLOSAMPLER_DEBUG_TAB.
    // Unset in normal use - no effect on real hosts.
    {
        static const char* debugSelectLabel = getenv("SFZDRUMMER_DEBUG_SELECT");
        static bool debugSelectDone = false;
        if (debugSelectLabel && !debugSelectDone) {
            std::lock_guard<std::mutex> lock(params.guiState.mutex);
            if (params.guiState.selectedId < 0) {
                for (auto& d : params.guiState.drums) {
                    if (d.label == debugSelectLabel) {
                        params.guiState.selectedId = d.id;
                        debugSelectDone = true;
                        break;
                    }
                }
            }
        }
    }

    // Debug-only, opt-in: SFZDRUMMER_DEBUG_ERROR=<message> fires that
    // message through the same setListError path a real load failure uses,
    // once, on the first frame - lets the error toast (see drawErrorToast)
    // be screenshot-verified without a real bad file pick/drop. Unset in
    // normal use - no effect on real hosts.
    {
        static const char* debugErrorMsg = getenv("SFZDRUMMER_DEBUG_ERROR");
        static bool debugErrorDone = false;
        if (debugErrorMsg && !debugErrorDone) {
            debugErrorDone = true;
            std::lock_guard<std::mutex> lock(params.guiState.mutex);
            params.guiState.listError = debugErrorMsg;
            params.guiState.listErrorSetAt = std::chrono::steady_clock::now();
        }
    }

    // Debug-only, opt-in: SFZDRUMMER_DEBUG_{SAVE,LOAD}_{PRESET,PROFILE}_PATH
    // auto-fire the matching button's callback once on the first frame -
    // plugin.cpp's own handlers check these SAME env vars again to bypass
    // zenity and use that literal path directly, so this block only needs
    // to trigger the call, not touch the path itself. Lets the whole save/
    // load pipeline (mutex-protected gather/apply, real file I/O,
    // regenerateAndLoadDrumSfz, listError) be screenshot-verified without a
    // real zenity click. Unset in normal use - no effect on real hosts.
    {
        static bool debugPresetDone = false;
        if (!debugPresetDone) {
            debugPresetDone = true;
            if (getenv("SFZDRUMMER_DEBUG_SAVE_PRESET_PATH") && onSavePreset) onSavePreset();
            if (getenv("SFZDRUMMER_DEBUG_LOAD_PRESET_PATH") && onLoadPreset) onLoadPreset();
            if (getenv("SFZDRUMMER_DEBUG_SAVE_PROFILE_PATH") && onSaveProfile) onSaveProfile();
            if (getenv("SFZDRUMMER_DEBUG_LOAD_PROFILE_PATH") && onLoadProfile) onLoadProfile();
        }
    }

    // Debug-only, opt-in: SFZDRUMMER_DEBUG_EXPLODE_KIT=1 fires
    // onExplodeDrumKit once on the first frame, bypassing both the "Load
    // regions as individual percussion" button and its confirmation modal
    // (no synthetic mouse input in this sandbox). Same convention as the
    // SAVE/LOAD_PRESET/PROFILE_PATH hooks above.
    {
        static bool debugExplodeDone = false;
        if (!debugExplodeDone) {
            debugExplodeDone = true;
            if (getenv("SFZDRUMMER_DEBUG_EXPLODE_KIT") && onExplodeDrumKit) onExplodeDrumKit();
        }
    }

    // Debug-only, opt-in: SFZDRUMMER_DEBUG_INIT_KIT=1 fires onInitKit once
    // on the first frame, bypassing both the "Init Kit" button and its
    // confirmation modal. Same convention as the hooks above.
    {
        static bool debugInitKitDone = false;
        if (!debugInitKitDone) {
            debugInitKitDone = true;
            if (getenv("SFZDRUMMER_DEBUG_INIT_KIT") && onInitKit) onInitKit();
        }
    }

    float width = ImGui::GetContentRegionAvail().x;
    const float pianoAreaHeight = ImGui::GetStyle().ItemSpacing.y +
                                  88.f + ImGui::GetStyle().ScrollbarSize + 4.f +
                                  ImGui::GetTextLineHeightWithSpacing();
    float bodyHeight = ImGui::GetContentRegionAvail().y - pianoAreaHeight;
    if (bodyHeight < 100.f) bodyHeight = 100.f;

    const float listWidth = 300.f;
    const float explorerWidth = 320.f;
    float editWidth = width - listWidth - explorerWidth - ImGui::GetStyle().ItemSpacing.x * 2;
    if (editWidth < 200.f) editWidth = 200.f;

    ImGui::BeginChild("##listcol", ImVec2(listWidth, bodyHeight), true);
    drawDrumList(params, onParamChanged);
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##explorercol", ImVec2(explorerWidth, bodyHeight), true);
    {
        float vol = params.previewVolume.load();
        ImGui::SetNextItemWidth(150);
        if (ImGui::SliderFloat("##previewvol", &vol, 0.0f, 1.0f, "Vol %.2f"))
            params.previewVolume = vol;
        ImGui::SameLine();
        bool enabled = params.previewEnabled.load();
        if (enabled) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.25f, 1.f));
        if (ImGui::Button("Preview")) params.previewEnabled = !enabled;
        if (enabled) ImGui::PopStyleColor();

        // Load/preview failures show as a temporary corner toast (see
        // drawErrorToast) instead of inline here - this column is too
        // narrow for an error message to read comfortably.
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ui.fileExplorer.draw(avail, onPreviewRequested, onLoadRequested);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##editcol", ImVec2(editWidth, bodyHeight), true);
    drawEditZone(params, ui, onParamChanged, onExplodeDrumKit);
    ImGui::EndChild();

    ImGui::Spacing();

    int rootMarker = -1;
    std::array<bool, 128> hasDrum{};
    {
        std::lock_guard<std::mutex> lock(params.guiState.mutex);
        if (params.guiState.selectedId >= 0)
            for (auto& d : params.guiState.drums)
                if (d.id == params.guiState.selectedId) {
                    rootMarker = d.rootNote;
                    break;
                }
        for (auto& d : params.guiState.drums)
            if (d.hasSource && d.rootNote >= 0 && d.rootNote < 128)
                hasDrum[static_cast<size_t>(d.rootNote)] = true;
    }
    drawPiano(params, ui, width, rootMarker, hasDrum, [&](int note) {
        {
            std::lock_guard<std::mutex> lock(params.guiState.mutex);
            if (params.guiState.selectedId < 0) return;
            for (auto& d : params.guiState.drums)
                if (d.id == params.guiState.selectedId) {
                    d.rootNote = note;
                    break;
                }
        }
        if (onParamChanged) onParamChanged();
    });

    ImGui::Text("Voices: %d", params.activeVoiceCount.load());

    ImGui::End();

    drawErrorToast(params);
}
