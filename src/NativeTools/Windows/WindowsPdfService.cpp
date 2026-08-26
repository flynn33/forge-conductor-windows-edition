#include "ForgeConductor/NativeTools/Windows/WindowsPdfService.h"

#include "ForgeConductor/Domain/Utf8.h"
#include "NativeFileOperations.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::NativeTools::Windows {
namespace {

constexpr int PageWidth = 612;
constexpr int PageHeight = 792;
constexpr int MarginX = 50;
constexpr int TopY = PageHeight - 50;
constexpr int BottomY = 50;
constexpr int LineHeight = 14;
constexpr std::size_t MaximumWrappedRows = 200'000U;
constexpr std::string_view EngineName = "forge-native-pdf-1.4";

enum class RowStyle : unsigned char { Normal, Heading1, Heading2, Heading3, Code, Blank };

struct RenderedPdf final {
    std::vector<std::byte> bytes;
    std::size_t pages{};
};

[[nodiscard]] Domain::Result<void> checkContext(
    const Domain::OperationContext& context,
    const char* const operation) noexcept
{
    if (context.isCancellationRequested()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Cancelled,
            std::string{operation} + " was cancelled."));
    }
    if (context.isExpired(std::chrono::steady_clock::now())) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::DeadlineExceeded,
            std::string{operation} + " exceeded its deadline."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] std::string_view trimAscii(std::string_view value) noexcept
{
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1U);
    }
    return value;
}

[[nodiscard]] std::size_t utf8SequenceBytes(const unsigned char first) noexcept
{
    if (first < 0x80U) {
        return 1U;
    }
    if ((first & 0xE0U) == 0xC0U) {
        return 2U;
    }
    if ((first & 0xF0U) == 0xE0U) {
        return 3U;
    }
    return 4U;
}

[[nodiscard]] std::size_t utf8Columns(const std::string_view value) noexcept
{
    std::size_t columns{};
    for (std::size_t offset = 0; offset < value.size();) {
        offset += (std::min)(utf8SequenceBytes(static_cast<unsigned char>(value[offset])),
                            value.size() - offset);
        ++columns;
    }
    return columns;
}

[[nodiscard]] std::size_t bytesForColumns(
    const std::string_view value,
    const std::size_t maximumColumns) noexcept
{
    std::size_t offset{};
    std::size_t columns{};
    while (offset < value.size() && columns < maximumColumns) {
        offset += (std::min)(utf8SequenceBytes(static_cast<unsigned char>(value[offset])),
                            value.size() - offset);
        ++columns;
    }
    return offset;
}

[[nodiscard]] std::string truncateColumns(
    const std::string_view value,
    const std::size_t maximumColumns)
{
    return std::string{value.substr(0, bytesForColumns(value, maximumColumns))};
}

[[nodiscard]] Domain::Result<std::string> expandTabs(
    const std::string_view input,
    const Domain::OperationContext& context) noexcept
{
    try {
        constexpr std::size_t ContextCheckStride = 4U * 1024U;
        if (input.size() > Contracts::IPdfService::MaximumPdfBytes) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "PDF code text exceeds the output bound."));
        }
        std::size_t tabCount{};
        for (std::size_t index = 0U; index < input.size(); ++index) {
            if ((index % ContextCheckStride) == 0U) {
                auto active = checkContext(context, "PDF tab expansion");
                if (!active) {
                    return Domain::Result<std::string>::failure(
                        std::move(active).error());
                }
            }
            tabCount += input[index] == '\t' ? 1U : 0U;
        }
        if (tabCount >
            (Contracts::IPdfService::MaximumPdfBytes - input.size()) / 3U) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "Expanded PDF code text exceeds the output bound."));
        }

        std::string expanded;
        expanded.reserve(input.size() + (tabCount * 3U));
        for (std::size_t index = 0U; index < input.size(); ++index) {
            if ((index % ContextCheckStride) == 0U) {
                auto active = checkContext(context, "PDF tab expansion");
                if (!active) {
                    return Domain::Result<std::string>::failure(
                        std::move(active).error());
                }
            }
            if (input[index] == '\t') {
                expanded.append("    ");
            } else {
                expanded.push_back(input[index]);
            }
        }
        return Domain::Result<std::string>::success(std::move(expanded));
    } catch (...) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "PDF code tabs could not be expanded."));
    }
}

