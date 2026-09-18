#include "SidebarMarkingPage.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
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
#include "undo/GroupUndoAction.h"
#include "undo/InsertLayerUndoAction.h"
#include "undo/RemoveLayerUndoAction.h"
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

}  // namespace

SidebarMarkingPage::SidebarMarkingPage(Control* control): AbstractSidebarPage(control) {
    buildUi();
    registerListener(control);
}

SidebarMarkingPage::~SidebarMarkingPage() {
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

    headerLabel = makeLeftLabel(_("Teacher marking"));
    gtk_widget_add_css_class(headerLabel, "title");
    gtk_box_append(GTK_BOX(root), headerLabel);

    scoreLabel = makeLeftLabel(_("Import a marking draft to begin."));
    gtk_box_append(GTK_BOX(root), scoreLabel);

    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* importButton = gtk_button_new_with_label(_("Import draft"));
    GtkWidget* addButton = gtk_button_new_with_label(_("Add from selection"));
    exportButton = gtk_button_new_with_label(_("Export review"));
    gtk_widget_set_sensitive(exportButton, false);
    gtk_box_append(GTK_BOX(actions), importButton);
    gtk_box_append(GTK_BOX(actions), addButton);
    gtk_box_append(GTK_BOX(actions), exportButton);
    gtk_box_append(GTK_BOX(root), actions);

    g_signal_connect(importButton, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer data) {
                         auto* self = static_cast<SidebarMarkingPage*>(data);
                         xoj::OpenDlg::showMultiFormatDialog(
                                 self->control->getGtkWindow(), {"*.xoppmark"},
                                 [self](fs::path path) {
                                     try {
                                         self->importManifest(path);
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

    detailBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_visible(detailBox, false);
    gtk_box_append(GTK_BOX(root), detailBox);

    gtk_box_append(GTK_BOX(detailBox), makeLeftLabel(_("Feedback title")));
    detailTitle = gtk_entry_new();
    gtk_box_append(GTK_BOX(detailBox), detailTitle);

    gtk_box_append(GTK_BOX(detailBox), makeLeftLabel(_("Question part")));
    detailPart = gtk_entry_new();
    gtk_box_append(GTK_BOX(detailBox), detailPart);

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
    gtk_box_append(GTK_BOX(marksRow), makeLeftLabel(_("Marks")));
    detailAwarded = gtk_spin_button_new_with_range(0.0, 1000.0, 0.5);
    detailMax = gtk_spin_button_new_with_range(0.0, 1000.0, 0.5);
    gtk_box_append(GTK_BOX(marksRow), detailAwarded);
    gtk_box_append(GTK_BOX(marksRow), makeLeftLabel("/"));
    gtk_box_append(GTK_BOX(marksRow), detailMax);
    gtk_box_append(GTK_BOX(detailBox), marksRow);

    GtkWidget* applyButton = gtk_button_new_with_label(_("Apply changes"));
    gtk_box_append(GTK_BOX(detailBox), applyButton);
    g_signal_connect(applyButton, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<SidebarMarkingPage*>(data)->applyDetailEdits();
                     }),
                     this);
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
        selectedAnnotation.reset();
        annotationElements.clear();
        gtk_widget_set_visible(detailBox, false);
    } else if (type == DOCUMENT_CHANGE_COMPLETE && marking && boundSourcePdf) {
        std::error_code sourceError;
        std::error_code documentError;
        const auto expected = fs::weakly_canonical(*boundSourcePdf, sourceError);
        const auto actual = fs::weakly_canonical(control->getDocument()->getPdfFilepath(), documentError);
        if (sourceError || documentError || expected != actual) {
            marking.reset();
            manifestPath.reset();
            boundSourcePdf.reset();
            selectedAnnotation.reset();
            annotationElements.clear();
            gtk_widget_set_visible(detailBox, false);
            updateHeader();
            rebuildList();
        }
    }
}

void SidebarMarkingPage::openManifest(const fs::path& path) { importManifest(path); }

bool SidebarMarkingPage::passesFilter(const MarkingAnnotation& annotation) const {
    const char* active = gtk_combo_box_get_active_id(GTK_COMBO_BOX(filterCombo));
    if (active == nullptr || std::string_view(active) == "all") {
        return true;
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
        ++displayOrdinal;
        const auto& annotation = marking->annotations[index];
        if (!passesFilter(annotation)) {
            continue;
        }
        std::ostringstream text;
        text << verdictGlyph(annotation.verdict) << "  " << displayOrdinal << ". " << annotation.partId;
        if (!annotation.title.empty()) {
            text << "\n" << annotation.title;
        }
        text << "\nPage " << annotation.page;
        if (annotation.maxMarks > 0.0) {
            text << "   " << marksText(annotation.awardedMarks, annotation.maxMarks);
        }
        if (annotation.reviewed) {
            text << "   Reviewed";
        }

        GtkWidget* button = gtk_button_new_with_label(text.str().c_str());
        gtk_widget_set_hexpand(button, true);
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
    selectedAnnotation = index;
    const auto& annotation = marking->annotations[index];
    gtk_entry_set_text(GTK_ENTRY(detailTitle), annotation.title.c_str());
    gtk_entry_set_text(GTK_ENTRY(detailPart), annotation.partId.c_str());
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(detailComment)), annotation.comment.c_str(), -1);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(detailHowToImprove)),
                             annotation.howToImprove.c_str(), -1);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(detailVerdict), xoj::marking::toString(annotation.verdict));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(detailReviewed), annotation.reviewed);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(detailAwarded), annotation.awardedMarks);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(detailMax), annotation.maxMarks);
    gtk_widget_set_visible(detailBox, true);
}

