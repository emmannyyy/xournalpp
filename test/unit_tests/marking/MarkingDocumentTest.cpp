#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <locale>
#include <regex>

#include <gtest/gtest.h>

#include "marking/MarkingDocument.h"
#include "marking/MarkingXml.h"

#include "config-test.h"

using namespace xoj::marking;

TEST(MarkingDocument, ValidatesMarkingContract) {
    MarkingDocument document;
    document.title = "Synthetic paper";
    document.mode = MarkingMode::Debox;
    document.sourceSha256 = std::string(64, 'a');
    document.parts.push_back({"q1-a", "Question 1(a)", 1.0, 2.0, "One of two points"});
    document.annotations.push_back(MarkingAnnotation{
            .id = "a01",
            .page = 1,
            .partId = "q1-a",
            .verdict = Verdict::Partial,
            .reviewed = true,
            .awardedMarks = 1.0,
            .maxMarks = 2.0,
            .box = {100, 120, 180, 880},
            .title = "Develop the explanation",
            .comment = "You identified the effect. Explain the next step.",
            .textAnchor = TextAnchor{"p1-b2", "the price will fall"},
    });

    EXPECT_TRUE(document.validate(1, true).empty());
    EXPECT_DOUBLE_EQ(document.awardedMarks(), 1.0);
    EXPECT_DOUBLE_EQ(document.maxMarks(), 2.0);
    EXPECT_EQ(document.reviewedCount(), 1U);
}

TEST(MarkingDocument, RejectsInvalidAnnotations) {
    MarkingDocument document;
    document.mode = MarkingMode::Debox;
    document.annotations.push_back(MarkingAnnotation{
            .id = "duplicate",
            .page = 0,
            .partId = "",
            .verdict = Verdict::Unresolved,
            .source = "invalid",
            .reviewed = false,
            .awardedMarks = 2.0,
            .maxMarks = 1.0,
            .box = {0, 0, 1000, 1000},
            .textAnchor = TextAnchor{"block", "text"},
            .diagramAnchor = DiagramAnchor{"diagram"},
    });
    document.annotations.push_back(document.annotations.front());

    const auto issues = document.validate(1, true);
    EXPECT_GE(issues.size(), 8U);
}

TEST(MarkingDocument, EnforcesModeSpecificContracts) {
    MarkingDocument stem;
    stem.title = "Synthetic STEM paper";
    stem.mode = MarkingMode::PageBoxes;
    stem.sourceSha256 = std::string(64, 'b');
    stem.cancelledWorkExcluded = true;
    stem.parts.push_back({"q1-a", "Question 1(a)", 1.0, 1.0, ""});
    stem.annotations.push_back(MarkingAnnotation{
            .id = "a01",
            .page = 1,
            .partId = "q1-a",
            .verdict = Verdict::Correct,
            .reviewed = true,
            .awardedMarks = 1.0,
            .maxMarks = 1.0,
            .box = {100, 100, 200, 500},
            .comment = "Correct.",
    });
    EXPECT_TRUE(stem.validate(1, true).empty());

    EXPECT_TRUE(stem.updatePartMarks("q1-a", 0.0, 1.0));
    EXPECT_DOUBLE_EQ(stem.awardedMarks(), 0.0);
    EXPECT_DOUBLE_EQ(stem.annotations.front().awardedMarks, 1.0);
    stem.annotations.front().textAnchor = TextAnchor{"p1-b1", "not allowed"};
    const auto stemIssues = stem.validate(1, true);
    EXPECT_TRUE(std::any_of(stemIssues.begin(), stemIssues.end(), [](const auto& issue) {
        return issue.message == "Page-box annotations must not contain extracted-document anchors";
    }));
    MarkingDocument debox;
    debox.title = "Synthetic economics paper";
    debox.mode = MarkingMode::Debox;
    debox.parts.push_back({"q2-a", "Question 2(a)", 0.0, 1.0, ""});
    debox.annotations.push_back(MarkingAnnotation{
            .id = "a01",
            .page = 1,
            .partId = "q2-a",
            .verdict = Verdict::Incorrect,
            .reviewed = true,
            .box = {200, 100, 300, 600},
            .comment = "Develop this point.",
    });
    const auto deboxIssues = debox.validate(1, true);
    EXPECT_TRUE(std::any_of(deboxIssues.begin(), deboxIssues.end(), [](const auto& issue) {
        return issue.message == "Debox annotations require exactly one text or diagram anchor";
    }));
}

