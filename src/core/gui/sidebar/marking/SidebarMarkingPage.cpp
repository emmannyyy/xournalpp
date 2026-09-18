#include "SidebarMarkingPage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glib-object.h>

#include "control/Control.h"
#include "control/ScrollHandler.h"
#include "control/layer/LayerController.h"
#include "control/settings/Settings.h"
#include "control/tools/EditSelection.h"
#include "gui/MainWindow.h"
#include "gui/XournalView.h"
#include "gui/dialog/XojOpenDlg.h"
#include "marking/MarkingXml.h"
#include "model/Document.h"
#include "model/Layer.h"
#include "model/Stroke.h"
#include "model/StrokeStyle.h"
#include "model/XojPage.h"
#include "pdf/base/XojPdfPage.h"
#include "util/PathUtil.h"
#include "util/Util.h"
#include "util/Color.h"
#include "util/glib_casts.h"
#include "util/gtk4_helper.h"
#include "util/i18n.h"
#include "util/raii/GObjectSPtr.h"

using xoj::marking::MarkingAnnotation;
using xoj::marking::MarkingXml;
using xoj::marking::NormalizedBox;
using xoj::marking::ValidationIssue;
using xoj::marking::Verdict;

namespace {

constexpr auto FEEDBACK_LAYER_NAME = "StudySzn feedback (generated)";

GtkWidget* makeLeftLabel(const char* text) {
    GtkWidget* label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_label_set_line_wrap(GTK_LABEL(label), true);
    return label;
}

std::string marksText(double awarded, double maximum) {
    std::ostringstream stream;
    stream << awarded << " / " << maximum;
    return stream.str();
}

const char* verdictGlyph(Verdict verdict) {
    switch (verdict) {
        case Verdict::Correct:
            return "✓";
        case Verdict::Partial:
            return "?";
        case Verdict::Incorrect:
            return "✗";
        case Verdict::Unresolved:
            return "!";
    }
    return "!";
}

void styleAnnotationStroke(Stroke& stroke, Verdict verdict) {
    switch (verdict) {
        case Verdict::Correct:
            stroke.setColor(Colors::green);
            stroke.setLineStyle(StrokeStyle::parseStyle("plain"));
            break;
        case Verdict::Partial:
            stroke.setColor(Colors::xopp_darkorange);
            stroke.setLineStyle(StrokeStyle::parseStyle("dash"));
            break;
        case Verdict::Incorrect:
            stroke.setColor(Colors::red);
            stroke.setLineStyle(StrokeStyle::parseStyle("dashdot"));
            break;
        case Verdict::Unresolved:
            stroke.setColor(Colors::magenta);
            stroke.setLineStyle(StrokeStyle::parseStyle("dot"));
            break;
    }
}

void clearBox(GtkWidget* box) {
#if GTK_MAJOR_VERSION == 3
    GList* children = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList* child = children; child != nullptr; child = child->next) {
        gtk_widget_destroy(GTK_WIDGET(child->data));
    }
    g_list_free(children);
#else
    while (GtkWidget* child = gtk_widget_get_first_child(box)) {
        gtk_box_remove(GTK_BOX(box), child);
    }
#endif
}

std::string sha256File(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Could not read the source PDF");
    }
    GChecksum* checksum = g_checksum_new(G_CHECKSUM_SHA256);
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (const auto count = input.gcount(); count > 0) {
            g_checksum_update(checksum, reinterpret_cast<const guchar*>(buffer.data()), static_cast<gssize>(count));
        }
    }
    const std::string result = g_checksum_get_string(checksum);
    g_checksum_free(checksum);
    return result;
}

void clearMarkingRecoveryPointer() {
    std::error_code error;
    fs::remove(Util::getConfigFile("emergencysave.xoppmark.path"), error);
}

}  // namespace

SidebarMarkingPage::SidebarMarkingPage(Control* control): AbstractSidebarPage(control) {
    buildUi();
    registerListener(control);
    recoveryTimeoutId = g_timeout_add_seconds(
            5,
            [](gpointer data) -> gboolean {
                (void)static_cast<SidebarMarkingPage*>(data)->checkpointRecoveryState();
                return G_SOURCE_CONTINUE;
            },
            this);
}

SidebarMarkingPage::~SidebarMarkingPage() {
    if (recoveryTimeoutId != 0) {
        g_source_remove(recoveryTimeoutId);
    }
    if (root) {
        g_object_unref(root);
    }
}

