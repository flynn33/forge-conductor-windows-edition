#include "ForgeConductor/Application/LegacyMemoryService.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Application {
namespace {

constexpr std::string_view PurgeAction = "purge_legacy_memory";
constexpr std::string_view PurgeScope = "legacy-global-memory";
constexpr std::string_view PurgeToken = "PURGE LEGACY GLOBAL MEMORY";

template <typename T>
[[nodiscard]] Domain::Result<T> internalFailure(const std::string_view message)
{
    return Domain::Result<T>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure, std::string{message}));
}

template <typename T, typename U>
[[nodiscard]] Domain::Result<T> propagateFailure(Domain::Result<U>&& source)
{
    return Domain::Result<T>::failure(std::move(source).error());
}

template <typename T>
[[nodiscard]] Domain::Result<T> dependencyIntegrityFailure(
    const std::string_view message)
{
    return Domain::Result<T>::failure(Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure, std::string{message}));
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto now = std::chrono::steady_clock::now();
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The legacy-memory operation was cancelled before admission."));
        }
        if (context.isExpired(now)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The legacy-memory operation deadline expired before admission."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The legacy-memory operation context could not be validated."));
    }
}

[[nodiscard]] bool asciiCaseInsensitivePrefix(
    const std::string_view value,
    const std::string_view prefix) noexcept
{
    if (prefix.size() > value.size()) {
        return false;
    }
    for (std::size_t index{}; index < prefix.size(); ++index) {
        auto left = static_cast<unsigned char>(value[index]);
        auto right = static_cast<unsigned char>(prefix[index]);
        if (left >= static_cast<unsigned char>('A') &&
            left <= static_cast<unsigned char>('Z')) {
            left = static_cast<unsigned char>(left + ('a' - 'A'));
        }
        if (right >= static_cast<unsigned char>('A') &&
            right <= static_cast<unsigned char>('Z')) {
            right = static_cast<unsigned char>(right + ('a' - 'A'));
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Domain::Result<std::vector<std::string>> canonicalizeTags(
    const std::vector<std::string>& tags,
    const Contracts::IUnicodeCanonicalizer& canonicalizer)
{
    auto prepared = Domain::prepareLegacyMemoryTags(tags);
    if (!prepared) {
        return Domain::Result<std::vector<std::string>>::failure(
            std::move(prepared).error());
    }

    try {
        std::map<Contracts::NfcUtf8Key, std::string> unique;
        for (auto& tag : prepared.value()) {
            auto key = canonicalizer.nfcKey(tag);
            if (!key) {
                return Domain::Result<std::vector<std::string>>::failure(
                    std::move(key).error());
            }
            unique.try_emplace(
                std::move(key).value(), std::move(tag));
        }

        std::vector<std::string> result;
        result.reserve(unique.size());
        for (auto& [key, original] : unique) {
            static_cast<void>(key);
            result.push_back(std::move(original));
        }
        return Domain::Result<std::vector<std::string>>::success(
            std::move(result));
    } catch (...) {
        return Domain::Result<std::vector<std::string>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Memory tags could not be canonically ordered."));
    }
}

[[nodiscard]] Domain::Result<Domain::LegacyMemoryUpsert> normalizeSetRequest(
    const Domain::LegacyMemorySetRequest& request,
    const Contracts::IUnicodeCanonicalizer& canonicalizer)
{
    auto key = Domain::normalizeLegacyMemoryKey(request.key);
    if (!key) {
        return Domain::Result<Domain::LegacyMemoryUpsert>::failure(
            std::move(key).error());
    }
    auto body = Domain::normalizeLegacyMemoryBody(request.body);
    if (!body) {
        return Domain::Result<Domain::LegacyMemoryUpsert>::failure(
            std::move(body).error());
    }
    auto tags = canonicalizeTags(request.tags, canonicalizer);
    if (!tags) {
        return Domain::Result<Domain::LegacyMemoryUpsert>::failure(
            std::move(tags).error());
    }
    return Domain::Result<Domain::LegacyMemoryUpsert>::success(
        Domain::LegacyMemoryUpsert{
            std::move(key).value(),
            std::move(body).value(),
            std::move(tags).value()});
}

[[nodiscard]] Domain::Result<bool> containsCanonicalTag(
    const std::vector<std::string>& tags,
    const Contracts::NfcUtf8Key& candidate,
    const Contracts::IUnicodeCanonicalizer& canonicalizer)
{
    for (const auto& tag : tags) {
        auto tagKey = canonicalizer.nfcKey(tag);
        if (!tagKey) {
            return Domain::Result<bool>::failure(std::move(tagKey).error());
        }
        if (tagKey.value() == candidate) {
            return Domain::Result<bool>::success(true);
        }
    }
    return Domain::Result<bool>::success(false);
}

[[nodiscard]] Domain::Result<void> validateProjectionPage(
    const std::vector<Domain::LegacyMemoryNoteProjection>& notes,
    const std::size_t limit,
    const bool includeSystem,
    const bool includeBody,
    const std::optional<std::string>& prefix,
    const std::optional<std::string>& tag,
    const Contracts::IUnicodeCanonicalizer& canonicalizer)
{
    if (notes.size() > limit) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "A legacy-memory dependency exceeded the normalized result limit."));
    }

    std::set<std::string> keys;
    std::optional<Contracts::NfcUtf8Key> tagKey;
    if (tag) {
        auto normalizedTag = canonicalizer.nfcKey(*tag);
        if (!normalizedTag) {
            return Domain::Result<void>::failure(
                std::move(normalizedTag).error());
        }
        tagKey.emplace(std::move(normalizedTag).value());
    }
    for (const auto& note : notes) {
        auto valid = Domain::validateLegacyMemoryProjection(note, includeBody);
        if (!valid) {
            return valid;
        }
        if (!includeSystem && Domain::isHiddenLegacyMemoryKey(note.key)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "A legacy-memory dependency exposed a hidden system key."));
        }
        if (prefix && !asciiCaseInsensitivePrefix(note.key, *prefix)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "A legacy-memory dependency returned a key outside the prefix filter."));
        }
        if (tagKey) {
            auto contains = containsCanonicalTag(
                note.tags, *tagKey, canonicalizer);
            if (!contains) {
                return Domain::Result<void>::failure(
                    std::move(contains).error());
            }
            if (!contains.value()) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "A legacy-memory dependency returned a note outside the tag filter."));
            }
        }
        if (!keys.insert(note.key).second) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "A legacy-memory dependency returned a duplicate note key."));
        }
    }
    return Domain::Result<void>::success();
}

} // namespace

