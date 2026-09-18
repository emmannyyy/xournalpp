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
    void applyDetailEdits();
    void addAnnotationFromSelection();
    void importManifest(const fs::path& path);
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
    GtkWidget* detailBox{};
    GtkWidget* detailTitle{};
    GtkWidget* detailPart{};
    GtkWidget* detailComment{};
    GtkWidget* detailHowToImprove{};
    GtkWidget* detailVerdict{};
    GtkWidget* detailReviewed{};
    GtkWidget* detailAwarded{};
    GtkWidget* detailMax{};
    GtkWidget* exportButton{};

    std::optional<xoj::marking::MarkingDocument> marking;
    std::optional<fs::path> manifestPath;
    std::optional<fs::path> boundSourcePdf;
    std::optional<size_t> selectedAnnotation;
    std::unordered_map<std::string, Element*> annotationElements;
};
