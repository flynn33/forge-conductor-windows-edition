#include "ForgeConductor/NativeTools/Windows/WindowsArtifactDocumentService.h"

#include "ForgeConductor/Domain/Utf8.h"
#include "Infrastructure/Windows/Detail/OperationContextGuard.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "NativeFileOperations.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <unordered_set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::NativeTools::Windows {
namespace {
using Json = nlohmann::json;
using Service = WindowsArtifactDocumentService;
namespace InfrastructureDetail = ForgeConductor::Infrastructure::Windows::Detail;
constexpr std::string_view XmlHeader = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>";
constexpr std::string_view RelNamespace = "http://schemas.openxmlformats.org/package/2006/relationships";
constexpr std::string_view OfficeRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
constexpr std::string_view DrawingNamespace = "http://schemas.openxmlformats.org/drawingml/2006/main";
constexpr std::string_view PresentationNamespace = "http://schemas.openxmlformats.org/presentationml/2006/main";

struct Failure final { Domain::Error error; };
[[noreturn]] void fail(std::string_view code, std::string message) {
    throw Failure{Domain::makeError(code, std::move(message))};
}
template<typename T> T take(Domain::Result<T> result) {
    if (!result) throw Failure{std::move(result).error()};
    return std::move(result).value();
}
void take(Domain::Result<void> result) {
    if (!result) throw Failure{std::move(result).error()};
}
void check(const Domain::OperationContext& context) {
    take(InfrastructureDetail::validateOperationContext(context,
        std::chrono::steady_clock::now(), "create an Office document"));
}
void bound(std::size_t size, std::size_t maximum, std::string_view description) {
    if (size > maximum) fail(Domain::ErrorCodes::PayloadTooLarge,
        std::string{description} + " exceeds its supported bound.");
}
void objectKeys(const Json& value, std::initializer_list<std::string_view> keys) {
    if (!value.is_object()) fail(Domain::ErrorCodes::InvalidRequest, "Document arguments must be objects.");
    for (const auto& [key, member] : value.items()) {
        static_cast<void>(member);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            fail(Domain::ErrorCodes::InvalidRequest, "Unknown document property: " + key);
    }
}
const Json& required(const Json& value, std::string_view key) {
    const auto found = value.find(std::string{key});
    if (found == value.end()) fail(Domain::ErrorCodes::InvalidRequest, "Missing document property: " + std::string{key});
    return *found;
}
std::string text(const Json& value, std::string_view description) {
    if (!value.is_string()) fail(Domain::ErrorCodes::InvalidRequest, std::string{description} + " must be text.");
    auto result = value.get<std::string>();
    bound(result.size(), Service::MaximumTextBytes, description);
    if (!Domain::isValidUtf8(result)) fail(Domain::ErrorCodes::InvalidRequest, std::string{description} + " must be valid UTF-8.");
    const auto wide = take(InfrastructureDetail::strictUtf8ToUtf16(result));
    for (const auto character : wide) {
        if ((character < 0x20 && character != L'\t' && character != L'\n' && character != L'\r') ||
            character == 0xfffe || character == 0xffff)
            fail(Domain::ErrorCodes::InvalidRequest, std::string{description} + " contains a character forbidden by XML 1.0.");
    }
    return result;
}
std::string title(const Json& value) {
    const auto found = value.find("title");
    return found == value.end() ? std::string{} : text(*found, "title");
}
std::string escape(std::string_view value) {
    std::string result;
    for (const auto character : value) {
        switch (character) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&apos;"; break;
        case '\r': result += "&#13;"; break;
        default: result.push_back(character); break;
        }
    }
    return result;
}
std::string spreadsheetEscape(std::string_view value) {
    // SpreadsheetML's escaped-string convention is distinct from XML escaping.
    // Shield a literal _xHHHH_ sequence so Excel does not decode user text.
    const auto hexDigit = [](char digit) {
        return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') || (digit >= 'A' && digit <= 'F');
    };
    std::string literal;
    for (std::size_t index = 0U; index < value.size(); ++index) {
        if (index + 6U < value.size() && value[index] == '_' && value[index + 1U] == 'x' && value[index + 6U] == '_' &&
            hexDigit(value[index + 2U]) && hexDigit(value[index + 3U]) && hexDigit(value[index + 4U]) && hexDigit(value[index + 5U]))
            literal += "_x005F_";
        else literal.push_back(value[index]);
    }
    return escape(literal);
}
const Json& array(const Json& value, std::string_view key, std::size_t minimum, std::size_t maximum) {
    const auto& result = required(value, key);
    if (!result.is_array() || result.size() < minimum)
        fail(Domain::ErrorCodes::InvalidRequest, std::string{key} + " must be an array with at least " + std::to_string(minimum) + " entries.");
    bound(result.size(), maximum, key);
    return result;
}
std::string relationship(std::string_view id, std::string_view type, std::string_view target) {
    return "<Relationship Id=\"" + std::string{id} + "\" Type=\"" + std::string{type} + "\" Target=\"" + std::string{target} + "\"/>";
}
std::string relationships(std::string content) {
    return std::string{XmlHeader} + "<Relationships xmlns=\"" + std::string{RelNamespace} + "\">" + content + "</Relationships>";
}
constexpr auto crcTable() {
    std::array<std::uint32_t, 256U> result{};
    for (std::uint32_t index = 0U; index < 256U; ++index) {
        auto value = index;
        for (unsigned bit = 0U; bit < 8U; ++bit) value = (value >> 1U) ^ ((value & 1U) ? 0xedb88320U : 0U);
        result[index] = value;
    }
    return result;
}
std::uint32_t crc32(std::string_view value, const Domain::OperationContext& context) {
    static constexpr auto table = crcTable();
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0U; index < value.size(); ++index) {
        if (index % (64U * 1024U) == 0U) check(context);
        crc = table[(crc ^ static_cast<unsigned char>(value[index])) & 255U] ^ (crc >> 8U);
    }
    return crc ^ 0xffffffffU;
}
struct Entry final { std::string name; std::string content; std::uint32_t crc{}; std::uint32_t offset{}; };
class Package final {
public:
    void add(std::string name, std::string content, std::string_view contentType = {}) {
        bound(content.size(), Service::MaximumOutputBytes, "An Office XML part");
        total_ += content.size() + name.size() * 2U + 76U;
        bound(total_ + 22U, Service::MaximumOutputBytes, "The Office package");
        if (!contentType.empty()) overrides_ += "<Override PartName=\"/" + name + "\" ContentType=\"" + std::string{contentType} + "\"/>";
        entries_.push_back({std::move(name), std::move(content), 0U, 0U});
    }
    void common(std::string_view mainPart, std::string_view documentTitle) {
        add("_rels/.rels", relationships(
            relationship("rId1", std::string{OfficeRel} + "officeDocument", mainPart) +
            relationship("rId2", "http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties", "docProps/core.xml")));
        add("docProps/core.xml", std::string{XmlHeader} +
            "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>" + escape(documentTitle) + "</dc:title></cp:coreProperties>",
            "application/vnd.openxmlformats-package.core-properties+xml");
    }
    std::vector<std::byte> zip(const Domain::OperationContext& context) {
        add("[Content_Types].xml", std::string{XmlHeader} +
            "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
            "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
            "<Default Extension=\"xml\" ContentType=\"application/xml\"/>" + overrides_ + "</Types>");
        std::vector<std::byte> result;
        result.reserve(total_ + 22U);
        const auto u16 = [&](std::uint16_t value) {
            result.push_back(static_cast<std::byte>(value & 255U));
            result.push_back(static_cast<std::byte>(value >> 8U));
        };
        const auto u32 = [&](std::uint32_t value) { u16(static_cast<std::uint16_t>(value)); u16(static_cast<std::uint16_t>(value >> 16U)); };
        const auto append = [&](std::string_view value) {
            const auto* begin = reinterpret_cast<const std::byte*>(value.data());
            result.insert(result.end(), begin, begin + value.size());
        };
        for (auto& entry : entries_) {
            check(context);
            entry.crc = crc32(entry.content, context);
            entry.offset = static_cast<std::uint32_t>(result.size());
            u32(0x04034b50U); u16(20U); u16(0x0800U); u16(0U); u16(0U); u16(33U);
            u32(entry.crc); u32(static_cast<std::uint32_t>(entry.content.size())); u32(static_cast<std::uint32_t>(entry.content.size()));
            u16(static_cast<std::uint16_t>(entry.name.size())); u16(0U); append(entry.name); append(entry.content);
        }
        const auto directoryOffset = static_cast<std::uint32_t>(result.size());
        for (const auto& entry : entries_) {
            check(context);
            u32(0x02014b50U); u16(20U); u16(20U); u16(0x0800U); u16(0U); u16(0U); u16(33U);
            u32(entry.crc); u32(static_cast<std::uint32_t>(entry.content.size())); u32(static_cast<std::uint32_t>(entry.content.size()));
            u16(static_cast<std::uint16_t>(entry.name.size())); u16(0U); u16(0U); u16(0U); u16(0U); u32(0U); u32(entry.offset); append(entry.name);
        }
        const auto directoryBytes = static_cast<std::uint32_t>(result.size()) - directoryOffset;
        u32(0x06054b50U); u16(0U); u16(0U); u16(static_cast<std::uint16_t>(entries_.size())); u16(static_cast<std::uint16_t>(entries_.size()));
        u32(directoryBytes); u32(directoryOffset); u16(0U);
        return result;
    }
private:
    std::vector<Entry> entries_;
    std::string overrides_;
    std::size_t total_{};
};

