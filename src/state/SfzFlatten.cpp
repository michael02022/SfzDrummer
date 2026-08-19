#include "SfzFlatten.h"

#include <sstream>

#include "SfzParsePipeline.h"

FlattenedSfz flattenMultisampleSfz(const std::string& sfzPath) {
    FlattenedSfz result;
    ParsedSfzRegions parsed = parseAndFlattenSfz(sfzPath);
    if (!parsed.ok) {
        result.error = parsed.error;
        return result;
    }

    std::ostringstream out;
    for (const auto& region : parsed.regions) {
        out << "<region>\n";
        for (const auto& kv : region) out << kv.first << "=" << kv.second << "\n";
    }

    result.ok = true;
    result.regionsText = out.str();
    result.regionCount = static_cast<int>(parsed.regions.size());
    return result;
}
