// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0

#include "ForgeDomain/Clock.h"
#include "ForgeDomain/Models.h"

#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

namespace Forge::Domain {

std::string iso8601(std::chrono::system_clock::time_point tp) {
    const auto time = std::chrono::system_clock::to_time_t(tp);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

std::string makeUuid() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<std::uint64_t> dist;
    const auto a = dist(gen);
    const auto b = dist(gen);
    std::ostringstream stream;
    stream << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(8) << static_cast<std::uint32_t>(a >> 32) << '-'
           << std::setw(4) << static_cast<std::uint16_t>(a >> 16) << '-'
           << std::setw(4) << static_cast<std::uint16_t>((a & 0xffff) | 0x4000) << '-'
           << std::setw(4) << static_cast<std::uint16_t>((b >> 48) | 0x8000) << '-'
           << std::setw(12) << (b & 0xffffffffffffULL);
    return stream.str();
}

ClientID ClientID::generate() {
    return ClientID{"client-" + makeUuid()};
}

} // namespace Forge::Domain
