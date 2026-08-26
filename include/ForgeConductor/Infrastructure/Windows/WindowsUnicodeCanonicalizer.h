#pragma once

#include "ForgeConductor/Contracts/IUnicodeCanonicalizer.h"

namespace ForgeConductor::Infrastructure::Windows {

class WindowsUnicodeCanonicalizer final
    : public Contracts::IUnicodeCanonicalizer {
public:
    [[nodiscard]] Domain::Result<Contracts::NfcUtf8Key> nfcKey(
        std::string_view value) const noexcept override;
};

} // namespace ForgeConductor::Infrastructure::Windows
