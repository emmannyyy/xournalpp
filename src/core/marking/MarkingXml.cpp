#include "MarkingXml.h"

#include <charconv>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <glib.h>

namespace xoj::marking {
namespace {

std::optional<std::string> attribute(const gchar** names, const gchar** values, std::string_view wanted) {
    if (names == nullptr || values == nullptr) {
        return std::nullopt;
    }
    for (size_t index = 0; names[index] != nullptr; ++index) {
        if (wanted == names[index]) {
            return std::string(values[index]);
        }
    }
    return std::nullopt;
}

std::string requiredAttribute(const gchar** names, const gchar** values, std::string_view wanted) {
    auto value = attribute(names, values, wanted);
    if (!value) {
        throw std::runtime_error("Missing required attribute: " + std::string(wanted));
    }
    return *value;
}

template <typename T>
T numberAttribute(const gchar** names, const gchar** values, std::string_view wanted, T fallback) {
    auto text = attribute(names, values, wanted);
    if (!text) {
        return fallback;
    }
    T value{};
    if constexpr (std::is_floating_point_v<T>) {
        char* end{};
        value = static_cast<T>(g_ascii_strtod(text->c_str(), &end));
        if (end != text->c_str() + text->size()) {
            throw std::runtime_error("Invalid numeric attribute: " + std::string(wanted));
        }
    } else {
        const auto* first = text->data();
        const auto* last = first + text->size();
        const auto result = std::from_chars(first, last, value);
        if (result.ec != std::errc{} || result.ptr != last) {
            throw std::runtime_error("Invalid numeric attribute: " + std::string(wanted));
        }
    }
    return value;
}

bool boolAttribute(const gchar** names, const gchar** values, std::string_view wanted, bool fallback) {
    const auto text = attribute(names, values, wanted);
    if (!text) {
        return fallback;
    }
    if (*text == "true") {
        return true;
    }
    if (*text == "false") {
        return false;
    }
    throw std::runtime_error("Invalid boolean attribute: " + std::string(wanted));
}

struct ParseState {
    MarkingDocument document;
    MarkingAnnotation* annotation{};
    std::string textElement;
    std::string text;
    std::exception_ptr error;
};

void startElement(GMarkupParseContext*, const gchar* elementName, const gchar** names, const gchar** values,
                  gpointer userData, GError**) {
    auto& state = *static_cast<ParseState*>(userData);
    if (state.error) {
        return;
    }
    try {
        const std::string_view element(elementName);
        if (element == "marking") {
            state.document.version = numberAttribute<int>(names, values, "version", 0);
        } else if (element == "assignment") {
            state.document.assignmentId = attribute(names, values, "id").value_or("");
            state.document.title = attribute(names, values, "title").value_or("");
            state.document.student = attribute(names, values, "student").value_or("");
            state.document.sourcePdf = attribute(names, values, "source-pdf").value_or("");
            state.document.cancelledWorkExcluded =
                    boolAttribute(names, values, "cancelled-work-excluded", false);
            auto mode = markingModeFromString(attribute(names, values, "mode").value_or(""));
            if (!mode) {
                throw std::runtime_error("Unknown marking mode");
            }
            state.document.mode = *mode;
        } else if (element == "part") {
            MarkingPart part;
            part.id = requiredAttribute(names, values, "id");
            part.label = attribute(names, values, "label").value_or(part.id);
            part.awardedMarks = numberAttribute<double>(names, values, "awarded", 0.0);
            part.maxMarks = numberAttribute<double>(names, values, "max", 0.0);
            part.rationale = attribute(names, values, "rationale").value_or("");
            state.document.parts.push_back(std::move(part));
        } else if (element == "annotation") {
            MarkingAnnotation annotation;
            annotation.id = requiredAttribute(names, values, "id");
            annotation.page = numberAttribute<size_t>(names, values, "page", 0);
            annotation.partId = requiredAttribute(names, values, "part-id");
            auto verdict = verdictFromString(attribute(names, values, "verdict").value_or(""));
            annotation.verdict = verdict.value_or(Verdict::Unresolved);
            annotation.source = attribute(names, values, "source").value_or("ai");
            annotation.severity = attribute(names, values, "severity").value_or("");
            annotation.targetType = attribute(names, values, "target-type").value_or("image");
            annotation.reviewed = boolAttribute(names, values, "reviewed", false);
            annotation.awardedMarks = numberAttribute<double>(names, values, "awarded", 0.0);
            annotation.maxMarks = numberAttribute<double>(names, values, "max", 0.0);
            state.document.annotations.push_back(std::move(annotation));
            state.annotation = &state.document.annotations.back();
        } else if (element == "box" && state.annotation) {
            state.annotation->box = {
                    numberAttribute<int>(names, values, "y0", -1),
                    numberAttribute<int>(names, values, "x0", -1),
                    numberAttribute<int>(names, values, "y1", -1),
                    numberAttribute<int>(names, values, "x1", -1),
            };
        } else if (element == "text-anchor" && state.annotation) {
            state.annotation->textAnchor = TextAnchor{
                    attribute(names, values, "block-id").value_or(""),
                    attribute(names, values, "exact").value_or(""),
            };
        } else if (element == "diagram-anchor" && state.annotation) {
            state.annotation->diagramAnchor =
                    DiagramAnchor{attribute(names, values, "block-id").value_or("")};
        } else if ((element == "title" || element == "comment" || element == "how-to-improve" ||
                    element == "evidence") &&
                   state.annotation) {
            state.textElement = std::string(element);
            state.text.clear();
        }
    } catch (...) {
        state.error = std::current_exception();
    }
}

void textContent(GMarkupParseContext*, const gchar* text, gsize length, gpointer userData, GError**) {
    auto& state = *static_cast<ParseState*>(userData);
    if (!state.textElement.empty()) {
        state.text.append(text, length);
    }
}

void endElement(GMarkupParseContext*, const gchar* elementName, gpointer userData, GError**) {
    auto& state = *static_cast<ParseState*>(userData);
    if (state.error) {
        return;
    }
    const std::string_view element(elementName);
    if (state.annotation && element == "title") {
        state.annotation->title = std::move(state.text);
        state.text.clear();
        state.textElement.clear();
    } else if (state.annotation && element == "comment") {
        state.annotation->comment = std::move(state.text);
        state.text.clear();
        state.textElement.clear();
    } else if (state.annotation && element == "how-to-improve") {
        state.annotation->howToImprove = std::move(state.text);
        state.text.clear();
        state.textElement.clear();
    } else if (state.annotation && element == "evidence") {
        state.annotation->evidenceFromScript = std::move(state.text);
        state.text.clear();
        state.textElement.clear();
    } else if (element == "annotation") {
        state.annotation = nullptr;
    }
}

std::string escape(const std::string& value) {
    gchar* escaped = g_markup_escape_text(value.c_str(), static_cast<gssize>(value.size()));
    std::string result(escaped);
    g_free(escaped);
    return result;
}

}  // namespace

MarkingDocument MarkingXml::load(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Could not open marking manifest");
    }
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (contents.size() > static_cast<size_t>(G_MAXSSIZE)) {
        throw std::runtime_error("Marking manifest is too large");
    }

