// Builds the whole-kit SFZ text from the drum list: one <master> block per
// drum that has a source loaded, each carrying key=<rootNote>
// output=<outputIndex> so it triggers on exactly one note and renders to
// exactly one of the plugin's 8 stereo outputs. Drums with no source loaded
// yet emit nothing (silent placeholder).
#pragma once

#include <string>
#include <vector>

#include "shared.hpp"

// bendUpCents/bendDownCents: whole-instrument bend range (SharedParams::
// bendUpCents/bendDownCents), NOT per-drum - always written as a <global>
// bendup=/benddown= header before the <master> list, same as SoloSampler's
// own always-present bendup=/benddown=. See shared.hpp's comment for the
// full rationale (Enable MPE forces these to +-4800, Bend Range combobox
// otherwise picks the value).
std::string buildDrumSfzText(const std::vector<SharedParams::DrumItem>& drums, int bendUpCents,
                              int bendDownCents);