std::string wordRun(std::string_view value) {
    std::string result = "<w:r><w:t xml:space=\"preserve\">";
    std::size_t start{};
    for (std::size_t index = 0U; index < value.size(); ++index) {
        if (value[index] != '\r' && value[index] != '\n' && value[index] != '\t') continue;
        result += escape(value.substr(start, index - start)) + "</w:t>";
        result += value[index] == '\t' ? "<w:tab/>" : "<w:br/>";
        if (value[index] == '\r' && index + 1U < value.size() && value[index + 1U] == '\n') ++index;
        start = index + 1U;
        result += "<w:t xml:space=\"preserve\">";
    }
    return result + escape(value.substr(start)) + "</w:t></w:r>";
}
Json document(Package& package, const Json& arguments, const Domain::OperationContext& context) {
    objectKeys(arguments, {"path", "title", "paragraphs"});
    const auto documentTitle = title(arguments);
    const auto& paragraphs = array(arguments, "paragraphs", 0U, Service::MaximumParagraphs);
    std::string content{XmlHeader};
    content += "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>";
    if (!documentTitle.empty()) content += "<w:p><w:pPr><w:pStyle w:val=\"Title\"/></w:pPr>" + wordRun(documentTitle) + "</w:p>";
    for (const auto& paragraph : paragraphs) {
        check(context);
        content += "<w:p>" + wordRun(text(paragraph, "paragraph")) + "</w:p>";
        bound(content.size(), Service::MaximumOutputBytes, "The Word document");
    }
    if (documentTitle.empty() && paragraphs.empty()) content += "<w:p/>";
    content += "<w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/>"
        "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/>"
        "</w:sectPr></w:body></w:document>";
    package.add("word/document.xml", std::move(content), "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml");
    package.add("word/styles.xml", std::string{XmlHeader} +
        "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\"/><w:sz w:val=\"22\"/></w:rPr></w:rPrDefault></w:docDefaults>"
        "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Title\"><w:name w:val=\"Title\"/><w:basedOn w:val=\"Normal\"/>"
        "<w:pPr><w:spacing w:after=\"240\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"36\"/></w:rPr></w:style></w:styles>",
        "application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml");
    package.add("word/_rels/document.xml.rels", relationships(relationship("rId1", std::string{OfficeRel} + "styles", "styles.xml")));
    package.common("word/document.xml", documentTitle);
    return Json{{"format", "docx"}, {"paragraphs", paragraphs.size()}};
}