    ParseState state;
    const GMarkupParser parser = {
            startElement,
            endElement,
            textContent,
            nullptr,
            nullptr,
    };
    GError* error = nullptr;
    GMarkupParseContext* context = g_markup_parse_context_new(&parser, G_MARKUP_TREAT_CDATA_AS_TEXT, &state, nullptr);
    const bool parsed =
            g_markup_parse_context_parse(context, contents.data(), static_cast<gssize>(contents.size()), &error) &&
                        g_markup_parse_context_end_parse(context, &error);
    g_markup_parse_context_free(context);

    if (state.error) {
        std::rethrow_exception(state.error);
    }
    if (!parsed) {
        const std::string message = error ? error->message : "Unknown XML parsing error";
        if (error) {
            g_error_free(error);
        }
        throw std::runtime_error("Could not parse marking manifest: " + message);
    }
    return std::move(state.document);
}

void MarkingXml::save(const MarkingDocument& document, const fs::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Could not create marking manifest");
    }

    output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    output << "<marking version=\"" << document.version << "\">\n";
    output << "  <assignment id=\"" << escape(document.assignmentId) << "\" title=\"" << escape(document.title)
           << "\" student=\"" << escape(document.student) << "\" mode=\"" << toString(document.mode)
           << "\" source-pdf=\"" << escape(document.sourcePdf) << "\" cancelled-work-excluded=\""
           << (document.cancelledWorkExcluded ? "true" : "false") << "\"/>\n";
    output << "  <score awarded=\"" << document.awardedMarks() << "\" max=\"" << document.maxMarks() << "\"/>\n";
    output << "  <parts>\n";
    for (const auto& part: document.parts) {
        output << "    <part id=\"" << escape(part.id) << "\" label=\"" << escape(part.label) << "\" awarded=\""
               << part.awardedMarks << "\" max=\"" << part.maxMarks << "\" rationale=\"" << escape(part.rationale)
               << "\"/>\n";
    }
    output << "  </parts>\n";
    output << "  <annotations>\n";
    for (const auto& annotation: document.annotations) {
        output << "    <annotation id=\"" << escape(annotation.id) << "\" page=\"" << annotation.page
               << "\" part-id=\"" << escape(annotation.partId) << "\" verdict=\"" << toString(annotation.verdict)
               << "\" source=\"" << escape(annotation.source) << "\" severity=\"" << escape(annotation.severity)
               << "\" target-type=\"" << escape(annotation.targetType) << "\" reviewed=\""
               << (annotation.reviewed ? "true" : "false") << "\" awarded=\""
               << annotation.awardedMarks << "\" max=\"" << annotation.maxMarks << "\">\n";
        output << "      <box y0=\"" << annotation.box.y0 << "\" x0=\"" << annotation.box.x0 << "\" y1=\""
               << annotation.box.y1 << "\" x1=\"" << annotation.box.x1 << "\"/>\n";
        output << "      <title>" << escape(annotation.title) << "</title>\n";
        output << "      <comment>" << escape(annotation.comment) << "</comment>\n";
        output << "      <how-to-improve>" << escape(annotation.howToImprove) << "</how-to-improve>\n";
        output << "      <evidence>" << escape(annotation.evidenceFromScript) << "</evidence>\n";
        if (annotation.textAnchor) {
            output << "      <text-anchor block-id=\"" << escape(annotation.textAnchor->blockId) << "\" exact=\""
                   << escape(annotation.textAnchor->exact) << "\"/>\n";
        }
        if (annotation.diagramAnchor) {
            output << "      <diagram-anchor block-id=\"" << escape(annotation.diagramAnchor->blockId) << "\"/>\n";
        }
        output << "    </annotation>\n";
    }
    output << "  </annotations>\n";
    output << "</marking>\n";

    if (!output) {
        throw std::runtime_error("Could not write marking manifest");
    }
}

}  // namespace xoj::marking
