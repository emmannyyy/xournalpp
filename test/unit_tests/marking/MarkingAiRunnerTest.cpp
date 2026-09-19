#include <chrono>
#include <fstream>
#include <optional>
#include <string>

#include <glib.h>
#include <glib/gstdio.h>
#include <gtest/gtest.h>

#include "marking/MarkingAiRunner.h"
#include "marking/MarkingXml.h"

using namespace xoj::marking;

TEST(MarkingAiRunner, ExtractsOnlyManifestPayload) {
    const std::string response =
            "Working...\n```xml\n<?xml version=\"1.0\"?>\n<marking version=\"1\"></marking>\n```\nDone";
    EXPECT_EQ(MarkingAiRunner::extractManifestXml(response), "<marking version=\"1\"></marking>");
    EXPECT_THROW((void)MarkingAiRunner::extractManifestXml("No structured output"), std::runtime_error);
}

TEST(MarkingAiRunner, BuildsWorkflowBoundReadOnlyPrompt) {
    AiMarkingRequest request{
            .sourceSha256 = std::string(64, 'a'),
            .instructions = "Prioritise method marks.",
            .workflow = AiMarkingWorkflow::PageBoxes,
    };
    request.rubric = "scheme.pdf";
    const auto prompt = MarkingAiRunner::buildPrompt(request);
    EXPECT_NE(prompt.find("mode=\"page-boxes\""), std::string::npos);
    EXPECT_NE(prompt.find(request.sourceSha256), std::string::npos);
    EXPECT_NE(prompt.find("Do not run\nshell commands"), std::string::npos);
    EXPECT_NE(prompt.find("Read task.txt"), std::string::npos);
    EXPECT_NE(prompt.find("part-id=\"q1-a\""), std::string::npos);
    EXPECT_NE(prompt.find("<box x0="), std::string::npos);
    EXPECT_NE(prompt.find("target-type=\"image\""), std::string::npos);
    EXPECT_EQ(prompt.find(" part=\"q1-a\""), std::string::npos);
    EXPECT_EQ(prompt.find(" box=\""), std::string::npos);
    EXPECT_EQ(prompt.find("Prioritise method marks."), std::string::npos);
}

TEST(MarkingAiRunner, BuildsDeboxTextAnchorContract) {
    AiMarkingRequest request{
            .sourceSha256 = std::string(64, 'a'),
            .workflow = AiMarkingWorkflow::Debox,
    };
    request.rubric = "scheme.pdf";
    const auto prompt = MarkingAiRunner::buildPrompt(request);
    EXPECT_NE(prompt.find("target-type=\"text\""), std::string::npos);
    EXPECT_NE(prompt.find("<text-anchor"), std::string::npos);
    EXPECT_NE(prompt.find("exact=\"Exact evidence visible"), std::string::npos);
}