void SidebarMarkingPage::buildUi() {
    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    g_object_ref(root);
    gtk_widget_set_margin_start(root, 10);
    gtk_widget_set_margin_end(root, 10);
    gtk_widget_set_margin_top(root, 10);
    gtk_widget_set_margin_bottom(root, 10);
    gtk_widget_set_size_request(root, 320, -1);

    headerLabel = makeLeftLabel(_("Teacher marking"));
    gtk_widget_add_css_class(headerLabel, "title");
    gtk_box_append(GTK_BOX(root), headerLabel);

    scoreLabel = makeLeftLabel(_("Import a marking draft to begin."));
    gtk_box_append(GTK_BOX(root), scoreLabel);

    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* importButton = gtk_button_new_with_label(_("Import draft"));
    GtkWidget* addButton = gtk_button_new_with_label(_("Add from selection"));
    GtkWidget* reviewAllButton = gtk_button_new_with_label(_("Mark all feedback reviewed"));
    exportButton = gtk_button_new_with_label(_("Export review"));
    gtk_widget_set_sensitive(exportButton, false);
    gtk_box_append(GTK_BOX(actions), importButton);
    gtk_box_append(GTK_BOX(actions), addButton);
    gtk_box_append(GTK_BOX(actions), reviewAllButton);
    gtk_box_append(GTK_BOX(actions), exportButton);
    gtk_box_append(GTK_BOX(root), actions);

    g_signal_connect(importButton, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer data) {
                         auto* self = static_cast<SidebarMarkingPage*>(data);
                         xoj::OpenDlg::showMultiFormatDialog(
                                 self->control->getGtkWindow(), {"*.xoppmark"},
                                 [self](fs::path path) {
                                     try {
                                         self->importManifest(path, true);
                                     } catch (const std::exception& exception) {
                                         self->showMessage(GTK_MESSAGE_ERROR, _("Could not import marking draft"),
                                                           exception.what());
                                     }
                                 });
                     }),
                     this);
    g_signal_connect(addButton, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<SidebarMarkingPage*>(data)->addAnnotationFromSelection();
                     }),
                     this);
    g_signal_connect(reviewAllButton, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<SidebarMarkingPage*>(data)->markAllReviewed();
                     }),
                     this);
    g_signal_connect(exportButton, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<SidebarMarkingPage*>(data)->showExportDialog();
                     }),
                     this);

    filterCombo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(filterCombo), "all", _("All feedback"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(filterCombo), "correct", _("Correct"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(filterCombo), "partial", _("Partial"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(filterCombo), "incorrect", _("Incorrect"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(filterCombo), "unresolved", _("Unresolved"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(filterCombo), "unreviewed", _("Not yet reviewed"));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(filterCombo), "all");
    gtk_box_append(GTK_BOX(root), filterCombo);
    g_signal_connect(filterCombo, "changed",
                     G_CALLBACK(+[](GtkComboBox*, gpointer data) {
                         static_cast<SidebarMarkingPage*>(data)->rebuildList();
                     }),
                     this);

    listBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, true);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), listBox);
    gtk_box_append(GTK_BOX(root), scroll);

    detailScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(detailScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(detailScroll, true);
    gtk_widget_set_visible(detailScroll, false);
    detailBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(detailScroll), detailBox);
    gtk_box_append(GTK_BOX(root), detailScroll);

    gtk_box_append(GTK_BOX(detailBox), makeLeftLabel(_("Feedback title")));
    detailTitle = gtk_entry_new();
    gtk_box_append(GTK_BOX(detailBox), detailTitle);

    gtk_box_append(GTK_BOX(detailBox), makeLeftLabel(_("Question part")));
    detailPart = gtk_combo_box_text_new();
    gtk_box_append(GTK_BOX(detailBox), detailPart);
    g_signal_connect(detailPart, "changed",
                     G_CALLBACK(+[](GtkComboBox*, gpointer data) {
                         static_cast<SidebarMarkingPage*>(data)->changeSelectedPart();
                     }),
                     this);

    gtk_box_append(GTK_BOX(detailBox), makeLeftLabel(_("AI evidence from the script")));
    detailEvidence = makeLeftLabel("");
    gtk_label_set_line_wrap(GTK_LABEL(detailEvidence), true);
    gtk_widget_add_css_class(detailEvidence, "dim-label");
    gtk_box_append(GTK_BOX(detailBox), detailEvidence);

    gtk_box_append(GTK_BOX(detailBox), makeLeftLabel(_("Comment to student")));
    detailComment = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(detailComment), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_size_request(detailComment, -1, 90);
    gtk_box_append(GTK_BOX(detailBox), detailComment);

    gtk_box_append(GTK_BOX(detailBox), makeLeftLabel(_("How to improve")));
    detailHowToImprove = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(detailHowToImprove), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_size_request(detailHowToImprove, -1, 70);
    gtk_box_append(GTK_BOX(detailBox), detailHowToImprove);

    GtkWidget* verdictRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    detailVerdict = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(detailVerdict), "correct", _("Correct"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(detailVerdict), "partial", _("Partial"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(detailVerdict), "incorrect", _("Incorrect"));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(detailVerdict), "unresolved", _("Unresolved"));
    detailReviewed = gtk_check_button_new_with_label(_("Reviewed"));
    gtk_box_append(GTK_BOX(verdictRow), detailVerdict);
    gtk_box_append(GTK_BOX(verdictRow), detailReviewed);
    gtk_box_append(GTK_BOX(detailBox), verdictRow);

    GtkWidget* marksRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(marksRow), makeLeftLabel(_("Part score")));
    detailAwarded = gtk_spin_button_new_with_range(0.0, 1000.0, 0.5);
    detailMax = gtk_spin_button_new_with_range(0.0, 1000.0, 0.5);
    gtk_box_append(GTK_BOX(marksRow), detailAwarded);
    gtk_box_append(GTK_BOX(marksRow), makeLeftLabel("/"));
    gtk_box_append(GTK_BOX(marksRow), detailMax);
    gtk_box_append(GTK_BOX(detailBox), marksRow);

    GtkWidget* navigationRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* previousButton = gtk_button_new_with_label(_("Previous"));
    GtkWidget* nextButton = gtk_button_new_with_label(_("Next"));
    gtk_widget_set_hexpand(previousButton, true);
    gtk_widget_set_hexpand(nextButton, true);
    gtk_box_append(GTK_BOX(navigationRow), previousButton);
    gtk_box_append(GTK_BOX(navigationRow), nextButton);
    gtk_box_append(GTK_BOX(detailBox), navigationRow);
    g_signal_connect(previousButton, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<SidebarMarkingPage*>(data)->navigateRelative(-1);
                     }),
                     this);
    g_signal_connect(nextButton, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<SidebarMarkingPage*>(data)->navigateRelative(1);
                     }),
                     this);

    GtkWidget* applyButton = gtk_button_new_with_label(_("Save feedback changes"));
    gtk_box_append(GTK_BOX(detailBox), applyButton);
    g_signal_connect(applyButton, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<SidebarMarkingPage*>(data)->applyDetailEdits();
                     }),
                     this);