class LegacyMemoryService::Impl final {
public:
    Impl(
        Contracts::ILegacyMemoryRepository& repository,
        std::shared_ptr<const Contracts::IUnicodeCanonicalizer>
            unicodeCanonicalizer) noexcept
        : repository_{repository},
          unicodeCanonicalizer_{std::move(unicodeCanonicalizer)}
    {
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemorySetOutcome> set(
        const Domain::LegacyMemorySetRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::LegacyMemorySetOutcome>(context, [&]() {
            auto normalized = normalizeSetRequest(
                request, *unicodeCanonicalizer_);
            if (!normalized) {
                return propagateFailure<Domain::LegacyMemorySetOutcome>(
                    std::move(normalized));
            }
            auto outcome = repository_.upsert(normalized.value(), context);
            if (!outcome) {
                return outcome;
            }
            auto value = std::move(outcome).value();
            auto valid = Domain::validateMemoryNote(value.note);
            if (!valid || !value.stored ||
                value.note.key != normalized.value().key ||
                value.note.body != normalized.value().body ||
                value.note.tags != normalized.value().tags) {
                return dependencyIntegrityFailure<Domain::LegacyMemorySetOutcome>(
                    "The legacy-memory write dependency returned an inconsistent note.");
            }
            return Domain::Result<Domain::LegacyMemorySetOutcome>::success(
                std::move(value));
        });
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryGetOutcome> get(
        const Domain::LegacyMemoryGetRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::LegacyMemoryGetOutcome>(context, [&]() {
            auto key = Domain::normalizeLegacyMemoryGetRequest(request);
            if (!key) {
                return propagateFailure<Domain::LegacyMemoryGetOutcome>(
                    std::move(key));
            }
            auto outcome = repository_.get(key.value(), context);
            if (!outcome) {
                return outcome;
            }
            auto value = std::move(outcome).value();
            if (value.key != key.value()) {
                return dependencyIntegrityFailure<Domain::LegacyMemoryGetOutcome>(
                    "The legacy-memory read dependency returned a mismatched key.");
            }
            if (value.note) {
                auto valid = Domain::validateMemoryNote(*value.note);
                if (!valid || value.note->key != key.value()) {
                    return dependencyIntegrityFailure<Domain::LegacyMemoryGetOutcome>(
                        "The legacy-memory read dependency returned an invalid note.");
                }
            }
            return Domain::Result<Domain::LegacyMemoryGetOutcome>::success(
                std::move(value));
        });
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryListOutcome> list(
        const Domain::LegacyMemoryListRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::LegacyMemoryListOutcome>(context, [&]() {
            auto query = Domain::normalizeLegacyMemoryListRequest(request);
            if (!query) {
                return propagateFailure<Domain::LegacyMemoryListOutcome>(
                    std::move(query));
            }
            auto outcome = repository_.list(query.value(), context);
            if (!outcome) {
                return outcome;
            }
            auto value = std::move(outcome).value();
            auto valid = validateProjectionPage(
                value.notes,
                query.value().limit,
                query.value().includeSystem,
                query.value().includeBody,
                query.value().prefix,
                query.value().tag,
                *unicodeCanonicalizer_);
            if (!valid || value.visibleTotal < value.notes.size()) {
                return dependencyIntegrityFailure<Domain::LegacyMemoryListOutcome>(
                    "The legacy-memory list dependency returned an inconsistent page.");
            }
            return Domain::Result<Domain::LegacyMemoryListOutcome>::success(
                std::move(value));
        });
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryDeleteOutcome> remove(
        const Domain::LegacyMemoryRemoveRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::LegacyMemoryDeleteOutcome>(context, [&]() {
            auto key = Domain::normalizeLegacyMemoryRemoveRequest(request);
            if (!key) {
                return propagateFailure<Domain::LegacyMemoryDeleteOutcome>(
                    std::move(key));
            }
            auto outcome = repository_.remove(key.value(), context);
            if (!outcome) {
                return outcome;
            }
            auto value = std::move(outcome).value();
            if (value.key != key.value() || value.deleted != value.existed ||
                value.systemKey != Domain::isSystemMemoryKey(key.value())) {
                return dependencyIntegrityFailure<Domain::LegacyMemoryDeleteOutcome>(
                    "The legacy-memory delete dependency returned an inconsistent outcome.");
            }
            return Domain::Result<Domain::LegacyMemoryDeleteOutcome>::success(
                std::move(value));
        });
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemorySearchOutcome> search(
        const Domain::LegacyMemorySearchRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::LegacyMemorySearchOutcome>(context, [&]() {
            auto query = Domain::normalizeLegacyMemorySearchRequest(request);
            if (!query) {
                return propagateFailure<Domain::LegacyMemorySearchOutcome>(
                    std::move(query));
            }
            auto outcome = repository_.search(query.value(), context);
            if (!outcome) {
                return outcome;
            }
            auto value = std::move(outcome).value();
            auto valid = validateProjectionPage(
                value.notes,
                query.value().limit,
                query.value().includeSystem,
                query.value().includeBody,
                std::nullopt,
                std::nullopt,
                *unicodeCanonicalizer_);
            if (!valid || value.query != query.value().query) {
                return dependencyIntegrityFailure<Domain::LegacyMemorySearchOutcome>(
                    "The legacy-memory search dependency returned an inconsistent page.");
            }
            return Domain::Result<Domain::LegacyMemorySearchOutcome>::success(
                std::move(value));
        });
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryPurgeOutcome> purge(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::LegacyMemoryPurgeOutcome>(context, [&]() {
            auto valid = Domain::validateDestructiveConfirmation(
                confirmation, PurgeAction, PurgeScope, PurgeToken);
            if (!valid) {
                return propagateFailure<Domain::LegacyMemoryPurgeOutcome>(
                    std::move(valid));
            }
            auto outcome = repository_.purge(confirmation, context);
            if (!outcome) {
                return outcome;
            }
            auto value = std::move(outcome).value();
            if (!value.verified) {
                return dependencyIntegrityFailure<Domain::LegacyMemoryPurgeOutcome>(
                    "The legacy-memory purge dependency did not verify its result.");
            }
            return Domain::Result<Domain::LegacyMemoryPurgeOutcome>::success(
                std::move(value));
        });
    }

    [[nodiscard]] Domain::Result<void> quickCheck(
        const Domain::OperationContext& context) noexcept
    {
        return execute<void>(context, [&]() {
            return repository_.quickCheck(context);
        });
    }

    void shutdown() noexcept
    {
        bool leader{};
        try {
            std::unique_lock lock{lifecycleMutex_};
            if (shutdownComplete_.load(std::memory_order_acquire)) {
                return;
            }
            if (!accepting_) {
                lifecycleChanged_.wait(lock, [&]() {
                    return shutdownComplete_.load(std::memory_order_acquire);
                });
                return;
            }

            accepting_ = false;
            leader = true;
            lifecycleChanged_.wait(
                lock, [&]() { return activeOperations_ == 0U; });
            lock.unlock();
            repository_.close();
            shutdownComplete_.store(true, std::memory_order_release);
            lifecycleChanged_.notify_all();
        } catch (...) {
            if (leader) {
                repository_.close();
                shutdownComplete_.store(true, std::memory_order_release);
                lifecycleChanged_.notify_all();
            }
        }
    }

private:
    class Admission final {
    public:
        explicit Admission(Impl& owner) noexcept : owner_{&owner} {}

        Admission(const Admission&) = delete;
        Admission& operator=(const Admission&) = delete;

        Admission(Admission&& other) noexcept
            : owner_{std::exchange(other.owner_, nullptr)}
        {
        }

        Admission& operator=(Admission&& other) noexcept
        {
            if (this != &other) {
                release();
                owner_ = std::exchange(other.owner_, nullptr);
            }
            return *this;
        }

        ~Admission() noexcept { release(); }

    private:
        void release() noexcept
        {
            if (owner_ != nullptr) {
                owner_->releaseOperation();
                owner_ = nullptr;
            }
        }

        Impl* owner_{};
    };

    [[nodiscard]] Domain::Result<Admission> admit(
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto valid = validateContext(context);
            if (!valid) {
                return propagateFailure<Admission>(std::move(valid));
            }

            std::lock_guard lock{lifecycleMutex_};
            valid = validateContext(context);
            if (!valid) {
                return propagateFailure<Admission>(std::move(valid));
            }
            if (!accepting_) {
                return Domain::Result<Admission>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The legacy-memory service is shutting down."));
            }
            if (activeOperations_ == std::numeric_limits<std::size_t>::max()) {
                return Domain::Result<Admission>::failure(Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The legacy-memory operation admission counter overflowed."));
            }
            ++activeOperations_;
            return Domain::Result<Admission>::success(Admission{*this});
        } catch (...) {
            return internalFailure<Admission>(
                "The legacy-memory operation could not be admitted.");
        }
    }

    void releaseOperation() noexcept
    {
        std::lock_guard lock{lifecycleMutex_};
        if (activeOperations_ != 0U) {
            --activeOperations_;
        }
        if (!accepting_ && activeOperations_ == 0U) {
            lifecycleChanged_.notify_all();
        }
    }

    template <typename T, typename Function>
    [[nodiscard]] Domain::Result<T> execute(
        const Domain::OperationContext& context,
        Function&& operation) noexcept
    {
        try {
            auto admitted = admit(context);
            if (!admitted) {
                return propagateFailure<T>(std::move(admitted));
            }
            [[maybe_unused]] auto admission = std::move(admitted).value();
            return std::forward<Function>(operation)();
        } catch (...) {
            return internalFailure<T>(
                "The legacy-memory application boundary failed internally.");
        }
    }

    Contracts::ILegacyMemoryRepository& repository_;
    std::shared_ptr<const Contracts::IUnicodeCanonicalizer>
        unicodeCanonicalizer_;
    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleChanged_;
    std::size_t activeOperations_{};
    bool accepting_{true};
    std::atomic<bool> shutdownComplete_{};
};

LegacyMemoryService::LegacyMemoryService(
    Contracts::ILegacyMemoryRepository& repository,
    std::shared_ptr<const Contracts::IUnicodeCanonicalizer>
        unicodeCanonicalizer)
    : implementation_{[&]() {
          if (!unicodeCanonicalizer) {
              throw std::invalid_argument{
                  "LegacyMemoryService requires Unicode canonicalization."};
          }
          return std::make_unique<Impl>(
              repository, std::move(unicodeCanonicalizer));
      }()}
{
}

LegacyMemoryService::~LegacyMemoryService() noexcept
{
    implementation_->shutdown();
}

Domain::Result<Domain::LegacyMemorySetOutcome> LegacyMemoryService::set(
    const Domain::LegacyMemorySetRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->set(request, context);
}

Domain::Result<Domain::LegacyMemoryGetOutcome> LegacyMemoryService::get(
    const Domain::LegacyMemoryGetRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->get(request, context);
}

Domain::Result<Domain::LegacyMemoryListOutcome> LegacyMemoryService::list(
    const Domain::LegacyMemoryListRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->list(request, context);
}

Domain::Result<Domain::LegacyMemoryDeleteOutcome> LegacyMemoryService::remove(
    const Domain::LegacyMemoryRemoveRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->remove(request, context);
}

Domain::Result<Domain::LegacyMemorySearchOutcome> LegacyMemoryService::search(
    const Domain::LegacyMemorySearchRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->search(request, context);
}

Domain::Result<Domain::LegacyMemoryPurgeOutcome> LegacyMemoryService::purge(
    const Domain::DestructiveConfirmation& confirmation,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->purge(confirmation, context);
}

Domain::Result<void> LegacyMemoryService::quickCheck(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->quickCheck(context);
}

void LegacyMemoryService::shutdown() noexcept
{
    implementation_->shutdown();
}

} // namespace ForgeConductor::Application
