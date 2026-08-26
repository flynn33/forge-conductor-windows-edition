#include "../TestSupport.h"

#include "Infrastructure/Windows/Detail/OverlappedPipeReader.h"

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class OverlappedPipeReaderTestAccess final {
public:
    [[nodiscard]] static std::shared_ptr<OverlappedPipeReader>
    create(IoCompletionPort& completionPort, UniqueHandle readHandle,
           const std::size_t maximumCaptureBytes)
    {
        return std::shared_ptr<OverlappedPipeReader>{
            new OverlappedPipeReader{completionPort, std::move(readHandle), maximumCaptureBytes}};
    }
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail

namespace ForgeConductor::Tests {
namespace {

using Infrastructure::Windows::Detail::IoCompletionPort;
using Infrastructure::Windows::Detail::OverlappedPipeReader;
using Infrastructure::Windows::Detail::OverlappedPipeReaderTestAccess;
using Infrastructure::Windows::Detail::PipeEndpoints;
using Infrastructure::Windows::Detail::UniqueHandle;
using namespace std::chrono_literals;

[[nodiscard]] std::wstring uniquePipeName(const std::wstring_view suffix)
{
    LARGE_INTEGER counter{};
    require(::QueryPerformanceCounter(&counter), "QueryPerformanceCounter failed");
    return L"\\\\.\\pipe\\ForgeConductor.OverlappedPipeReaderTests." +
           std::to_wstring(::GetCurrentProcessId()) + L"." + std::to_wstring(counter.QuadPart) +
           L"." + std::wstring{suffix};
}

[[nodiscard]] PipeEndpoints createEndpoints(IoCompletionPort& completionPort,
                                            const std::wstring_view suffix)
{
    return take(OverlappedPipeReader::create(completionPort, uniquePipeName(suffix), 4'096U));
}

void testPeekFailurePropagates()
{
    auto completionPort = take(IoCompletionPort::create());
    auto reader = OverlappedPipeReaderTestAccess::create(*completionPort, UniqueHandle{}, 128U);

    reader->finishAvailable();
    const auto capture = reader->capture();
    require(capture.readError == ERROR_INVALID_HANDLE,
            "PeekNamedPipe failure was not preserved in the pipe capture");
    require(capture.truncated, "PeekNamedPipe failure did not mark the pipe capture incomplete");

    completionPort->shutdown();
}

void testPendingReadCancellationIsBounded()
{
    auto completionPort = take(IoCompletionPort::create());
    auto endpoints = createEndpoints(*completionPort, L"cancel");
    take(endpoints.reader->start());
    require(!endpoints.reader->waitUntilIdle(10ms),
            "empty connected pipe did not retain a pending read for cancellation");

    const auto started = std::chrono::steady_clock::now();
    endpoints.reader->cancelAndWait();
    require(std::chrono::steady_clock::now() - started < 3s,
            "pending pipe cancellation exceeded its bounded reap path");
    require(endpoints.reader->waitUntilIdle(0ms),
            "cancelled pipe reader did not acknowledge an idle state");
    const auto capture = endpoints.reader->capture();
    require(capture.readError == ERROR_SUCCESS,
            "intentional pending-read cancellation surfaced as an output error");

    endpoints.childWriter.reset();
    completionPort->shutdown();
}

void testCompletionPortShutdownIsBounded()
{
    auto completionPort = take(IoCompletionPort::create());
    auto endpoints = createEndpoints(*completionPort, L"shutdown");
    take(endpoints.reader->start());
    require(!endpoints.reader->waitUntilIdle(10ms),
            "empty connected pipe did not retain a pending read for shutdown");

    const auto started = std::chrono::steady_clock::now();
    completionPort->shutdown();
    require(std::chrono::steady_clock::now() - started < 3s,
            "completion-port shutdown exceeded its bounded worker acknowledgement path");
    require(endpoints.reader->waitUntilIdle(0ms),
            "completion-port shutdown did not reap its pending pipe read");
    require(endpoints.reader->capture().readError == ERROR_SUCCESS,
            "completion-port shutdown surfaced intentional cancellation as an output error");
}

void testBrokenPipeIsCleanEof()
{
    auto completionPort = take(IoCompletionPort::create());
    auto endpoints = createEndpoints(*completionPort, L"eof");
    endpoints.childWriter.reset();

    take(endpoints.reader->start());
    require(endpoints.reader->waitUntilIdle(2s),
            "closed child pipe did not reach an idle EOF state");
    const auto capture = endpoints.reader->capture();
    require(capture.readError == ERROR_SUCCESS,
            "normal broken-pipe EOF surfaced as an output error");

    completionPort->shutdown();
}

} // namespace

void registerOverlappedPipeReaderTests(TestRegistry& tests)
{
    addTest(tests, "pipe_reader.peek_failure_propagates", testPeekFailurePropagates);
    addTest(tests, "pipe_reader.pending_read_cancellation_is_bounded",
            testPendingReadCancellationIsBounded);
    addTest(tests, "pipe_reader.completion_port_shutdown_is_bounded",
            testCompletionPortShutdownIsBounded);
    addTest(tests, "pipe_reader.broken_pipe_is_clean_eof", testBrokenPipeIsCleanEof);
}

} // namespace ForgeConductor::Tests