std::string cellName(std::size_t column, std::size_t row) {
    std::string name;
    do { name.insert(name.begin(), static_cast<char>('A' + (column - 1U) % 26U)); column = (column - 1U) / 26U; } while (column != 0U);
    return name + std::to_string(row);
}
Json spreadsheet(Package& package, const Json& arguments, const Domain::OperationContext& context) {
    objectKeys(arguments, {"path", "sheets"});
    const auto& sheets = array(arguments, "sheets", 1U, Service::MaximumSheets);
    std::vector<std::wstring> names;
    std::size_t rowCount{}, cellCount{};
    std::string workbook = std::string{XmlHeader} + "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"" + std::string{OfficeRel.substr(0U, OfficeRel.size() - 1U)} + "\"><sheets>";
    std::string links;
    for (std::size_t sheetIndex = 0U; sheetIndex < sheets.size(); ++sheetIndex) {
        check(context);
        const auto& sheet = sheets[sheetIndex];
        objectKeys(sheet, {"name", "rows"});
        const auto name = text(required(sheet, "name"), "sheet name");
        const auto wide = take(InfrastructureDetail::strictUtf8ToUtf16(name));
        if (wide.empty() || wide.size() > 31U || name.find_first_of("[]:*?/\\\t\r\n") != std::string::npos || name.front() == '\'' || name.back() == '\'')
            fail(Domain::ErrorCodes::InvalidRequest, "Sheet names must contain 1 through 31 UTF-16 units, without Excel's reserved characters or leading/trailing apostrophes.");
        for (const auto& previous : names) {
            if (::CompareStringOrdinal(wide.data(), static_cast<int>(wide.size()), previous.data(), static_cast<int>(previous.size()), TRUE) == CSTR_EQUAL)
                fail(Domain::ErrorCodes::InvalidRequest, "Sheet names must be unique without regard to case.");
        }
        names.push_back(wide);
        const auto& rows = array(sheet, "rows", 0U, Service::MaximumRowsPerSheet);
        rowCount += rows.size();
        const auto number = std::to_string(sheetIndex + 1U);
        workbook += "<sheet name=\"" + spreadsheetEscape(name) + "\" sheetId=\"" + number + "\" r:id=\"rId" + number + "\"/>";
        links += relationship("rId" + number, std::string{OfficeRel} + "worksheet", "worksheets/sheet" + number + ".xml");
        std::string xml = std::string{XmlHeader} + "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>";
        for (std::size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
            check(context);
            const auto& row = rows[rowIndex];
            if (!row.is_array()) fail(Domain::ErrorCodes::InvalidRequest, "Each spreadsheet row must be an array.");
            bound(row.size(), Service::MaximumColumns, "Spreadsheet columns");
            cellCount += row.size();
            bound(cellCount, Service::MaximumCells, "Spreadsheet cells");
            xml += "<row r=\"" + std::to_string(rowIndex + 1U) + "\">";
            for (std::size_t column = 0U; column < row.size(); ++column) {
                const auto& value = row[column];
                if (value.is_null()) continue;
                const auto address = cellName(column + 1U, rowIndex + 1U);
                if (value.is_string()) {
                    const auto valueText = text(value, "cell text");
                    if (take(InfrastructureDetail::strictUtf8ToUtf16(valueText)).size() > 32'767U)
                        fail(Domain::ErrorCodes::PayloadTooLarge, "Cell text exceeds Excel's 32767 UTF-16-unit limit.");
                    xml += "<c r=\"" + address + "\" t=\"inlineStr\"><is><t xml:space=\"preserve\">" + spreadsheetEscape(valueText) + "</t></is></c>";
                } else if (value.is_boolean()) {
                    xml += "<c r=\"" + address + "\" t=\"b\"><v>" + (value.get<bool>() ? "1" : "0") + "</v></c>";
                } else if (value.is_number()) {
                    const auto numeric = value.get<double>();
                    if (!std::isfinite(numeric) || std::abs(numeric) > 1.0e307 ||
                        (numeric != 0.0 && std::abs(numeric) < std::numeric_limits<double>::min()) ||
                        (value.is_number_integer() && std::abs(numeric) > 999'999'999'999'999.0))
                        fail(Domain::ErrorCodes::InvalidRequest, "Numeric cells must be zero or finite normal doubles within 1e307; integers must fit Excel's 15-digit precision. Use text for values outside Excel's numeric range or larger integers.");
                    xml += "<c r=\"" + address + "\" t=\"n\"><v>" + value.dump() + "</v></c>";
                } else fail(Domain::ErrorCodes::InvalidRequest, "Cells must be text, numbers, booleans, or null.");
            }
            xml += "</row>";
            bound(xml.size(), Service::MaximumOutputBytes, "A spreadsheet worksheet");
        }
        xml += "</sheetData></worksheet>";
        package.add("xl/worksheets/sheet" + number + ".xml", std::move(xml), "application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml");
    }
    package.add("xl/workbook.xml", workbook + "</sheets></workbook>", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml");
    package.add("xl/_rels/workbook.xml.rels", relationships(std::move(links)));
    package.common("xl/workbook.xml", "");
    return Json{{"format", "xlsx"}, {"sheets", sheets.size()}, {"rows", rowCount}, {"cells", cellCount}};
}

std::string groupShape() {
    return "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
        "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
        "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>";
}
std::string presentationRoot(std::string_view element) {
    return std::string{XmlHeader} + "<p:" + std::string{element} + " xmlns:p=\"" + std::string{PresentationNamespace} +
        "\" xmlns:a=\"" + std::string{DrawingNamespace} + "\" xmlns:r=\"" + std::string{OfficeRel.substr(0U, OfficeRel.size() - 1U)} + "\">";
}
std::string drawingParagraph(std::string_view value, bool heading) {
    const auto properties = heading ? "<a:rPr lang=\"en-US\" sz=\"3200\" b=\"1\"/>" : "<a:rPr lang=\"en-US\" sz=\"2000\"/>";
    std::string result = "<a:p><a:pPr/><a:r>" + std::string{properties} + "<a:t>";
    std::size_t start{};
    for (std::size_t index = 0U; index < value.size(); ++index) {
        if (value[index] != '\r' && value[index] != '\n') continue;
        result += escape(value.substr(start, index - start)) + "</a:t></a:r><a:br/><a:r>" + properties + "<a:t>";
        if (value[index] == '\r' && index + 1U < value.size() && value[index + 1U] == '\n') ++index;
        start = index + 1U;
    }
    return result + escape(value.substr(start)) + "</a:t></a:r><a:endParaRPr lang=\"en-US\"/></a:p>";
}
std::string textShape(std::string_view id, bool heading, std::string paragraphs) {
    return "<p:sp><p:nvSpPr><p:cNvPr id=\"" + std::string{id} + "\" name=\"" + (heading ? "Title" : "Body") +
        "\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x=\"685800\" y=\"" +
        (heading ? "457200" : "1600200") + "\"/><a:ext cx=\"10820400\" cy=\"" + (heading ? "914400" : "4800600") +
        "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr>"
        "<p:txBody><a:bodyPr wrap=\"square\"><a:spAutoFit/></a:bodyPr><a:lstStyle/>" + paragraphs + "</p:txBody></p:sp>";
}
std::string theme() {
    std::string xml = std::string{XmlHeader} + "<a:theme xmlns:a=\"" + std::string{DrawingNamespace} + "\" name=\"Forge\"><a:themeElements><a:clrScheme name=\"Forge\">";
    constexpr std::pair<std::string_view, std::string_view> colors[]{
        {"dk1", "172033"}, {"lt1", "FFFFFF"}, {"dk2", "334155"}, {"lt2", "F1F5F9"},
        {"accent1", "2563EB"}, {"accent2", "0891B2"}, {"accent3", "16A34A"}, {"accent4", "9333EA"},
        {"accent5", "EA580C"}, {"accent6", "DC2626"}, {"hlink", "2563EB"}, {"folHlink", "9333EA"}};
    for (const auto& [name, color] : colors) xml += "<a:" + std::string{name} + "><a:srgbClr val=\"" + std::string{color} + "\"/></a:" + std::string{name} + ">";
    xml += "</a:clrScheme><a:fontScheme name=\"Forge\"><a:majorFont><a:latin typeface=\"Calibri\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:majorFont>"
        "<a:minorFont><a:latin typeface=\"Calibri\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:minorFont></a:fontScheme><a:fmtScheme name=\"Forge\"><a:fillStyleLst>";
    for (unsigned index = 0U; index < 3U; ++index) xml += "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>";
    xml += "</a:fillStyleLst><a:lnStyleLst>";
    for (unsigned index = 0U; index < 3U; ++index) xml += "<a:ln w=\"9525\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:prstDash val=\"solid\"/></a:ln>";
    xml += "</a:lnStyleLst><a:effectStyleLst>";
    for (unsigned index = 0U; index < 3U; ++index) xml += "<a:effectStyle><a:effectLst/></a:effectStyle>";
    xml += "</a:effectStyleLst><a:bgFillStyleLst>";
    for (unsigned index = 0U; index < 3U; ++index) xml += "<a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill>";
    return xml + "</a:bgFillStyleLst></a:fmtScheme></a:themeElements></a:theme>";
}
Json presentation(Package& package, const Json& arguments, const Domain::OperationContext& context) {
    objectKeys(arguments, {"path", "title", "slides"});
    const auto documentTitle = title(arguments);
    const auto& slides = array(arguments, "slides", 1U, Service::MaximumSlides);
    std::string content = presentationRoot("presentation") + "<p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst><p:sldIdLst>";
    std::string links = relationship("rId1", std::string{OfficeRel} + "slideMaster", "slideMasters/slideMaster1.xml") +
        relationship("rId2", std::string{OfficeRel} + "presProps", "presProps.xml");
    for (std::size_t index = 0U; index < slides.size(); ++index) {
        check(context);
        const auto& slide = slides[index];
        objectKeys(slide, {"title", "body"});
        const auto heading = text(required(slide, "title"), "slide title");
        const auto& body = array(slide, "body", 0U, Service::MaximumSlideParagraphs);
        std::string paragraphs;
        for (const auto& paragraph : body) {
            check(context);
            paragraphs += drawingParagraph(text(paragraph, "slide paragraph"), false);
            bound(paragraphs.size(), Service::MaximumOutputBytes, "Slide text");
        }
        if (body.empty()) paragraphs = "<a:p/>";
        const auto number = std::to_string(index + 1U);
        const auto relationshipId = "rId" + std::to_string(index + 3U);
        content += "<p:sldId id=\"" + std::to_string(index + 256U) + "\" r:id=\"" + relationshipId + "\"/>";
        links += relationship(relationshipId, std::string{OfficeRel} + "slide", "slides/slide" + number + ".xml");
        package.add("ppt/slides/slide" + number + ".xml", presentationRoot("sld") + "<p:cSld><p:spTree>" + groupShape() +
            textShape("2", true, drawingParagraph(heading, true)) + textShape("3", false, std::move(paragraphs)) +
            "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>",
            "application/vnd.openxmlformats-officedocument.presentationml.slide+xml");
        package.add("ppt/slides/_rels/slide" + number + ".xml.rels", relationships(relationship("rId1", std::string{OfficeRel} + "slideLayout", "../slideLayouts/slideLayout1.xml")));
    }
    content += "</p:sldIdLst><p:sldSz cx=\"12192000\" cy=\"6858000\" type=\"screen16x9\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/><p:defaultTextStyle/></p:presentation>";
    package.add("ppt/presentation.xml", std::move(content), "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml");
    package.add("ppt/_rels/presentation.xml.rels", relationships(std::move(links)));
    package.add("ppt/presProps.xml", presentationRoot("presentationPr") + "</p:presentationPr>", "application/vnd.openxmlformats-officedocument.presentationml.presProps+xml");
    package.add("ppt/slideMasters/slideMaster1.xml", presentationRoot("sldMaster") + "<p:cSld><p:spTree>" + groupShape() + "</p:spTree></p:cSld>"
        "<p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/>"
        "<p:sldLayoutIdLst><p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/></p:sldLayoutIdLst><p:txStyles><p:titleStyle/><p:bodyStyle/><p:otherStyle/></p:txStyles></p:sldMaster>",
        "application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml");
    package.add("ppt/slideMasters/_rels/slideMaster1.xml.rels", relationships(
        relationship("rId1", std::string{OfficeRel} + "slideLayout", "../slideLayouts/slideLayout1.xml") +
        relationship("rId2", std::string{OfficeRel} + "theme", "../theme/theme1.xml")));
    package.add("ppt/slideLayouts/slideLayout1.xml", presentationRoot("sldLayout") + "<p:cSld name=\"Blank\"><p:spTree>" + groupShape() +
        "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>",
        "application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml");
    package.add("ppt/slideLayouts/_rels/slideLayout1.xml.rels", relationships(relationship("rId1", std::string{OfficeRel} + "slideMaster", "../slideMasters/slideMaster1.xml")));
    package.add("ppt/theme/theme1.xml", theme(), "application/vnd.openxmlformats-officedocument.theme+xml");
    package.common("ppt/presentation.xml", documentTitle);
    return Json{{"format", "pptx"}, {"slides", slides.size()}};
}

Contracts::AuthorizedPath destination(Contracts::IWorkspaceAuthority& resolver,
    const Contracts::WorkspaceAuthority& authority, std::string_view value,
    const Domain::OperationContext& context) {
    auto requested = take(Domain::PathText::create(value));
    std::optional<Domain::PathText> base;
    const auto native = take(InfrastructureDetail::strictUtf8ToUtf16(value));
    if (!std::filesystem::path{native}.is_absolute()) {
        const auto workspace = take(resolver.defaultWorkspacePath(authority, context));
        requested = take(Domain::PathText::create(workspace.value() + "\\" + std::string{value}));
    }
    // Authorize before probing. Existence alone never expands workspace grants.
    auto probe = resolver.authorize(authority, {requested, base, Domain::FileAccess::Write, true}, context);
    if (!probe) {
        // A create-only authority can create new artifacts, but the atomic store
        // enforces that its capability cannot overwrite an existing file.
        return take(resolver.authorize(authority, {requested, base, Domain::FileAccess::Create, true}, context));
    }
    auto existing = Detail::openAuthorizedObject(probe.value(), Domain::FileAccess::Write,
        InfrastructureDetail::MissingPathPolicy::Reject, context, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    if (existing) {
        if (existing.value().isDirectory()) fail(Domain::ErrorCodes::InvalidRequest, "An Office document destination must name a regular file.");
        return std::move(probe).value();
    }
    if (existing.error().code != Domain::ErrorCodes::RecordNotFound) throw Failure{std::move(existing).error()};
    return take(resolver.authorize(authority, {requested, base, Domain::FileAccess::Create, true}, context));
}
} // namespace

WindowsArtifactDocumentService::WindowsArtifactDocumentService(
    Contracts::IWorkspaceAuthority& workspaceAuthority, Contracts::IAtomicFileStore& atomicFileStore) noexcept
    : workspaceAuthority_{workspaceAuthority}, atomicFileStore_{atomicFileStore} {}

Domain::Result<std::string> WindowsArtifactDocumentService::execute(
    std::string_view name, std::string_view arguments, const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context) noexcept {
    try {
        check(context);
        bound(arguments.size(), MaximumInputBytes, "Document JSON input");
        std::vector<std::unordered_set<std::string>> keys;
        const auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
            check(context);
            if (depth < 0 || depth > 16) fail(Domain::ErrorCodes::LimitExceeded, "Document JSON nesting exceeds depth 16.");
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            else if (event == Json::parse_event_t::key) {
                if (keys.empty() || !keys.back().insert(parsed.get<std::string>()).second)
                    fail(Domain::ErrorCodes::InvalidRequest, "Document JSON contains a duplicate object key.");
            } else if (event == Json::parse_event_t::object_end) keys.pop_back();
            return true;
        };
        const auto input = Json::parse(arguments, callback, true, false);
        if (!input.is_object()) fail(Domain::ErrorCodes::InvalidRequest, "Document arguments must be a JSON object.");
        const auto path = text(required(input, "path"), "path");
        const auto extension = name == "document_write" ? ".docx" : name == "spreadsheet_write" ? ".xlsx" : name == "presentation_write" ? ".pptx" : "";
        if (*extension == '\0') fail(Domain::ErrorCodes::InvalidRequest, "Unknown Office document tool.");
        auto lowercasePath = path;
        std::transform(lowercasePath.begin(), lowercasePath.end(), lowercasePath.begin(), [](unsigned char value) {
            return static_cast<char>(value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value);
        });
        if (!lowercasePath.ends_with(extension)) fail(Domain::ErrorCodes::InvalidRequest, "The artifact path must end in " + std::string{extension} + '.');
        Package package;
        auto receipt = name == "document_write" ? document(package, input, context) :
            name == "spreadsheet_write" ? spreadsheet(package, input, context) : presentation(package, input, context);
        auto bytes = package.zip(context);
        auto authorized = destination(workspaceAuthority_, authority, path, context);
        check(context);
        take(Detail::ensureAuthorizedParentDirectories(authorized, context));
        take(atomicFileStore_.replace(authorized, bytes, false, context));
        receipt["ok"] = true;
        receipt["path"] = authorized.canonicalPath().value();
        receipt["bytes_written"] = bytes.size();
        receipt["engine"] = "forge-native-ooxml-1";
        return Domain::Result<std::string>::success(receipt.dump());
    } catch (Failure& failure) {
        return Domain::Result<std::string>::failure(std::move(failure.error));
    } catch (const Json::exception&) {
        return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InvalidRequest, "Document input must be valid bounded JSON with the supported value types."));
    } catch (...) {
        return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure, "The native Office document could not be created."));
    }
}

} // namespace ForgeConductor::NativeTools::Windows
