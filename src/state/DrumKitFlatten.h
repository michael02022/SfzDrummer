// Alternative to SfzFlatten.h's flattenMultisampleSfz for a .sfz that's
// actually a whole pre-mapped drum kit (multiple keys, each with its own
// round robins/velocity layers/crossfades already set up): instead of
// collapsing every region in the file into one blob, this groups them BY
// their effective key, producing one ready-to-use region blob PER key found
// - so the user can pick out a single drum/percussion from an existing kit
// (see the Sample tab's "Drum Kit Mode" checkbox + key slider, plugin.cpp's
// loadIntoSelectedDrum, DrumSfzBuilder.cpp).
#pragma once

#include <string>
#include <vector>

// Every region that resolves to this same key (round robins/velocity
// layers/crossfades all belong to the same drum/percussion) collapsed into
// one region blob - exactly the same shape flattenMultisampleSfz produces
// for a single drum, just scoped to one key out of the whole kit.
struct DrumKitKeyGroup {
    int key = 0;              // the note this group of regions triggers on
    std::string regionsText;  // concatenated "<region> key=val ...\n" blocks
    int regionCount = 0;      // round robins/velocity layers/etc. in this group
};

struct FlattenedDrumKit {
    bool ok = false;
    std::string error; // set iff !ok
    std::vector<DrumKitKeyGroup> keyGroups; // sorted by key ascending
};

// GUI thread only (file dialog accept / XDND drop), same convention as
// flattenMultisampleSfz.
FlattenedDrumKit flattenDrumKit(const std::string& sfzPath);
