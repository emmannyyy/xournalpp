/*
 * StudySzn Marker
 *
 * Local AI-provider runner for teacher marking.
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "filesystem.h"

namespace xoj::marking {

enum class AiMarkingWorkflow { Debox, PageBoxes };

struct AiMarkingRequest {
    fs::path sourcePdf;
    fs::path rubric;
    fs::path outputDirectory;
    std::string sourceSha256;
    std::string instructions;
    AiMarkingWorkflow workflow{AiMarkingWorkflow::Debox};
};

struct AiMarkingResult {
    bool succeeded{};
    bool cancelled{};
    fs::path manifestPath;
    fs::path stagedSourcePdf;
    std::string error;
};

class MarkingAiRunner final {
public:
    using Completion = std::function<void(AiMarkingResult)>;

    MarkingAiRunner();
    ~MarkingAiRunner();

    MarkingAiRunner(const MarkingAiRunner&) = delete;
    MarkingAiRunner& operator=(const MarkingAiRunner&) = delete;

    [[nodiscard]] bool start(const AiMarkingRequest& request, Completion completion, std::string& error);
    void cancel();
    [[nodiscard]] bool isRunning() const;

    [[nodiscard]] static std::optional<fs::path> findCursorAgent();
    [[nodiscard]] static std::string extractManifestXml(const std::string& output);
    [[nodiscard]] static std::string buildPrompt(const AiMarkingRequest& request);

private:
    struct State;
    std::shared_ptr<State> state;
};

}  // namespace xoj::marking
