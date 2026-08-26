#include "ForgeConductor/NativeTools/Windows/WindowsPdfService.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "Fakes/AtomicFileStoreFake.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Infrastructure/Windows/Detail/WindowsPathResolver.h"

#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;
namespace NativeTools = ForgeConductor::NativeTools::Windows;

std::atomic_size_t assertionCount{};

void require(const bool condition, const std::string_view expression)
{
    assertionCount.fetch_add(1U, std::memory_order_relaxed);
    if (!condition) {
        throw std::runtime_error{"Requirement failed: " + std::string{expression}};
    }
}

#define REQUIRE(condition) require(static_cast<bool>(condition), #condition)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

template <typename T>
void requireError(const Domain::Result<T>& result, const std::string_view code)
{
    REQUIRE(!result);
    REQUIRE(result.error().code == code);
}

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::PathText filesystemPath(
    const std::filesystem::path& value)
{
    return take(ForgeConductor::Infrastructure::Windows::Detail::
        WindowsPathResolver::toPathText(value.wstring()));
}

[[nodiscard]] Domain::OperationContext context(
    const std::chrono::steady_clock::time_point now,
    const std::stop_token cancellation = {},
    const bool expired = false)
{
    return Domain::OperationContext{
        parse<Domain::OperationId>("10101010-1010-4010-8010-101010101010"),
        now + (expired ? 0s : 30s),
        cancellation,
        parse<Domain::CorrelationId>("pdf-service-tests")};
}

struct Fixture final {
    Fixture()
        : authorityIssuer{
              parse<Domain::AuthorityId>("20202020-2020-4020-8020-202020202020"),
              parse<Domain::ClientId>("pdf-service-client"),
              {root},
              Domain::FileAccess::Write,
              {Domain::FileAccess::Read, Domain::FileAccess::Write},
              {},
              false,
              1U},
          atomicFileStore{Contracts::IPdfService::MaximumPdfBytes},
          service{atomicFileStore}
    {
        authorityIssuer.setNow(now);
        atomicFileStore.setNow(now);
        const auto operation = context(now);
        authority.emplace(take(authorityIssuer.authorityFor(
            parse<Domain::ProjectId>("30303030-3030-4030-8030-303030303030"),
            operation)));
        source.emplace(take(authorityIssuer.authorize(
            authority.value(),
            Domain::PathAuthorizationRequest{
                path("C:/pdf-service/source.txt"), root,
                Domain::FileAccess::Read, false},
            operation)));
        destination.emplace(take(authorityIssuer.authorize(
            authority.value(),
            Domain::PathAuthorizationRequest{
                path("C:/pdf-service/output.pdf"), root,
                Domain::FileAccess::Write, false},
            operation)));
        atomicFileStore.replaceResult.set(Domain::Result<void>::success());
    }

    [[nodiscard]] const Contracts::AuthorizedPath& sourcePath() const
    {
        return source.value();
    }

    [[nodiscard]] const Contracts::AuthorizedPath& destinationPath() const
    {
        return destination.value();
    }

    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const Domain::PathText root = path("C:/pdf-service");
    Fakes::DeterministicWorkspaceAuthority authorityIssuer;
    Fakes::RecordingAtomicFileStoreFake atomicFileStore;
    NativeTools::WindowsPdfService service;
    std::optional<Contracts::WorkspaceAuthority> authority;
    std::optional<Contracts::AuthorizedPath> source;
    std::optional<Contracts::AuthorizedPath> destination;
};

[[nodiscard]] std::string capturedPdf(const Fixture& fixture)
{
    REQUIRE(fixture.atomicFileStore.lastReplace().has_value());
    const auto& bytes =
        fixture.atomicFileStore.lastReplace()->capturedContent;
    std::string result;
    result.reserve(bytes.size());
    for (const auto byte : bytes) {
        result.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    }
    return result;
}

void writesValidEscapedPdfAndReceipt()
{
    Fixture fixture;
    const auto receipt = take(fixture.service.write(
        "P13 title",
        "# Heading\nA (test) \\ path\n- item",
        fixture.destinationPath(),
        context(fixture.now)));
    const auto pdf = capturedPdf(fixture);

    REQUIRE(pdf.starts_with("%PDF-1.4\n"));
    REQUIRE(pdf.find("/Type /Catalog") != std::string::npos);
    REQUIRE(pdf.find("/Type /Pages") != std::string::npos);
    REQUIRE(pdf.find("A \\(test\\) \\\\ path") != std::string::npos);
    REQUIRE(pdf.find("xref\n") != std::string::npos);
    REQUIRE(pdf.find("trailer\n") != std::string::npos);
    REQUIRE(pdf.ends_with("%%EOF\n"));
    REQUIRE(receipt.path == fixture.destinationPath().canonicalPath());
    REQUIRE(receipt.bytesWritten == pdf.size());
    REQUIRE(receipt.pages == 1U);
    REQUIRE(receipt.engine == "forge-native-pdf-1.4");
    REQUIRE(receipt.title == "P13 title");

    const auto marker = pdf.rfind("startxref\n");
    REQUIRE(marker != std::string::npos);
    const auto numberStart = marker + std::string_view{"startxref\n"}.size();
    const auto numberEnd = pdf.find('\n', numberStart);
    REQUIRE(numberEnd != std::string::npos);
    const auto xrefOffset = static_cast<std::size_t>(
        std::stoull(pdf.substr(numberStart, numberEnd - numberStart)));
    REQUIRE(pdf.substr(xrefOffset).starts_with("xref\n"));
}

