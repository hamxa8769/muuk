#include "muuk/ctrader_fix/pdf_writer.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace muuk::ctrader_fix {

namespace {

// Helvetica advance widths (per 1000 em) for ASCII 32..126, from the
// standard Adobe font metrics.
constexpr std::array<int, 95> kHelveticaWidths = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,  // ' ' .. '/'
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556,                                // '0' .. '9'
    278, 278, 584, 584, 584, 556, 1015,                                              // ':' .. '@'
    667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778, 667,  // 'A' .. 'P'
    778, 722, 667, 611, 722, 667, 944, 667, 667, 611,                                // 'Q' .. 'Z'
    278, 278, 278, 469, 556, 333,                                                    // '[' .. '`'
    556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556, 556,  // 'a' .. 'p'
    556, 333, 500, 278, 556, 500, 722, 500, 500, 500,                                // 'q' .. 'z'
    334, 260, 334, 584,                                                              // '{' .. '~'
};

std::string num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    return buf;
}

std::string colorOp(PdfColor c, bool stroke) {
    return num(c.r) + " " + num(c.g) + " " + num(c.b) + (stroke ? " RG\n" : " rg\n");
}

std::string escapeText(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const unsigned char ch : s) {
        if (ch == '(' || ch == ')' || ch == '\\') {
            out += '\\';
            out += static_cast<char>(ch);
        } else if (ch >= 32 && ch < 127) {
            out += static_cast<char>(ch);
        } else {
            out += '?';
        }
    }
    return out;
}

}  // namespace

PdfDocument::PdfDocument() { newPage(); }

void PdfDocument::newPage() { pages_.emplace_back(); }

void PdfDocument::text(float x, float y, const std::string& s, float size, bool bold, PdfColor color) {
    current() += "BT\n" + colorOp(color, false) + (bold ? "/F2 " : "/F1 ") + num(size) + " Tf\n" + num(x) + " " +
                 num(kPageHeight - y) + " Td\n(" + escapeText(s) + ") Tj\nET\n";
}

void PdfDocument::textRight(float xRight, float y, const std::string& s, float size, bool bold, PdfColor color) {
    text(xRight - textWidth(s, size, bold), y, s, size, bold, color);
}

void PdfDocument::fillRect(float x, float y, float w, float h, PdfColor color) {
    current() += colorOp(color, false) + num(x) + " " + num(kPageHeight - y - h) + " " + num(w) + " " + num(h) +
                 " re\nf\n";
}

void PdfDocument::strokeRect(float x, float y, float w, float h, PdfColor color, float lineWidth) {
    current() += num(lineWidth) + " w\n" + colorOp(color, true) + num(x) + " " + num(kPageHeight - y - h) + " " +
                 num(w) + " " + num(h) + " re\nS\n";
}

void PdfDocument::line(float x1, float y1, float x2, float y2, PdfColor color, float lineWidth) {
    current() += num(lineWidth) + " w\n" + colorOp(color, true) + num(x1) + " " + num(kPageHeight - y1) + " m\n" +
                 num(x2) + " " + num(kPageHeight - y2) + " l\nS\n";
}

float PdfDocument::textWidth(const std::string& s, float size, bool bold) {
    int units = 0;
    for (const unsigned char ch : s) {
        units += (ch >= 32 && ch < 127) ? kHelveticaWidths[ch - 32] : 556;
    }
    // Helvetica-Bold runs ~5% wider on average; close enough for alignment.
    return static_cast<float>(units) * size / 1000.0f * (bold ? 1.05f : 1.0f);
}

std::string PdfDocument::serialize() const {
    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;  // offsets[i] = byte offset of object i+1

    auto beginObject = [&](int number) {
        offsets.resize(static_cast<std::size_t>(number));
        offsets[static_cast<std::size_t>(number - 1)] = out.size();
        out += std::to_string(number) + " 0 obj\n";
    };

    const int pageCountValue = pageCount();
    // 1 catalog, 2 page tree, 3/4 fonts, then (page, content) pairs.
    beginObject(1);
    out += "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    beginObject(2);
    out += "<< /Type /Pages /Kids [";
    for (int i = 0; i < pageCountValue; ++i) out += " " + std::to_string(5 + 2 * i) + " 0 R";
    out += " ] /Count " + std::to_string(pageCountValue) + " >>\nendobj\n";

    beginObject(3);
    out += "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>\nendobj\n";
    beginObject(4);
    out += "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>\nendobj\n";

    for (int i = 0; i < pageCountValue; ++i) {
        const int pageObj = 5 + 2 * i;
        beginObject(pageObj);
        out += "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + num(kPageWidth) + " " + num(kPageHeight) +
               "] /Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> /Contents " + std::to_string(pageObj + 1) +
               " 0 R >>\nendobj\n";
        const std::string& content = pages_[static_cast<std::size_t>(i)];
        beginObject(pageObj + 1);
        out += "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream\nendobj\n";
    }

    const std::size_t xrefOffset = out.size();
    const std::size_t objectCount = offsets.size();
    out += "xref\n0 " + std::to_string(objectCount + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t off : offsets) {
        char entry[32];
        std::snprintf(entry, sizeof(entry), "%010zu 00000 n \n", off);
        out += entry;
    }
    out += "trailer\n<< /Size " + std::to_string(objectCount + 1) + " /Root 1 0 R >>\nstartxref\n" +
           std::to_string(xrefOffset) + "\n%%EOF\n";
    return out;
}

void PdfDocument::save(const std::filesystem::path& path) const {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write PDF: " + path.string());
    const std::string bytes = serialize();
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace muuk::ctrader_fix