#if GTK_MAJOR_VERSION == 3
    gtk_widget_show_all(root);
#else
    gtk_widget_set_visible(root, true);
#endif
}

void SidebarMarkingPage::enableSidebar() {}
void SidebarMarkingPage::disableSidebar() {}
void SidebarMarkingPage::layout() {}
std::string SidebarMarkingPage::getName() { return _("Marking"); }
std::string SidebarMarkingPage::getIconName() { return "document-edit-symbolic"; }
bool SidebarMarkingPage::hasData() { return true; }
GtkWidget* SidebarMarkingPage::getWidget() { return root; }

void SidebarMarkingPage::documentChanged(DocumentChangeType type) {
    if (type == DOCUMENT_CHANGE_CLEARED) {
        if (marking) {
            syncAnnotationGeometry();
            if (selectedAnnotation) {
                (void)commitDetailEdits();
            }
            try {
                saveWorkingManifest();
            } catch (const std::exception& exception) {
                showMessage(GTK_MESSAGE_ERROR, _("Could not save marking edits"), exception.what());
            }
        }
        selectedAnnotation.reset();
        highlightedPage.reset();
        annotationElements.clear();
        gtk_widget_set_visible(detailScroll, false);
    } else if (type == DOCUMENT_CHANGE_COMPLETE && marking && boundSourcePdf) {
        std::error_code sourceError;
        std::error_code documentError;
        const auto expected = fs::weakly_canonical(*boundSourcePdf, sourceError);
        const auto actual = fs::weakly_canonical(control->getDocument()->getPdfFilepath(), documentError);
        if (sourceError || documentError || expected != actual) {
            clearMarkingRecoveryPointer();
            marking.reset();
            manifestPath.reset();
            boundSourcePdf.reset();
            selectedAnnotation.reset();
            highlightedPage.reset();
            annotationElements.clear();
            gtk_widget_set_visible(detailScroll, false);
            updateHeader();
            rebuildList();
        }
    }
}

void SidebarMarkingPage::openManifest(const fs::path& path) { importManifest(path, true); }

void SidebarMarkingPage::restoreManifest(const fs::path& path) { importManifest(path, false); }

bool SidebarMarkingPage::passesFilter(const MarkingAnnotation& annotation) const {
    const char* active = gtk_combo_box_get_active_id(GTK_COMBO_BOX(filterCombo));
    if (active == nullptr || std::string_view(active) == "all") {
        return true;
    }
    if (std::string_view(active) == "unreviewed") {
        return !annotation.reviewed;
    }
    return std::string_view(active) == xoj::marking::toString(annotation.verdict);
}

void SidebarMarkingPage::rebuildList() {
    clearBox(listBox);
    if (!marking) {
        return;
    }

    std::vector<size_t> orderedIndices(marking->annotations.size());
    std::iota(orderedIndices.begin(), orderedIndices.end(), 0);
    std::stable_sort(orderedIndices.begin(), orderedIndices.end(), [this](size_t left, size_t right) {
        const auto& a = marking->annotations[left];
        const auto& b = marking->annotations[right];
        return a.page == b.page ? left < right : a.page < b.page;
    });
    size_t displayOrdinal = 0;
    for (const size_t index: orderedIndices) {
        const auto& annotation = marking->annotations[index];
        if (!passesFilter(annotation)) {
            continue;
        }
        ++displayOrdinal;
        const auto part = std::find_if(marking->parts.begin(), marking->parts.end(),
                                       [&annotation](const auto& candidate) {
                                           return candidate.id == annotation.partId;
                                       });
        std::ostringstream text;
        text << verdictGlyph(annotation.verdict) << "  " << displayOrdinal << ". "
             << (part == marking->parts.end() ? annotation.partId : part->label);
        if (!annotation.title.empty()) {
            text << "\n" << annotation.title;
        }
        if (!annotation.comment.empty()) {
            constexpr size_t PREVIEW_LENGTH = 90;
            text << "\n"
                 << annotation.comment.substr(0, PREVIEW_LENGTH)
                 << (annotation.comment.size() > PREVIEW_LENGTH ? "…" : "");
        }
        text << "\nPage " << annotation.page;
        if (annotation.reviewed) {
            text << "   Reviewed";
        }

        GtkWidget* button = gtk_button_new_with_label(text.str().c_str());
        gtk_widget_set_hexpand(button, true);
        if (selectedAnnotation && *selectedAnnotation == index) {
            gtk_widget_add_css_class(button, "suggested-action");
        }
#if GTK_MAJOR_VERSION == 3
        auto* child = gtk_bin_get_child(GTK_BIN(button));
#else
        auto* child = gtk_button_get_child(GTK_BUTTON(button));
#endif
        if (GTK_IS_LABEL(child)) {
            gtk_label_set_xalign(GTK_LABEL(child), 0.0F);
            gtk_label_set_line_wrap(GTK_LABEL(child), true);
        }
        gtk_box_append(GTK_BOX(listBox), button);
#if GTK_MAJOR_VERSION == 3
        gtk_widget_show(button);
#endif
        g_signal_connect_data(
                button, "clicked",
                G_CALLBACK(+[](GtkButton*, gpointer data) {
                    auto* context = static_cast<RowContext*>(data);
                    context->page->showAnnotation(context->index);
                    context->page->navigateToAnnotation(context->index);
                }),
                new RowContext{this, index}, xoj::util::closure_notify_cb<RowContext>, GConnectFlags(0));
    }
}