[[nodiscard]] Domain::Result<std::vector<std::string>> wrapLine(
    const std::string_view input,
    const std::size_t width,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (width == 0U) {
            return Domain::Result<std::vector<std::string>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "PDF line wrapping requires a positive width."));
        }
        std::vector<std::string> result;
        std::string current;
        std::size_t currentColumns{};
        std::size_t cursor{};

        const auto flush = [&]() {
            if (!current.empty()) {
                result.push_back(std::move(current));
                current.clear();
                currentColumns = 0U;
            }
        };

        while (cursor < input.size()) {
            auto active = checkContext(context, "PDF line wrapping");
            if (!active) {
                return Domain::Result<std::vector<std::string>>::failure(
                    std::move(active).error());
            }
            while (cursor < input.size() && input[cursor] == ' ') {
                ++cursor;
            }
            if (cursor >= input.size()) {
                break;
            }
            const auto wordEnd = input.find(' ', cursor);
            const auto end = wordEnd == std::string_view::npos ? input.size() : wordEnd;
            std::string_view word = input.substr(cursor, end - cursor);
            cursor = end;
            auto wordColumns = utf8Columns(word);

            while (wordColumns > width) {
                active = checkContext(context, "PDF line wrapping");
                if (!active) {
                    return Domain::Result<std::vector<std::string>>::failure(
                        std::move(active).error());
                }
                if (!current.empty()) {
                    flush();
                }
                const auto bytes = bytesForColumns(word, width);
                result.emplace_back(word.substr(0, bytes));
                word.remove_prefix(bytes);
                wordColumns -= width;
            }

            const auto separatorColumns = current.empty() ? 0U : 1U;
            if (!current.empty() &&
                currentColumns + separatorColumns + wordColumns > width) {
                flush();
            }
            if (!current.empty()) {
                current.push_back(' ');
                ++currentColumns;
            }
            current.append(word);
            currentColumns += wordColumns;
        }
        flush();
        if (result.empty()) {
            result.emplace_back();
        }
        return Domain::Result<std::vector<std::string>>::success(
            std::move(result));
    } catch (...) {
        return Domain::Result<std::vector<std::string>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The PDF line could not be wrapped."));
    }
}

[[nodiscard]] std::string pdfLiteral(const std::string_view value)
{
    static constexpr char Octal[] = "01234567";
    std::string result;
    result.reserve(value.size() + 16U);
    for (const unsigned char byte : value) {
        if (byte == '\r') {
            continue;
        }
        if (byte == '\\' || byte == '(' || byte == ')') {
            result.push_back('\\');
            result.push_back(static_cast<char>(byte));
        } else if (byte < 0x20U || byte > 0x7EU) {
            result.push_back('\\');
            result.push_back(Octal[(byte >> 6U) & 0x07U]);
            result.push_back(Octal[(byte >> 3U) & 0x07U]);
            result.push_back(Octal[byte & 0x07U]);
        } else {
            result.push_back(static_cast<char>(byte));
        }
    }
    return result;
}

void stripInlineMarkup(std::string& value)
{
    value.erase(std::remove_if(value.begin(), value.end(), [](const char candidate) {
        return candidate == '*' || candidate == '_' || candidate == '`';
    }), value.end());
}

