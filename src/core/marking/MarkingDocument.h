/*
 * Xournal++ Teacher Marking
 *
 * Structured marking data shared by the desktop review UI and manifest I/O.
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace xoj::marking {

enum class MarkingMode { Debox, PageBoxes };
enum class Verdict { Correct, Partial, Incorrect, Unresolved };

struct NormalizedBox {
    int y0{};
    int x0{};
    int y1{};
    int x1{};

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool isNearFullPage() const;
    [[nodiscard]] std::array<int, 4> asArray() const;
};

struct TextAnchor {
    std::string blockId;
    std::string exact;
};

struct DiagramAnchor {
    std::string blockId;
};

struct MarkingPart {
    std::string id;
    std::string label;
    double awardedMarks{};
    double maxMarks{};
    std::string rationale;
};

struct MarkingAnnotation {
    std::string id;
    size_t page{1};  // One-based, matching teacher-facing page numbers.
    std::string partId;
    Verdict verdict{Verdict::Unresolved};
    std::string source{"ai"};
    std::string severity;
    std::string targetType{"image"};
    bool reviewed{false};
    double awardedMarks{};
    double maxMarks{};
    NormalizedBox box;
    std::string title;
    std::string comment;
    std::string howToImprove;
    std::string evidenceFromScript;
    std::optional<TextAnchor> textAnchor;
    std::optional<DiagramAnchor> diagramAnchor;
};

struct ValidationIssue {
    enum class Severity { Error, Warning };

    Severity severity{Severity::Error};
    std::string location;
    std::string message;
};

class MarkingDocument {
public:
    static constexpr int CURRENT_VERSION = 1;

    int version{CURRENT_VERSION};
    std::string assignmentId;
    std::string title;
    std::string student;
    MarkingMode mode{MarkingMode::PageBoxes};
    std::string sourcePdf;
    std::string sourceSha256;
    bool cancelledWorkExcluded{false};
    std::vector<MarkingPart> parts;
    std::vector<MarkingAnnotation> annotations;

    [[nodiscard]] double awardedMarks() const;
    [[nodiscard]] double maxMarks() const;
    [[nodiscard]] size_t reviewedCount() const;
    bool updatePartMarks(const std::string& partId, double awarded, double maximum);
    [[nodiscard]] std::vector<ValidationIssue> validate(std::optional<size_t> pageCount = std::nullopt,
                                                        bool requireReviewed = false) const;
};

[[nodiscard]] const char* toString(MarkingMode mode);
[[nodiscard]] const char* toString(Verdict verdict);
[[nodiscard]] std::optional<MarkingMode> markingModeFromString(const std::string& value);
[[nodiscard]] std::optional<Verdict> verdictFromString(const std::string& value);

}  // namespace xoj::marking
