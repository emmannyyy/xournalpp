/*
 * Xournal++ Teacher Marking
 *
 * Reader and writer for the versioned .xoppmark manifest.
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include "filesystem.h"

#include "MarkingDocument.h"

namespace xoj::marking {

class MarkingXml {
public:
    static MarkingDocument load(const fs::path& path);
    static void save(const MarkingDocument& document, const fs::path& path);
};

}  // namespace xoj::marking