void createsMultiplePagesAndConvertsTextFiles()
{
    Fixture fixture;
    std::string body;
    for (std::size_t index = 0; index < 180U; ++index) {
        body.append("Line ");
        body.append(std::to_string(index));
        body.append(" contains bounded document text.\n");
    }
    const auto multipage = take(fixture.service.write(
        "Multipage", body, fixture.destinationPath(), context(fixture.now)));
    REQUIRE(multipage.pages > 1U);
    REQUIRE(capturedPdf(fixture).find("/Count " + std::to_string(multipage.pages)) !=
            std::string::npos);

    const std::string sourceText{"## Source\nConverted text"};
    std::vector<std::byte> sourceBytes;
    sourceBytes.reserve(sourceText.size());
    for (const unsigned char byte : sourceText) {
        sourceBytes.push_back(static_cast<std::byte>(byte));
    }
    fixture.atomicFileStore.readResult.set(
        Domain::Result<std::vector<std::byte>>::success(std::move(sourceBytes)));
    const auto before = fixture.atomicFileStore.calls();
    const auto converted = take(fixture.service.fromTextFile(
        "Converted", fixture.sourcePath(), fixture.destinationPath(), context(fixture.now)));
    REQUIRE(fixture.atomicFileStore.calls() == before + 2U);
    REQUIRE(converted.pages == 1U);
    REQUIRE(capturedPdf(fixture).find("Converted text") != std::string::npos);
}

void rejectsInvalidAndOversizedInputBeforeWriting()
{
    Fixture fixture;
    const auto initialCalls = fixture.atomicFileStore.calls();
    const std::string malformed{"\xF0\x28\x8C\x28", 4U};
    requireError(
        fixture.service.write(
            "bad", malformed, fixture.destinationPath(), context(fixture.now)),
        Domain::ErrorCodes::InvalidRequest);
    requireError(
        fixture.service.write(
            "bad", std::string{"nul\0text", 8U}, fixture.destinationPath(),
            context(fixture.now)),
        Domain::ErrorCodes::InvalidRequest);
    requireError(
        fixture.service.write(
            std::string(Contracts::IPdfService::MaximumTitleBytes + 1U, 't'),
            "body", fixture.destinationPath(), context(fixture.now)),
        Domain::ErrorCodes::PayloadTooLarge);
    requireError(
        fixture.service.write(
            "title",
            std::string(Contracts::IPdfService::MaximumTextBytes + 1U, 'b'),
            fixture.destinationPath(), context(fixture.now)),
        Domain::ErrorCodes::PayloadTooLarge);
    REQUIRE(fixture.atomicFileStore.calls() == initialCalls);

    fixture.atomicFileStore.readResult.set(
        Domain::Result<std::vector<std::byte>>::success(
            {std::byte{0xC3U}, std::byte{0x28U}}));
    requireError(
        fixture.service.fromTextFile(
            "bad source", fixture.sourcePath(), fixture.destinationPath(),
            context(fixture.now)),
        Domain::ErrorCodes::InvalidRequest);
}

void honorsCancellationAndDeadlinesBeforeEffects()
{
    Fixture fixture;
    std::stop_source stop;
    stop.request_stop();
    const auto before = fixture.atomicFileStore.calls();
    requireError(
        fixture.service.write(
            "cancelled", "body", fixture.destinationPath(),
            context(fixture.now, stop.get_token())),
        Domain::ErrorCodes::Cancelled);
    requireError(
        fixture.service.write(
            "expired", "body", fixture.destinationPath(),
            context(fixture.now, {}, true)),
        Domain::ErrorCodes::DeadlineExceeded);
    REQUIRE(fixture.atomicFileStore.calls() == before);
}

void boundsMaximumUnbrokenLineWithAnActiveDeadline()
{
    Fixture fixture;
    const std::string unbroken(
        Contracts::IPdfService::MaximumTextBytes, 'x');
    const auto started = std::chrono::steady_clock::now();
    auto deadline = context(started);
    deadline.deadline = started + 5ms;
    const auto before = fixture.atomicFileStore.calls();
    requireError(
        fixture.service.write(
            "deadline", unbroken, fixture.destinationPath(), deadline),
        Domain::ErrorCodes::DeadlineExceeded);
    REQUIRE(fixture.atomicFileStore.calls() == before);
}

