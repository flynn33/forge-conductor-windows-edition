#include "ForgeConductor/Domain/ResourcePolicy.h"

namespace ForgeConductor::Domain {
namespace {

constexpr std::uint64_t GiB = 1'073'741'824ULL;

} // namespace

ResourceBudgets budgetsForProfile(const ResourceProfile profile) noexcept
{
    switch (profile) {
    case ResourceProfile::Constrained8GiB:
        return ResourceBudgets{
            1.0,
            2.0,
            0.2,
            1,
            600,
            1'800,
            4,
            5,
            4'194'304,
            80'000,
            20'000,
            120,
            1'048'576,
            2'097'152,
            240,
            110,
            100,
            2.0,
            1.0,
            48,
            5.0,
            48,
            32};
    case ResourceProfile::Standard16GiB:
        return ResourceBudgets{
            2.0,
            4.0,
            0.2,
            1,
            1'200,
            3'600,
            8,
            8,
            8'388'608,
            80'000,
            20'000,
            120,
            1'048'576,
            2'097'152,
            320,
            150,
            120,
            2.0,
            1.0,
            64,
            5.0,
            64,
            40};
    case ResourceProfile::Expanded32GiBPlus:
        return ResourceBudgets{
            2.0,
            5.0,
            0.5,
            1,
            1'800,
            7'200,
            16,
            10,
            10'485'760,
            80'000,
            20'000,
            120,
            1'048'576,
            2'097'152,
            400,
            180,
            140,
            2.0,
            1.0,
            80,
            5.0,
            72,
            48};
    }
    return {};
}

ResourceProfile selectResourceProfile(const std::uint64_t physicalMemoryBytes) noexcept
{
    if (physicalMemoryBytes <= 8 * GiB) {
        return ResourceProfile::Constrained8GiB;
    }
    if (physicalMemoryBytes < 32 * GiB) {
        return ResourceProfile::Standard16GiB;
    }
    return ResourceProfile::Expanded32GiBPlus;
}

} // namespace ForgeConductor::Domain