[[nodiscard]] Domain::Result<RenderedPdf> renderPdf(
    const std::string_view title,
    const std::string_view body,
    const Domain::OperationContext& context)
{
    try {
        if (title.size() > Contracts::IPdfService::MaximumTitleBytes ||
            body.size() > Contracts::IPdfService::MaximumTextBytes) {
            return Domain::Result<RenderedPdf>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "PDF title or body exceeds the native writer input bound."));
        }
        if (title.find('\0') != std::string_view::npos ||
            body.find('\0') != std::string_view::npos ||
            !Domain::isValidUtf8(title) || !Domain::isValidUtf8(body)) {
            return Domain::Result<RenderedPdf>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "PDF title and body must be valid NUL-free UTF-8."));
        }
        auto active = checkContext(context, "PDF rendering");
        if (!active) {
            return Domain::Result<RenderedPdf>::failure(std::move(active).error());
        }

        std::vector<std::string> pages;
        std::string stream;
        int y = TopY;
        std::size_t wrappedRows{};
        std::size_t renderedStreamBytes{};
        bool inCode{};

        const auto emit = [&](const int yy, const int size, const std::string_view text) {
            stream.append("BT\n/F1 ");
            stream.append(std::to_string(size));
            stream.append(" Tf\n");
            stream.append(std::to_string(MarginX));
            stream.push_back(' ');
            stream.append(std::to_string(yy));
            stream.append(" Td\n(");
            stream.append(pdfLiteral(text));
            stream.append(") Tj\nET\n");
        };
        const auto flush = [&]() {
            if (stream.empty()) {
                stream = "BT /F1 11 Tf 50 750 Td ( ) Tj ET";
            }
            renderedStreamBytes += stream.size();
            pages.push_back(std::move(stream));
            stream.clear();
            y = TopY;
        };
        const auto emitTitle = [&](const bool repeated) {
            if (title.empty()) {
                return;
            }
            if (repeated) {
                emit(PageHeight - 36, 8, truncateColumns(title, 80U));
                y = TopY - 10;
                return;
            }
            emit(y, 18, truncateColumns(title, 120U));
            y -= 22;
            emit(y, 9, "Forge Conductor document export");
            y -= 20;
        };
        emitTitle(false);

        const auto emitRow = [&](const RowStyle style, const std::string_view text) {
            if (++wrappedRows > MaximumWrappedRows) {
                return false;
            }
            if (style == RowStyle::Blank) {
                y -= LineHeight / 2;
                if (y < BottomY) {
                    flush();
                    emitTitle(true);
                }
                return renderedStreamBytes + stream.size() <=
                    Contracts::IPdfService::MaximumPdfBytes;
            }
            int fontSize = 11;
            int extra{};
            if (style == RowStyle::Heading1) {
                fontSize = 16;
                extra = 6;
            } else if (style == RowStyle::Heading2) {
                fontSize = 13;
                extra = 4;
            } else if (style == RowStyle::Heading3) {
                fontSize = 12;
                extra = 2;
            } else if (style == RowStyle::Code) {
                fontSize = 9;
            }
            if (y - LineHeight - extra < BottomY) {
                flush();
                emitTitle(true);
            }
            y -= extra;
            emit(y, fontSize, text);
            y -= LineHeight + (style == RowStyle::Heading1 ? 4 : 0);
            return renderedStreamBytes + stream.size() <=
                Contracts::IPdfService::MaximumPdfBytes;
        };

        std::size_t lineStart{};
        std::size_t lineNumber{};
        while (lineStart <= body.size()) {
            if ((lineNumber++ & 0xFFU) == 0U) {
                active = checkContext(context, "PDF rendering");
                if (!active) {
                    return Domain::Result<RenderedPdf>::failure(std::move(active).error());
                }
            }
            const auto newline = body.find('\n', lineStart);
            const auto lineEnd = newline == std::string_view::npos ? body.size() : newline;
            const auto raw = body.substr(lineStart, lineEnd - lineStart);
            auto trimmed = trimAscii(raw);
            if (body.empty()) {
                trimmed = "(empty document)";
            }
            if (trimmed.starts_with("```")) {
                inCode = !inCode;
                if (!emitRow(RowStyle::Blank, {})) {
                    return Domain::Result<RenderedPdf>::failure(Domain::makeError(
                        Domain::ErrorCodes::PayloadTooLarge,
                        "PDF layout exceeds its row or output bound."));
                }
            } else if (inCode) {
                auto code = expandTabs(raw, context);
                if (!code) {
                    return Domain::Result<RenderedPdf>::failure(
                        std::move(code).error());
                }
                auto wrapped = wrapLine(code.value(), 92U, context);
                if (!wrapped) {
                    return Domain::Result<RenderedPdf>::failure(
                        std::move(wrapped).error());
                }
                for (const auto& part : wrapped.value()) {
                    if (!emitRow(RowStyle::Code, part)) {
                        return Domain::Result<RenderedPdf>::failure(Domain::makeError(
                            Domain::ErrorCodes::PayloadTooLarge,
                            "PDF layout exceeds its row or output bound."));
                    }
                }
            } else if (trimmed.empty()) {
                if (!emitRow(RowStyle::Blank, {})) {
                    return Domain::Result<RenderedPdf>::failure(Domain::makeError(
                        Domain::ErrorCodes::PayloadTooLarge,
                        "PDF layout exceeds its row or output bound."));
                }
            } else {
                RowStyle style = RowStyle::Normal;
                std::string text{trimmed};
                if (trimmed.starts_with("### ")) {
                    style = RowStyle::Heading3;
                    text.assign(trimmed.substr(4U));
                } else if (trimmed.starts_with("## ")) {
                    style = RowStyle::Heading2;
                    text.assign(trimmed.substr(3U));
                } else if (trimmed.starts_with("# ")) {
                    style = RowStyle::Heading1;
                    text.assign(trimmed.substr(2U));
                    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) {
                        return static_cast<char>(std::toupper(c));
                    });
                } else if (trimmed.starts_with("- ") || trimmed.starts_with("* ")) {
                    text = "- ";
                    text.append(trimmed.substr(2U));
                }
                stripInlineMarkup(text);
                const auto width = style == RowStyle::Heading1 ||
                    style == RowStyle::Heading2 || style == RowStyle::Heading3 ? 88U : 92U;
                auto wrapped = wrapLine(text, width, context);
                if (!wrapped) {
                    return Domain::Result<RenderedPdf>::failure(
                        std::move(wrapped).error());
                }
                for (const auto& part : wrapped.value()) {
                    if (!emitRow(style, part)) {
                        return Domain::Result<RenderedPdf>::failure(Domain::makeError(
                            Domain::ErrorCodes::PayloadTooLarge,
                            "PDF layout exceeds its row or output bound."));
                    }
                }
            }
            if (newline == std::string_view::npos) {
                break;
            }
            lineStart = newline + 1U;
        }
        if (!stream.empty() || pages.empty()) {
            flush();
        }

        std::vector<std::string> objects;
        objects.emplace_back("<< /Type /Catalog /Pages 2 0 R >>");
        objects.emplace_back();
        objects.emplace_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
        std::vector<std::size_t> pageIds;
        for (const auto& page : pages) {
            std::string content = "<< /Length ";
            content.append(std::to_string(page.size()));
            content.append(" >>\nstream\n");
            content.append(page);
            content.append("\nendstream");
            objects.push_back(std::move(content));
            const auto contentId = objects.size();
            std::string pageObject = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ";
            pageObject.append(std::to_string(PageWidth));
            pageObject.push_back(' ');
            pageObject.append(std::to_string(PageHeight));
            pageObject.append("] /Contents ");
            pageObject.append(std::to_string(contentId));
            pageObject.append(" 0 R /Resources << /Font << /F1 3 0 R >> >> >>");
            objects.push_back(std::move(pageObject));
            pageIds.push_back(objects.size());
        }
        std::string pagesObject = "<< /Type /Pages /Kids [ ";
        for (const auto id : pageIds) {
            pagesObject.append(std::to_string(id));
            pagesObject.append(" 0 R ");
        }
        pagesObject.append("] /Count ");
        pagesObject.append(std::to_string(pageIds.size()));
        pagesObject.append(" >>");
        objects[1] = std::move(pagesObject);

        std::string pdf = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
        std::vector<std::size_t> offsets{0U};
        offsets.reserve(objects.size() + 1U);
        for (std::size_t index = 0; index < objects.size(); ++index) {
            offsets.push_back(pdf.size());
            pdf.append(std::to_string(index + 1U));
            pdf.append(" 0 obj\n");
            pdf.append(objects[index]);
            pdf.append("\nendobj\n");
            if (pdf.size() > Contracts::IPdfService::MaximumPdfBytes) {
                return Domain::Result<RenderedPdf>::failure(Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "The generated PDF exceeds its output bound."));
            }
        }
        const auto xref = pdf.size();
        pdf.append("xref\n0 ");
        pdf.append(std::to_string(objects.size() + 1U));
        pdf.append("\n0000000000 65535 f \n");
        for (std::size_t index = 1U; index < offsets.size(); ++index) {
            auto value = std::to_string(offsets[index]);
            if (value.size() > 10U) {
                return Domain::Result<RenderedPdf>::failure(Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "A PDF object offset exceeds the supported format."));
            }
            pdf.append(10U - value.size(), '0');
            pdf.append(value);
            pdf.append(" 00000 n \n");
        }
        pdf.append("trailer\n<< /Size ");
        pdf.append(std::to_string(objects.size() + 1U));
        pdf.append(" /Root 1 0 R >>\nstartxref\n");
        pdf.append(std::to_string(xref));
        pdf.append("\n%%EOF\n");
        if (pdf.size() > Contracts::IPdfService::MaximumPdfBytes) {
            return Domain::Result<RenderedPdf>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "The generated PDF exceeds its output bound."));
        }

        std::vector<std::byte> bytes(pdf.size());
        std::transform(pdf.begin(), pdf.end(), bytes.begin(), [](const unsigned char byte) {
            return static_cast<std::byte>(byte);
        });
        return Domain::Result<RenderedPdf>::success(
            RenderedPdf{std::move(bytes), pageIds.size()});
    } catch (...) {
        return Domain::Result<RenderedPdf>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The native PDF could not be rendered."));
    }
}