void SidebarMarkingPage::updateHeader() {
    if (!marking) {
        gtk_label_set_text(GTK_LABEL(headerLabel), _("Teacher marking"));
        gtk_label_set_text(GTK_LABEL(scoreLabel), _("Import a marking draft to begin."));
        gtk_widget_set_sensitive(exportButton, false);
        return;
    }

    const std::string heading = marking->student.empty() ? marking->title : marking->title + " — " + marking->student;
    gtk_label_set_text(GTK_LABEL(headerLabel), heading.c_str());

    std::ostringstream status;
    status << marksText(marking->awardedMarks(), marking->maxMarks()) << "   " << marking->reviewedCount() << " / "
           << marking->annotations.size() << " reviewed";
    const auto issues = marking->validate(std::nullopt, false);
    const auto errors = std::count_if(issues.begin(), issues.end(), [](const auto& issue) {
        return issue.severity == ValidationIssue::Severity::Error;
    });
    if (errors > 0) {
        status << "   " << errors << " errors";
    }
    gtk_label_set_text(GTK_LABEL(scoreLabel), status.str().c_str());
    gtk_widget_set_sensitive(exportButton, true);
}

void SidebarMarkingPage::showAnnotation(size_t index) {
    if (!marking || index >= marking->annotations.size()) {
        return;
    }
    if (selectedAnnotation && *selectedAnnotation != index) {
        if (!commitDetailEdits()) {
            return;
        }
        try {
            saveWorkingManifest();
        } catch (const std::exception& exception) {
            showMessage(GTK_MESSAGE_ERROR, _("Could not save marking edits"), exception.what());
        }
        updateHeader();
    }
    selectedAnnotation = index;
    const auto& annotation = marking->annotations[index];
    gtk_entry_set_text(GTK_ENTRY(detailTitle), annotation.title.c_str());
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(detailPart), annotation.partId.c_str());
    gtk_label_set_text(GTK_LABEL(detailEvidence),
                       annotation.evidenceFromScript.empty() ? _("No AI evidence was supplied.")
                                                             : annotation.evidenceFromScript.c_str());
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(detailComment)), annotation.comment.c_str(), -1);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(detailHowToImprove)),
                             annotation.howToImprove.c_str(), -1);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(detailVerdict), xoj::marking::toString(annotation.verdict));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(detailReviewed), annotation.reviewed);
    const auto part = std::find_if(marking->parts.begin(), marking->parts.end(),
                                   [&annotation](const auto& candidate) {
                                       return candidate.id == annotation.partId;
                                   });
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(detailAwarded),
                              part == marking->parts.end() ? 0.0 : part->awardedMarks);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(detailMax), part == marking->parts.end() ? 0.0 : part->maxMarks);
    gtk_widget_set_visible(detailScroll, true);
    g_idle_add(
            [](gpointer data) -> gboolean {
                static_cast<SidebarMarkingPage*>(data)->rebuildList();
                return G_SOURCE_REMOVE;
            },
            this);
}

void SidebarMarkingPage::navigateToAnnotation(size_t index) {
    if (!marking || index >= marking->annotations.size()) {
        return;
    }
    syncAnnotationGeometry();
    const auto& annotation = marking->annotations[index];
    Document* document = control->getDocument();
    if (annotation.page == 0 || annotation.page > document->getPageCount()) {
        return;
    }

    const auto previousHighlightedPage = highlightedPage;
    document->lock();
    for (const auto& [id, element]: annotationElements) {
        if (auto* stroke = dynamic_cast<Stroke*>(element)) {
            stroke->setWidth(id == annotation.id ? 4.0 : 1.8);
        }
    }
    highlightedPage = annotation.page - 1;
    document->unlock();
    if (previousHighlightedPage && *previousHighlightedPage < document->getPageCount() &&
        *previousHighlightedPage != *highlightedPage) {
        control->getWindow()->getXournal()->layerChanged(*previousHighlightedPage);
    }
    control->getWindow()->getXournal()->layerChanged(*highlightedPage);

    document->lock_shared();
    const PageRef page = document->getPage(annotation.page - 1);
    const double width = page->getWidth();
    const double height = page->getHeight();
    document->unlock_shared();

    const auto& box = annotation.box;
    control->getScrollHandler()->jumpToPage(annotation.page - 1,
                                            {box.x0 * width / 1000.0, box.y0 * height / 1000.0,
                                             box.x1 * width / 1000.0, box.y1 * height / 1000.0});
}