TEST(MarkingDocument, PartScoresDoNotOverwriteStemEvidenceBoxes) {
    MarkingDocument document;
    document.mode = MarkingMode::PageBoxes;
    document.parts.push_back({"q1", "Question 1", 2.0, 3.0, ""});
    document.annotations.push_back(MarkingAnnotation{
            .id = "method",
            .partId = "q1",
            .awardedMarks = 1.0,
            .maxMarks = 1.0,
    });
    document.annotations.push_back(MarkingAnnotation{
            .id = "answer",
            .partId = "q1",
            .awardedMarks = 0.0,
            .maxMarks = 1.0,
    });

    ASSERT_TRUE(document.updatePartMarks("q1", 1.5, 3.0));
    EXPECT_DOUBLE_EQ(document.parts.front().awardedMarks, 1.5);
    EXPECT_DOUBLE_EQ(document.annotations[0].awardedMarks, 1.0);
    EXPECT_DOUBLE_EQ(document.annotations[1].awardedMarks, 0.0);
}

TEST(MarkingXml, RoundTripsHumanitiesAndStemAnchors) {
    MarkingDocument original;
    original.assignmentId = "demo";
    original.title = "Synthetic mixed marking";
    original.student = "Anonymous";
    original.mode = MarkingMode::Debox;
    original.sourcePdf = "submission.pdf";
    original.sourceSha256 = std::string(64, 'a');
    original.parts.push_back({"q2-a", "Question 2(a)", 8.0, 10.0, "Sound analysis"});
    original.annotations.push_back(MarkingAnnotation{
            .id = "a01",
            .page = 2,
            .partId = "q2-a",
            .verdict = Verdict::Correct,
            .reviewed = true,
            .awardedMarks = 1.0,
            .maxMarks = 1.0,
            .box = {200, 100, 260, 800},
            .title = "Accurate definition",
            .comment = "Correct. Your definition includes both key ideas.",
            .howToImprove = "Keep this precision.",
            .evidenceFromScript = "scarce resources",
            .textAnchor = TextAnchor{"p2-b3", "scarce resources"},
    });
    original.annotations.push_back(MarkingAnnotation{
            .id = "a02",
            .page = 3,
            .partId = "q2-a",
            .verdict = Verdict::Partial,
            .reviewed = false,
            .box = {300, 150, 650, 850},
            .title = "Complete the diagram",
            .comment = "Label the new equilibrium and state the price change.",
            .diagramAnchor = DiagramAnchor{"p3-d1"},
    });

    const auto path = std::filesystem::temp_directory_path() / "xournalpp-marking-roundtrip.xoppmark";
    MarkingXml::save(original, path);
    const auto loaded = MarkingXml::load(path);
    std::filesystem::remove(path);

    ASSERT_EQ(loaded.annotations.size(), 2U);
    ASSERT_EQ(loaded.parts.size(), 1U);
    EXPECT_EQ(loaded.assignmentId, original.assignmentId);
    EXPECT_EQ(loaded.mode, MarkingMode::Debox);
    EXPECT_EQ(loaded.sourceSha256, original.sourceSha256);
    EXPECT_EQ(loaded.annotations[0].textAnchor->exact, "scarce resources");
    EXPECT_EQ(loaded.annotations[0].source, "ai");
    EXPECT_EQ(loaded.annotations[0].howToImprove, "Keep this precision.");
    EXPECT_EQ(loaded.annotations[0].evidenceFromScript, "scarce resources");
    EXPECT_EQ(loaded.annotations[1].diagramAnchor->blockId, "p3-d1");
    EXPECT_EQ(loaded.annotations[1].box.asArray(), (std::array<int, 4>{300, 150, 650, 850}));
}

TEST(MarkingXml, WritesLocaleIndependentMarks) {
    class CommaDecimal final: public std::numpunct<char> {
    protected:
        char do_decimal_point() const override { return ','; }
    };

    MarkingDocument original;
    original.title = "Locale test";
    original.mode = MarkingMode::Debox;
    original.sourcePdf = "submission.pdf";
    original.parts.push_back({"q1", "Question 1", 1.5, 2.5, ""});

    const auto path = std::filesystem::temp_directory_path() / "xournalpp-marking-locale.xoppmark";
    const auto previous = std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
    MarkingXml::save(original, path);
    std::locale::global(previous);
    const auto loaded = MarkingXml::load(path);
    std::filesystem::remove(path);

    ASSERT_EQ(loaded.parts.size(), 1U);
    EXPECT_DOUBLE_EQ(loaded.parts.front().awardedMarks, 1.5);
    EXPECT_DOUBLE_EQ(loaded.parts.front().maxMarks, 2.5);
}

