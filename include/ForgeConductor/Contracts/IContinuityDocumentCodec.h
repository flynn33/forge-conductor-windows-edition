#pragma once

#include "ForgeConductor/Domain/ContinuityModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <string>
#include <string_view>

namespace ForgeConductor::Contracts {

// A lifecycle handoff paired with the exact canonical UTF-8 bytes whose
// integrity digest is stored in the typed value. JSON ownership remains below
// the Application layer.
struct ContinuityDocument final {
    Domain::ContinuityHandoff handoff;
    std::string canonicalUtf8;
};

class IContinuityDocumentCodec {
public:
    virtual ~IContinuityDocumentCodec() = default;

    [[nodiscard]] virtual Domain::Result<ContinuityDocument> encode(
        const Domain::ContinuityHandoff& handoff,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<ContinuityDocument> decode(
        std::string_view canonicalUtf8,
        const Domain::OperationContext& context) noexcept = 0;
};

} // namespace ForgeConductor::Contracts
