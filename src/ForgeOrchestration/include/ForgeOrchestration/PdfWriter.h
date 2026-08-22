// Copyright 2026 Jim Daley
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Forge::Orchestration {

class PdfWriter final {
public:
    void writeText(const std::filesystem::path& path, const std::string& title, const std::string& body) const;
    void writeFromFile(const std::filesystem::path& source, const std::filesystem::path& dest) const;
};

} // namespace Forge::Orchestration