#ifndef _WIN32
TEST(MarkingAiRunner, RunsHeadlessProviderInStagedWorkspace) {
    gchar* uuid = g_uuid_string_random();
    const fs::path root = fs::temp_directory_path() / ("studyszn-marker-ai-test-" + std::string(uuid));
    g_free(uuid);
    fs::create_directories(root);
    const fs::path source = root / "source.pdf";
    const fs::path rubric = root / "rubric.txt";
    const fs::path fakeAgent = root / "agent";
    const fs::path output = root / "job";
    {
        std::ofstream(source) << "student";
        std::ofstream(rubric) << "rubric";
        std::ofstream script(fakeAgent);
        script << "#!/bin/sh\n"
                  "[ -z \"$STUDYSZN_TEST_SECRET\" ] || exit 9\n"
                  "[ -z \"$CURSOR_API_KEY\" ] || exit 10\n"
                  "for arg in \"$@\"; do\n"
                  "  case \"$arg\" in *SECRET_TEACHER_TEXT*) exit 8 ;; esac\n"
                  "done\n"
                  "printf '%s\\n' '<marking version=\"1\"></marking>'\n";
    }
    ASSERT_EQ(g_chmod(fakeAgent.string().c_str(), 0700), 0);

    const char* previous = g_getenv("STUDYSZN_CURSOR_AGENT");
    const std::optional<std::string> previousValue = previous ? std::optional<std::string>(previous) : std::nullopt;
    g_setenv("STUDYSZN_CURSOR_AGENT", fakeAgent.string().c_str(), true);
    g_setenv("STUDYSZN_TEST_SECRET", "must-not-reach-provider", true);
    const char* previousApiKey = g_getenv("CURSOR_API_KEY");
    const std::optional<std::string> previousApiKeyValue =
            previousApiKey ? std::optional<std::string>(previousApiKey) : std::nullopt;
    g_setenv("CURSOR_API_KEY", "must-not-reach-provider", true);

    gchar* computedSha = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "student", -1);
    ASSERT_NE(computedSha, nullptr);
    const std::string expectedSha(computedSha);
    g_free(computedSha);

    MarkingAiRunner runner;
    AiMarkingRequest request{
            .sourcePdf = source,
            .rubric = rubric,
            .outputDirectory = output,
            .sourceSha256 = expectedSha,
            .instructions = "SECRET_TEACHER_TEXT",
    };
    std::optional<AiMarkingResult> result;
    std::string error;
    ASSERT_TRUE(runner.start(request, [&result](AiMarkingResult value) { result = std::move(value); }, error))
            << error;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!result && std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, false)) {
        }
        g_usleep(1000);
    }
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->succeeded) << result->error;
    EXPECT_TRUE(fs::exists(result->manifestPath));
    EXPECT_TRUE(fs::exists(result->stagedSourcePdf));
    EXPECT_EQ(fs::status(result->stagedSourcePdf).permissions() & fs::perms::owner_write, fs::perms::none);
    EXPECT_EQ(fs::status(output / "task.txt").permissions() & fs::perms::owner_write, fs::perms::none);

    if (previousValue) {
        g_setenv("STUDYSZN_CURSOR_AGENT", previousValue->c_str(), true);
    } else {
        g_unsetenv("STUDYSZN_CURSOR_AGENT");
    }
    g_unsetenv("STUDYSZN_TEST_SECRET");
    if (previousApiKeyValue) {
        g_setenv("CURSOR_API_KEY", previousApiKeyValue->c_str(), true);
    } else {
        g_unsetenv("CURSOR_API_KEY");
    }
    fs::remove_all(root);
}

TEST(MarkingAiRunner, CleansWorkspaceWhenStagingFails) {
    gchar* uuid = g_uuid_string_random();
    const fs::path root = fs::temp_directory_path() / ("studyszn-marker-ai-stage-failure-" + std::string(uuid));
    g_free(uuid);
    fs::create_directories(root);
    const fs::path source = root / "source.pdf";
    const fs::path rubric = root / "rubric.txt";
    const fs::path output = root / "job";
    std::ofstream(source) << "student";
    std::ofstream(rubric) << "rubric";
    const char* previous = g_getenv("STUDYSZN_CURSOR_AGENT");
    const std::optional<std::string> previousValue = previous ? std::optional<std::string>(previous) : std::nullopt;
    g_setenv("STUDYSZN_CURSOR_AGENT", TEST_FAKE_CURSOR_AGENT, true);

    MarkingAiRunner runner;
    AiMarkingRequest request{
            .sourcePdf = source,
            .rubric = rubric,
            .outputDirectory = output,
            .sourceSha256 = std::string(64, '0'),
    };
    std::string error;
    EXPECT_FALSE(runner.start(request, [](AiMarkingResult) {}, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(fs::exists(output));
    if (previousValue) {
        g_setenv("STUDYSZN_CURSOR_AGENT", previousValue->c_str(), true);
    } else {
        g_unsetenv("STUDYSZN_CURSOR_AGENT");
    }
    fs::remove_all(root);
}

TEST(MarkingAiRunner, CancelsAndReapsProvider) {
    gchar* uuid = g_uuid_string_random();
    const fs::path root = fs::temp_directory_path() / ("studyszn-marker-ai-cancel-" + std::string(uuid));
    g_free(uuid);
    fs::create_directories(root);
    const fs::path source = root / "source.pdf";
    const fs::path rubric = root / "rubric.txt";
    const fs::path fakeAgent = root / "agent";
    std::ofstream(source) << "student";
    std::ofstream(rubric) << "rubric";
    std::ofstream(fakeAgent) << "#!/bin/sh\nsleep 30\n";
    ASSERT_EQ(g_chmod(fakeAgent.string().c_str(), 0700), 0);

    const char* previous = g_getenv("STUDYSZN_CURSOR_AGENT");
    const std::optional<std::string> previousValue = previous ? std::optional<std::string>(previous) : std::nullopt;
    g_setenv("STUDYSZN_CURSOR_AGENT", fakeAgent.string().c_str(), true);
    gchar* computedSha = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "student", -1);
    ASSERT_NE(computedSha, nullptr);

    MarkingAiRunner runner;
    AiMarkingRequest request{
            .sourcePdf = source,
            .rubric = rubric,
            .outputDirectory = root / "job",
            .sourceSha256 = computedSha,
    };
    g_free(computedSha);
    std::optional<AiMarkingResult> result;
    std::string error;
    ASSERT_TRUE(runner.start(request, [&result](AiMarkingResult value) { result = std::move(value); }, error))
            << error;
    runner.cancel();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!result && std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, false)) {
        }
        g_usleep(1000);
    }
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->cancelled);
    EXPECT_FALSE(result->succeeded);
    EXPECT_FALSE(runner.isRunning());

    if (previousValue) {
        g_setenv("STUDYSZN_CURSOR_AGENT", previousValue->c_str(), true);
    } else {
        g_unsetenv("STUDYSZN_CURSOR_AGENT");
    }
    fs::remove_all(root);
}

