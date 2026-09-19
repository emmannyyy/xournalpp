/*
 * Xournal++ Teacher Marking
 *
 * Review sidebar for structured teacher feedback.
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>

#include <gtk/gtk.h>

#include "gui/sidebar/AbstractSidebarPage.h"
#include "marking/MarkingAiRunner.h"
#include "marking/MarkingDocument.h"

#include "filesystem.h"

class Control;
class Element;

class SidebarMarkingPage final: public AbstractSidebarPage {
public:
    explicit SidebarMarkingPage(Control* control);
    ~SidebarMarkingPage() override;

    void enableSidebar() override;
    void disableSidebar() override;
    void layout() override;
    std::string getName() override;
    std::string getIconName() override;
    bool hasData() override;
    GtkWidget* getWidget() override;
    void documentChanged(DocumentChangeType type) override;
    void openManifest(const fs::path& path);
    void restoreManifest(const fs::path& path);

private:
    struct RowContext {
        SidebarMarkingPage* page;
        size_t index;
    };

    void buildUi();
    void rebuildList();
    void updateHeader();
    void showAnnotation(size_t index);
    void navigateToAnnotation(size_t index);
    void navigateRelative(int direction);
    void changeSelectedPart();
    [[nodiscard]] bool commitDetailEdits();
    void saveWorkingManifest();
    [[nodiscard]] bool checkpointRecoveryState(bool reportFailure = false);
    void applyDetailEdits();
    void markAllReviewed();
    void deleteSelectedAnnotation();
    void addAnnotationFromSelection();
    void chooseAiRubric();
    void runAiMarking();
    void cancelAiMarking();
    void finishAiMarking(xoj::marking::AiMarkingResult result);
    void updateAiControls(bool running, const std::string& status);
    void importManifest(const fs::path& path, bool allowSourcePdfSwitch);
    void activateManifest(xoj::marking::MarkingDocument draft, const fs::path& path, const fs::path& sourcePath);
    void exportManifest(const fs::path& path);
    void materializeAnnotations();
    void syncAnnotationGeometry();
    void showExportDialog();
    void showMessage(GtkMessageType type, const std::string& title, const std::string& message);
    [[nodiscard]] bool passesFilter(const xoj::marking::MarkingAnnotation& annotation) const;

    GtkWidget* root{};
    GtkWidget* headerLabel{};
    GtkWidget* scoreLabel{};
    GtkWidget* filterCombo{};
    GtkWidget* listBox{};
    GtkWidget* detailScroll{};
    GtkWidget* detailBox{};
    GtkWidget* detailTitle{};
    GtkWidget* detailPart{};
    GtkWidget* detailComment{};
    GtkWidget* detailHowToImprove{};
    GtkWidget* detailEvidence{};
    GtkWidget* detailVerdict{};
    GtkWidget* detailReviewed{};
    GtkWidget* detailAwarded{};
    GtkWidget* detailMax{};
    GtkWidget* exportButton{};
    GtkWidget* aiWorkflow{};
    GtkWidget* aiRubricButton{};
    GtkWidget* aiRubricLabel{};
    GtkWidget* aiInstructions{};
    GtkWidget* aiRunButton{};
    GtkWidget* aiCancelButton{};
    GtkWidget* aiStatus{};

    xoj::marking::MarkingAiRunner aiRunner;
    std::optional<fs::path> aiRubricPath;
    std::string activeAiSourceSha;
    std::optional<std::string> activeAiDraftSha;
    xoj::marking::AiMarkingWorkflow activeAiWorkflow{xoj::marking::AiMarkingWorkflow::Debox};
    std::optional<xoj::marking::MarkingDocument> marking;
    std::optional<fs::path> manifestPath;
    std::optional<fs::path> boundSourcePdf;
    std::optional<size_t> selectedAnnotation;
    std::optional<size_t> highlightedPage;
    std::unordered_map<std::string, Element*> annotationElements;
    guint recoveryTimeoutId{};
};
