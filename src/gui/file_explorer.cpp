#include "file_explorer.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <strings.h>

#include "state/SampleInfo.h"

namespace fs = std::filesystem;

void FileExplorer::refresh() {
    entries_.clear();
    selectedIndex_ = -1;
    std::error_code ec;
    for (auto& de : fs::directory_iterator(cwd_, ec)) {
        Entry e;
        e.name = de.path().filename().string();
        if (e.name.empty() || e.name[0] == '.') continue;
        e.isDir = de.is_directory(ec);
        if (e.isDir) {
            e.isSample = e.isSfz = false;
            e.size = 0;
        } else {
            e.isSample = isSupportedAudioFile(e.name);
            e.isSfz = isSfzFile(e.name);
            if (!e.isSample && !e.isSfz) continue; // only show what a drum can use
            e.size = static_cast<uint64_t>(de.file_size(ec));
        }
        entries_.push_back(std::move(e));
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    snprintf(pathEdit_, sizeof(pathEdit_), "%s", cwd_.c_str());
    needRefresh_ = false;
}

void FileExplorer::jumpTo(const std::string& dir) {
    if (dir.empty()) return;
    initialized_ = true;
    cwd_ = dir;
    needRefresh_ = true;
}

void FileExplorer::draw(const ImVec2& size, const std::string& kitPath,
                        const std::function<void(const std::string&)>& onPreview,
                        const std::function<void(const std::string&)>& onLoad) {
    bool justInitialized = false;
    if (!initialized_) {
        initialized_ = true;
        justInitialized = true;
        if (!kitPath.empty()) {
            cwd_ = kitPath;
        } else {
            const char* home = getenv("HOME");
            cwd_ = home ? home : "/";
        }
    }
    if (needRefresh_) refresh();

    ImGui::BeginChild("##fileexplorer", size, true);

    // Kit Path navigation shortcut - see file_explorer.hpp's draw() comment.
    // Purely a jumpTo() trigger on the click that lands on "Kit"; nothing
    // else about this widget's own state depends on which tab is "active" -
    // except on the very first draw, where "Kit" starts selected (matching
    // cwd_'s own Kit Path default above) whenever one's already set, so a
    // user with a Kit Path configured lands on that tab, not "Files".
    if (!kitPath.empty() && ImGui::BeginTabBar("##fexplorertabs")) {
        if (ImGui::BeginTabItem("Files")) ImGui::EndTabItem();
        ImGuiTabItemFlags kitTabFlags =
            justInitialized ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("Kit", nullptr, kitTabFlags)) {
            if (ImGui::IsItemClicked()) jumpTo(kitPath);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (ImGui::Button("Up")) {
        fs::path p(cwd_);
        if (p.has_parent_path() && p != p.root_path()) {
            cwd_ = p.parent_path().string();
            needRefresh_ = true;
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##path", pathEdit_, sizeof(pathEdit_),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::error_code ec;
        if (fs::is_directory(pathEdit_, ec)) {
            cwd_ = pathEdit_;
            needRefresh_ = true;
        }
    }

    // "Activates" entries_[index] the same way a double click does: a
    // directory is entered, a sample/.sfz is loaded into the selected drum.
    // Shared by the mouse double-click handler below and the Enter key.
    auto activate = [&](int index) {
        if (index < 0 || index >= static_cast<int>(entries_.size())) return;
        const Entry& e = entries_[static_cast<size_t>(index)];
        std::string fullPath = (fs::path(cwd_) / e.name).string();
        if (e.isDir) {
            cwd_ = fullPath;
            needRefresh_ = true;
        } else if (onLoad) {
            onLoad(fullPath);
        }
    };

    // Up/Down arrow keys move selectedIndex_ (auditioning sample rows as
    // they're passed over, same as a single click) and Enter activates
    // whatever's currently selected - lets the user audition and pick a
    // drum's source without touching the mouse. Guarded on !IsAnyItemActive
    // so typing in the path field above (or, later, a multi-line opcode
    // editor elsewhere in the plugin) never gets hijacked by these keys.
    if (!entries_.empty() && !ImGui::IsAnyItemActive()) {
        const int last = static_cast<int>(entries_.size()) - 1;
        int newIndex = selectedIndex_;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            newIndex = std::min(last, selectedIndex_ + 1);
        else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            newIndex = std::max(0, selectedIndex_ - 1);
        if (newIndex != selectedIndex_) {
            selectedIndex_ = newIndex;
            scrollToSelected_ = true;
            const Entry& e = entries_[static_cast<size_t>(selectedIndex_)];
            if (!e.isDir && e.isSample && onPreview)
                onPreview((fs::path(cwd_) / e.name).string());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
            activate(selectedIndex_);
    }

    if (ImGui::BeginChild("##felist", ImVec2(0, 0), false)) {
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            const Entry& e = entries_[static_cast<size_t>(i)];
            std::string label = (e.isDir ? "[DIR] " : e.isSfz ? "[SFZ] " : "      ") + e.name;
            bool isSelected = i == selectedIndex_;
            ImGui::PushID(i);
            if (ImGui::Selectable(label.c_str(), isSelected,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                selectedIndex_ = i;
                if (ImGui::IsMouseDoubleClicked(0)) {
                    activate(i);
                } else if (!e.isDir && e.isSample) {
                    // Sample rows preview on a plain single click; .sfz rows
                    // only ever load (double click) - per spec, preview is
                    // sample-only.
                    if (onPreview) onPreview((fs::path(cwd_) / e.name).string());
                }
            }
            if (!e.isDir && ImGui::IsItemHovered())
                ImGui::SetTooltip("%.1f KB", e.size / 1024.0);
            if (isSelected && scrollToSelected_) {
                ImGui::SetScrollHereY(0.25f);
                scrollToSelected_ = false;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::EndChild();
}
