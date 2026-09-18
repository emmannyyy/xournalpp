#include "MarkingDocument.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <string>
#include <unordered_set>

namespace xoj::marking {

bool NormalizedBox::isValid() const {
    return y0 >= 0 && x0 >= 0 && y1 <= 1000 && x1 <= 1000 && y1 > y0 && x1 > x0;
}

bool NormalizedBox::isNearFullPage() const { return (y1 - y0) > 850 && (x1 - x0) > 850; }

std::array<int, 4> NormalizedBox::asArray() const { return {y0, x0, y1, x1}; }

double MarkingDocument::awardedMarks() const {
    return std::accumulate(parts.begin(), parts.end(), 0.0,
                           [](double total, const MarkingPart& part) { return total + part.awardedMarks; });
}

double MarkingDocument::maxMarks() const {
    return std::accumulate(parts.begin(), parts.end(), 0.0,
                           [](double total, const MarkingPart& part) { return total + part.maxMarks; });
}

size_t MarkingDocument::reviewedCount() const {
    return static_cast<size_t>(
            std::count_if(annotations.begin(), annotations.end(), [](const auto& annotation) {
                return annotation.reviewed;
            }));
}

bool MarkingDocument::updatePartMarks(const std::string& partId, double awarded, double maximum) {
    if (!std::isfinite(awarded) || !std::isfinite(maximum) || maximum < 0.0 || awarded < 0.0 ||
        awarded > maximum) {
        return false;
    }
    const auto part = std::find_if(parts.begin(), parts.end(),
                                   [&partId](const auto& candidate) { return candidate.id == partId; });
    if (part == parts.end()) {
        return false;
    }
    part->awardedMarks = awarded;
    part->maxMarks = maximum;
    return true;
}

std::vector<ValidationIssue> MarkingDocument::validate(std::optional<size_t> pageCount, bool requireReviewed) const {
    std::vector<ValidationIssue> issues;
    const auto error = [&issues](std::string location, std::string message) {
        issues.push_back({ValidationIssue::Severity::Error, std::move(location), std::move(message)});
    };
    const auto warning = [&issues](std::string location, std::string message) {
        issues.push_back({ValidationIssue::Severity::Warning, std::move(location), std::move(message)});
    };

    if (version != CURRENT_VERSION) {
        error("manifest", "Unsupported marking manifest version");
    }
    if (title.empty()) {
        warning("assignment", "Assignment title is empty");
    }
    if (mode == MarkingMode::PageBoxes && !cancelledWorkExcluded) {
        error("assignment", "STEM producer must attest that cancelled work was excluded");
    }
    if (!sourceSha256.empty() &&
        (sourceSha256.size() != 64 ||
         !std::all_of(sourceSha256.begin(), sourceSha256.end(),
                      [](unsigned char character) { return std::isxdigit(character) != 0; }))) {
        error("assignment", "Source PDF SHA-256 must be 64 hexadecimal characters");
    }

    std::unordered_set<std::string> partIds;
    for (size_t index = 0; index < parts.size(); ++index) {
        const auto& part = parts[index];
        const std::string location = "part[" + std::to_string(index) + "]";
        if (part.id.empty()) {
            error(location, "Part ID is required");
        } else if (!partIds.insert(part.id).second) {
            error(location, "Part ID must be unique");
        }
        if (!std::isfinite(part.awardedMarks) || !std::isfinite(part.maxMarks) || part.maxMarks < 0.0 ||
            part.awardedMarks < 0.0 || part.awardedMarks > part.maxMarks) {
            error(location, "Part marks must satisfy 0 <= awarded <= max");
        }
    }

    std::unordered_set<std::string> annotationIds;
    for (size_t index = 0; index < annotations.size(); ++index) {
        const auto& annotation = annotations[index];
        const std::string location = "annotation[" + std::to_string(index) + "]";

        if (annotation.id.empty()) {
            error(location, "Annotation ID is required");
        } else if (!annotationIds.insert(annotation.id).second) {
            error(location, "Annotation ID must be unique");
        }
        if (annotation.page == 0 || (pageCount && annotation.page > *pageCount)) {
            error(location, "Annotation page is outside the document");
        }
        if (annotation.partId.empty()) {
            error(location, "Question part ID is required");
        } else if (!partIds.contains(annotation.partId)) {
            error(location, "Question part ID does not reference a known part");
        }
        if (annotation.verdict == Verdict::Unresolved) {
            if (requireReviewed) {
                error(location, "Annotation verdict is unresolved");
            } else {
                warning(location, "Annotation verdict is unresolved");
            }
        }
        if (annotation.source != "ai" && annotation.source != "teacher") {
            error(location, "Annotation source must be ai or teacher");
        }
        if (!annotation.box.isValid()) {
            error(location, "Bounding box must be ordered integers from 0 to 1000");
        } else if (annotation.box.isNearFullPage()) {
            error(location, "Near-full-page bounding boxes are not allowed");
        }
        if (!std::isfinite(annotation.awardedMarks) || !std::isfinite(annotation.maxMarks) ||
            annotation.maxMarks < 0.0 || annotation.awardedMarks < 0.0 ||
            annotation.awardedMarks > annotation.maxMarks) {
            error(location, "Annotation marks must satisfy 0 <= awarded <= max");
        }
        if (annotation.textAnchor && annotation.diagramAnchor) {
            error(location, "Text and diagram anchors are mutually exclusive");
        }
        if (mode == MarkingMode::Debox && !annotation.textAnchor && !annotation.diagramAnchor) {
            error(location, "Debox annotations require exactly one text or diagram anchor");
        }
        if (mode == MarkingMode::PageBoxes) {
            if (annotation.textAnchor || annotation.diagramAnchor) {
                error(location, "Page-box annotations must not contain extracted-document anchors");
            }
            if (annotation.targetType != "image") {
                error(location, "Page-box annotations must target the original page image");
            }
        }
        if (requireReviewed && !annotation.reviewed) {
            error(location, "Annotation has not been reviewed");
        }
        if (annotation.comment.empty()) {
            warning(location, "Student-facing comment is empty");
        }
        for (size_t prior = 0; prior < index; ++prior) {
            const auto& other = annotations[prior];
            if (other.page == annotation.page &&
                std::abs(other.box.y0 - annotation.box.y0) + std::abs(other.box.x0 - annotation.box.x0) < 8) {
                error(location, "Annotation origin duplicates or nearly duplicates an earlier box");
                break;
            }
        }
    }

    return issues;
}

const char* toString(MarkingMode mode) {
    switch (mode) {
        case MarkingMode::Debox:
            return "debox";
        case MarkingMode::PageBoxes:
            return "page-boxes";
    }
    return "page-boxes";
}

const char* toString(Verdict verdict) {
    switch (verdict) {
        case Verdict::Correct:
            return "correct";
        case Verdict::Partial:
            return "partial";
        case Verdict::Incorrect:
            return "incorrect";
        case Verdict::Unresolved:
            return "unresolved";
    }
    return "unresolved";
}

std::optional<MarkingMode> markingModeFromString(const std::string& value) {
    if (value == "debox") {
        return MarkingMode::Debox;
    }
    if (value == "page-boxes" || value == "image_bbox") {
        return MarkingMode::PageBoxes;
    }
    return std::nullopt;
}

std::optional<Verdict> verdictFromString(const std::string& value) {
    if (value == "correct") {
        return Verdict::Correct;
    }
    if (value == "partial") {
        return Verdict::Partial;
    }
    if (value == "incorrect") {
        return Verdict::Incorrect;
    }
    if (value == "unresolved") {
        return Verdict::Unresolved;
    }
    return std::nullopt;
}

}  // namespace xoj::marking
