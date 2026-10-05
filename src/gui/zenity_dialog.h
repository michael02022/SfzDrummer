// Native OS file dialogs via zenity(1) - used for the Preset/Profile save-
// load UI (see drawEditorUI's preset row), which is explicitly meant to
// feel like the user's own OS-level files, unlike the sample/sfz loader's
// in-house ImGui FileDialog (file_dialog.hpp). GUI thread only: both
// functions block until the user closes the dialog, same as any native
// modal save/open dialog would.
#pragma once

#include <string>
#include <vector>

struct ZenityFilter {
    std::string name;    // shown in the dialog's filter dropdown
    std::string pattern; // e.g. "*.sspreset", or several space-separated globs
};

// Returns the chosen path, or "" if the user cancelled, zenity isn't
// installed, or the call otherwise failed - callers should treat "" as
// "nothing to do", not as an error to report.
std::string zenitySaveFile(const std::string& title, const std::string& defaultPath,
                           const std::vector<ZenityFilter>& filters);
std::string zenityOpenFile(const std::string& title, const std::string& defaultPath,
                           const std::vector<ZenityFilter>& filters);

// Folder picker (Kit Path's "Set Kit Folder" button) - same shape/blocking
// behavior as zenityOpenFile, just directory-only (--directory), no
// filters (folders don't have file-type filters).
std::string zenitySelectFolder(const std::string& title, const std::string& defaultPath);
