// X11 + GLX + ImGui window with its own GUI thread per plugin instance. The
// window is created as a child (embeddable via clap.gui/set_parent) and all
// of the instance's X11 traffic happens on its own Display, on its own
// thread. Implements XDND as a drag & drop target for files.
//
// Ported from NeoLooper (sibling project, /mnt/QuinientosM2/github/NeoLooper)
// verbatim aside from naming - see project memory for why: this window-level
// XDND handling (one hand-written X11 event pump, not a Component-tree
// hit-test system) is what structurally avoids the JUCE/Reaper drag-and-drop
// bug that motivated dropping JUCE for this plugin.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class GuiWindow {
public:
    // drawUI: builds the ImGui interface (called every frame, GUI thread).
    // onFileDrop: called once per drop gesture with every file path it
    // carried (GUI thread) - a single-file drop still calls it with a
    // one-element vector, so the callback always sees exactly what the user
    // dropped together in one go, never split into several calls.
    GuiWindow(std::function<void()> drawUI,
              std::function<void(const std::vector<std::string>&)> onFileDrop);
    ~GuiWindow();

    bool create(uint32_t w, uint32_t h);
    void destroy();

    // Callable from the host's main thread (queued to the GUI thread):
    void setParent(unsigned long x11Window);
    void setVisible(bool visible);
    void setSize(uint32_t w, uint32_t h);

    void getSize(uint32_t& w, uint32_t& h) const {
        w = width_.load();
        h = height_.load();
    }

private:
    void threadMain();

    std::function<void()> drawUI_;
    std::function<void(const std::vector<std::string>&)> onFileDrop_;

    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> width_{560}, height_{680};

    // command mailbox, host -> GUI thread
    std::mutex cmdMutex_;
    unsigned long pendingParent_ = 0;
    int pendingVisible_ = -1; // -1 nothing, 0 hide, 1 show
    uint32_t pendingW_ = 0, pendingH_ = 0;
    bool hasPendingSize_ = false;
};