[[nodiscard]] Domain::Result<Domain::PdfWriteReceipt> renderAndWrite(
    Contracts::IAtomicFileStore& atomicFileStore,
    const std::string_view title,
    const std::string_view body,
    const Contracts::AuthorizedPath& destination,
    const Domain::OperationContext& context)
{
    auto rendered = renderPdf(title, body, context);
    if (!rendered) {
        return Domain::Result<Domain::PdfWriteReceipt>::failure(std::move(rendered).error());
    }
    auto active = checkContext(context, "PDF publication");
    if (!active) {
        return Domain::Result<Domain::PdfWriteReceipt>::failure(std::move(active).error());
    }
    auto parents = Detail::ensureAuthorizedParentDirectories(destination, context);
    if (!parents) {
        return Domain::Result<Domain::PdfWriteReceipt>::failure(
            std::move(parents).error());
    }
    const auto bytesWritten = rendered.value().bytes.size();
    const auto pages = rendered.value().pages;
    auto written = atomicFileStore.replace(
        destination, rendered.value().bytes, false, context);
    if (!written) {
        return Domain::Result<Domain::PdfWriteReceipt>::failure(std::move(written).error());
    }
    return Domain::Result<Domain::PdfWriteReceipt>::success(Domain::PdfWriteReceipt{
        destination.canonicalPath(),
        bytesWritten,
        pages,
        std::string{EngineName},
        std::string{title}});
}

} // namespace

