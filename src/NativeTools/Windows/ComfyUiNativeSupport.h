#pragma once

#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Domain/Utf8.h"
#include "Infrastructure/Windows/Detail/UniqueHandle.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace ForgeConductor::NativeTools::Windows::ComfyDetail {
using Json = nlohmann::json;
using Handle = Infrastructure::Windows::Detail::UniqueHandle;
struct Failure final { Domain::Error error; };
[[noreturn]] void fail(std::string_view code, std::string message);
template<class T> T take(Domain::Result<T> value) { if (!value) throw Failure{value.error()}; return std::move(value).value(); }
void check(const Domain::OperationContext& context);
std::wstring wide(std::string_view value);
std::string utf8(std::wstring_view value);
std::string pathText(const std::filesystem::path& path);
Json parse(std::string_view value, std::size_t maximum = 16U * 1024U * 1024U);
bool promptGraphMatches(const Json& submitted, const Json& observed);
std::string mediaContentType(std::span<const unsigned char> prefix, const Json& format, bool hasVideo);
std::string text(const Json& value, std::string_view key, std::size_t maximum = 32768U,
                 bool required = true, std::string fallback = {});
bool contained(const std::filesystem::path& root, const std::filesystem::path& path);
void regularParents(const std::filesystem::path& path);
void writeJson(const std::filesystem::path& path, const Json& value);
void replaceContents(const std::filesystem::path& path, std::string_view bytes,
                     const Domain::OperationContext& context);
Json readJson(const std::filesystem::path& path, std::size_t maximum = 16U * 1024U * 1024U);
std::uint64_t preparationDownloadCharge(const Json& manifest);
void reconcilePreparationReceives(Json& manifest, std::string_view previousState,
                                  std::span<const std::uint64_t> stagingBytes);
std::uint64_t admitPreparationReceive(Json& manifest, std::size_t transfer, std::uint64_t proposed);
void recordPreparationReceive(Json& manifest, std::size_t transfer, std::uint64_t bytes);
void finishPreparationReceive(Json& manifest, bool certain);
Json fileFacts(HANDLE file, const Domain::OperationContext& context);
Json fileFacts(const std::filesystem::path& path, const Domain::OperationContext& context);
Json copyFileContents(HANDLE source, HANDLE output, std::uint64_t maximum,
                      const Domain::OperationContext& context,
                      const std::function<void(std::uint64_t)>& beforeWrite = {});
Json stageBuildInterpreter(const std::filesystem::path& provider,
                          const std::filesystem::path& overlay,
                          const std::filesystem::path& directory,
                          const Domain::OperationContext& context,
                          const std::function<void(const std::filesystem::path&, Domain::FileAccess)>& authorize,
                          const std::function<void(std::uint64_t)>& reserve);
Json directoryFacts(const std::filesystem::path& path, std::uint64_t maximum,
                    const Domain::OperationContext& context);
bool retainsDirectoryFiles(const Json& expected, const Json& observed);
Json resolveFfmpegRelease(const Json& releases);
std::uint64_t archiveRequiredBytes(std::string_view verboseInventory, std::uint64_t allocationUnit,
                                  std::uint64_t maximum);
std::wstring quote(std::wstring_view value);
struct Process final { Handle process; Handle job; DWORD pid{}; std::uint64_t creationTime{}; };
Process startProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                     const std::filesystem::path& directory, const std::filesystem::path& log,
                     bool transientLifetime = false);
Json runProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                const std::filesystem::path& directory, const std::filesystem::path& log,
                const Domain::OperationContext& context, const std::function<void()>& observe = {});
struct HttpResult final { unsigned status{}; std::string contentType; std::string body; std::uint64_t bytes{}; std::string location; };
inline bool reusablePublisherMetadata(unsigned status) noexcept { return status==200U; }
inline Json publisherMetadataDocument(unsigned status,std::string_view body,std::string_view sha256,std::uint64_t bytes) {
    Json value{{"status",status},{"body_sha256",sha256},{"body_bytes",bytes}};
    if(reusablePublisherMetadata(status)){try{value["data"]=parse(body);}catch(const Json::parse_error&){fail(Domain::ErrorCodes::MalformedMessage,"Successful publisher metadata did not contain valid JSON.");}}
    else{const auto diagnostic=body.substr(0,2048U);value["data"]={{"diagnostic",Domain::isValidUtf8(diagnostic)?std::string{diagnostic}:std::string{"Non-UTF8 publisher response; inspect its retained SHA-256 and byte count."}}};}
    return value;
}
inline Json retainedPublisherMetadata(Json& manifest,std::string_view key,std::string_view url,
    const std::filesystem::path& transaction,const Domain::OperationContext& context) {
    const auto encodedKey=std::string{key};if(!manifest.at("metadata").contains(encodedKey))return nullptr;
    const auto receipt=manifest.at("metadata").at(encodedKey);const std::filesystem::path path{wide(text(receipt,"path"))};
    if(text(receipt,"url")!=url||!contained(transaction,path)||fileFacts(path,context).at("sha256")!=receipt.at("sha256"))fail(Domain::ErrorCodes::IntegrityFailure,"Retained publisher metadata changed or belongs to another request.");
    const auto cached=readJson(path);if(reusablePublisherMetadata(cached.at("status").get<unsigned>()))return cached;
    if(!manifest.contains("metadata_retries"))manifest["metadata_retries"]=Json::array();
    manifest["metadata_retries"].push_back({{"url",url},{"previous",receipt},{"status",cached.at("status")}});manifest["metadata"].erase(encodedKey);return nullptr;
}
HttpResult http(std::string_view url, std::string_view method, std::string_view contentType,
                std::string_view body, std::size_t maximum, const Domain::OperationContext& context,
                HANDLE output = nullptr, std::uint64_t streamMaximum = 0U,
                const std::function<void(std::uint64_t)>& received = {},
                HANDLE upload = nullptr, std::uint64_t uploadBytes = 0U,
                std::string_view prefix = {}, std::string_view suffix = {},
                const std::function<std::uint64_t(std::uint64_t)>& beforeRead = {});
HttpResult download(std::string_view url, std::size_t maximum, const Domain::OperationContext& context,
                    HANDLE output, std::uint64_t streamMaximum,
                    const std::function<void(std::uint64_t)>& received = {},
                    const std::function<std::uint64_t(std::uint64_t)>& beforeRead = {});
std::string encode(std::string_view value);
class Cdp final {
public:
    explicit Cdp(std::string_view websocket, const Domain::OperationContext& context);
    ~Cdp();
    Cdp(const Cdp&) = delete;
    Cdp& operator=(const Cdp&) = delete;
    Json call(std::string_view method, const Json& parameters, const Domain::OperationContext& context);
    Json receive(const Domain::OperationContext& context);
private:
    struct State;
    std::unique_ptr<State> state_;
    unsigned id_{};
};
} // namespace ForgeConductor::NativeTools::Windows::ComfyDetail
