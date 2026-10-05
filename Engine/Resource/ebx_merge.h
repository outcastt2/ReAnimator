#pragma once

#include "ebx_document.h"

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace dingosdk::frostbite::ebx {

struct MergeSummary {
    std::size_t instances{};   // instances taken from the edits
    std::size_t arrayEntries{};// root array entries taken from the edits
    std::size_t renumbered{};  // carried instances whose object id was re-derived
    // Root array entries left behind because `carry` turned their target down.
    struct Dropped {
        std::size_t edit{};    // index into `edits`
        ImportReference target;
    };
    std::vector<Dropped> dropped;
};

// Whether a root array entry an edit appended comes across, asked for an entry
// that is a reference into another document. Such a reference only resolves
// while the document it names is loaded; one that is not loads as a null object.
using CarryImport = std::function<bool(const ImportReference&)>;

// Combines several independently edited copies of one EBX asset with the base
// they were all derived from. Each edit contributes the instances it added and
// the entries it appended to the root's arrays; anything an edit merely changed
// in place stays as the base had it, because two edits to one value cannot both
// be honoured. Edits are applied in the order given, so the last one wins a tie.
[[nodiscard]] Document merge_documents(const Document& base,
                                       std::span<const Document* const> edits,
                                       MergeSummary* summary = nullptr,
                                       const CarryImport& carry = {});

} // namespace dingosdk::frostbite::ebx