void SidebarMarkingPage::changeSelectedPart() {
    if (!marking || !selectedAnnotation || *selectedAnnotation >= marking->annotations.size()) {
        return;
    }
    const char* selectedPartId = gtk_combo_box_get_active_id(GTK_COMBO_BOX(detailPart));
    if (selectedPartId == nullptr) {
        return;
    }
    auto& annotation = marking->annotations[*selectedAnnotation];
    if (annotation.partId == selectedPartId) {
        return;
    }

    // The score controls still belong to the annotation's previous part. Persist
    // those values before changing the relationship, then display the new part's
    // authoritative score instead of copying the old score into it.
    if (!marking->updatePartMarks(annotation.partId, gtk_spin_button_get_value(GTK_SPIN_BUTTON(detailAwarded)),
                                  gtk_spin_button_get_value(GTK_SPIN_BUTTON(detailMax)))) {
        showMessage(GTK_MESSAGE_ERROR, _("Invalid part score"),
                    _("Awarded marks must be between zero and the maximum marks."));
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(detailPart), annotation.partId.c_str());
        return;
    }
    annotation.partId = selectedPartId;
    const auto part = std::find_if(marking->parts.begin(), marking->parts.end(),
                                   [&annotation](const auto& candidate) {
                                       return candidate.id == annotation.partId;
                                   });
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(detailAwarded),
                              part == marking->parts.end() ? 0.0 : part->awardedMarks);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(detailMax), part == marking->parts.end() ? 0.0 : part->maxMarks);
}

bool SidebarMarkingPage::commitDetailEdits() {
    if (!marking || !selectedAnnotation || *selectedAnnotation >= marking->annotations.size()) {
        return true;
    }
    auto& annotation = marking->annotations[*selectedAnnotation];
    annotation.title = gtk_entry_get_text(GTK_ENTRY(detailTitle));
    const char* partId = gtk_combo_box_get_active_id(GTK_COMBO_BOX(detailPart));
    if (partId) {
        annotation.partId = partId;
    }

    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(detailComment));
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar* text = gtk_text_buffer_get_text(buffer, &start, &end, false);
    annotation.comment = text;
    g_free(text);

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(detailHowToImprove));
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    text = gtk_text_buffer_get_text(buffer, &start, &end, false);
    annotation.howToImprove = text;
    g_free(text);

    const char* verdict = gtk_combo_box_get_active_id(GTK_COMBO_BOX(detailVerdict));
    annotation.verdict = verdict ? xoj::marking::verdictFromString(verdict).value_or(Verdict::Unresolved)
                                 : Verdict::Unresolved;
    annotation.reviewed = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(detailReviewed));
    if (!marking->updatePartMarks(annotation.partId, gtk_spin_button_get_value(GTK_SPIN_BUTTON(detailAwarded)),
                                  gtk_spin_button_get_value(GTK_SPIN_BUTTON(detailMax)))) {
        showMessage(GTK_MESSAGE_ERROR, _("Invalid part score"),
                    _("Awarded marks must be between zero and the maximum marks."));
        return false;
    }
    if (const auto mapped = annotationElements.find(annotation.id); mapped != annotationElements.end()) {
        Document* document = control->getDocument();
        document->lock();
        bool found = false;
        if (annotation.page > 0 && annotation.page <= document->getPageCount()) {
            const auto page = document->getPage(annotation.page - 1);
            for (auto* layer: page->getLayers()) {
                for (auto& candidate: layer->getElements()) {
                    if (candidate.get() == mapped->second) {
                        if (auto* stroke = dynamic_cast<Stroke*>(candidate.get())) {
                            styleAnnotationStroke(*stroke, annotation.verdict);
                        }
                        found = true;
                        break;
                    }
                }
                if (found) {
                    break;
                }
            }
        }
        document->unlock();
        if (found) {
            control->getWindow()->getXournal()->layerChanged(annotation.page - 1);
        } else {
            annotationElements.erase(mapped);
        }
    }
    return true;
}

void SidebarMarkingPage::applyDetailEdits() {
    if (!commitDetailEdits()) {
        return;
    }
    try {
        saveWorkingManifest();
    } catch (const std::exception& exception) {
        showMessage(GTK_MESSAGE_ERROR, _("Could not save marking edits"), exception.what());
    }
    updateHeader();
    rebuildList();
}

void SidebarMarkingPage::saveWorkingManifest() {
    if (marking && manifestPath) {
        MarkingXml::save(*marking, *manifestPath);
        const auto recoveryPointer = Util::getConfigFile("emergencysave.xoppmark.path");
        std::ofstream pointer(recoveryPointer, std::ios::binary | std::ios::trunc);
        if (!pointer) {
            throw std::runtime_error("Could not create the marking recovery pointer");
        }
        pointer << fs::absolute(*manifestPath).string();
        if (!pointer) {
            throw std::runtime_error("Could not write the marking recovery pointer");
        }
    }
}