TEST(MarkingXml, LoadsPublicWorkflowFixtures) {
    const auto root = std::filesystem::path(PROJECT_SOURCE_DIR) / "test" / "resources" / "marking";
    const auto economics = MarkingXml::load(root / "humanities-debox.xoppmark");
    const auto stem = MarkingXml::load(root / "stem-page-boxes.xoppmark");

    EXPECT_EQ(economics.mode, MarkingMode::Debox);
    EXPECT_TRUE(economics.validate(2, true).empty());
    EXPECT_EQ(stem.mode, MarkingMode::PageBoxes);
    EXPECT_TRUE(stem.validate(2, true).empty());

    for (const auto& filename: {"humanities-debox.xoppmark", "stem-page-boxes.xoppmark"}) {
        std::ifstream input(root / filename);
        const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        EXPECT_EQ(contents.find("http://"), std::string::npos);
        EXPECT_EQ(contents.find("https://"), std::string::npos);
        EXPECT_FALSE(std::regex_search(
                contents, std::regex(R"([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})")));
    }
}

TEST(MarkingXml, CompletesTeacherEditReviewExportReloadWorkflow) {
    const auto root = std::filesystem::path(PROJECT_SOURCE_DIR) / "test" / "resources" / "marking";
    for (const auto& filename: {"humanities-debox.xoppmark", "stem-page-boxes.xoppmark"}) {
        auto document = MarkingXml::load(root / filename);
        ASSERT_FALSE(document.parts.empty());
        ASSERT_FALSE(document.annotations.empty());

        auto& part = document.parts.front();
        const double revisedAward = part.maxMarks / 2.0;
        ASSERT_TRUE(document.updatePartMarks(part.id, revisedAward, part.maxMarks));
        document.annotations.front().comment = "Teacher-reviewed feedback";
        for (auto& annotation: document.annotations) {
            annotation.reviewed = true;
        }

        const auto exported = std::filesystem::temp_directory_path() / filename;
        MarkingXml::save(document, exported);
        const auto reloaded = MarkingXml::load(exported);
        std::filesystem::remove(exported);

        ASSERT_FALSE(reloaded.parts.empty());
        ASSERT_FALSE(reloaded.annotations.empty());
        EXPECT_DOUBLE_EQ(reloaded.parts.front().awardedMarks, revisedAward);
        EXPECT_EQ(reloaded.annotations.front().comment, "Teacher-reviewed feedback");
        EXPECT_TRUE(std::all_of(reloaded.annotations.begin(), reloaded.annotations.end(),
                                [](const auto& annotation) { return annotation.reviewed; }));
        EXPECT_TRUE(reloaded.validate(2, true).empty());
    }
}

TEST(MarkingXml, LoadsPrivateEconomicsAndStemTrialsWhenConfigured) {
    const char* trialRoot = std::getenv("XOPP_MARKING_TRIAL_ROOT");
    if (trialRoot == nullptr) {
        GTEST_SKIP() << "Set XOPP_MARKING_TRIAL_ROOT to run private marking trials";
    }

    const auto root = std::filesystem::path(trialRoot);
    const auto economics = MarkingXml::load(root / "economics-debox" / "marking.xoppmark");
    const auto stem = MarkingXml::load(root / "stem-page-boxes" / "marking.xoppmark");

    EXPECT_EQ(economics.mode, MarkingMode::Debox);
    EXPECT_GE(economics.annotations.size(), 50U);
    EXPECT_TRUE(std::all_of(economics.annotations.begin(), economics.annotations.end(),
                            [](const auto& annotation) { return annotation.box.isValid(); }));
    EXPECT_TRUE(std::all_of(economics.annotations.begin(), economics.annotations.end(), [](const auto& annotation) {
        return annotation.textAnchor.has_value() != annotation.diagramAnchor.has_value();
    }));
    const auto hasError = [](const auto& issues) {
        return std::any_of(issues.begin(), issues.end(), [](const auto& issue) {
            return issue.severity == ValidationIssue::Severity::Error;
        });
    };
    EXPECT_FALSE(hasError(economics.validate(std::nullopt, false)));

    EXPECT_EQ(stem.mode, MarkingMode::PageBoxes);
    EXPECT_GE(stem.annotations.size(), 50U);
    EXPECT_TRUE(std::all_of(stem.annotations.begin(), stem.annotations.end(),
                            [](const auto& annotation) { return annotation.box.isValid(); }));
    EXPECT_TRUE(std::none_of(stem.annotations.begin(), stem.annotations.end(), [](const auto& annotation) {
        return annotation.textAnchor.has_value() || annotation.diagramAnchor.has_value();
    }));
    EXPECT_FALSE(hasError(stem.validate(std::nullopt, false)));
}