WindowsPdfService::WindowsPdfService(
    Contracts::IAtomicFileStore& atomicFileStore) noexcept
    : atomicFileStore_{atomicFileStore}
{
}

Domain::Result<Domain::PdfWriteReceipt> WindowsPdfService::write(
    const std::string_view title,
    const std::string_view body,
    const Contracts::AuthorizedPath& destination,
    const Domain::OperationContext& context) noexcept
{
    try {
        return renderAndWrite(
            atomicFileStore_, title, body, destination, context);
    } catch (...) {
        return Domain::Result<Domain::PdfWriteReceipt>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The PDF write boundary failed."));
    }
}

Domain::Result<Domain::PdfWriteReceipt> WindowsPdfService::fromTextFile(
    const std::string_view title,
    const Contracts::AuthorizedPath& source,
    const Contracts::AuthorizedPath& destination,
    const Domain::OperationContext& context) noexcept
{
    try {
        auto bytes = atomicFileStore_.read(source, MaximumTextBytes, context);
        if (!bytes) {
            return Domain::Result<Domain::PdfWriteReceipt>::failure(std::move(bytes).error());
        }
        std::string body;
        body.resize(bytes.value().size());
        std::transform(bytes.value().begin(), bytes.value().end(), body.begin(),
                       [](const std::byte byte) {
                           return static_cast<char>(std::to_integer<unsigned char>(byte));
                       });
        if (!Domain::isValidUtf8(body) || body.find('\0') != std::string::npos) {
            return Domain::Result<Domain::PdfWriteReceipt>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The PDF source file must contain valid NUL-free UTF-8 text."));
        }
        return renderAndWrite(
            atomicFileStore_, title, body, destination, context);
    } catch (...) {
        return Domain::Result<Domain::PdfWriteReceipt>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The PDF text-file conversion boundary failed."));
    }
}

} // namespace ForgeConductor::NativeTools::Windows