bool SidebarMarkingPage::checkpointRecoveryState(bool reportFailure) {
    if (!marking || !manifestPath) {
        return true;
    }
    try {
        if (selectedAnnotation && !commitDetailEdits()) {
            return false;
        }
        syncAnnotationGeometry();
        saveWorkingManifest();
        return true;
    } catch (const std::exception& exception) {
        g_warning("Could not checkpoint structured marking state: %s", exception.what());
        if (reportFailure) {
            showMessage(GTK_MESSAGE_ERROR, _("Could not preserve the current marking draft"),
                        _("The new draft was not opened. Fix the current manifest's permissions or choose a writable "
                          "location, then try again."));
        }
        return false;
    }
}

void SidebarMarkingPage::navigateRelative(int direction) {
    if (!marking || marking->annotations.empty()) {
        return;
    }
    std::vector<size_t> visible;
    visible.reserve(marking->annotations.size());
    for (size_t index = 0; index < marking->annotations.size(); ++index) {
        if (passesFilter(marking->annotations[index])) {
            visible.push_back(index);
        }
    }
    if (visible.empty()) {
        return;
    }
    const auto current = selectedAnnotation ? std::find(visible.begin(), visible.end(), *selectedAnnotation)
                                            : visible.end();
    size_t position = current == visible.end() ? 0U : static_cast<size_t>(std::distance(visible.begin(), current));
    if (current != visible.end()) {
        position = direction < 0 ? (position + visible.size() - 1) % visible.size()
                                 : (position + 1) % visible.size();
    }
    const size_t next = visible[position];
    showAnnotation(next);
    navigateToAnnotation(next);
}

void SidebarMarkingPage::markAllReviewed() {
    if (!marking) {
        return;
    }
    if (!commitDetailEdits()) {
        return;
    }
    for (auto& annotation: marking->annotations) {
        annotation.reviewed = true;
    }
    if (selectedAnnotation) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(detailReviewed), true);
    }
    try {
        saveWorkingManifest();
    } catch (const std::exception& exception) {
        showMessage(GTK_MESSAGE_ERROR, _("Could not save marking edits"), exception.what());
    }
    updateHeader();
    rebuildList();
}

void SidebarMarkingPage::addAnnotationFromSelection() {
    if (!marking) {
        showMessage(GTK_MESSAGE_INFO, _("Import a marking draft first"),
                    _("A marking draft provides the assignment and scoring context."));
        return;
    }
    EditSelection* selection = control->getWindow()->getXournal()->getSelection();
    const size_t pageNumber = control->getCurrentPageNo();
    if (!selection || pageNumber == npos) {
        showMessage(GTK_MESSAGE_INFO, _("Select evidence first"),
                    _("Draw or select a rectangle around the student's evidence, then choose Add from selection."));
        return;
    }

    const auto rect = selection->getSnappedBounds();
    const PageRef page = control->getCurrentPage();
    const double width = page->getWidth();
    const double height = page->getHeight();
    const auto clamp = [](double value) { return std::clamp(static_cast<int>(std::lround(value)), 0, 1000); };
    NormalizedBox box{
            clamp(rect.y / height * 1000.0),
            clamp(rect.x / width * 1000.0),
            clamp((rect.y + rect.height) / height * 1000.0),
            clamp((rect.x + rect.width) / width * 1000.0),
    };
    if (!box.isValid() || box.isNearFullPage()) {
        showMessage(GTK_MESSAGE_ERROR, _("Selection is not usable"),
                    _("Choose a tight, non-empty region around one piece of evidence."));
        return;
    }

    if (marking->parts.empty()) {
        showMessage(GTK_MESSAGE_ERROR, _("No question parts are available"),
                    _("The marking draft must define at least one question part before feedback can be added."));
        return;
    }
    const std::string partId =
            selectedAnnotation ? marking->annotations[*selectedAnnotation].partId : marking->parts.front().id;
    MarkingAnnotation annotation;
    size_t idNumber = marking->annotations.size() + 1;
    do {
        annotation.id = "a" + std::to_string(idNumber++);
    } while (std::any_of(marking->annotations.begin(), marking->annotations.end(),
                         [&annotation](const auto& existing) { return existing.id == annotation.id; }));
    annotation.page = pageNumber + 1;
    annotation.partId = partId;
    annotation.verdict = Verdict::Partial;
    annotation.box = box;
    annotation.title = _("New feedback");
    annotation.comment = _("Explain what is correct and what you should improve.");
    annotation.source = "teacher";
    if (marking->mode == xoj::marking::MarkingMode::Debox) {
        annotation.diagramAnchor = xoj::marking::DiagramAnchor{"teacher-selection-" + annotation.id};
    }
    syncAnnotationGeometry();
    marking->annotations.push_back(std::move(annotation));
    materializeAnnotations();

    updateHeader();
    rebuildList();
    showAnnotation(marking->annotations.size() - 1);
}

