#include "Infrastructure/Windows/Detail/DashboardConnectionResponseCatalog.h"

#include "ForgeConductor/Dashboard/DashboardHttpResponse.h"

#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Dashboard = ForgeConductor::Dashboard;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Domain = ForgeConductor::Domain;

using Catalog = Detail::DashboardConnectionResponseCatalog;
using ImmutableBytes = Catalog::ImmutableBytes;

std::size_t assertionCount{};

void require(const bool condition, const std::string_view message)
{
    ++assertionCount;
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

static_assert(std::is_final_v<Catalog>);
static_assert(!std::is_copy_constructible_v<Catalog>);
static_assert(!std::is_move_constructible_v<Catalog>);
static_assert(std::is_same_v<
              ImmutableBytes::element_type,
              const std::vector<std::byte>>);
static_assert(noexcept(Catalog::create()));
static_assert(noexcept(
    std::declval<const Catalog&>().genericServiceUnavailable()));
static_assert(noexcept(
    std::declval<const Catalog&>().streamUnavailable()));
static_assert(noexcept(
    std::declval<const Catalog&>().internalFailure()));

[[nodiscard]] std::string text(const ImmutableBytes& bytes)
{
    require(bytes != nullptr, "response catalog returned null bytes");
    return std::string{
        reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}

void requireCompleteError(
    const ImmutableBytes& bytes,
    const std::string_view statusLine,
    const std::string_view code,
    const std::string_view message)
{
    require(bytes != nullptr && !bytes->empty(),
            "fixed response bytes were missing");
    require(bytes->size() <=
                Dashboard::DashboardHttpResponseEncoder::
                    MaximumEncodedResponseBytes,
            "fixed response exceeded the response ceiling");
    const auto wire = text(bytes);
    require(wire.starts_with(statusLine),
            "fixed response used the wrong status line");
    require(wire.find("Content-Type: application/json; charset=utf-8\r\n") !=
                std::string::npos,
            "fixed response lost its JSON content type");
    require(wire.find("Connection: close\r\n") != std::string::npos,
            "fixed response did not close the connection");
    require(wire.find("Cache-Control: no-store\r\n") != std::string::npos,
            "fixed response lost its no-store policy");
    require(wire.find("X-Content-Type-Options: nosniff\r\n") !=
                std::string::npos,
            "fixed response lost its security headers");
    const auto separator = wire.find("\r\n\r\n");
    require(separator != std::string::npos,
            "fixed response had no header terminator");
    const auto body = wire.substr(separator + 4U);
    require(body.find("\"ok\":false") != std::string::npos,
            "fixed response lost its closed error shape");
    require(body.find("\"code\":\"" + std::string{code} + "\"") !=
                std::string::npos,
            "fixed response used the wrong error code");
    require(body.find("\"message\":\"" + std::string{message} + "\"") !=
                std::string::npos,
            "fixed response used the wrong error message");
    require(wire.find(
                "Content-Length: " + std::to_string(body.size()) + "\r\n") !=
                std::string::npos,
            "fixed response content length did not match its body");
}

void catalogPreencodesThreeClosedImmutableResponses()
{
    auto catalog = take(Catalog::create());
    requireCompleteError(
        catalog->genericServiceUnavailable(),
        "HTTP/1.1 503 Service Unavailable\r\n",
        "service_unavailable",
        "The dashboard service is temporarily unavailable.");
    requireCompleteError(
        catalog->streamUnavailable(),
        "HTTP/1.1 503 Service Unavailable\r\n",
        "stream_unavailable",
        "The dashboard telemetry stream is temporarily unavailable.");
    requireCompleteError(
        catalog->internalFailure(),
        "HTTP/1.1 500 Internal Server Error\r\n",
        "internal_failure",
        "The dashboard operation failed safely.");

    require(catalog->genericServiceUnavailable() !=
                catalog->streamUnavailable(),
            "generic and stream overload responses shared the wrong bytes");
    require(catalog->genericServiceUnavailable() != catalog->internalFailure(),
            "generic overload and internal failure shared the wrong bytes");
}

void accessorsShareStableStorageThatOutlivesTheCatalog()
{
    auto catalog = take(Catalog::create());
    const auto genericAddress =
        catalog->genericServiceUnavailable().get();
    const auto streamAddress = catalog->streamUnavailable().get();
    const auto internalAddress = catalog->internalFailure().get();
    require(
        catalog->genericServiceUnavailable().get() == genericAddress &&
            catalog->streamUnavailable().get() == streamAddress &&
            catalog->internalFailure().get() == internalAddress,
        "catalog accessors replaced pre-encoded storage");

    auto generic = catalog->genericServiceUnavailable();
    auto stream = catalog->streamUnavailable();
    auto internal = catalog->internalFailure();
    catalog.reset();

    require(generic.get() == genericAddress && !generic->empty(),
            "generic response did not survive catalog destruction");
    require(stream.get() == streamAddress && !stream->empty(),
            "stream response did not survive catalog destruction");
    require(internal.get() == internalAddress && !internal->empty(),
            "internal response did not survive catalog destruction");
    require(text(generic).starts_with("HTTP/1.1 503"),
            "surviving generic response changed");
    require(text(stream).find("stream_unavailable") != std::string::npos,
            "surviving stream response changed");
    require(text(internal).starts_with("HTTP/1.1 500"),
            "surviving internal response changed");
}

} // namespace

int main()
{
    try {
        catalogPreencodesThreeClosedImmutableResponses();
        accessorsShareStableStorageThatOutlivesTheCatalog();
        std::cout << "Dashboard connection response catalog tests passed ("
                  << assertionCount << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard connection response catalog tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard connection response catalog tests failed with "
                     "an unknown error.\n";
        return 1;
    }
}
