#include "Infrastructure/TestSupport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "ForgeConductor/NativeTools/Windows/WindowsArtifactDocumentService.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"

#include <Windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <aclapi.h>
#include <sddl.h>
#include <xmllite.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {
using Json = nlohmann::json;
using Service = NativeTools::Windows::WindowsArtifactDocumentService;
using Infrastructure::Windows::WindowsAtomicFileStore;
using Infrastructure::Windows::WindowsWorkspaceAuthority;
using Infrastructure::Windows::WindowsWorkspaceAuthorityPolicy;
namespace Detail = Infrastructure::Windows::Detail;

Domain::PathText pathText(const std::filesystem::path& value) {
    return take(Domain::PathText::create(take(Detail::strictUtf16ToUtf8(value.native()))));
}
Domain::OperationContext context() { return TestContext{}.active(); }
class Fixture final {
public:
    explicit Fixture(std::filesystem::path directory = {},
        Domain::FileAccess intent = Domain::FileAccess::Write,
        std::vector<Domain::FileAccess> grants = {Domain::FileAccess::Read, Domain::FileAccess::Write, Domain::FileAccess::Create})
        : cleanup_{directory.empty()} {
        if (directory.empty()) {
            static unsigned sequence{};
            std::wstring temporary(32768U, L'\0');
            const auto length = ::GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
            require(length != 0U && length < temporary.size(), "GetTempPathW failed.");
            temporary.resize(length);
            directory = std::filesystem::path{temporary} / ("ForgeConductor.OOXML." + std::to_string(::GetCurrentProcessId()) +
                '.' + std::to_string(::GetTickCount64()) + '.' + std::to_string(++sequence));
        }
        std::filesystem::create_directories(directory);
        root = std::filesystem::canonical(directory);
        const auto project = parse<Domain::ProjectId>("10000000-0000-4000-8000-000000000001");
        issuer = std::make_unique<WindowsWorkspaceAuthority>(std::vector<WindowsWorkspaceAuthorityPolicy>{
            {parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000001"), project,
             parse<Domain::ClientId>("artifact-document-tests"), {pathText(root)}, intent,
             std::move(grants), {}, false, 1U}});
        authority = std::make_unique<Contracts::WorkspaceAuthority>(take(issuer->authorityFor(project, context())));
        service = std::make_unique<Service>(*issuer, store);
    }
    ~Fixture() {
        if (cleanup_) { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    }
    Json write(std::string_view name, Json input) {
        return Json::parse(take(service->execute(name, input.dump(), *authority, context())));
    }
    std::filesystem::path root;
    WindowsAtomicFileStore store;
    std::unique_ptr<WindowsWorkspaceAuthority> issuer;
    std::unique_ptr<Contracts::WorkspaceAuthority> authority;
    std::unique_ptr<Service> service;
private:
    bool cleanup_;
};
std::string read(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    require(static_cast<bool>(input), "Could not read an artifact fixture.");
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}
void overwrite(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "Could not write an artifact fixture.");
}
std::uint16_t u16(std::string_view value, std::size_t offset) {
    require(offset <= value.size() && value.size() - offset >= 2U, "Truncated ZIP field.");
    return static_cast<std::uint16_t>(static_cast<unsigned char>(value[offset]) |
        (static_cast<unsigned char>(value[offset + 1U]) << 8U));
}
std::uint32_t u32(std::string_view value, std::size_t offset) {
    return u16(value, offset) | (static_cast<std::uint32_t>(u16(value, offset + 2U)) << 16U);
}
std::uint32_t checksum(std::string_view bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc ^= static_cast<unsigned char>(byte);
        for (unsigned bit = 0U; bit < 8U; ++bit) crc = (crc >> 1U) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    }
    return ~crc;
}
struct XmlElement final {
    std::string name;
    std::string namespaceUri;
    std::map<std::string, std::string> attributes;
};
std::vector<XmlElement> parseXml(std::string_view xml) {
    Microsoft::WRL::ComPtr<IStream> stream;
    require(SUCCEEDED(::CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf())), "Create XML input stream failed.");
    ULONG written{};
    require(SUCCEEDED(stream->Write(xml.data(), static_cast<ULONG>(xml.size()), &written)) && written == xml.size(),
            "XML input stream write failed.");
    LARGE_INTEGER beginning{};
    require(SUCCEEDED(stream->Seek(beginning, STREAM_SEEK_SET, nullptr)), "XML input rewind failed.");
    Microsoft::WRL::ComPtr<IXmlReader> reader;
    require(SUCCEEDED(::CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr)),
            "Create Windows XML parser failed.");
    require(SUCCEEDED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)), "XML DTD policy failed.");
    require(SUCCEEDED(reader->SetInput(stream.Get())), "Set XML parser input failed.");
    std::vector<XmlElement> elements;
    XmlNodeType type{};
    HRESULT status{};
    while ((status = reader->Read(&type)) == S_OK) {
        if (type != XmlNodeType_Element) continue;
        const wchar_t* name{};
        UINT length{};
        require(SUCCEEDED(reader->GetLocalName(&name, &length)), "XML element name read failed.");
        XmlElement element;
        element.name = take(Detail::strictUtf16ToUtf8(std::wstring_view{name, length}));
        require(SUCCEEDED(reader->GetNamespaceUri(&name, &length)), "XML namespace read failed.");
        element.namespaceUri = take(Detail::strictUtf16ToUtf8(std::wstring_view{name, length}));
        if (reader->MoveToFirstAttribute() == S_OK) {
            do {
                require(SUCCEEDED(reader->GetLocalName(&name, &length)), "XML attribute name read failed.");
                const auto key = take(Detail::strictUtf16ToUtf8(std::wstring_view{name, length}));
                require(SUCCEEDED(reader->GetValue(&name, &length)), "XML attribute value read failed.");
                element.attributes[key] = take(Detail::strictUtf16ToUtf8(std::wstring_view{name, length}));
            } while (reader->MoveToNextAttribute() == S_OK);
            require(reader->MoveToElement() == S_OK, "XML element restore failed.");
        }
        elements.push_back(std::move(element));
    }
    require(status == S_FALSE && !elements.empty(), "The generated package contains malformed XML.");
    return elements;
}
using Parts = std::map<std::string, std::string>;
Parts unzip(std::string_view zip) {
    require(zip.size() >= 22U, "ZIP is too short.");
    const auto end = zip.size() - 22U;
    require(u32(zip, end) == 0x06054b50U && u16(zip, end + 4U) == 0U && u16(zip, end + 6U) == 0U,
            "ZIP end-of-directory is invalid.");
    const auto count = u16(zip, end + 10U);
    require(count == u16(zip, end + 8U) && u16(zip, end + 20U) == 0U, "ZIP entry count is inconsistent.");
    const auto centralStart = u32(zip, end + 16U);
    require(centralStart + u32(zip, end + 12U) == end, "ZIP central directory extent is invalid.");
    Parts parts;
    std::size_t cursor = centralStart;
    std::size_t nextLocal{};
    for (unsigned index = 0U; index < count; ++index) {
        require(u32(zip, cursor) == 0x02014b50U && u16(zip, cursor + 10U) == 0U, "ZIP central entry is invalid.");
        const auto crc = u32(zip, cursor + 16U);
        const auto size = u32(zip, cursor + 20U);
        const auto nameBytes = u16(zip, cursor + 28U);
        const auto name = std::string{zip.substr(cursor + 46U, nameBytes)};
        const auto local = u32(zip, cursor + 42U);
        require(local == nextLocal && u32(zip, local) == 0x04034b50U && u16(zip, local + 8U) == 0U,
                "ZIP local entry is missing, overlapping or compressed unexpectedly.");
        require(u32(zip, local + 14U) == crc && u32(zip, local + 18U) == size &&
                    u32(zip, local + 22U) == size && u32(zip, cursor + 24U) == size,
                "ZIP local/central checksums or sizes disagree.");
        const auto localNameBytes = u16(zip, local + 26U);
        require(name == zip.substr(local + 30U, localNameBytes) && u16(zip, local + 28U) == 0U,
                "ZIP local/central names disagree.");
        const auto data = static_cast<std::size_t>(local) + 30U + localNameBytes;
        require(data <= centralStart && size <= centralStart - data, "ZIP part content is out of bounds.");
        auto content = std::string{zip.substr(data, size)};
        require(checksum(content) == crc && parts.emplace(name, content).second,
                "ZIP payload checksum is invalid or its name is duplicated.");
        static_cast<void>(parseXml(content));
        nextLocal = data + size;
        cursor += 46U + nameBytes + u16(zip, cursor + 30U) + u16(zip, cursor + 32U);
    }
    require(cursor == end && nextLocal == centralStart, "ZIP directory or local entries are not complete.");
    require(parts.contains("[Content_Types].xml") && parts.contains("_rels/.rels"), "Office package roots are missing.");
    for (const auto& element : parseXml(parts.at("[Content_Types].xml"))) {
        if (element.name == "Override") require(parts.contains(element.attributes.at("PartName").substr(1U)), "Content type points to a missing part.");
    }
    for (const auto& [part, content] : parts) {
        if (!part.ends_with(".rels")) continue;
        const auto relPath = std::filesystem::path{part};
        const auto sourceDirectory = relPath.parent_path().parent_path();
        for (const auto& element : parseXml(content)) {
            if (element.name != "Relationship") continue;
            require(element.namespaceUri == "http://schemas.openxmlformats.org/package/2006/relationships",
                    "Package relationship namespace is incorrect.");
            const auto target = (sourceDirectory / element.attributes.at("Target")).lexically_normal().generic_string();
            require(parts.contains(target), "An Office relationship points to a missing part.");
        }
    }
    return parts;
}
void generateAndValidate(Fixture& fixture) {
    const auto wordPath = fixture.root / L"reports" / L"document.docx";
    const auto wordReceipt = fixture.write("document_write", Json{{"path", pathText(wordPath).value()},
        {"title", "Native <title> & \"quotes\""}, {"paragraphs", Json::array({"  preserved  ", "line1\r\nline2\ttab", "Unicode \xe2\x82\xac"})}});
    const auto wordBytes = read(wordPath);
    require(wordReceipt.at("bytes_written") == wordBytes.size() && wordReceipt.at("paragraphs") == 3U && wordReceipt.at("format") == "docx", "Word receipt is inaccurate.");
    const auto word = unzip(wordBytes);
    require(word.at("word/document.xml").find("Native &lt;title&gt; &amp; &quot;quotes&quot;") != std::string::npos &&
                word.at("word/document.xml").find("<w:br/>") != std::string::npos && word.at("word/document.xml").find("<w:tab/>") != std::string::npos,
            "Word text escaping, paragraphs or controls were lost.");
    require(parseXml(word.at("word/document.xml")).front().namespaceUri == "http://schemas.openxmlformats.org/wordprocessingml/2006/main", "Word namespace is incorrect.");
    const auto spreadsheetPath = fixture.root / L"tables.xlsx";
    const auto spreadsheetReceipt = fixture.write("spreadsheet_write", Json{{"path", pathText(spreadsheetPath).value()},
        {"sheets", Json::array({Json{{"name", "Data & Facts"}, {"rows", Json::array({Json::array({"=SUM(A1)", 42, true, nullptr, "_x0041_", "\xe2\x82\xac"}), Json::array({"<tag>", 1.25, false})})}},
            Json{{"name", "Empty"}, {"rows", Json::array()}}})}});
    const auto spreadsheetBytes = read(spreadsheetPath);
    require(spreadsheetReceipt.at("bytes_written") == spreadsheetBytes.size() && spreadsheetReceipt.at("sheets") == 2U &&
                spreadsheetReceipt.at("rows") == 2U && spreadsheetReceipt.at("cells") == 9U, "Spreadsheet receipt is inaccurate.");
    const auto spreadsheet = unzip(spreadsheetBytes);
    const auto& cells = spreadsheet.at("xl/worksheets/sheet1.xml");
    require(cells.find("<f>") == std::string::npos && cells.find("=SUM(A1)") != std::string::npos &&
                cells.find("t=\"b\"><v>1</v>") != std::string::npos && cells.find("t=\"n\"><v>42</v>") != std::string::npos &&
                cells.find("_x005F_x0041_") != std::string::npos && cells.find("r=\"E1\"") != std::string::npos,
            "Spreadsheet lost literal text, types, blank alignment or escaped-string semantics.");
    const auto boundaryPath = fixture.root / L"numeric-boundaries.xlsx";
    const auto minimum = std::numeric_limits<double>::min();
    const auto boundaryReceipt = fixture.write("spreadsheet_write", Json{{"path", pathText(boundaryPath).value()},
        {"sheets", Json::array({Json{{"name", "Numeric limits"}, {"rows", Json::array({Json::array({0.0, -0.0, minimum, -minimum, "1e-309"})})}}})}});
    const auto boundaryBytes = read(boundaryPath);
    const auto boundaryPackage = unzip(boundaryBytes);
    const auto& boundaryCells = boundaryPackage.at("xl/worksheets/sheet1.xml");
    require(boundaryReceipt.at("cells") == 5U && boundaryReceipt.at("bytes_written") == boundaryBytes.size() &&
        boundaryCells.find("r=\"A1\" t=\"n\"><v>0.0</v>") != std::string::npos &&
        boundaryCells.find("r=\"B1\" t=\"n\"><v>-0.0</v>") != std::string::npos &&
        boundaryCells.find("r=\"C1\" t=\"n\"><v>2.2250738585072014e-308</v>") != std::string::npos &&
        boundaryCells.find("r=\"D1\" t=\"n\"><v>-2.2250738585072014e-308</v>") != std::string::npos &&
        boundaryCells.find("r=\"E1\" t=\"inlineStr\"><is><t xml:space=\"preserve\">1e-309</t>") != std::string::npos,
        "Zero/minimum-normal numeric boundaries or the literal text alternative were lost.");
    const auto presentationPath = fixture.root / L"presentation.pptx";
    const auto presentationReceipt = fixture.write("presentation_write", Json{{"path", pathText(presentationPath).value()}, {"title", "Deck"},
        {"slides", Json::array({Json{{"title", "First <slide>"}, {"body", Json::array({"body & details", "line1\nline2"})}},
            Json{{"title", "Second"}, {"body", Json::array()}}})}});
    const auto presentationBytes = read(presentationPath);
    require(presentationReceipt.at("bytes_written") == presentationBytes.size() && presentationReceipt.at("slides") == 2U, "Presentation receipt is inaccurate.");
    const auto presentation = unzip(presentationBytes);
    require(presentation.contains("ppt/slideMasters/slideMaster1.xml") && presentation.contains("ppt/slideLayouts/slideLayout1.xml") &&
                presentation.contains("ppt/theme/theme1.xml") && presentation.contains("ppt/presProps.xml") &&
                presentation.at("ppt/slides/slide1.xml").find("First &lt;slide&gt;") != std::string::npos &&
                presentation.at("ppt/slides/slide1.xml").find("<a:br/>") != std::string::npos &&
                presentation.at("ppt/presentation.xml").find("id=\"257\"") != std::string::npos,
            "Presentation omitted required parts or slide content.");
    // The same authorized path supports atomic replacement, not append or an
    // output-mode-dependent malformed package.
    fixture.write("document_write", Json{{"path", pathText(wordPath).value()}, {"paragraphs", Json::array({"replacement"})}});
    require(unzip(read(wordPath)).at("word/document.xml").find("replacement") != std::string::npos,
            "An existing document could not be atomically replaced.");
    const auto relativeReceipt = fixture.write("document_write", Json{{"path", "reports/relative.DOCX"},
        {"paragraphs", Json::array({"relative workspace path"})}});
    require(relativeReceipt.at("path") == pathText(fixture.root / L"reports" / L"relative.DOCX").value() &&
                unzip(read(fixture.root / L"reports" / L"relative.DOCX")).contains("word/document.xml"),
            "A relative artifact path was not resolved inside the trusted root.");
}
void rejectInvalidInputsBeforePublication() {
    Fixture fixture;
    const auto target = fixture.root / L"keep.docx";
    overwrite(target, "original bytes");
    const Json base{{"path", pathText(target).value()}, {"paragraphs", Json::array({"valid"})}};
    for (const auto& value : {Json{{"path", pathText(target).value()}, {"paragraphs", Json::array({std::string{"bad\1", 4U}})}},
                             Json{{"path", pathText(target).value()}, {"paragraphs", Json::array({Json::object()})}},
                             Json{{"path", pathText(target).value()}, {"paragraphs", Json::array()}, {"unexpected", true}}}) {
        requireError(fixture.service->execute("document_write", value.dump(), *fixture.authority, context()), Domain::ErrorCodes::InvalidRequest, "Malformed document input was accepted.");
        require(read(target) == "original bytes", "Invalid input modified an existing artifact.");
    }
    auto oversized = base;
    oversized["paragraphs"] = Json::array({std::string(Service::MaximumTextBytes + 1U, 'x')});
    requireError(fixture.service->execute("document_write", oversized.dump(), *fixture.authority, context()), Domain::ErrorCodes::PayloadTooLarge, "Oversized document text was accepted.");
    requireError(fixture.service->execute("document_write", "{\"path\":\"a.docx\",\"path\":\"b.docx\",\"paragraphs\":[]}", *fixture.authority, context()), Domain::ErrorCodes::InvalidRequest, "Duplicate JSON keys were accepted.");
    requireError(fixture.service->execute("document_write", "not JSON", *fixture.authority, context()), Domain::ErrorCodes::InvalidRequest, "Malformed JSON was accepted.");
    std::string invalidUtf8{"{\"path\":\"invalid.docx\",\"paragraphs\":[\""};
    invalidUtf8.push_back(static_cast<char>(0xff));
    invalidUtf8 += "\"]}";
    requireError(fixture.service->execute("document_write", invalidUtf8, *fixture.authority, context()), Domain::ErrorCodes::InvalidRequest, "Invalid UTF-8 document input was accepted.");
    auto tooManyParagraphs = base;
    tooManyParagraphs["paragraphs"] = std::vector<std::string>(Service::MaximumParagraphs + 1U);
    requireError(fixture.service->execute("document_write", tooManyParagraphs.dump(), *fixture.authority, context()), Domain::ErrorCodes::PayloadTooLarge, "The document paragraph bound was ignored.");
    std::string deep{"{\"path\":\"deep.docx\",\"paragraphs\":"};
    deep += std::string(18U, '[') + "\"nested\"" + std::string(18U, ']') + '}';
    requireError(fixture.service->execute("document_write", deep, *fixture.authority, context()), Domain::ErrorCodes::LimitExceeded, "The document JSON depth bound was ignored.");
    Json sheets{{"path", pathText(fixture.root / L"bad.xlsx").value()}, {"sheets", Json::array({Json{{"name", "Data"}, {"rows", Json::array()}}, Json{{"name", "data"}, {"rows", Json::array()}}})}};
    requireError(fixture.service->execute("spreadsheet_write", sheets.dump(), *fixture.authority, context()), Domain::ErrorCodes::InvalidRequest, "Case-insensitive duplicate sheet names were accepted.");
    sheets["sheets"] = Json::array({Json{{"name", "bad/name"}, {"rows", Json::array()}}});
    requireError(fixture.service->execute("spreadsheet_write", sheets.dump(), *fixture.authority, context()), Domain::ErrorCodes::InvalidRequest, "Invalid sheet names were accepted.");
    sheets["sheets"] = Json::array({Json{{"name", "Data"}, {"rows", Json::array({Json::array({1234567890123456LL})})}}});
    requireError(fixture.service->execute("spreadsheet_write", sheets.dump(), *fixture.authority, context()), Domain::ErrorCodes::InvalidRequest, "A lossy large integer was accepted as an Excel numeric cell.");
    const auto numericTarget = fixture.root / L"numeric-preserved.xlsx";
    overwrite(numericTarget, "preserved numeric artifact"); sheets["path"] = pathText(numericTarget).value();
    for (const auto subnormal : {1.0e-309, -1.0e-309}) {
        sheets["sheets"] = Json::array({Json{{"name", "Data"}, {"rows", Json::array({Json::array({subnormal})})}}});
        requireError(fixture.service->execute("spreadsheet_write", sheets.dump(), *fixture.authority, context()),
            Domain::ErrorCodes::InvalidRequest, "A nonzero subnormal was accepted as an Excel numeric cell.");
        require(read(numericTarget) == "preserved numeric artifact", "Rejected subnormal input modified the existing artifact.");
    }
    sheets["sheets"] = Json::array({Json{{"name", "Data"}, {"rows", Json::array({std::vector<std::string>(Service::MaximumColumns + 1U)})}}});
    requireError(fixture.service->execute("spreadsheet_write", sheets.dump(), *fixture.authority, context()), Domain::ErrorCodes::PayloadTooLarge, "The spreadsheet column bound was ignored.");
    sheets["sheets"] = Json::array({Json{{"name", "Data"}, {"rows", Json::array({Json::array({std::string(32'768U, 'x')})})}}});
    requireError(fixture.service->execute("spreadsheet_write", sheets.dump(), *fixture.authority, context()), Domain::ErrorCodes::PayloadTooLarge, "The spreadsheet UTF-16 cell text bound was ignored.");
    const auto emptyRow = Json::array();
    sheets["sheets"] = Json::array({Json{{"name", "Data"}, {"rows", std::vector<Json>(Service::MaximumRowsPerSheet + 1U, emptyRow)}}});
    requireError(fixture.service->execute("spreadsheet_write", sheets.dump(), *fixture.authority, context()), Domain::ErrorCodes::PayloadTooLarge, "The spreadsheet row bound was ignored.");
    const auto fullRow = Json(std::vector<Json>(Service::MaximumColumns, nullptr));
    sheets["sheets"] = Json::array({Json{{"name", "Data"}, {"rows", std::vector<Json>(Service::MaximumCells / Service::MaximumColumns + 1U, fullRow)}}});
    requireError(fixture.service->execute("spreadsheet_write", sheets.dump(), *fixture.authority, context()), Domain::ErrorCodes::PayloadTooLarge, "The spreadsheet total cell bound was ignored.");
    require(!std::filesystem::exists(fixture.root / L"bad.xlsx"), "Invalid spreadsheet input published an artifact.");
    Json slides{{"path", "bad.pptx"}, {"slides", std::vector<Json>(Service::MaximumSlides + 1U,
        Json{{"title", "slide"}, {"body", Json::array()}})}};
    requireError(fixture.service->execute("presentation_write", slides.dump(), *fixture.authority, context()), Domain::ErrorCodes::PayloadTooLarge, "The presentation slide bound was ignored.");
    slides["slides"] = Json::array({Json{{"title", "slide"}, {"body", std::vector<std::string>(Service::MaximumSlideParagraphs + 1U)}}});
    requireError(fixture.service->execute("presentation_write", slides.dump(), *fixture.authority, context()), Domain::ErrorCodes::PayloadTooLarge, "The slide paragraph bound was ignored.");
    require(!std::filesystem::exists(fixture.root / L"bad.pptx"), "Invalid presentation input published an artifact.");
    std::stop_source cancelled;
    cancelled.request_stop();
    auto stopped = context(); stopped.cancellation = cancelled.get_token();
    requireError(fixture.service->execute("document_write", base.dump(), *fixture.authority, stopped), Domain::ErrorCodes::Cancelled, "Document creation ignored cancellation.");
    auto expired = context(); expired.deadline = std::chrono::steady_clock::now();
    requireError(fixture.service->execute("document_write", base.dump(), *fixture.authority, expired), Domain::ErrorCodes::DeadlineExceeded, "Document creation ignored deadline.");
    require(read(target) == "original bytes", "Rejected input or cancellation modified an artifact.");
}
void authorityAclAndAtomicFailuresPreserveTargets() {
    Fixture fixture;
    const auto outside = fixture.root.parent_path() / L"outside-artifact.docx";
    const Json denied{{"path", pathText(outside).value()}, {"paragraphs", Json::array({"denied"})}};
    requireError(fixture.service->execute("document_write", denied.dump(), *fixture.authority, context()), Domain::ErrorCodes::PathOutsideAuthority, "Artifact writer escaped workspace authority.");
    Fixture readOnly{fixture.root, Domain::FileAccess::Read, {Domain::FileAccess::Read}};
    const auto target = fixture.root / L"locked.docx";
    overwrite(target, "keep locked bytes");
    const Json input{{"path", pathText(target).value()}, {"paragraphs", Json::array({"new content"})}};
    requireError(readOnly.service->execute("document_write", input.dump(), *readOnly.authority, context()), Domain::ErrorCodes::Unauthorized, "A read-only authority wrote an artifact.");
    Fixture createOnly{fixture.root, Domain::FileAccess::Create, {Domain::FileAccess::Create}};
    requireError(createOnly.service->execute("document_write", input.dump(), *createOnly.authority, context()), Domain::ErrorCodes::Unauthorized, "Create-only authority overwrote an existing artifact.");
    const Json newInput{{"path", pathText(fixture.root / L"create-only.docx").value()}, {"paragraphs", Json::array({"created"})}};
    require(createOnly.service->execute("document_write", newInput.dump(), *createOnly.authority, context()).hasValue(), "Create-only authority could not create a new artifact.");
    {
        Detail::UniqueHandle held{::CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        require(static_cast<bool>(held), "Could not retain the non-delete-sharing artifact reader.");
        requireError(fixture.service->execute("document_write", input.dump(), *fixture.authority, context()), Domain::ErrorCodes::Conflict, "A blocked atomic replacement did not return a conflict.");
        require(read(target) == "keep locked bytes", "A failed atomic replacement damaged the existing file.");
    }
    const auto restricted = fixture.root / L"restricted";
    std::filesystem::create_directory(restricted);
    PSECURITY_DESCRIPTOR raw{};
    require(::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;;0x6;;;WD)(A;;FA;;;WD)", SDDL_REVISION_1, &raw, nullptr) != FALSE, "Could not create denied-creation DACL.");
    PACL acl{}; BOOL present{}, defaulted{};
    require(::GetSecurityDescriptorDacl(raw, &present, &acl, &defaulted) != FALSE && present, "Could not read denied-creation DACL.");
    auto native = restricted.native();
    const auto applied = ::SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    ::LocalFree(raw);
    require(applied == ERROR_SUCCESS, "Could not apply denied-creation DACL.");
    const Json blocked{{"path", pathText(restricted / L"blocked.docx").value()}, {"paragraphs", Json::array({"blocked"})}};
    require(!fixture.service->execute("document_write", blocked.dump(), *fixture.authority, context()), "An OS ACL creation denial was ignored.");
    require(!std::filesystem::exists(restricted / L"blocked.docx"), "An OS ACL denial published an artifact.");
}
} // namespace
} // namespace ForgeConductor::Tests

int main(int argc, const char* const argv[]) {
    using namespace ForgeConductor::Tests;
    try {
        if (argc == 3 && std::string_view{argv[1]} == "--emit-fixtures") {
            Fixture fixture{std::filesystem::path{argv[2]}};
            generateAndValidate(fixture);
            std::cout << "PASS emitted DOCX/XLSX/PPTX fixtures\n";
            return 0;
        }
        require(argc == 1, "Unexpected artifact document test arguments.");
        Fixture fixture;
        generateAndValidate(fixture);
        std::cout << "PASS artifact_documents.packages_xml_crc_relationships\n";
        rejectInvalidInputsBeforePublication();
        std::cout << "PASS artifact_documents.input_bounds_cancellation\n";
        authorityAclAndAtomicFailuresPreserveTargets();
        std::cout << "PASS artifact_documents.authority_acl_atomic_publication\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