void SidebarMarkingPage::navigateToAnnotation(size_t index) {
    if (!marking || index >= marking->annotations.size()) {
        return;
    }
    const auto& annotation = marking->annotations[index];
    Document* document = control->getDocument();
    if (annotation.page == 0 || annotation.page > document->getPageCount()) {
        return;
    }

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

void SidebarMarkingPage::applyDetailEdits() {
    if (!marking || !selectedAnnotation || *selectedAnnotation >= marking->annotations.size()) {
        return;
    }
    auto& annotation = marking->annotations[*selectedAnnotation];
    annotation.title = gtk_entry_get_text(GTK_ENTRY(detailTitle));
    annotation.partId = gtk_entry_get_text(GTK_ENTRY(detailPart));

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
    annotation.awardedMarks = gtk_spin_button_get_value(GTK_SPIN_BUTTON(detailAwarded));
    annotation.maxMarks = gtk_spin_button_get_value(GTK_SPIN_BUTTON(detailMax));
    if (marking->mode == xoj::marking::MarkingMode::PageBoxes && annotation.maxMarks > 0.0) {
        marking->updatePartMarks(annotation.partId, annotation.awardedMarks, annotation.maxMarks);
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

    const auto rect = selection->getRect();
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

    const std::string partId =
            selectedAnnotation ? marking->annotations[*selectedAnnotation].partId : std::string("unassigned");
    MarkingAnnotation annotation;
    annotation.id = "a" + std::to_string(marking->annotations.size() + 1);
    annotation.page = pageNumber + 1;
    annotation.partId = partId;
    annotation.verdict = Verdict::Partial;
    annotation.box = box;
    annotation.title = _("New feedback");
    annotation.comment = _("Explain what is correct and what you should improve.");
    annotation.source = "teacher";
    if (marking->mode == xoj::marking::MarkingMode::Debox) {
        annotation.diagramAnchor = xoj::marking::DiagramAnchor{"teacher-selection-" + annotation.id};
    } else {
        const auto part = std::find_if(marking->parts.begin(), marking->parts.end(),
                                       [&annotation](const auto& candidate) {
                                           return candidate.id == annotation.partId;
                                       });
        if (part != marking->parts.end()) {
            annotation.awardedMarks = part->awardedMarks;
            annotation.maxMarks = part->maxMarks;
        }
    }
    syncAnnotationGeometry();
    marking->annotations.push_back(std::move(annotation));
    materializeAnnotations();

    updateHeader();
    rebuildList();
    showAnnotation(marking->annotations.size() - 1);
}

void SidebarMarkingPage::importManifest(const fs::path& path) {
    annotationElements.clear();
    marking = MarkingXml::load(path);
    manifestPath = path;
    boundSourcePdf.reset();
    selectedAnnotation.reset();
    gtk_widget_set_visible(detailBox, false);
    updateHeader();
    rebuildList();

    if (!marking->sourcePdf.empty()) {
        fs::path sourcePath = path.parent_path() / fs::path(marking->sourcePdf);
        if (fs::exists(sourcePath)) {
            boundSourcePdf = fs::absolute(sourcePath);
            control->openMarkingSourcePdf(sourcePath, [this](bool opened) {
                if (opened) {
                    materializeAnnotations();
                } else {
                    marking.reset();
                    manifestPath.reset();
                    boundSourcePdf.reset();
                    annotationElements.clear();
                    updateHeader();
                    rebuildList();
                    showMessage(GTK_MESSAGE_ERROR, _("Could not open source PDF"),
                                _("The marking draft was closed because it could not be bound to its source PDF."));
                }
            });
        } else {
            marking.reset();
            manifestPath.reset();
            boundSourcePdf.reset();
            annotationElements.clear();
            updateHeader();
            rebuildList();
            showMessage(GTK_MESSAGE_ERROR, _("Source PDF not found"),
                        _("The marking draft was closed because its source PDF could not be found beside it."));
        }
    } else {
        marking.reset();
        manifestPath.reset();
        updateHeader();
        rebuildList();
        showMessage(GTK_MESSAGE_ERROR, _("Source PDF is required"),
                    _("The marking draft was closed because it does not identify a source PDF."));
    }
}

void SidebarMarkingPage::materializeAnnotations() {
    annotationElements.clear();
    if (!marking) {
        return;
    }

    Document* document = control->getDocument();
    auto undo = std::make_unique<GroupUndoAction>();
    bool changedDocument = false;
    for (size_t pageIndex = 0; pageIndex < document->getPageCount(); ++pageIndex) {
        document->lock();
        PageRef page = document->getPage(pageIndex);
        const double width = page->getWidth();
        const double height = page->getHeight();
        Layer* previousLayer = nullptr;
        Layer::Index previousPosition = 0;
        Layer::Index candidatePosition = 0;
        for (auto* candidate: page->getLayers()) {
            if (candidate->getName() == FEEDBACK_LAYER_NAME) {
                previousLayer = candidate;
                previousPosition = candidatePosition;
                break;
            }
            ++candidatePosition;
        }
        document->unlock();

        if (previousLayer) {
            control->getLayerController()->removeLayer(page, previousLayer);
            undo->addAction(std::make_unique<RemoveLayerUndoAction>(
                    control->getLayerController(), page, previousLayer, previousPosition));
            changedDocument = true;
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
            undo->addAction(std::make_unique<InsertLayerUndoAction>(
                    control->getLayerController(), page, insertedLayer, position));
            changedDocument = true;
        }
    }
    if (changedDocument) {
        control->getUndoRedoHandler()->addUndoAction(std::move(undo));
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
        const auto bounds = element->second->getBoundingBox();
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
    syncAnnotationGeometry();
    const size_t pageCount = control->getDocument()->getPageCount();
    const auto issues = marking->validate(pageCount == 0 ? std::nullopt : std::optional<size_t>(pageCount), true);
    const auto firstError = std::find_if(issues.begin(), issues.end(), [](const auto& issue) {
        return issue.severity == ValidationIssue::Severity::Error;
    });
    if (firstError != issues.end()) {
        throw std::runtime_error(firstError->location + ": " + firstError->message);
    }
    if (boundSourcePdf) {
        std::error_code error;
        const auto destinationDirectory = fs::absolute(path).parent_path();
        const auto relativeSource = fs::relative(*boundSourcePdf, destinationDirectory, error);
        marking->sourcePdf = error ? boundSourcePdf->string() : relativeSource.string();
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
