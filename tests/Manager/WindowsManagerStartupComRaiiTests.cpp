#include "Infrastructure/Windows/Detail/UniqueComInterface.h"
#include "Infrastructure/Windows/Detail/UniqueBstr.h"
#include "Infrastructure/Windows/Detail/UniqueVariant.h"
#include "Infrastructure/Windows/Detail/Win32Error.h"
#include "Infrastructure/TestSupport.h"

#include <Windows.h>
#include <oleauto.h>

#include <atomic>
#include <cstddef>
#include <string>

namespace ForgeConductor::Tests {
namespace {

namespace Detail = Infrastructure::Windows::Detail;

class FakeUnknown final : public IUnknown {
public:
    explicit FakeUnknown(bool& destroyed) noexcept
        : destroyed_{destroyed}
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        const IID& interfaceId,
        void** result) noexcept override
    {
        if (result == nullptr) {
            return E_POINTER;
        }
        *result = nullptr;
        if (interfaceId != IID_IUnknown) {
            return E_NOINTERFACE;
        }
        *result = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override
    {
        return static_cast<ULONG>(++references_);
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override
    {
        const auto remaining = --references_;
        if (remaining == 0U) {
            destroyed_ = true;
            delete this;
            return 0U;
        }
        return static_cast<ULONG>(remaining);
    }

    [[nodiscard]] std::size_t referenceCount() const noexcept
    {
        return references_.load();
    }

private:
    ~FakeUnknown() = default;

    std::atomic_size_t references_{1U};
    bool& destroyed_;
};

void queryInterfaceIsSafeWhenSourceAliasesDestination()
{
    bool destroyed = false;
    auto* raw = new FakeUnknown{destroyed};
    Detail::UniqueComInterface<IUnknown> owner{raw};

    const HRESULT queried = Detail::queryComInterface(owner.get(), owner);
    require(queried == S_OK, "an aliasing IUnknown query failed");
    require(
        owner.get() == raw && !destroyed && raw->referenceCount() == 1U,
        "an aliasing query released its source before QueryInterface completed");

    const HRESULT nullQuery =
        Detail::queryComInterface<IUnknown, IUnknown>(nullptr, owner);
    require(
        nullQuery == E_POINTER && owner.get() == raw && !destroyed,
        "a null query unexpectedly cleared the existing COM owner");

    owner.reset();
    require(destroyed, "the alias-safe COM owner leaked its final reference");
}

void variantPutNeverOrphansALockedSafeArray()
{
    Detail::UniqueVariant value;
    SAFEARRAY* array = ::SafeArrayCreateVector(VT_UI1, 0L, 1U);
    require(array != nullptr, "the SAFEARRAY fixture could not be allocated");

    value.get().vt = static_cast<VARTYPE>(VT_ARRAY | VT_UI1);
    value.get().parray = array;
    require(
        ::SafeArrayLock(array) == S_OK,
        "the SAFEARRAY fixture could not be locked");

    require(
        value.put() == nullptr &&
            value.get().vt == static_cast<VARTYPE>(VT_ARRAY | VT_UI1) &&
            value.get().parray == array,
        "UniqueVariant reinitialized and orphaned a locked SAFEARRAY");

    require(
        ::SafeArrayUnlock(array) == S_OK,
        "the SAFEARRAY fixture could not be unlocked");
    require(
        value.put() != nullptr && value.get().vt == VT_EMPTY,
        "UniqueVariant did not clear the SAFEARRAY after it became releasable");
}

void bstrOwnershipPreservesEmbeddedNulls()
{
    const std::wstring source{L'a', L'\0', L'b'};
    auto value = Detail::UniqueBstr::copy(source);
    require(
        value && value.view().size() == source.size() &&
            value.view() == std::wstring_view{source},
        "UniqueBstr truncated an embedded NUL");

    Detail::UniqueBstr moved{std::move(value)};
    require(
        !value && moved.view() == std::wstring_view{source},
        "UniqueBstr move ownership changed the BSTR contents");

    BSTR detached = moved.detach();
    require(
        detached != nullptr && !moved && ::SysStringLen(detached) == 3U,
        "UniqueBstr detach did not transfer exact ownership");
    ::SysFreeString(detached);

    auto replacement = Detail::UniqueBstr::copy(L"first");
    BSTR* output = replacement.put();
    require(
        output != nullptr && !replacement,
        "UniqueBstr put did not release previous storage");
    *output = ::SysAllocString(L"second");
    require(
        replacement.view() == L"second",
        "UniqueBstr put did not adopt output storage");
}

void hresultErrorsRetainStableDiagnostics()
{
    const auto denied = Detail::makeHResultError(
        "open Task Scheduler",
        E_ACCESSDENIED,
        Domain::ErrorCodes::Unauthorized,
        true);
    require(
        denied.code == Domain::ErrorCodes::Unauthorized && denied.retryable &&
            denied.message.find("0x80070005") != std::string::npos,
        "the HRESULT mapper lost its exact code or caller metadata");

    constexpr HRESULT unknown = static_cast<HRESULT>(0x81234567UL);
    const auto unmapped = Detail::makeHResultError(
        "perform Task Scheduler operation",
        unknown,
        Domain::ErrorCodes::IntegrityFailure);
    require(
        unmapped.code == Domain::ErrorCodes::IntegrityFailure &&
            !unmapped.retryable &&
            unmapped.message.find("0x81234567") != std::string::npos,
        "the HRESULT mapper relied on localized text for an unknown code");
}

} // namespace

void registerWindowsManagerStartupComRaiiTests(TestRegistry& tests)
{
    addTest(
        tests,
        "manager_startup.com_raii.alias_query",
        queryInterfaceIsSafeWhenSourceAliasesDestination);
    addTest(
        tests,
        "manager_startup.com_raii.locked_safearray",
        variantPutNeverOrphansALockedSafeArray);
    addTest(
        tests,
        "manager_startup.com_raii.bstr",
        bstrOwnershipPreservesEmbeddedNulls);
    addTest(
        tests,
        "manager_startup.com_raii.hresult",
        hresultErrorsRetainStableDiagnostics);
}

} // namespace ForgeConductor::Tests