TEST(MarkingAiRunner, ProducesValidHumanitiesAndStemDrafts) {
    gchar* uuid = g_uuid_string_random();
    const fs::path root = fs::temp_directory_path() / ("studyszn-marker-ai-workflows-" + std::string(uuid));
    g_free(uuid);
    fs::create_directories(root);
    const fs::path source = root / "source.pdf";
    const fs::path rubric = root / "rubric.md";
    std::ofstream(source) << "synthetic-student";
    std::ofstream(rubric) << "synthetic-rubric";

    const char* previous = g_getenv("STUDYSZN_CURSOR_AGENT");
    const std::optional<std::string> previousValue = previous ? std::optional<std::string>(previous) : std::nullopt;
    g_setenv("STUDYSZN_CURSOR_AGENT", TEST_FAKE_CURSOR_AGENT, true);
    gchar* computedSha = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "synthetic-student", -1);
    ASSERT_NE(computedSha, nullptr);
    const std::string expectedSha(computedSha);
    g_free(computedSha);

    for (const auto workflow: {AiMarkingWorkflow::Debox, AiMarkingWorkflow::PageBoxes}) {
        MarkingAiRunner runner;
        AiMarkingRequest request{
                .sourcePdf = source,
                .rubric = rubric,
                .outputDirectory = root / (workflow == AiMarkingWorkflow::Debox ? "debox" : "stem"),
                .sourceSha256 = expectedSha,
                .instructions = "Mark every response.",
                .workflow = workflow,
        };
        std::optional<AiMarkingResult> result;
        std::string error;
        ASSERT_TRUE(runner.start(request, [&result](AiMarkingResult value) { result = std::move(value); }, error))
                << error;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!result && std::chrono::steady_clock::now() < deadline) {
            while (g_main_context_iteration(nullptr, false)) {
            }
            g_usleep(1000);
        }
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->succeeded) << result->error;

        const auto draft = MarkingXml::load(result->manifestPath);
        EXPECT_TRUE(draft.validate(1, false).empty());
        ASSERT_EQ(draft.annotations.size(), 1);
        if (workflow == AiMarkingWorkflow::Debox) {
            EXPECT_EQ(draft.mode, MarkingMode::Debox);
            EXPECT_TRUE(draft.annotations.front().textAnchor.has_value());
        } else {
            EXPECT_EQ(draft.mode, MarkingMode::PageBoxes);
            EXPECT_EQ(draft.annotations.front().targetType, "image");
            EXPECT_FALSE(draft.annotations.front().textAnchor.has_value());
            EXPECT_FALSE(draft.annotations.front().diagramAnchor.has_value());
        }
    }

    if (previousValue) {
        g_setenv("STUDYSZN_CURSOR_AGENT", previousValue->c_str(), true);
    } else {
        g_unsetenv("STUDYSZN_CURSOR_AGENT");
    }
    fs::remove_all(root);
}
#endif