void SidebarMarkingPage::importManifest(const fs::path& path, bool allowSourcePdfSwitch) {
    xoj::marking::MarkingDocument draft = MarkingXml::load(path);
    const auto issues = draft.validate(std::nullopt, false);
    std::ostringstream validationErrors;
    for (const auto& issue: issues) {
        if (issue.severity == ValidationIssue::Severity::Error) {
            validationErrors << issue.location << ": " << issue.message << "\n";
        }
    }
    if (!validationErrors.str().empty()) {
        throw std::runtime_error("The marking draft needs correction before it can be opened:\n" +
                                 validationErrors.str());
    }

    const fs::path sourceReference(draft.sourcePdf);
    if (sourceReference.empty() || sourceReference.is_absolute() ||
        std::find(sourceReference.begin(), sourceReference.end(), fs::path("..")) != sourceReference.end()) {
        throw std::runtime_error(
                "The marking draft must reference a PDF in its own folder. Absolute and parent paths are not allowed.");
    }
    const fs::path sourcePath = fs::absolute(path.parent_path() / sourceReference);
    if (!fs::exists(sourcePath)) {
        throw std::runtime_error(
                "Source PDF not found. Put the PDF beside the .xoppmark file, then import the draft again.");
    }
    if (!draft.sourceSha256.empty() &&
        g_ascii_strcasecmp(sha256File(sourcePath).c_str(), draft.sourceSha256.c_str()) != 0) {
        throw std::runtime_error(
                "The PDF beside this draft is not the student script it was created for. Choose the matching files.");
    }

    std::error_code expectedError;
    std::error_code actualError;
    const auto expected = fs::weakly_canonical(sourcePath, expectedError);
    const auto currentPath = control->getDocument()->getPdfFilepath();
    const auto actual = currentPath.empty() ? fs::path() : fs::weakly_canonical(currentPath, actualError);
    if (!expectedError && !actualError && !actual.empty() && expected == actual) {
        activateManifest(std::move(draft), path, sourcePath);
        materializeAnnotations();
        return;
    }
    if (!allowSourcePdfSwitch) {
        throw std::runtime_error(
                "The saved marking draft belongs to a different PDF, so it was not attached to the recovered document.");
    }

    // Keep the current draft authoritative until the PDF switch has actually
    // succeeded. Cancelling the document-close prompt must leave both the old
    // PDF and its marking state intact.
    if (!checkpointRecoveryState(true)) {
        return;
    }
    control->openMarkingSourcePdf(
            sourcePath, [this, draft = std::move(draft), path, sourcePath](bool opened) mutable {
        if (opened) {
            activateManifest(std::move(draft), path, sourcePath);
            materializeAnnotations();
        } else {
            showMessage(GTK_MESSAGE_ERROR, _("Source PDF was not opened"),
                        _("The current draft is unchanged. Save any open document, then import the new draft again."));
        }
    });
}

void SidebarMarkingPage::activateManifest(xoj::marking::MarkingDocument draft, const fs::path& path,
                                          const fs::path& sourcePath) {
    annotationElements.clear();
    marking = std::move(draft);
    manifestPath = path;
    boundSourcePdf = sourcePath;
    selectedAnnotation.reset();
    highlightedPage.reset();
    gtk_widget_set_visible(detailScroll, false);

    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(detailPart));
    for (const auto& part: marking->parts) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(detailPart), part.id.c_str(), part.label.c_str());
    }
    updateHeader();
    rebuildList();
}

void SidebarMarkingPage::materializeAnnotations() {
    annotationElements.clear();
    highlightedPage.reset();
    if (!marking) {
        return;
    }

    Document* document = control->getDocument();
    for (size_t pageIndex = 0; pageIndex < document->getPageCount(); ++pageIndex) {
        document->lock();
        PageRef page = document->getPage(pageIndex);
        const double width = page->getWidth();
        const double height = page->getHeight();
        Layer* previousLayer = nullptr;
        for (auto* candidate: page->getLayers()) {
            if (candidate->getName() == FEEDBACK_LAYER_NAME) {
                previousLayer = candidate;
                break;
            }
        }
        document->unlock();

        if (previousLayer) {
            control->getLayerController()->removeLayer(page, previousLayer);
            delete previousLayer;
        }

        auto layer = std::make_unique<Layer>();
        layer->setName(FEEDBACK_LAYER_NAME);
        for (auto& annotation: marking->annotations) {
            if (annotation.page != pageIndex + 1 || !annotation.box.isValid()) {
                continue;
            }
            const double x0 = annotation.box.x0 * width / 1000.0;
            const double y0 = annotation.box.y0 * height / 1000.0;
            const double x1 = annotation.box.x1 * width / 1000.0;
            const double y1 = annotation.box.y1 * height / 1000.0;

            auto stroke = std::make_unique<Stroke>();
            stroke->setToolType(StrokeTool::PEN);
            stroke->setWidth(1.8);
            styleAnnotationStroke(*stroke, annotation.verdict);
            stroke->addPoint(Point(x0, y0));
            stroke->addPoint(Point(x1, y0));
            stroke->addPoint(Point(x1, y1));
            stroke->addPoint(Point(x0, y1));
            stroke->addPoint(Point(x0, y0));
            stroke->freeUnusedPointItems();
            annotationElements[annotation.id] = stroke.get();
            layer->addElement(std::move(stroke));
        }

        if (!layer->getElements().empty()) {
            const auto position = page->getLayerCount();
            Layer* insertedLayer = layer.release();
            control->getLayerController()->insertLayer(page, insertedLayer, position);
        }
    }
}

