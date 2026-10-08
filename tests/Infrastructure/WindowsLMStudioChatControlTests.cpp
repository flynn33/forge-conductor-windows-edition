#include "TestSupport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Infrastructure/Windows/Detail/LMStudioNativeChatFile.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

namespace Detail = Infrastructure::Windows::Detail;
using Json = nlohmann::json;
constexpr std::size_t MiB = 1024U * 1024U;

class NativeChatFixture final {
public:
    NativeChatFixture()
    {
        std::vector<wchar_t> buffer(32U * 1024U, L'\0');
        const DWORD length = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        require(length > 0U && length < buffer.size(), "native chat fixture temp path must be available");
        const std::filesystem::path root{std::wstring{buffer.data(), static_cast<std::size_t>(length)}};
        for (std::uint64_t attempt = 0U; attempt < 32U; ++attempt) {
            directory_ = root / (L"forge-native-chat-snapshot-" + std::to_wstring(::GetCurrentProcessId()) +
                L"-" + std::to_wstring(::GetCurrentThreadId()) + L"-" + std::to_wstring(::GetTickCount64()) +
                L"-" + std::to_wstring(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(directory_, error)) { return; }
        }
        throw TestFailure{"could not create private native chat snapshot fixture"};
    }

    NativeChatFixture(const NativeChatFixture&) = delete;
    NativeChatFixture& operator=(const NativeChatFixture&) = delete;
    ~NativeChatFixture()
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    [[nodiscard]] std::wstring path() const { return (directory_ / L"private.conversation.json").native(); }

    void write(const std::string_view bytes, const bool replace = false) const
    {
        Detail::UniqueHandle file{::CreateFileW(path().c_str(), GENERIC_WRITE, 0U, nullptr,
            replace ? TRUNCATE_EXISTING : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        require(static_cast<bool>(file), "private native chat fixture write handle must open");
        std::size_t offset{};
        while (offset < bytes.size()) {
            const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(64U * 1024U, bytes.size() - offset));
            DWORD written{};
            require(::WriteFile(file.get(), bytes.data() + offset, wanted, &written, nullptr) != FALSE && written > 0U,
                "private native chat fixture bytes must write completely");
            offset += written;
        }
        require(::FlushFileBuffers(file.get()) != FALSE, "private native chat fixture bytes must flush");
    }

    [[nodiscard]] BY_HANDLE_FILE_INFORMATION revision() const
    {
        Detail::UniqueHandle file{::CreateFileW(path().c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        require(static_cast<bool>(file), "private native chat revision handle must open");
        BY_HANDLE_FILE_INFORMATION result{};
        require(::GetFileInformationByHandle(file.get(), &result) != FALSE, "private native chat revision must be available");
        return result;
    }

    [[nodiscard]] std::string bytes() const
    {
        const auto information = revision();
        require(information.nFileSizeHigh == 0U && information.nFileSizeLow <= 65U * MiB,
            "independent private fixture read must remain bounded");
        std::string result(information.nFileSizeLow, '\0');
        Detail::UniqueHandle file{::CreateFileW(path().c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        require(static_cast<bool>(file), "private native chat independent read must open");
        std::size_t offset{};
        while (offset < result.size()) {
            const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(64U * 1024U, result.size() - offset));
            DWORD read{};
            require(::ReadFile(file.get(), result.data() + offset, wanted, &read, nullptr) != FALSE && read > 0U,
                "private native chat independent read must be complete");
            offset += read;
        }
        return result;
    }

    void advanceWriteTime(const FILETIME previous) const
    {
        Detail::UniqueHandle file{::CreateFileW(path().c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        require(static_cast<bool>(file), "private native chat write-time handle must open");
        ULARGE_INTEGER ticks{};
        ticks.LowPart = previous.dwLowDateTime;
        ticks.HighPart = previous.dwHighDateTime;
        ticks.QuadPart += 10'000'000ULL;
        const FILETIME later{ticks.LowPart, ticks.HighPart};
        require(::SetFileTime(file.get(), nullptr, nullptr, &later) != FALSE,
            "private native chat same-size revision change must be deterministic");
    }

    [[nodiscard]] std::size_t entries() const
    {
        std::size_t count{};
        for (const auto& entry : std::filesystem::directory_iterator{directory_}) {
            static_cast<void>(entry);
            ++count;
        }
        return count;
    }

private:
    std::filesystem::path directory_;
};

[[nodiscard]] std::string nativeChatBytes(const std::size_t size)
{
    const std::string prefix = R"json({
  "plugins" : ["foreign/native", "mcp/forge-conductor", "mcp/forge-conductor-fallback", "mcp/forge-conductor-clu"],
  "lastUsedModel": {"identifier":"private-large-model", "unknown_model_field": {"keep":true}},
  "messages": [{"role":"assistant","currentlySelected":0,"versions":[{"steps":[
    {"type":"genInfo","genInfo":{"stopReason":"toolCalls","stats":{"totalTokensCount":731},"unknown_generation_field":{"keep":[1,2,3]}}},
    {"type":"contentBlock","content":[{"type":"toolCallRequest","callId":7,"toolCallRequestId":"native-large-request","pluginIdentifier":"mcp/forge-conductor","name":"fs_read","arguments":{"path":"private.txt"}}]},
    {"type":"contentBlock","content":[{"type":"toolCallResult","callId":7,"toolCallRequestId":"native-large-request","name":"fs_read","content":"native exact result"}]}
  ]}]}],
  "unknown_before": {"escaped":"a\"b\\c", "number":731},
  "unknown_large": ")json";
    const std::string suffix = R"json(",
  "unknown_after": {"retained":["foreign", {"nested":true}]}
}
)json";
    require(size > prefix.size() + suffix.size(), "native chat fixture must have a positive opaque payload");
    std::string result;
    result.reserve(size);
    result.append(prefix);
    result.append(size - prefix.size() - suffix.size(), 'q');
    result.append(suffix);
    require(result.size() == size, "private native chat fixture must reach the exact byte boundary");
    return result;
}

void largeNativeSnapshotPreservesBytesAndRevision()
{
    NativeChatFixture fixture;
    auto expected = nativeChatBytes(33U * MiB);
    fixture.write(expected);
    const auto before = fixture.revision();
    TestContext context;
    const auto snapshot = take(Detail::readNativeChatFile(fixture.path(), context.active()));
    require(snapshot.bytes == expected, "a native chat above 32 MiB must retain every byte, including unknown fields");
    require(Detail::sameRevision(before, snapshot.revision) && Detail::sameRevision(before, fixture.revision()),
        "a successful large snapshot must preserve its native file identity, size, and write revision");
    const auto document = Json::parse(snapshot.bytes);
    require(document["plugins"] == Json::array({"foreign/native", "mcp/forge-conductor", "mcp/forge-conductor-fallback", "mcp/forge-conductor-clu"}),
        "the shared snapshot must preserve foreign and current native integrations in order");
    require(document["lastUsedModel"]["identifier"] == "private-large-model" &&
        document["lastUsedModel"]["unknown_model_field"]["keep"] == true,
        "the entire saved native model acknowledgment data must remain available");
    const auto& steps = document["messages"][0]["versions"][0]["steps"];
    require(steps[0]["genInfo"]["unknown_generation_field"]["keep"] == Json::array({1,2,3}) &&
        steps[1]["content"][0]["toolCallRequestId"] == "native-large-request" &&
        steps[2]["content"][0]["content"] == "native exact result",
        "native completed-tool-boundary generation, request, and result evidence must not be truncated");
    require(document["unknown_before"]["escaped"] == "a\"b\\c" &&
        document["unknown_after"]["retained"][1]["nested"] == true,
        "unknown JSON fields before and after the large payload must survive unchanged");
    require(fixture.entries() == 1U, "a successful native snapshot must not create a replacement or sidecar");

    const auto payload = expected.find(std::string{"\"unknown_large\": \""});
    require(payload != std::string::npos, "private opaque field must be locatable for a same-size change");
    expected[payload + std::string_view{"\"unknown_large\": \""}.size()] = 'r';
    fixture.write(expected, true);
    fixture.advanceWriteTime(before.ftLastWriteTime);
    const auto current = take(Detail::readNativeChatFile(fixture.path(), context.active()));
    require(current.bytes == expected && current.bytes != snapshot.bytes &&
        !Detail::sameRevision(snapshot.revision, current.revision),
        "same-size changes must produce a new full native snapshot and a different write revision");
}

void exactNativeSnapshotBoundIsInclusive()
{
    NativeChatFixture fixture;
    const auto expected = nativeChatBytes(64U * MiB);
    fixture.write(expected);
    const auto before = fixture.revision();
    TestContext context;
    const auto snapshot = take(Detail::readNativeChatFile(fixture.path(), context.active()));
    require(snapshot.bytes == expected && snapshot.bytes.size() == 64U * MiB,
        "the normal conversation reader's exact 64 MiB boundary must also be accepted by native control snapshots");
    require(Detail::sameRevision(before, snapshot.revision) && Detail::sameRevision(before, fixture.revision()) && fixture.entries() == 1U,
        "exact-boundary admission must remain read-only and retain native revision checks");
}

void oversizedNativeSnapshotIsRefusedWithoutChanges()
{
    NativeChatFixture fixture;
    const auto expected = nativeChatBytes(64U * MiB + 1U);
    fixture.write(expected);
    const auto before = fixture.revision();
    TestContext context;
    const auto result = Detail::readNativeChatFile(fixture.path(), context.active());
    requireError(result, Domain::ErrorCodes::HostCapabilityUnavailable,
        "native snapshots above the 64 MiB bound must still be refused");
    require(result.error().message == "The selected LM Studio chat exceeds the 64 MiB native snapshot bound.",
        "a shared native snapshot rejection must not be described as an integration-field update failure");
    require(Detail::sameRevision(before, fixture.revision()) && fixture.bytes() == expected && fixture.entries() == 1U,
        "oversized native snapshot refusal must not change any bytes, revision, or sibling files");
}

void interruptedLargeNativeSnapshotsRemainReadOnly()
{
    NativeChatFixture fixture;
    const auto expected = nativeChatBytes(33U * MiB);
    fixture.write(expected);
    const auto before = fixture.revision();
    TestContext cancelled;
    require(cancelled.cancellation.request_stop(), "native snapshot fixture cancellation must be requested");
    requireError(Detail::readNativeChatFile(fixture.path(), cancelled.active()), Domain::ErrorCodes::Cancelled,
        "a large native snapshot must preserve cancellation rather than returning an obsolete size error");
    TestContext expired;
    requireError(Detail::readNativeChatFile(fixture.path(), expired.expired()), Domain::ErrorCodes::DeadlineExceeded,
        "a large native snapshot must preserve its per-chunk deadline guard");
    require(Detail::sameRevision(before, fixture.revision()) && fixture.bytes() == expected && fixture.entries() == 1U,
        "cancelled and expired snapshot reads must not alter private native chat data");
}

void nativeSnapshotRevisionComparisonRetainsAllGuards()
{
    BY_HANDLE_FILE_INFORMATION original{};
    original.dwVolumeSerialNumber = 7U;
    original.nFileIndexHigh = 8U;
    original.nFileIndexLow = 9U;
    original.nFileSizeHigh = 10U;
    original.nFileSizeLow = 11U;
    original.ftLastWriteTime.dwHighDateTime = 12U;
    original.ftLastWriteTime.dwLowDateTime = 13U;
    require(Detail::sameRevision(original, original), "an unchanged native file revision must compare equal");
    const std::array<void (*)(BY_HANDLE_FILE_INFORMATION&), 7U> mutations{
        [](BY_HANDLE_FILE_INFORMATION& value) { ++value.dwVolumeSerialNumber; },
        [](BY_HANDLE_FILE_INFORMATION& value) { ++value.nFileIndexHigh; },
        [](BY_HANDLE_FILE_INFORMATION& value) { ++value.nFileIndexLow; },
        [](BY_HANDLE_FILE_INFORMATION& value) { ++value.nFileSizeHigh; },
        [](BY_HANDLE_FILE_INFORMATION& value) { ++value.nFileSizeLow; },
        [](BY_HANDLE_FILE_INFORMATION& value) { ++value.ftLastWriteTime.dwHighDateTime; },
        [](BY_HANDLE_FILE_INFORMATION& value) { ++value.ftLastWriteTime.dwLowDateTime; }};
    for (const auto change : mutations) {
        auto changed = original;
        change(changed);
        require(!Detail::sameRevision(original, changed),
            "volume, both file index/size halves, and both write-time halves must each reject revision drift");
    }
}

} // namespace

void registerWindowsLMStudioChatControlTests(TestRegistry& tests)
{
    addTest(tests, "WindowsLMStudioChatControl.large_native_snapshot_preserves_bytes_and_revision", largeNativeSnapshotPreservesBytesAndRevision);
    addTest(tests, "WindowsLMStudioChatControl.exact_native_snapshot_bound_is_inclusive", exactNativeSnapshotBoundIsInclusive);
    addTest(tests, "WindowsLMStudioChatControl.oversized_native_snapshot_is_refused_without_changes", oversizedNativeSnapshotIsRefusedWithoutChanges);
    addTest(tests, "WindowsLMStudioChatControl.interrupted_large_native_snapshots_remain_read_only", interruptedLargeNativeSnapshotsRemainReadOnly);
    addTest(tests, "WindowsLMStudioChatControl.native_snapshot_revision_comparison_retains_all_guards", nativeSnapshotRevisionComparisonRetainsAllGuards);
}

} // namespace ForgeConductor::Tests
