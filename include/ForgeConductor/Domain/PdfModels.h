#pragma once

#include "ForgeConductor/Domain/FileSystemModels.h"

#include <cstddef>
#include <string>

namespace ForgeConductor::Domain {

struct PdfWriteReceipt final {
    PathText path;
    std::size_t bytesWritten{};
    std::size_t pages{};
    std::string engine;
    std::string title;
};

} // namespace ForgeConductor::Domain
