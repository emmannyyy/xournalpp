#include <iostream>
#include <regex>
#include <string>

int main(int argc, char** argv) {
    std::string prompt;
    for (int index = 1; index < argc; ++index) {
        prompt += argv[index];
        prompt += '\n';
    }

    std::smatch match;
    const std::regex identity(
            R"IDENTITY(mode="(debox|page-boxes)" source-pdf="student\.pdf" source-sha256="([0-9a-f]{64})")IDENTITY");
    if (!std::regex_search(prompt, match, identity)) {
        std::cerr << "Missing workflow identity\n";
        return 2;
    }
    const std::string mode = match[1];
    const std::string sha = match[2];
    const bool stem = mode == "page-boxes";

    std::cout << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
              << "<marking version=\"1\">\n"
              << "  <assignment id=\"ai-e2e\" title=\"AI end-to-end test\" student=\"Synthetic student\" mode=\""
              << mode << "\" source-pdf=\"student.pdf\" source-sha256=\"" << sha
              << "\" cancelled-work-excluded=\"true\"/>\n"
              << "  <score awarded=\"" << (stem ? "2" : "3") << "\" max=\"4\"/>\n"
              << "  <parts><part id=\"q1\" label=\"Question 1\" awarded=\"" << (stem ? "2" : "3")
              << "\" max=\"4\" rationale=\"The response shows relevant understanding.\"/></parts>\n"
              << "  <annotations>\n"
              << "    <annotation id=\"a1\" page=\"1\" part-id=\"q1\" verdict=\"partial\" source=\"ai\" "
                 "severity=\"major\" target-type=\""
              << (stem ? "image" : "text")
              << "\" reviewed=\"false\" awarded=\"" << (stem ? "2" : "3") << "\" max=\"4\">\n"
              << "      <box x0=\"120\" y0=\"180\" x1=\"620\" y1=\"360\"/>\n"
              << "      <title>Develop this response</title>\n"
              << "      <comment>The core idea is present, but one required step is missing.</comment>\n"
              << "      <how-to-improve>Add the missing explanation and link it explicitly to the answer.</how-to-improve>\n"
              << "      <evidence>The student provided a relevant intermediate statement.</evidence>\n";
    if (!stem) {
        std::cout << "      <text-anchor block-id=\"ai-page-1-a1\" "
                     "exact=\"The student provided a relevant intermediate statement.\"/>\n";
    }
    std::cout << "    </annotation>\n"
              << "  </annotations>\n"
              << "</marking>\n";
    return 0;
}
