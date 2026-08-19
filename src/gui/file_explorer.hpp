// Embedded (not popup) directory browser for the "FILE EXPLORER" pane -
// unlike SoloSampler's FileDialog (its own ImGui::Begin window, opened/
// closed on demand), this draws inline inside whatever region the caller
// gives it, always visible, no multi-select/staging (a drum takes exactly
// one sample or SFZ, never a layered stack).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"

class FileExplorer {
public:
    // Draws the browser filling `size`. onPreview fires on a single click of
    // a sample row (never an .sfz row - preview is sample-only, per spec).
    // onLoad fires on a double click of a sample OR .sfz row.
    void draw(const ImVec2& size, const std::function<void(const std::string&)>& onPreview,
              const std::function<void(const std::string&)>& onLoad);

    // Navigates straight to `dir`, refreshing on the next draw() - used
    // after a successful drag-and-drop import (see plugin.cpp's guiCreate)
    // so the explorer lands wherever the user's drums actually live instead
    // of wherever it was last left. Not used for explorer-driven loads
    // (double click) - the user is already in that folder in that case.
    void jumpTo(const std::string& dir);

private:
    void refresh();

    bool initialized_ = false;
    bool needRefresh_ = true;
    std::string cwd_;
    char pathEdit_[1024] = {};

    struct Entry {
        std::string name;
        bool isDir;
        bool isSample;
        bool isSfz;
        uint64_t size;
    };
    std::vector<Entry> entries_;
    // Index into entries_, -1 = nothing selected. Drives both the highlight
    // and Up/Down arrow-key navigation (see draw()'s keyboard handling).
    int selectedIndex_ = -1;
    // Set for one frame right after a keyboard nav move, so the entry loop
    // scrolls it into view exactly once.
    bool scrollToSelected_ = false;
};
