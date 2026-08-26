#pragma once

#include "ForgeConductor/Contracts/IContinuityDocumentCodec.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"

#include <memory>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows {

class WindowsContinuityDocumentCodec final
    : public Contracts::IContinuityDocumentCodec {
public:
    WindowsContinuityDocumentCodec(
        std::shared_ptr<Contracts::IHasher> hasher,
        std::shared_ptr<Contracts::IClock> clock) noexcept;

    WindowsContinuityDocumentCodec(
        const WindowsContinuityDocumentCodec&) = delete;
    WindowsContinuityDocumentCodec& operator=(
        const WindowsContinuityDocumentCodec&) = delete;
    WindowsContinuityDocumentCodec(
        WindowsContinuityDocumentCodec&&) = delete;
    WindowsContinuityDocumentCodec& operator=(
        WindowsContinuityDocumentCodec&&) = delete;

    [[nodiscard]] Domain::Result<Contracts::ContinuityDocument> encode(
        const Domain::ContinuityHandoff& handoff,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Contracts::ContinuityDocument> decode(
        std::string_view canonicalUtf8,
        const Domain::OperationContext& context) noexcept override;

private:
    std::shared_ptr<Contracts::IHasher> hasher_;
    std::shared_ptr<Contracts::IClock> clock_;
};

} // namespace ForgeConductor::Infrastructure::Windows