void boundsMaximumTabExpansionWithAnActiveDeadline()
{
    Fixture fixture;
    std::string codeBlock{"```\n"};
    codeBlock.append(
        Contracts::IPdfService::MaximumTextBytes - 8U,
        '\t');
    codeBlock.append("\n```");
    REQUIRE(codeBlock.size() == Contracts::IPdfService::MaximumTextBytes);

    const auto started = std::chrono::steady_clock::now();
    auto deadline = context(started);
    deadline.deadline = started + 5ms;
    const auto before = fixture.atomicFileStore.calls();
    requireError(
        fixture.service.write(
            "tab deadline", codeBlock, fixture.destinationPath(), deadline),
        Domain::ErrorCodes::DeadlineExceeded);
    REQUIRE(fixture.atomicFileStore.calls() == before);
    REQUIRE(std::chrono::steady_clock::now() - started < 5s);
}

void validateWithWindowsDataPdf(const std::filesystem::path& pdfPath)
{
    const auto file =
        winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(
            pdfPath.wstring()).get();
    const auto document =
        winrt::Windows::Data::Pdf::PdfDocument::LoadFromFileAsync(file).get();
    REQUIRE(document.PageCount() >= 1U);
    const auto page = document.GetPage(0U);
    winrt::Windows::Storage::Streams::InMemoryRandomAccessStream rendered;
    page.RenderToStreamAsync(rendered).get();
    REQUIRE(rendered.Size() > 0U);
    page.Close();
    rendered.Close();
}

void publishesBinaryPdfThroughTheWindowsAtomicStore()
{
    const auto suffix = std::to_wstring(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() /
        (std::wstring{L"forge-conductor-p13-pdf-"} + suffix);
    std::error_code cleanupError;
    struct Cleanup final {
        const std::filesystem::path& root;
        std::error_code& error;
        ~Cleanup() { std::filesystem::remove_all(root, error); }
    } cleanup{root, cleanupError};
    REQUIRE(std::filesystem::create_directories(root));

    const auto now = std::chrono::steady_clock::now();
    Fakes::DeterministicWorkspaceAuthority issuer{
        parse<Domain::AuthorityId>("40404040-4040-4040-8040-404040404040"),
        parse<Domain::ClientId>("pdf-windows-store-client"),
        {filesystemPath(root)},
        Domain::FileAccess::Write,
        {Domain::FileAccess::Read, Domain::FileAccess::Write,
         Domain::FileAccess::Create},
        {},
        false,
        1U};
    issuer.setNow(now);
    const auto operation = context(now);
    const auto authority = take(issuer.authorityFor(
        parse<Domain::ProjectId>("50505050-5050-4050-8050-505050505050"),
        operation));
    const auto destinationFile =
        root / L"nested" / L"publication" / L"native-output.pdf";
    const auto destination = take(issuer.authorize(
        authority,
        Domain::PathAuthorizationRequest{
            filesystemPath(destinationFile), filesystemPath(root),
            Domain::FileAccess::Create, true},
        operation));

    ForgeConductor::Infrastructure::Windows::WindowsAtomicFileStore store;
    NativeTools::WindowsPdfService service{store};
    const auto receipt = take(service.write(
        "Binary publication", "The real Windows store writes this PDF.",
        destination, operation));
    REQUIRE(receipt.bytesWritten > 16U);
    REQUIRE(std::filesystem::is_regular_file(destinationFile));
    REQUIRE(std::filesystem::is_directory(destinationFile.parent_path()));

    std::ifstream input{destinationFile, std::ios::binary};
    REQUIRE(input.good());
    const std::vector<unsigned char> bytes{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    REQUIRE(bytes.size() == receipt.bytesWritten);
    REQUIRE(bytes[0] == static_cast<unsigned char>('%'));
    REQUIRE(bytes[9] == static_cast<unsigned char>('%'));
    REQUIRE(bytes[10] == 0xE2U);
    REQUIRE(bytes[11] == 0xE3U);
    REQUIRE(bytes[12] == 0xCFU);
    REQUIRE(bytes[13] == 0xD3U);
    input.close();

    validateWithWindowsDataPdf(destinationFile);
}

} // namespace

int main()
{
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        writesValidEscapedPdfAndReceipt();
        std::cout << "PASS native_tools.pdf_valid_escape_receipt\n";
        createsMultiplePagesAndConvertsTextFiles();
        std::cout << "PASS native_tools.pdf_multipage_conversion\n";
        rejectsInvalidAndOversizedInputBeforeWriting();
        std::cout << "PASS native_tools.pdf_input_bounds\n";
        honorsCancellationAndDeadlinesBeforeEffects();
        std::cout << "PASS native_tools.pdf_context\n";
        boundsMaximumUnbrokenLineWithAnActiveDeadline();
        std::cout << "PASS native_tools.pdf_maximum_unbroken_deadline\n";
        boundsMaximumTabExpansionWithAnActiveDeadline();
        std::cout << "PASS native_tools.pdf_maximum_tab_deadline\n";
        publishesBinaryPdfThroughTheWindowsAtomicStore();
        std::cout << "PASS native_tools.pdf_windows_runtime_load_render\n";
        winrt::uninit_apartment();
        std::cout << "SUMMARY passed=7 failed=0 assertions="
                  << assertionCount.load(std::memory_order_relaxed) << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        winrt::uninit_apartment();
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
