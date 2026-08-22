// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeOrchestration/PdfWriter.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace Forge::Orchestration {
namespace {

std::string escapePdf(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        if (ch == '(' || ch == ')' || ch == '\\') {
            out.push_back('\\');
        }
        if (ch == '\r') {
            continue;
        }
        out.push_back(ch == '\n' ? ' ' : ch);
    }
    return out;
}

} // namespace

void PdfWriter::writeText(
    const std::filesystem::path& path,
    const std::string& title,
    const std::string& body) const {
    std::filesystem::create_directories(path.parent_path());
    std::ostringstream content;
    content << "BT /F1 16 Tf 72 720 Td (" << escapePdf(title) << ") Tj ET\n";
    int y = 690;
    std::string line;
    std::istringstream stream(body);
    while (std::getline(stream, line) && y > 72) {
        content << "BT /F1 11 Tf 72 " << y << " Td (" << escapePdf(line) << ") Tj ET\n";
        y -= 16;
    }
    const auto contentStr = content.str();

    std::ostringstream pdf;
    pdf << "%PDF-1.4\n";
    std::vector<std::streamoff> offsets;
    auto mark = [&] {
        offsets.push_back(static_cast<std::streamoff>(pdf.tellp()));
    };
    mark();
    pdf << "1 0 obj<< /Type /Catalog /Pages 2 0 R >>endobj\n";
    mark();
    pdf << "2 0 obj<< /Type /Pages /Kids [3 0 R] /Count 1 >>endobj\n";
    mark();
    pdf << "3 0 obj<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
           "/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>endobj\n";
    mark();
    pdf << "4 0 obj<< /Length " << contentStr.size() << " >>stream\n" << contentStr << "endstream\nendobj\n";
    mark();
    pdf << "5 0 obj<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>endobj\n";
    const auto xref = pdf.tellp();
    pdf << "xref\n0 6\n0000000000 65535 f \n";
    for (const auto offset : offsets) {
        pdf.width(10);
        pdf.fill('0');
        pdf << offset;
        pdf << " 00000 n \n";
    }
    pdf << "trailer<< /Size 6 /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Cannot write PDF: " + path.string());
    }
    out << pdf.str();
}

void PdfWriter::writeFromFile(const std::filesystem::path& source, const std::filesystem::path& dest) const {
    std::ifstream in(source, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot read " + source.string());
    }
    std::ostringstream body;
    body << in.rdbuf();
    writeText(dest, source.filename().string(), body.str());
}

} // namespace Forge::Orchestration
