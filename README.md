# SfzDrummer

A multi-output SFZ drum machine [CLAP](https://cleveraudio.org/) plugin, backed by the [sfizz](https://github.com/sfz/sfizz) SFZ engine (using the [sfizioso](https://github.com/rullopat/sfizioso) fork). Sibling project to [SoloSampler](https://github.com/michael02022/SoloSampler), sharing the same core architecture (in-memory generated SFZ, sfizz backend, self-contained [Dear ImGui](https://github.com/ocornut/imgui) GUI over raw X11/OpenGL2 — no JUCE, no other framework).

Unlike SoloSampler (one instrument, one stereo output), SfzDrummer holds a whole **list of independent percussion modules** — each one its own SFZ `<master>` block, its own root note, and its own choice of one of **8 stereo audio outputs** — so each drum can be routed to its own channel strip in the DAW's mixer.

## Features

- **Drum list**: add/remove percussion modules, each with a note, an editable label, and an output routing (1-8). Root note can also be set by right-clicking the on-screen piano.
- **File explorer**: browse and load a sample or `.sfz` (drag & drop also supported) into whichever drum is selected; single-click a sample row to audition it.
- **Per-drum tabs** — Sample, Amp, Fil, Pitch, Opcodes:
  - **Sample**: Drum Kit Mode (pick a single key out of a whole pre-mapped kit `.sfz` instead of using it as one blob) and **"Load regions as individual percussion"**, which explodes every key found in a loaded kit into its own separate percussion module in one click; Volume, Pan (+ Pan Random/Alternate), Width, Quality, Polyphony/Note Polyphony, Loop Mode, sample-start Offset (+ velocity-linked amount), Exclusive Class/Off By, Transpose, Tune.
  - **Amp**: a velocity/gain curve editor, Veltrack/Random, a full amp envelope (Delay/Attack/Hold/Decay/Sustain/Release + curve shapes, with a second coarse "Decay Time (Extra)" slider for multi-second decays), and velocity-to-attack/sustain/volume depths.
  - **Fil**: 23 SFZ v2/ARIA filter types, Cutoff/Resonance/Random Cutoff/Veltrack, and an optional filter envelope.
  - **Pitch**: Veltrack/Random, an optional pitch envelope with a velocity-to-pitch-depth slider (and an "Invert vel2pitch" curve option).
  - **Opcodes**: a free-form text box, written verbatim into that drum's own `<master>` block, for any opcode the UI doesn't expose.
- **MPE**: an "Enable MPE" toggle (engine-level, real per-note pitch bend) alongside an always-present Bend Range combobox (200/1200/2400/4800 cents) — enabling MPE snaps Bend Range to 4800 cents.
- **Presets**: `.drmpreset` (the whole kit, every drum, baked SFZ content included) and `.drmprofile` (one selected drum's own design, everything except its loaded sample/SFZ — reusable across different sounds) via native `zenity` file dialogs, defaulting to `~/SfzdrummerPresets/`.
- **Init Kit**: one-click (confirmed) reset back to an empty kit with MPE/Bend Range at their defaults — the same state a freshly-loaded plugin instance starts in.

## Requirements

- Linux with X11 (the GUI is X11 + OpenGL2 — no Windows/macOS support currently)
- CMake ≥ 3.16 and a C++20 compiler (g++/clang++)
- `pkg-config`, `libsndfile`, X11 development headers, OpenGL development headers, POSIX threads
- `xxd` (embeds the Font Awesome icon font into the binary at build time)
- `zenity` at runtime (native file dialogs for Save/Load Preset/Profile)

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake pkg-config libsndfile1-dev libx11-dev libgl1-mesa-dev xxd zenity
```

The vendored sfizz shared library (`lib/libsfizz.so.1.2.3`) and its headers, the CLAP headers (`external/clap/`), Dear ImGui (`external/imgui/`), and the Font Awesome font (`external/fontawesome/`) all ship inside this repository — no submodules to fetch, no extra download step.

## Building

```sh
cmake -B build
cmake --build build -j$(nproc)
```

This produces `build/SfzDrummer.clap`.

## Installing

Copy (or symlink) the built plugin into your CLAP plugin directory:

```sh
mkdir -p ~/.clap
cp build/SfzDrummer.clap ~/.clap/
```

Most CLAP hosts on Linux also scan `/usr/lib/clap/` and `/usr/local/lib/clap/` system-wide.

## Testing

Three standalone test executables are built alongside the plugin:

```sh
./build/mini_host build/SfzDrummer.clap [sample.wav] [gui]   # dlopen's the .clap, drives it without a real DAW
./build/test_sfz_flatten                                     # SFZ parsing/flattening regression tests
./build/test_preset_file                                     # .drmpreset/.drmprofile round-trip regression tests
```

`mini_host` with a sample path exercises real audio rendering/multi-output routing checks; adding `gui` on the end opens the actual plugin window.

## Project layout

```
src/
  plugin.cpp          CLAP entry point, descriptor, state save/load, preset I/O, per-drum SFZ regeneration
  shared.hpp           GUI-thread ↔ audio-thread shared parameter struct (the drum list lives here)
  gui/                 Dear ImGui widget layer, X11 window/GL context, file explorer, file dialogs
  sfizz/                Thin RAII wrapper around the sfizz C API
  state/                Per-drum SFZ text generation, drum-kit flattening, sample analysis, preset files
lib/                    Vendored sfizz shared library + headers
external/               Vendored CLAP headers, Dear ImGui, Font Awesome
test/                   mini_host, test_sfz_flatten, test_preset_file
```

## Third-party components

| Component | Location | License |
|---|---|---|
| [sfizz](https://github.com/sfz/sfizz) (sfizioso fork) | `lib/` | BSD-2-Clause |
| [CLAP](https://github.com/free-audio/clap) | `external/clap/` | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | `external/imgui/` | MIT |
| Font Awesome 4 | `external/fontawesome/` | SIL OFL 1.1 (font), zlib (header) |

## License

This project's own source code doesn't declare a license yet — add one (e.g. MIT) before distributing it publicly if that matters for your use case.