void SidebarMarkingPage::syncAnnotationGeometry() {
    if (!marking) {
        return;
    }
    Document* document = control->getDocument();
    for (auto& annotation: marking->annotations) {
        const auto element = annotationElements.find(annotation.id);
        if (element == annotationElements.end() || annotation.page == 0 ||
            annotation.page > document->getPageCount()) {
            continue;
        }
        document->lock_shared();
        const PageRef page = document->getPage(annotation.page - 1);
        bool elementStillExists = false;
        for (const auto* layer: page->getLayersView()) {
            for (const auto* candidate: layer->getElementsView()) {
                if (candidate == element->second) {
                    elementStillExists = true;
                    break;
                }
            }
            if (elementStillExists) {
                break;
            }
        }
        if (!elementStillExists) {
            document->unlock_shared();
            annotationElements.erase(element);
            continue;
        }
        const double width = page->getWidth();
        const double height = page->getHeight();
        const auto bounds = element->second->getSnappedBounds();
        document->unlock_shared();
        const auto clamp = [](double value) { return std::clamp(static_cast<int>(std::lround(value)), 0, 1000); };
        annotation.box = {
                clamp(bounds.y / height * 1000.0),
                clamp(bounds.x / width * 1000.0),
                clamp((bounds.y + bounds.height) / height * 1000.0),
                clamp((bounds.x + bounds.width) / width * 1000.0),
        };
    }
}

void SidebarMarkingPage::showExportDialog() {
    GtkWidget* dialog = gtk_file_chooser_dialog_new(_("Export marking review"), control->getGtkWindow(),
                                                    GTK_FILE_CHOOSER_ACTION_SAVE, _("_Cancel"), GTK_RESPONSE_CANCEL,
                                                    _("_Export"), GTK_RESPONSE_OK, nullptr);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), true);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "reviewed.xoppmark");
    g_signal_connect(
            dialog, "response",
            G_CALLBACK(+[](GtkDialog* dialog, int response, gpointer data) {
                auto* self = static_cast<SidebarMarkingPage*>(data);
                if (response == GTK_RESPONSE_OK) {
                    auto file = xoj::util::GObjectSPtr<GFile>(
                            gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog)), xoj::util::adopt);
                    const fs::path path = Util::fromGFile(file.get());
                    try {
                        self->exportManifest(path);
                    } catch (const std::exception& exception) {
                        self->showMessage(GTK_MESSAGE_ERROR, _("Could not export marking review"), exception.what());
                    }
                }
                gtk_window_close(GTK_WINDOW(dialog));
            }),
            this);
    gtk_window_present(GTK_WINDOW(dialog));
}

void SidebarMarkingPage::exportManifest(const fs::path& path) {
    if (!marking) {
        return;
    }
    if (!commitDetailEdits()) {
        throw std::runtime_error("Awarded marks must be between zero and the maximum marks.");
    }
    syncAnnotationGeometry();
    const size_t pageCount = control->getDocument()->getPageCount();
    const auto issues = marking->validate(pageCount == 0 ? std::nullopt : std::optional<size_t>(pageCount), true);
    std::ostringstream errors;
    for (const auto& issue: issues) {
        if (issue.severity == ValidationIssue::Severity::Error) {
            errors << "• " << issue.location << ": " << issue.message << "\n";
        }
    }
    if (!errors.str().empty()) {
        throw std::runtime_error("Resolve these items before exporting:\n" + errors.str());
    }
    if (boundSourcePdf) {
        const auto destinationDirectory = fs::absolute(path).parent_path();
        const auto source = fs::absolute(*boundSourcePdf);
        const auto portableSource = destinationDirectory / source.filename();
        std::error_code error;
        if (source != portableSource) {
            if (fs::exists(portableSource)) {
                if (!fs::equivalent(source, portableSource, error) || error) {
                    throw std::runtime_error(
                            _("The export folder already contains a different PDF with the same name. Choose another "
                              "folder so the review can remain portable."));
                }
            } else {
                fs::copy_file(source, portableSource, fs::copy_options::none, error);
                if (error) {
                    throw std::runtime_error(
                            _("Could not copy the source PDF beside the exported review. Choose a writable folder."));
                }
            }
        }
        marking->sourcePdf = portableSource.filename().string();
        marking->sourceSha256 = sha256File(source);
    }
    MarkingXml::save(*marking, path);
    manifestPath = path;
    showMessage(GTK_MESSAGE_INFO, _("Marking review exported"),
                _("The revised marking manifest is ready for re-import or a future sync adapter."));
}

void SidebarMarkingPage::showMessage(GtkMessageType type, const std::string& title, const std::string& message) {
    GtkWidget* dialog = gtk_message_dialog_new(control->getGtkWindow(), GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s",
                                               title.c_str());
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", message.c_str());
    g_signal_connect(dialog, "response", G_CALLBACK(+[](GtkDialog* dialog, int, gpointer) {
                         gtk_window_close(GTK_WINDOW(dialog));
                     }),
                     nullptr);
    gtk_window_present(GTK_WINDOW(dialog));
}
