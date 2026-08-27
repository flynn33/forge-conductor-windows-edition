#include "ForgeConductor/Mcp/WindowsStdioMcpTransport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <future>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Mcp = ForgeConductor::Mcp;
using namespace std::chrono_literals;

std::size_t assertions{};

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        ++assertions;                                                            \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} +      \
                                     #condition};                                \
        }                                                                        \
    } while (false)

class ScopedHandle final {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(const HANDLE handle) noexcept : handle_{handle} {}
    ~ScopedHandle() noexcept { reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept
        : handle_{std::exchange(other.handle_, nullptr)}
    {
    }

    ScopedHandle& operator=(ScopedHandle&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

    [[nodiscard]] HANDLE release() noexcept
    {
        return std::exchange(handle_, nullptr);
    }

    void reset(const HANDLE replacement = nullptr) noexcept
    {
        const HANDLE previous = std::exchange(handle_, replacement);
        if (previous != nullptr && previous != INVALID_HANDLE_VALUE) {
            static_cast<void>(::CloseHandle(previous));
        }
    }

private:
    HANDLE handle_{};
};

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view expectedCode)
{
    REQUIRE(!result);
    ++assertions;
    if (result.error().code != expectedCode) {
        throw std::runtime_error{
            std::string{"Expected error code "} + std::string{expectedCode} +
            ", received " + result.error().code};
    }
}

template <typename T>
[[nodiscard]] T parseId(const std::string_view value)
{
    auto parsed = T::parse(value);
    if (!parsed) {
        throw std::runtime_error{parsed.error().message};
    }
    return std::move(parsed).value();
}

[[nodiscard]] Domain::OperationContext context(
    const Domain::MonotonicTimePoint deadline,
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parseId<Domain::OperationId>(
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        deadline,
        cancellation,
        parseId<Domain::CorrelationId>("mcp-stdio-transport-test")};
}

[[nodiscard]] Domain::OperationContext activeContext()
{
    return context(std::chrono::steady_clock::now() + 10s);
}

struct PipeFixture final {
    std::unique_ptr<Mcp::WindowsStdioMcpTransport> transport;
    ScopedHandle inputWriter;
    ScopedHandle outputReader;
};

[[nodiscard]] PipeFixture createFixture(const DWORD pipeBytes = 65'536U)
{
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);

    HANDLE inputReaderRaw{};
    HANDLE inputWriterRaw{};
    if (::CreatePipe(
            &inputReaderRaw,
            &inputWriterRaw,
            &attributes,
            pipeBytes) == FALSE) {
        throw std::runtime_error{"CreatePipe failed for MCP input."};
    }
    ScopedHandle inputReader{inputReaderRaw};
    ScopedHandle inputWriter{inputWriterRaw};

    HANDLE outputReaderRaw{};
    HANDLE outputWriterRaw{};
    if (::CreatePipe(
            &outputReaderRaw,
            &outputWriterRaw,
            &attributes,
            pipeBytes) == FALSE) {
        throw std::runtime_error{"CreatePipe failed for MCP output."};
    }
    ScopedHandle outputReader{outputReaderRaw};
    ScopedHandle outputWriter{outputWriterRaw};

    auto transport = take(Mcp::WindowsStdioMcpTransport::createFromOwnedHandles(
        inputReader.release(), outputWriter.release()));
    return PipeFixture{
        std::move(transport),
        std::move(inputWriter),
        std::move(outputReader)};
}

void writeAll(const HANDLE handle, const std::string_view bytes)
{
    std::size_t offset{};
    while (offset < bytes.size()) {
        DWORD written{};
        const auto request = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        if (::WriteFile(
                handle,
                bytes.data() + offset,
                request,
                &written,
                nullptr) == FALSE ||
            written == 0U) {
            throw std::runtime_error{"WriteFile failed in the MCP fixture."};
        }
        offset += written;
    }
}

[[nodiscard]] std::string readExactly(
    const HANDLE handle,
    const std::size_t byteCount)
{
    std::string bytes(byteCount, '\0');
    std::size_t offset{};
    while (offset < bytes.size()) {
        DWORD read{};
        const auto request = static_cast<DWORD>(bytes.size() - offset);
        if (::ReadFile(
                handle,
                bytes.data() + offset,
                request,
                &read,
                nullptr) == FALSE ||
            read == 0U) {
            throw std::runtime_error{"ReadFile failed in the MCP fixture."};
        }
        offset += read;
    }
    return bytes;
}

[[nodiscard]] Domain::Result<std::optional<Domain::McpFrame>> receiveAsync(
    Mcp::WindowsStdioMcpTransport& transport)
{
    return transport.receive(activeContext());
}

void testTypeAndOwnedHandleContract()
{
    static_assert(std::is_final_v<Mcp::WindowsStdioMcpTransport>);
    static_assert(std::is_base_of_v<
                  ForgeConductor::Contracts::IMcpTransport,
                  Mcp::WindowsStdioMcpTransport>);
    static_assert(!std::is_copy_constructible_v<Mcp::WindowsStdioMcpTransport>);
    static_assert(!std::is_move_constructible_v<Mcp::WindowsStdioMcpTransport>);
    static_assert(Mcp::WindowsStdioMcpTransport::MaximumFrameBytes ==
                  1'048'576U);

    const auto invalid = Mcp::WindowsStdioMcpTransport::createFromOwnedHandles(
        nullptr, nullptr);
    requireError(invalid, Domain::ErrorCodes::InvalidRequest);

    const auto unsupported =
        Mcp::WindowsStdioMcpTransport::createFromOwnedHandles(
            ::CreateEventW(nullptr, TRUE, FALSE, nullptr),
            ::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    requireError(
        unsupported,
        Domain::ErrorCodes::HostCapabilityUnavailable);
}

void testSplitReadsAndBufferedFrames()
{
    auto fixture = createFixture();
    auto first = std::async(
        std::launch::async, receiveAsync, std::ref(*fixture.transport));

    writeAll(fixture.inputWriter.get(), "{\"json");
    std::this_thread::sleep_for(20ms);
    writeAll(
        fixture.inputWriter.get(),
        "rpc\":\"2.0\",\"id\":1}\n{\"id\":2}\n");

    auto firstResult = first.get();
    REQUIRE(firstResult);
    REQUIRE(firstResult.value().has_value());
    REQUIRE(firstResult.value()->utf8Json ==
            "{\"jsonrpc\":\"2.0\",\"id\":1}");

    const auto secondResult = fixture.transport->receive(activeContext());
    REQUIRE(secondResult);
    REQUIRE(secondResult.value().has_value());
    REQUIRE(secondResult.value()->utf8Json == "{\"id\":2}");
}

void testExactCapAndOversizeRecovery()
{
    {
        auto fixture = createFixture();
        std::string exact{"{\"x\":\""};
        exact.append(
            Mcp::WindowsStdioMcpTransport::MaximumFrameBytes - 8U,
            'a');
        exact += "\"}";
        REQUIRE(exact.size() ==
                Mcp::WindowsStdioMcpTransport::MaximumFrameBytes);

        auto received = std::async(
            std::launch::async, receiveAsync, std::ref(*fixture.transport));
        writeAll(fixture.inputWriter.get(), exact);
        writeAll(fixture.inputWriter.get(), "\n");
        auto result = received.get();
        REQUIRE(result);
        REQUIRE(result.value().has_value());
        REQUIRE(result.value()->utf8Json.size() == exact.size());
    }

    {
        auto fixture = createFixture();
        std::string oversized(
            Mcp::WindowsStdioMcpTransport::MaximumFrameBytes + 1U,
            'x');
        oversized += "\n{\"recovered\":true}\n";

        auto received = std::async(
            std::launch::async, receiveAsync, std::ref(*fixture.transport));
        writeAll(fixture.inputWriter.get(), oversized);
        requireError(received.get(), Domain::ErrorCodes::PayloadTooLarge);

        const auto recovered = fixture.transport->receive(activeContext());
        REQUIRE(recovered);
        REQUIRE(recovered.value().has_value());
        REQUIRE(recovered.value()->utf8Json == "{\"recovered\":true}");
    }
}

void testMalformedFramingAndRecovery()
{
    auto fixture = createFixture();
    const std::string malformed =
        "\n"
        "Content-Length: 2\n"
        "{\"cr\":true}\r\n";
    writeAll(fixture.inputWriter.get(), malformed);
    writeAll(
        fixture.inputWriter.get(),
        std::string{"{\"nul\":\"x"} + std::string{1U, '\0'} + "y\"}\n");
    writeAll(
        fixture.inputWriter.get(),
        std::string{"{\"bad\":\""} +
            std::string{1U, static_cast<char>(0xff)} + "\"}\n");
    writeAll(
        fixture.inputWriter.get(),
        "{\"broken\":\n[1]\n{\"ok\":true}\n");

    for (std::size_t index{}; index < 5U; ++index) {
        requireError(
            fixture.transport->receive(activeContext()),
            Domain::ErrorCodes::MalformedMessage);
    }

    const auto malformedJson = fixture.transport->receive(activeContext());
    REQUIRE(malformedJson);
    REQUIRE(malformedJson.value().has_value());
    REQUIRE(malformedJson.value()->utf8Json == "{\"broken\":");

    const auto nonObjectJson = fixture.transport->receive(activeContext());
    REQUIRE(nonObjectJson);
    REQUIRE(nonObjectJson.value().has_value());
    REQUIRE(nonObjectJson.value()->utf8Json == "[1]");

    const auto recovered = fixture.transport->receive(activeContext());
    REQUIRE(recovered);
    REQUIRE(recovered.value().has_value());
    REQUIRE(recovered.value()->utf8Json == "{\"ok\":true}");
}

void testCleanAndPartialEndOfStream()
{
    {
        auto fixture = createFixture();
        fixture.inputWriter.reset();
        const auto ended = fixture.transport->receive(activeContext());
        REQUIRE(ended);
        REQUIRE(!ended.value().has_value());
    }
    {
        auto fixture = createFixture();
        writeAll(fixture.inputWriter.get(), "{\"partial\":true}");
        fixture.inputWriter.reset();
        requireError(
            fixture.transport->receive(activeContext()),
            Domain::ErrorCodes::MalformedMessage);
    }
}

void testOutputNewlineValidationAndBrokenPipe()
{
    {
        auto fixture = createFixture();
        const Domain::McpFrame frame{
            "{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":{}}"};
        const auto sent = fixture.transport->send(frame, activeContext());
        REQUIRE(sent);
        REQUIRE(readExactly(
                    fixture.outputReader.get(), frame.utf8Json.size() + 1U) ==
                frame.utf8Json + "\n");

        requireError(
            fixture.transport->send(Domain::McpFrame{"{}\n"}, activeContext()),
            Domain::ErrorCodes::MalformedMessage);
        requireError(
            fixture.transport->send(
                Domain::McpFrame{"{ \"id\" : 1 }"}, activeContext()),
            Domain::ErrorCodes::MalformedMessage);
    }
    {
        auto fixture = createFixture();
        fixture.outputReader.reset();
        requireError(
            fixture.transport->send(Domain::McpFrame{"{}"}, activeContext()),
            Domain::ErrorCodes::TransportClosed);
    }
}

void testCancellationDeadlineAndShutdown()
{
    {
        auto fixture = createFixture();
        std::stop_source cancellation;
        auto pending = std::async(std::launch::async, [&]() {
            return fixture.transport->receive(context(
                std::chrono::steady_clock::now() + 10s,
                cancellation.get_token()));
        });
        std::this_thread::sleep_for(20ms);
        cancellation.request_stop();
        REQUIRE(pending.wait_for(2s) == std::future_status::ready);
        requireError(pending.get(), Domain::ErrorCodes::Cancelled);

        writeAll(fixture.inputWriter.get(), "{\"after\":\"cancel\"}\n");
        const auto reusable = fixture.transport->receive(activeContext());
        REQUIRE(reusable);
        REQUIRE(reusable.value().has_value());
    }
    {
        auto fixture = createFixture();
        const auto started = std::chrono::steady_clock::now();
        const auto expired = fixture.transport->receive(
            context(started + 50ms));
        requireError(expired, Domain::ErrorCodes::DeadlineExceeded);
        REQUIRE(std::chrono::steady_clock::now() - started < 2s);
    }
    {
        auto fixture = createFixture();
        auto pending = std::async(
            std::launch::async, receiveAsync, std::ref(*fixture.transport));
        std::this_thread::sleep_for(20ms);
        fixture.transport->shutdown();
        REQUIRE(pending.wait_for(2s) == std::future_status::ready);
        requireError(pending.get(), Domain::ErrorCodes::TransportClosed);
        requireError(
            fixture.transport->send(Domain::McpFrame{"{}"}, activeContext()),
            Domain::ErrorCodes::TransportClosed);
        requireError(
            fixture.transport->receive(activeContext()),
            Domain::ErrorCodes::TransportClosed);
    }
    {
        auto fixture = createFixture();
        std::stop_source cancellation;
        cancellation.request_stop();
        requireError(
            fixture.transport->send(
                Domain::McpFrame{"{}"},
                context(
                    std::chrono::steady_clock::now() + 10s,
                    cancellation.get_token())),
            Domain::ErrorCodes::Cancelled);
        requireError(
            fixture.transport->send(
                Domain::McpFrame{"{}"},
                context(std::chrono::steady_clock::now() - 1ms)),
            Domain::ErrorCodes::DeadlineExceeded);
    }
}

void testDestructorDrainsActiveReceive()
{
    auto fixture = createFixture();
    auto* transport = fixture.transport.get();
    std::promise<void> admitted;
    auto pending = std::async(
        std::launch::async,
        [transport, &admitted] {
            admitted.set_value();
            return transport->receive(activeContext());
        });
    admitted.get_future().wait();
    writeAll(fixture.inputWriter.get(), "{");
    std::this_thread::sleep_for(50ms);

    const auto started = std::chrono::steady_clock::now();
    fixture.transport.reset();
    REQUIRE(std::chrono::steady_clock::now() - started < 2s);
    REQUIRE(pending.wait_for(2s) == std::future_status::ready);
    requireError(pending.get(), Domain::ErrorCodes::TransportClosed);
}

void testInterruptedSendPoisonsTransport()
{
    auto fixture = createFixture(4096U);
    std::string payload{"{\"value\":\""};
    payload.append(200'000U, 'x');
    payload.append("\"}");

    std::stop_source cancellation;
    auto pending = std::async(std::launch::async, [&] {
        return fixture.transport->send(
            Domain::McpFrame{payload},
            context(
                std::chrono::steady_clock::now() + 10s,
                cancellation.get_token()));
    });
    std::this_thread::sleep_for(50ms);
    cancellation.request_stop();
    REQUIRE(pending.wait_for(2s) == std::future_status::ready);
    requireError(pending.get(), Domain::ErrorCodes::Cancelled);
    requireError(
        fixture.transport->send(Domain::McpFrame{"{}"}, activeContext()),
        Domain::ErrorCodes::TransportClosed);
}

void testConcurrentSendsAreSerialized()
{
    auto fixture = createFixture();
    constexpr std::size_t SendCount = 24U;
    std::vector<std::string> frames;
    frames.reserve(SendCount);
    std::size_t wireBytes{};
    for (std::size_t index{}; index < SendCount; ++index) {
        frames.emplace_back("{\"id\":" + std::to_string(index) + "}");
        wireBytes += frames.back().size() + 1U;
    }

    std::vector<std::future<Domain::Result<void>>> sends;
    sends.reserve(SendCount);
    for (const auto& frame : frames) {
        sends.emplace_back(std::async(std::launch::async, [&fixture, frame]() {
            return fixture.transport->send(
                Domain::McpFrame{frame}, activeContext());
        }));
    }
    for (auto& send : sends) {
        REQUIRE(send.get());
    }

    const auto wire = readExactly(fixture.outputReader.get(), wireBytes);
    REQUIRE(!wire.empty());
    REQUIRE(wire.back() == '\n');
    std::set<std::string> observed;
    std::size_t offset{};
    while (offset < wire.size()) {
        const auto newline = wire.find('\n', offset);
        REQUIRE(newline != std::string::npos);
        observed.insert(wire.substr(offset, newline - offset));
        offset = newline + 1U;
    }
    REQUIRE(observed.size() == SendCount);
    const std::set<std::string> expected{frames.begin(), frames.end()};
    REQUIRE(observed == expected);
}

} // namespace

int main()
{
    std::string_view currentTest{"startup"};
    try {
        currentTest = "type and owned handles";
        testTypeAndOwnedHandleContract();
        currentTest = "split and buffered frames";
        testSplitReadsAndBufferedFrames();
        currentTest = "cap and oversize recovery";
        testExactCapAndOversizeRecovery();
        currentTest = "malformed framing recovery";
        testMalformedFramingAndRecovery();
        currentTest = "end of stream";
        testCleanAndPartialEndOfStream();
        currentTest = "output and broken pipe";
        testOutputNewlineValidationAndBrokenPipe();
        currentTest = "cancellation deadline shutdown";
        testCancellationDeadlineAndShutdown();
        currentTest = "destructor drain";
        testDestructorDrainsActiveReceive();
        currentTest = "interrupted send poison";
        testInterruptedSendPoisonsTransport();
        currentTest = "concurrent sends";
        testConcurrentSendsAreSerialized();
        std::cout << "MCP stdio transport tests passed: " << assertions
                  << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MCP stdio transport tests failed after " << assertions
                  << " assertions in " << currentTest << ": "
                  << error.what() << '\n';
        return 1;
    }
}
