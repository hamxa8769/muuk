#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace muuk::ctrader_fix {

struct PdfColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

// A deliberately small PDF 1.4 writer: A4 pages, the two built-in
// Helvetica fonts (so nothing has to be embedded), text, lines and filled
// or stroked rectangles. That's all a report needs, and it keeps the app
// free of a PDF library dependency.
//
// Coordinates are in points with the origin at the TOP-left of the page
// (y grows downward) -- the writer flips them to PDF's bottom-left space.
// A text call's y is the baseline. Text is plain ASCII; anything else is
// written as '?'.
class PdfDocument {
public:
    static constexpr float kPageWidth = 595.0f;   // A4
    static constexpr float kPageHeight = 842.0f;

    PdfDocument();  // starts with one empty page

    void newPage();
    [[nodiscard]] int pageCount() const { return static_cast<int>(pages_.size()); }

    void text(float x, float y, const std::string& s, float size, bool bold = false, PdfColor color = {});
    void textRight(float xRight, float y, const std::string& s, float size, bool bold = false, PdfColor color = {});
    void fillRect(float x, float y, float w, float h, PdfColor color);
    void strokeRect(float x, float y, float w, float h, PdfColor color, float lineWidth = 0.5f);
    void line(float x1, float y1, float x2, float y2, PdfColor color, float lineWidth = 0.5f);

    // Approximate rendered width, from Helvetica's metrics.
    [[nodiscard]] static float textWidth(const std::string& s, float size, bool bold = false);

    [[nodiscard]] std::string serialize() const;
    void save(const std::filesystem::path& path) const;

private:
    std::string& current() { return pages_.back(); }
    std::vector<std::string> pages_;  // one content stream per page
};

}  // namespace muuk::ctrader_fix
