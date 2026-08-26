#include "EtwProvider.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <type_traits>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] constexpr UCHAR levelFor(
    const Domain::DiagnosticSeverity severity) noexcept
{
    switch (severity) {
    case Domain::DiagnosticSeverity::Info: return TRACE_LEVEL_INFORMATION;
    case Domain::DiagnosticSeverity::Warn: return TRACE_LEVEL_WARNING;
    case Domain::DiagnosticSeverity::Error: return TRACE_LEVEL_ERROR;
    case Domain::DiagnosticSeverity::Critical: return TRACE_LEVEL_CRITICAL;
    }
    return TRACE_LEVEL_INFORMATION;
}

[[nodiscard]] constexpr GUID providerId() noexcept
{
    return GUID{
        0x3e260a73,
        0xb267,
        0x4f7d,
        {0x9c, 0xf8, 0xe1, 0x43, 0x51, 0xe6, 0x4c, 0xd2}};
}

} // namespace

EtwProvider::EtwProvider() noexcept
{
    REGHANDLE registration{};
    const auto id = providerId();
    if (EventRegister(&id, nullptr, nullptr, &registration) == ERROR_SUCCESS) {
        registration_.store(registration, std::memory_order_release);
    }
}

EtwProvider::~EtwProvider() noexcept
{
    shutdown();
}

bool EtwProvider::healthy() const noexcept
{
    return registration_.load(std::memory_order_acquire) != 0;
}

bool EtwProvider::write(
    const Domain::DiagnosticEnvelope& envelope) noexcept
{
    const auto registration = registration_.load(std::memory_order_acquire);
    if (registration == 0) {
        return false;
    }

    const auto severity = static_cast<std::uint32_t>(envelope.severity);
    const auto category = static_cast<std::uint32_t>(envelope.category);
    const auto processId = envelope.processId;
    const auto timestampTicks =
        static_cast<std::uint64_t>(envelope.timestamp.time_since_epoch().count());

    EVENT_DESCRIPTOR descriptor{
        1U,
        0U,
        0U,
        levelFor(envelope.severity),
        0U,
        0U,
        0U};
    std::array<EVENT_DATA_DESCRIPTOR, 4U> data{};
    EventDataDescCreate(&data[0], &severity, sizeof(severity));
    EventDataDescCreate(&data[1], &category, sizeof(category));
    EventDataDescCreate(&data[2], &processId, sizeof(processId));
    EventDataDescCreate(&data[3], &timestampTicks, sizeof(timestampTicks));

    return EventWriteTransfer(
               registration,
               &descriptor,
               nullptr,
               nullptr,
               static_cast<ULONG>(data.size()),
               data.data()) == ERROR_SUCCESS;
}

void EtwProvider::shutdown() noexcept
{
    const auto registration =
        registration_.exchange(0, std::memory_order_acq_rel);
    if (registration != 0) {
        (void)EventUnregister(registration);
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
