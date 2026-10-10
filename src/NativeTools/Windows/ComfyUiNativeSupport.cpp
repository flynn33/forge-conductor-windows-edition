#include "ComfyUiNativeSupport.h"
#include "ForgeConductor/Domain/Utf8.h"
#include "ForgeConductor/Domain/Identifiers.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "Infrastructure/Windows/Detail/OperationContextGuard.h"
#include <Windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <utility>

namespace ForgeConductor::NativeTools::Windows::ComfyDetail {
namespace Utf = Infrastructure::Windows::Detail;
[[noreturn]] void fail(std::string_view code, std::string message) { throw Failure{Domain::makeError(code, std::move(message))}; }
void check(const Domain::OperationContext& context) {
    auto result = Utf::validateOperationContext(context, std::chrono::steady_clock::now(), "ComfyUI native operation");
    if (!result) throw Failure{result.error()};
}
std::wstring wide(std::string_view value) { return take(Utf::strictUtf8ToUtf16(value)); }
std::string utf8(std::wstring_view value) { return take(Utf::strictUtf16ToUtf8(value)); }
std::string pathText(const std::filesystem::path& path) { return utf8(path.native()); }
Json parse(std::string_view value, const std::size_t maximum) {
    if (value.size() > maximum || !Domain::isValidUtf8(value) || value.find('\0') != std::string_view::npos)
        fail(Domain::ErrorCodes::PayloadTooLarge, "ComfyUI JSON exceeds its UTF-8 bound.");
    std::vector<std::unordered_set<std::string>> keys;
    return Json::parse(value, [&](int depth, Json::parse_event_t event, Json& item) {
        if (depth > 64) fail(Domain::ErrorCodes::LimitExceeded, "ComfyUI JSON exceeds depth 64.");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(item.get<std::string>()).second)
            fail(Domain::ErrorCodes::MalformedMessage, "ComfyUI JSON has duplicate keys.");
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
}
bool promptGraphMatches(const Json& submitted,const Json& observed) {
    if(submitted==observed) return true;
    if(!submitted.is_object() || !observed.is_object() || submitted.size()!=observed.size()) return false;
    for(const auto& [id,node]:submitted.items()) {
        if(!observed.contains(id)) return false;
        const auto& actual=observed.at(id);
        if(node==actual) continue;
        if(!node.is_object() || !actual.is_object() || node.size()!=actual.size()) return false;
        for(const auto& [field,value]:node.items()) {
            if(!actual.contains(field)) return false;
            const auto& retained=actual.at(field);
            if(field!="inputs") {if(value!=retained) return false;continue;}
            if(!value.is_object() || !retained.is_object() || value.size()!=retained.size()) return false;
            for(const auto& [input,expected]:value.items()) {
                if(!retained.contains(input)) return false;
                const auto& received=retained.at(input);
                if(expected==received) continue;
                // ComfyUI validation removes this wrapper before queueing.
                if(!expected.is_object() || expected.size()!=1U || !expected.contains("__value__") || expected.at("__value__")!=received) return false;
            }
        }
    }
    return true;
}
std::string mediaContentType(std::span<const unsigned char> prefix,const Json& format,const bool hasVideo) {
    prefix=prefix.first((std::min)(prefix.size(),std::size_t{1024U}));
    if(!format.is_object() || !format.contains("format_name") || !format.at("format_name").is_string())
        fail(Domain::ErrorCodes::IntegrityFailure,"Verified media has no FFprobe container identity.");
    const auto name=format.at("format_name").get<std::string>();
    if(name.empty() || name.size()>128U)fail(Domain::ErrorCodes::IntegrityFailure,"FFprobe container identity exceeds its content classification bound.");
    const auto identifies=[&](std::string_view token){
        for(std::size_t start=0U;start<name.size();){const auto end=name.find(',',start);const auto length=end==std::string::npos?name.size()-start:end-start;
            if(std::string_view{name}.substr(start,length)==token)return true;if(end==std::string::npos)break;start=end+1U;}
        return false;
    };
    if(identifies("matroska") || identifies("webm")) {
        const auto invalid=[](){fail(Domain::ErrorCodes::IntegrityFailure,"Media has no complete unambiguous EBML Header DocType within the 1024-byte inspection bound.");};
        if(prefix.size()<5U || prefix[0]!=0x1aU || prefix[1]!=0x45U || prefix[2]!=0xdfU || prefix[3]!=0xa3U)invalid();
        std::size_t cursor=4U;
        const auto vint=[&](std::size_t end,bool identifier){
            if(cursor>=end)invalid();const auto first=prefix[cursor];unsigned char marker=0x80U;unsigned width=1U;
            while(marker!=0U && (first&marker)==0U){marker>>=1U;++width;}
            if(marker==0U || width>(identifier?4U:8U) || width>end-cursor)invalid();
            std::uint64_t data=static_cast<std::uint64_t>(first&static_cast<unsigned char>(marker-1U)),value=identifier?first:data;
            for(unsigned index=1U;index<width;++index){data=(data<<8U)|prefix[cursor+index];value=(value<<8U)|prefix[cursor+index];}
            const auto unknown=(1ULL<<(width*7U))-1ULL;if(data==unknown || (identifier&&data==0U))invalid();
            cursor+=width;return value;
        };
        const auto bytes=vint(prefix.size(),false);if(bytes>prefix.size()-cursor)invalid();const auto end=cursor+static_cast<std::size_t>(bytes);
        std::string docType;unsigned elements{};
        while(cursor<end) {
            if(++elements>256U)invalid();const auto id=vint(end,true),size=vint(end,false);if(size>end-cursor)invalid();
            if(id==0x4282U) {
                if(!docType.empty() || size==0U || size>32U)invalid();
                docType.assign(reinterpret_cast<const char*>(prefix.data()+cursor),static_cast<std::size_t>(size));
                while(!docType.empty() && docType.back()=='\0')docType.pop_back();
                if(docType.empty())invalid();
            }
            cursor+=static_cast<std::size_t>(size);
        }
        if(docType=="webm")return hasVideo?"video/webm":"audio/webm";
        if(docType=="matroska")return hasVideo?"video/x-matroska":"audio/x-matroska";
        invalid();
    }
    if(identifies("mov") || identifies("mp4") || identifies("m4a") || identifies("3gp") || identifies("3g2") || identifies("mj2")) {
        const auto tags=format.value("tags",Json::object());
        if(!tags.is_object() || !tags.contains("major_brand") || !tags.at("major_brand").is_string() || tags.at("major_brand").get_ref<const std::string&>().size()!=4U)
            fail(Domain::ErrorCodes::IntegrityFailure,"ISO media has no bounded four-byte major_brand content identity.");
        const auto brand=tags.at("major_brand").get<std::string>();
        if(prefix.size()>=12U && prefix[4]=='f' && prefix[5]=='t' && prefix[6]=='y' && prefix[7]=='p' &&
            std::string_view{reinterpret_cast<const char*>(prefix.data()+8U),4U}!=brand)
            fail(Domain::ErrorCodes::IntegrityFailure,"ISO media header and FFprobe major_brand identities disagree.");
        if(brand=="qt  ")return "video/quicktime";
        static const std::set<std::string> threeGpp{"3gp4","3gp5","3gp6","3gp7","3gp8","3gp9","3ge6","3ge7","3ge9","3gf9","3gg6","3gg9","3gh9","3gm9","3gmA","3gr6","3gr9","3gs6","3gs9","3gt8","3gt9","3gtv","3gvr"};
        if(threeGpp.contains(brand))return hasVideo?"video/3gpp":"audio/3gpp";
        if(brand=="3g2a")return hasVideo?"video/3gpp2":"audio/3gpp2";
        static const std::set<std::string> mp4Brands{"isom","iso2","iso3","iso4","iso5","iso6","iso7","iso8","iso9","isoa","isob","isoc","mp41","mp42","avc1","av01","M4A ","M4B ","M4P ","M4V "};
        if(mp4Brands.contains(brand))return hasVideo?"video/mp4":"audio/mp4";
        fail(Domain::ErrorCodes::IntegrityFailure,"ISO media has no recognized unambiguous major_brand; container MIME type cannot be inferred from the shared FFprobe demuxer.");
    }
    if(name=="gif")return "image/gif";
    if(name=="apng")return "image/apng";
    if(name=="webp" || name=="webp_pipe")return "image/webp";
    if(name=="png_pipe")return "image/png";
    if(name=="avi")return "video/x-msvideo";
    if(name=="ogg")return hasVideo?"video/ogg":"audio/ogg";
    if(!hasVideo) {
        if(name=="wav")return "audio/wav";
        if(name=="mp3")return "audio/mpeg";
        if(name=="flac")return "audio/flac";
        if(name=="aac")return "audio/aac";
    }
    fail(Domain::ErrorCodes::IntegrityFailure,"Generated media container has no supported content MIME classification: "+name);
}
std::string text(const Json& value, std::string_view key, std::size_t maximum, bool required, std::string fallback) {
    const auto item = value.find(std::string{key});
    if (item == value.end()) {
        if (required) fail(Domain::ErrorCodes::InvalidRequest, std::string{key} + " is required.");
        return fallback;
    }
    if (!item->is_string()) fail(Domain::ErrorCodes::InvalidRequest, std::string{key} + " must be text.");
    auto result = item->get<std::string>();
    if ((required && result.empty()) || result.size() > maximum || result.find('\0') != std::string::npos || !Domain::isValidUtf8(result))
        fail(Domain::ErrorCodes::InvalidRequest, std::string{key} + " is outside its UTF-8 bound.");
    return result;
}
bool contained(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto a = std::filesystem::absolute(root).lexically_normal();
    const auto b = std::filesystem::absolute(path).lexically_normal();
    auto x = a.begin(), y = b.begin();
    for (; x != a.end(); ++x, ++y) {
        if (y == b.end() || _wcsicmp(x->c_str(), y->c_str()) != 0) return false;
    }
    return true;
}
void regularParents(const std::filesystem::path& path) {
    auto current = std::filesystem::absolute(path).lexically_normal();
    while (!current.empty()) {
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            fail(Domain::ErrorCodes::PathOutsideAuthority, "ComfyUI path crosses a reparse point: " + pathText(current));
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
}
void writeJson(const std::filesystem::path& path, const Json& value) {
    regularParents(path);
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = std::filesystem::path{path.native() + L".new"};
    const auto encoded = value.dump(2);
    Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!file) fail(Domain::ErrorCodes::InternalFailure, "Cannot write ComfyUI manifest.");
    DWORD written{};
    if (encoded.size() > MAXDWORD || !WriteFile(file.get(), encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr) || written != encoded.size() || !FlushFileBuffers(file.get()))
        fail(Domain::ErrorCodes::InternalFailure, "Cannot flush ComfyUI manifest.");
    file.reset();
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        fail(Domain::ErrorCodes::InternalFailure, "Cannot commit ComfyUI manifest.");
}
void replaceContents(const std::filesystem::path& path,std::string_view bytes,const Domain::OperationContext& context) {
    check(context);regularParents(path);if(bytes.size()>1024U*1024U)fail(Domain::ErrorCodes::PayloadTooLarge,"ComfyUI configuration replacement exceeds its bound.");
    Infrastructure::Windows::WindowsUuidGenerator generator;
    const auto staged=std::filesystem::path{path.native()+L".forge-"+wide(take(generator.next()).value())+L".new"};regularParents(staged);
    Handle file{CreateFileW(staged.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
    if(!file)fail(Domain::ErrorCodes::Conflict,"Cannot stage ComfyUI configuration replacement.");
    DWORD written{};
    if(!WriteFile(file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)||written!=bytes.size()||!FlushFileBuffers(file.get()))
        fail(Domain::ErrorCodes::InternalFailure,"Cannot flush ComfyUI configuration replacement; retained staging file: "+pathText(staged));
    file.reset();check(context);
    if(!MoveFileExW(staged.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        fail(Domain::ErrorCodes::Conflict,"Cannot atomically replace ComfyUI configuration; original and staging evidence retained: "+pathText(staged));
}
Json readJson(const std::filesystem::path& path, std::size_t maximum) {
    regularParents(path);
    std::ifstream file{path, std::ios::binary};
    if (!file) fail(Domain::ErrorCodes::RecordNotFound, "ComfyUI JSON file does not exist: " + pathText(path));
    std::string value; std::array<char, 8192> buffer{};
    while (file) {
        file.read(buffer.data(), buffer.size()); const auto count = static_cast<std::size_t>(file.gcount());
        if (count > maximum - value.size()) fail(Domain::ErrorCodes::PayloadTooLarge, "ComfyUI JSON file is too large.");
        value.append(buffer.data(), count);
    }
    return parse(value, maximum);
}
Json fileFacts(HANDLE file, const Domain::OperationContext& context) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        fail(Domain::ErrorCodes::InvalidRequest, "ComfyUI artifact is not a regular file.");
    // CNG retains this buffer until Cleanup destroys the hash handle.
    std::vector<UCHAR> object;
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    struct Cleanup { BCRYPT_ALG_HANDLE& a; BCRYPT_HASH_HANDLE& h; ~Cleanup() { if(h) BCryptDestroyHash(h); if(a) BCryptCloseAlgorithmProvider(a, 0); } } cleanup{algorithm, hash};
    DWORD length{}, objectBytes{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &length, 0) < 0)
        fail(Domain::ErrorCodes::InternalFailure, "Cannot initialize ComfyUI artifact hashing.");
    object.resize(objectBytes); std::array<UCHAR, 32> digest{};
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0)
        fail(Domain::ErrorCodes::InternalFailure, "Cannot create ComfyUI artifact hash.");
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) fail(Domain::ErrorCodes::InternalFailure, "Cannot seek ComfyUI artifact.");
    std::array<UCHAR, 65536> buffer{}; std::uint64_t total{};
    for (;;) {
        check(context); DWORD read{};
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            fail(Domain::ErrorCodes::InternalFailure, "Cannot read ComfyUI artifact.");
        if (!read) break;
        if (BCryptHashData(hash, buffer.data(), read, 0) < 0) fail(Domain::ErrorCodes::InternalFailure, "Cannot hash ComfyUI artifact.");
        total += read;
    }
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        fail(Domain::ErrorCodes::InternalFailure, "Cannot finish ComfyUI artifact hash.");
    BY_HANDLE_FILE_INFORMATION after{};
    if (!GetFileInformationByHandle(file, &after) || info.nFileSizeHigh != after.nFileSizeHigh || info.nFileSizeLow != after.nFileSizeLow ||
        CompareFileTime(&info.ftLastWriteTime, &after.ftLastWriteTime) != 0)
        fail(Domain::ErrorCodes::Conflict, "ComfyUI artifact changed while reading.");
    std::string seal; constexpr char digits[] = "0123456789abcdef";
    for (auto byte : digest) { seal += digits[byte >> 4]; seal += digits[byte & 15]; }
    return {{"bytes",total},{"sha256",seal},{"volume_serial",info.dwVolumeSerialNumber},
        {"file_id",(static_cast<std::uint64_t>(info.nFileIndexHigh)<<32)|info.nFileIndexLow},
        {"modified_time",(static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime}};
}
Json fileFacts(const std::filesystem::path& path, const Domain::OperationContext& context) {
    regularParents(path);
    Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!file) fail(Domain::ErrorCodes::RecordNotFound, "Cannot open ComfyUI artifact: " + pathText(path));
    auto result = fileFacts(file.get(), context); result["path"] = pathText(path); return result;
}
Json copyFileContents(HANDLE source, HANDLE output, const std::uint64_t maximum,
    const Domain::OperationContext& context, const std::function<void(std::uint64_t)>& beforeWrite) {
    check(context);
    BY_HANDLE_FILE_INFORMATION original{}, destination{};
    if (!GetFileInformationByHandle(source, &original) ||
        !GetFileInformationByHandle(output, &destination) ||
        ((original.dwFileAttributes | destination.dwFileAttributes) &
            (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        fail(Domain::ErrorCodes::InvalidRequest, "Dependency copying requires regular source and staging files.");
    if (original.dwVolumeSerialNumber == destination.dwVolumeSerialNumber &&
        original.nFileIndexHigh == destination.nFileIndexHigh && original.nFileIndexLow == destination.nFileIndexLow)
        fail(Domain::ErrorCodes::InvalidRequest, "Dependency source and staging must be different files.");
    if (destination.nFileSizeHigh || destination.nFileSizeLow)
        fail(Domain::ErrorCodes::Conflict, "Dependency copying requires an empty staging file.");
    const auto expectedBytes = (static_cast<std::uint64_t>(original.nFileSizeHigh) << 32U) | original.nFileSizeLow;
    if (expectedBytes > maximum) fail(Domain::ErrorCodes::LimitExceeded, "Dependency copy exceeds its file size bound.");
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(source, zero, nullptr, FILE_BEGIN) || !SetFilePointerEx(output, zero, nullptr, FILE_BEGIN))
        fail(Domain::ErrorCodes::InternalFailure, "Cannot seek dependency copy files.");
    std::array<unsigned char, 65536U> buffer{};
    std::uint64_t copied{};
    for (;;) {
        check(context);
        DWORD read{};
        if (!ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            fail(Domain::ErrorCodes::InternalFailure, "Cannot read the sealed dependency source.");
        if (!read) break;
        if (read > maximum - copied) fail(Domain::ErrorCodes::LimitExceeded, "Dependency source exceeded its copy size bound.");
        if (beforeWrite) beforeWrite(read);
        check(context);
        DWORD written{};
        if (!WriteFile(output, buffer.data(), read, &written, nullptr) || written != read)
            fail(Domain::ErrorCodes::InternalFailure, "Cannot write the staged dependency copy.");
        copied += read;
    }
    BY_HANDLE_FILE_INFORMATION after{};
    if (copied != expectedBytes || !GetFileInformationByHandle(source, &after) ||
        original.nFileSizeHigh != after.nFileSizeHigh || original.nFileSizeLow != after.nFileSizeLow ||
        CompareFileTime(&original.ftLastWriteTime, &after.ftLastWriteTime) != 0)
        fail(Domain::ErrorCodes::IntegrityFailure, "Dependency source changed during its staged copy.");
    if (!FlushFileBuffers(output)) fail(Domain::ErrorCodes::InternalFailure, "Cannot flush the staged dependency copy.");
    auto facts = fileFacts(output, context);
    if (facts.at("bytes") != copied) fail(Domain::ErrorCodes::IntegrityFailure, "Staged dependency copy length changed.");
    return facts;
}
Json stageBuildInterpreter(const std::filesystem::path& provider,
    const std::filesystem::path& overlay, const std::filesystem::path& directory,
    const Domain::OperationContext& context,
    const std::function<void(const std::filesystem::path&, Domain::FileAccess)>& authorize,
    const std::function<void(std::uint64_t)>& reserve) {
    namespace Fs = std::filesystem;
    check(context); regularParents(provider); regularParents(overlay); regularParents(directory);
    if (!provider.is_absolute() || !overlay.is_absolute() || !directory.is_absolute() ||
        !Fs::is_directory(overlay) || !contained(overlay, directory) || contained(directory, provider))
        fail(Domain::ErrorCodes::InvalidRequest, "Build interpreter paths must identify the installed interpreter and its inactive dependency overlay.");
    std::vector<Fs::path> configurations, components{provider};
    for (const auto& entry : Fs::directory_iterator(provider.parent_path())) {
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().native();
        if (_wcsicmp(entry.path().extension().c_str(), L"._pth") == 0) configurations.push_back(entry.path());
        else if (_wcsicmp(entry.path().extension().c_str(), L".dll") == 0 &&
            (_wcsnicmp(name.c_str(), L"python", 6U) == 0 || _wcsnicmp(name.c_str(), L"vcruntime", 9U) == 0))
            components.push_back(entry.path());
    }
    if (configurations.empty()) return {{"python", pathText(provider)}, {"embedded", false}};
    if (configurations.size() > 8U || components.size() > 32U)
        fail(Domain::ErrorCodes::LimitExceeded, "Embedded build interpreter component inventory exceeds its bound.");
    const auto createDirectory = [&](const Fs::path& path) {
        authorize(path, Domain::FileAccess::Write);
        if (!Fs::exists(path)) authorize(path, Domain::FileAccess::Create);
        reserve(0U); Fs::create_directories(path); regularParents(path);
    };
    createDirectory(directory);
    Json copied = Json::array(), originals = Json::array(); std::uint64_t total{}, copiedBytes{};
    for (const auto& source : components) {
        check(context); authorize(source, Domain::FileAccess::Read); const auto expected = fileFacts(source, context);
        const auto bytes = expected.at("bytes").get<std::uint64_t>();
        if (bytes > 64ULL * 1024ULL * 1024ULL - total)
            fail(Domain::ErrorCodes::LimitExceeded, "Embedded build interpreter copying exceeds its content bound.");
        total += bytes; const auto destination = directory / source.filename();
        if (Fs::exists(destination)) {
            authorize(destination, Domain::FileAccess::Read); const auto observed = fileFacts(destination, context);
            if (observed.at("sha256") != expected.at("sha256") || observed.at("bytes") != expected.at("bytes"))
                fail(Domain::ErrorCodes::IntegrityFailure, "Retained build interpreter differs from its installed component.");
        } else {
            authorize(destination, Domain::FileAccess::Create); reserve(bytes);
            Handle input{CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
            Handle output{CreateFileW(destination.c_str(), GENERIC_READ | GENERIC_WRITE, 0U, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
            if (!input || !output) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Cannot stage an embedded build interpreter component.");
            const auto observed = copyFileContents(input.get(), output.get(), bytes, context, reserve);
            if (observed.at("sha256") != expected.at("sha256") || fileFacts(input.get(), context).at("sha256") != expected.at("sha256"))
                fail(Domain::ErrorCodes::IntegrityFailure, "Installed interpreter changed while its inactive build copy was staged.");
            copiedBytes += bytes;
        }
        copied.push_back({{"source", expected}, {"copy", fileFacts(destination, context)}});
    }
    const auto writeConfiguration = [&](const Fs::path& path, const std::string& content) {
        check(context); regularParents(path);
        if (Fs::exists(path)) {
            authorize(path, Domain::FileAccess::Read); std::ifstream input{path, std::ios::binary};
            std::string observed(content.size() + 1U, '\0'); input.read(observed.data(), static_cast<std::streamsize>(observed.size()));
            observed.resize(static_cast<std::size_t>(input.gcount()));
            if (observed != content) fail(Domain::ErrorCodes::IntegrityFailure, "Retained build interpreter search paths changed.");
        } else {
            authorize(path, Domain::FileAccess::Create); authorize(path, Domain::FileAccess::Write); reserve(content.size());
            replaceContents(path, content, context);
        }
        copied.push_back({{"generated", fileFacts(path, context)}});
    };
    for (const auto& configuration : configurations) {
        authorize(configuration, Domain::FileAccess::Read); const auto before = fileFacts(configuration, context);
        if (before.at("bytes").get<std::uint64_t>() > 1024U * 1024U)
            fail(Domain::ErrorCodes::LimitExceeded, "Installed embedded interpreter search paths exceed their bound.");
        std::ifstream input{configuration, std::ios::binary}; std::string line, content = pathText(overlay) + "\n";
        while (std::getline(input, line)) {
            const auto begin = line.find_first_not_of(" \t\r"), end = line.find_last_not_of(" \t\r");
            if (begin == std::string::npos) continue;
            line = line.substr(begin, end - begin + 1U);
            if (line.starts_with('#') || line == "import site") continue;
            if (line.starts_with("import ")) fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Installed embedded interpreter uses an unsupported search-path import.");
            content += pathText(Fs::absolute(configuration.parent_path() / Fs::path{wide(line)}).lexically_normal()) + "\n";
        }
        content += pathText(provider.parent_path() / L"Lib" / L"site-packages") + "\nimport site\n";
        if (fileFacts(configuration, context).at("sha256") != before.at("sha256"))
            fail(Domain::ErrorCodes::IntegrityFailure, "Installed interpreter search paths changed during build preparation.");
        writeConfiguration(directory / configuration.filename(), content); originals.push_back(before);
    }
    const auto site = directory / L"Lib" / L"site-packages"; createDirectory(site);
    // pip also uses a temporary sitecustomize module so build dependencies and
    // their .pth files reach child interpreters without changing the provider.
    writeConfiguration(site / L"sitecustomize.py", "import site\nsite.addsitedir(" + Json(pathText(overlay)).dump() + ")\n");
    const auto executable = directory / provider.filename();
    return {{"python", pathText(executable)}, {"embedded", true}, {"components", copied},
        {"provider_path_configuration", originals}, {"component_bytes", total}, {"copied_bytes", copiedBytes}};
}
Json directoryFacts(const std::filesystem::path& path,std::uint64_t maximum,const Domain::OperationContext& context) {
    regularParents(path);Json files=Json::object();std::uint64_t bytes{};std::size_t entries{};
    for(auto item=std::filesystem::recursive_directory_iterator{path};item!=std::filesystem::recursive_directory_iterator{};++item){
        check(context);regularParents(item->path());if(++entries>100000U)fail(Domain::ErrorCodes::LimitExceeded,"Installed dependency tree exceeds its entry bound.");
        const auto name=item->path().filename().native();
        if(name==L".git"||name==L"__pycache__"){if(item->is_directory())item.disable_recursion_pending();continue;}
        if(item->is_directory())continue;
        if(!item->is_regular_file())fail(Domain::ErrorCodes::PathOutsideAuthority,"Installed dependency tree contains an unsupported filesystem entry.");
        if(item->path().extension()==L".pyc"||(item->path().parent_path()==path&&name==L".forge-dependency.json"))continue;
        const auto size=item->file_size();if(size>maximum-bytes||files.size()>=8192U)fail(Domain::ErrorCodes::LimitExceeded,"Installed dependency tree exceeds its content bound.");bytes+=size;
        const auto facts=fileFacts(item->path(),context);files[pathText(std::filesystem::relative(item->path(),path))]={{"bytes",facts.at("bytes")},{"sha256",facts.at("sha256")}};
    }
    return files;
}
bool retainsDirectoryFiles(const Json& expected,const Json& observed) {
    if(!expected.is_object()||!observed.is_object())return false;
    for(const auto& [path,facts]:expected.items())if(!observed.contains(path)||observed.at(path)!=facts)return false;
    return true;
}
Json resolveFfmpegRelease(const Json& releases) {
    if(!releases.is_array()||releases.size()>128U)fail(Domain::ErrorCodes::MalformedMessage,"FFmpeg publisher release inventory exceeds its supported shape.");
    constexpr std::string_view prefix{"ffmpeg-N-"},suffix{"-win64-gpl.zip"};
    for(const auto& release:releases){const auto tag=release.value("tag_name",std::string{});
        if(!tag.starts_with("autobuild-")||tag.size()!=26U||release.value("prerelease",false)||release.value("draft",false))continue;
        bool dated=true;for(std::size_t index=10U;index<tag.size();++index){const bool separator=index==14U||index==17U||index==20U||index==23U;dated=dated&&(separator?tag[index]=='-':tag[index]>='0'&&tag[index]<='9');}if(!dated)continue;
        for(const auto& asset:release.value("assets",Json::array())){const auto name=asset.value("name",std::string{});
            if(!name.starts_with(prefix)||!name.ends_with(suffix)||name.size()<=prefix.size()+suffix.size())continue;
            const auto identity=name.substr(prefix.size(),name.size()-prefix.size()-suffix.size());const auto split=identity.find("-g");if(split==std::string::npos)continue;
            const auto revision=identity.substr(0,split),commit=identity.substr(split+2U);
            if(revision.empty()||revision.size()>20U||revision.find_first_not_of("0123456789")!=std::string::npos||commit.size()<7U||commit.size()>40U||commit.find_first_not_of("0123456789abcdef")!=std::string::npos)continue;
            const auto seal=asset.contains("digest")&&asset.at("digest").is_string()?asset.at("digest").get<std::string>():std::string{};
            if(!seal.starts_with("sha256:")||!Domain::Sha256Digest::parse(seal.substr(7U)))continue;
            const auto url=text(asset,"browser_download_url",16384U);if(url!="https://github.com/BtbN/FFmpeg-Builds/releases/download/"+tag+"/"+name)continue;
            if(!asset.contains("size")||!asset.at("size").is_number_unsigned()||asset.at("size").get<std::uint64_t>()==0ULL)continue;
            return {{"kind","component"},{"url",url},{"sha256",seal.substr(7U)},{"target","ffmpeg-"+tag},{"archive",true},{"bytes",asset.at("size")},
                {"provenance",{{"kind","identified_publisher_release_asset"},{"repository","https://github.com/BtbN/FFmpeg-Builds"},{"release_id",release.at("id")},{"asset_id",asset.at("id")},{"asset_name",name},{"version",tag}}}};
        }
    }
    fail(Domain::ErrorCodes::HostCapabilityUnavailable,"No versioned FFmpeg Windows release has a publisher SHA-256 asset seal.");
}
std::uint64_t archiveRequiredBytes(std::string_view verboseInventory,std::uint64_t allocationUnit,std::uint64_t maximum) {
    if(!allocationUnit||verboseInventory.size()>=1024U*1024U)fail(Domain::ErrorCodes::LimitExceeded,"Dependency archive inventory exceeds its supported bound.");
    std::istringstream lines{std::string{verboseInventory}};std::string line;std::uint64_t total{};std::size_t entries{};
    while(std::getline(lines,line)){
        if(line.empty()||line=="\r")continue;
        if(line.front()!='-'&&line.front()!='d')fail(Domain::ErrorCodes::PathOutsideAuthority,"Dependency archive contains a link or unsupported filesystem entry.");
        std::istringstream fields{line};std::string mode,links,owner,group,size;
        if(!(fields>>mode>>links>>owner>>group>>size))fail(Domain::ErrorCodes::MalformedMessage,"Dependency archive has an incomplete verbose inventory entry.");
        std::uint64_t bytes{};const auto parsed=std::from_chars(size.data(),size.data()+size.size(),bytes);
        if(parsed.ec!=std::errc{}||parsed.ptr!=size.data()+size.size())fail(Domain::ErrorCodes::MalformedMessage,"Dependency archive has an invalid declared file size.");
        const auto remainder=bytes%allocationUnit;const auto padding=remainder?allocationUnit-remainder:0ULL;
        if(bytes>UINT64_MAX-padding)fail(Domain::ErrorCodes::LimitExceeded,"Dependency archive file size overflows its disk bound.");
        bytes+=padding;if(mode.front()=='d')bytes=(std::max)(bytes,allocationUnit);
        if(total>maximum||bytes>maximum-total||++entries>8192U)fail(Domain::ErrorCodes::LimitExceeded,"Expanded dependency exceeds its preparation disk bound.");
        total+=bytes;
    }
    return total;
}
std::wstring quote(std::wstring_view value) {
    std::wstring result{L"\""}; std::size_t slashes{};
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'\"' ? slashes * 2U + 1U : slashes, L'\\'); slashes = 0; result += ch;
    }
    result.append(slashes * 2U, L'\\'); result += L'\"'; return result;
}
Process startProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
    const std::filesystem::path& directory, const std::filesystem::path& log, const bool transientLifetime) {
    regularParents(executable); regularParents(directory); regularParents(log);
    std::wstring command = quote(executable.native());
    for (const auto& argument : arguments) { command += L' '; command += quote(wide(argument)); }
    if (command.size() >= 32767U) fail(Domain::ErrorCodes::PayloadTooLarge, "ComfyUI process arguments exceed the Windows limit.");
    std::filesystem::create_directories(log.parent_path());
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle output{CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!output || !input) fail(Domain::ErrorCodes::InternalFailure, "Cannot create external ComfyUI process streams.");
    SIZE_T bytes{}; InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<std::byte> storage(bytes); auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) fail(Domain::ErrorCodes::InternalFailure, "Cannot initialize external process attributes.");
    struct Attributes { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Attributes(){DeleteProcThreadAttributeList(p);} } cleanup{attributes};
    HANDLE inherited[]{input.get(), output.get()};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
        fail(Domain::ErrorCodes::InternalFailure, "Cannot restrict external process handle inheritance.");
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = input.get(); startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output.get();
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(), &startup.StartupInfo, &info))
        fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Cannot start external ComfyUI component (Windows " + std::to_string(GetLastError()) + ").");
    Process result; result.process.reset(info.hProcess); Handle thread{info.hThread}; result.pid = info.dwProcessId;
    result.job.reset(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!result.job || (transientLifetime && !SetInformationJobObject(result.job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) ||
        !AssignProcessToJobObject(result.job.get(), result.process.get())) {
        TerminateProcess(result.process.get(), 1); fail(Domain::ErrorCodes::HostCapabilityUnavailable, "Cannot retain external ComfyUI process ownership.");
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(result.process.get(), &created, &exited, &kernel, &user);
    result.creationTime = (static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) { TerminateJobObject(result.job.get(), 1); fail(Domain::ErrorCodes::InternalFailure, "Cannot resume external ComfyUI component."); }
    return result;
}
Json runProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
    const std::filesystem::path& directory, const std::filesystem::path& log, const Domain::OperationContext& context, const std::function<void()>& observe) {
    check(context);const auto prior=std::filesystem::is_regular_file(log)?std::filesystem::file_size(log):0ULL;auto process = startProcess(executable, arguments, directory, log, true);
    try {
        while (WaitForSingleObject(process.process.get(), 100U) == WAIT_TIMEOUT) { check(context); if (observe) observe(); }
    } catch (...) { TerminateJobObject(process.job.get(), 1); WaitForSingleObject(process.process.get(), 5000U); throw; }
    DWORD code{}; if (!GetExitCodeProcess(process.process.get(), &code)) fail(Domain::ErrorCodes::InternalFailure, "Cannot observe external component exit.");
    std::ifstream stream{log, std::ios::binary};stream.seekg(static_cast<std::streamoff>(prior));std::string output; std::array<char,8192> buffer{};
    while (stream && output.size() < 1024U*1024U) { stream.read(buffer.data(), (std::min)(buffer.size(),1024U*1024U-output.size())); output.append(buffer.data(),static_cast<std::size_t>(stream.gcount())); }
    return {{"exit_code",code},{"log",pathText(log)},{"output",Domain::isValidUtf8(output)?output:std::string{"Non-UTF8 output retained in the log."}}};
}
namespace {
class Internet final {
public:
    explicit Internet(HINTERNET value=nullptr):value_{value}{} ~Internet(){if(value_)WinHttpCloseHandle(value_);}
    HINTERNET get() const {return value_;} HINTERNET release(){return std::exchange(value_,nullptr);}
private: HINTERNET value_{};
};
class Async final {
public:
    explicit Async(HINTERNET value):handle_{value} {
        DWORD_PTR context=reinterpret_cast<DWORD_PTR>(this);
        if (!WinHttpSetOption(handle_,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context)) ||
            WinHttpSetStatusCallback(handle_,callback,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0)==WINHTTP_INVALID_STATUS_CALLBACK)
            fail(Domain::ErrorCodes::InternalFailure,"Cannot initialize native ComfyUI HTTP.");
    }
    ~Async(){close();std::unique_lock lock{mutex_}; changed_.wait(lock,[&]{return closed_;});}
    void close(){std::lock_guard lock{api_};auto h=std::exchange(handle_,nullptr);if(h)WinHttpCloseHandle(h);}
    template<class F> DWORD invoke(F function){std::lock_guard lock{api_};return handle_?(function(handle_)?ERROR_SUCCESS:GetLastError()):ERROR_WINHTTP_OPERATION_CANCELLED;}
    template<class F> DWORD submit(DWORD expected,F function){
        {std::lock_guard apiLock{api_};if(!handle_)return ERROR_WINHTTP_OPERATION_CANCELLED;
            {std::lock_guard lock{mutex_};expected_=expected;done_=false;error_=0;}
            if(!function(handle_)) return GetLastError();}
        std::unique_lock lock{mutex_}; changed_.wait(lock,[&]{return done_||closed_;});return done_?error_:ERROR_WINHTTP_OPERATION_CANCELLED;
    }
    DWORD count()const{return count_;}
private:
    static void CALLBACK callback(HINTERNET,DWORD_PTR context,DWORD status,void* value,DWORD length){
        if(!context)return;auto& self=*reinterpret_cast<Async*>(context);std::lock_guard lock{self.mutex_};
        if(status==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)self.closed_=true;
        else if(status==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR){self.done_=true;self.error_=value&&length>=sizeof(WINHTTP_ASYNC_RESULT)?static_cast<WINHTTP_ASYNC_RESULT*>(value)->dwError:ERROR_WINHTTP_INTERNAL_ERROR;}
        else if(status==self.expected_){self.done_=true;self.count_=status==WINHTTP_CALLBACK_STATUS_READ_COMPLETE?length:0;
            if(status==WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE){if(value&&length==sizeof(DWORD))self.count_=*static_cast<DWORD*>(value);else self.error_=ERROR_WINHTTP_INTERNAL_ERROR;}}
        self.changed_.notify_all();
    }
    HINTERNET handle_{};std::mutex api_,mutex_;std::condition_variable changed_;DWORD expected_{},error_{},count_{};bool done_{},closed_{};
};
}
namespace {
constexpr std::uint64_t ReceiveWindowBytes=16ULL*1024ULL*1024ULL;
std::uint64_t receiveNumber(const Json& value,std::string_view key,std::uint64_t fallback=0ULL){
    const auto item=value.find(std::string{key});if(item==value.end())return fallback;
    if(!item->is_number_integer()||(!item->is_number_unsigned()&&item->get<std::int64_t>()<0))fail(Domain::ErrorCodes::IntegrityFailure,"Preparation receive ledger has an invalid byte count.");return item->get<std::uint64_t>();
}
std::uint64_t receiveSum(std::uint64_t left,std::uint64_t right){if(right>UINT64_MAX-left)fail(Domain::ErrorCodes::IntegrityFailure,"Preparation receive ledger overflow.");return left+right;}
std::uint64_t receivePending(const Json& manifest){
    if(!manifest.contains("receive_admission"))return 0ULL;const auto& admission=manifest.at("receive_admission");if(!admission.is_object())fail(Domain::ErrorCodes::IntegrityFailure,"Preparation receive admission is invalid.");
    const auto remaining=receiveNumber(admission,"remaining_bytes"),reserved=receiveNumber(admission,"reserved_bytes"),transfer=receiveNumber(admission,"transfer_index",UINT64_MAX);
    if(!manifest.at("downloads").is_array()||transfer>=manifest.at("downloads").size()||reserved>ReceiveWindowBytes||remaining>reserved)fail(Domain::ErrorCodes::IntegrityFailure,"Preparation receive admission is invalid.");return remaining;
}
}
std::uint64_t preparationDownloadCharge(const Json& manifest){
    return receiveSum(receiveSum(receiveNumber(manifest,"downloaded_bytes"),receiveNumber(manifest,"uncertain_download_bytes")),receivePending(manifest));
}
void finishPreparationReceive(Json& manifest,bool certain){
    const auto pending=receivePending(manifest);if(!certain&&pending){
        manifest["uncertain_download_bytes"]=receiveSum(receiveNumber(manifest,"uncertain_download_bytes"),pending);
        if(!manifest.contains("receive_uncertainties"))manifest["receive_uncertainties"]=Json::array();
        auto evidence=manifest.at("receive_admission");evidence["reason"]="Native receive completion was not durably confirmed; remaining admission is conservatively charged.";manifest["receive_uncertainties"].push_back(std::move(evidence));
    }manifest.erase("receive_admission");
}
void reconcilePreparationReceives(Json& manifest,std::string_view previousState,std::span<const std::uint64_t> stagingBytes){
    if(!manifest.at("downloads").is_array()||stagingBytes.size()!=manifest.at("downloads").size())fail(Domain::ErrorCodes::IntegrityFailure,"Preparation staging ledger shape changed.");
    const bool legacy=!manifest.contains("receive_accounting_version");if(!legacy&&receiveNumber(manifest,"receive_accounting_version")!=1ULL)fail(Domain::ErrorCodes::IntegrityFailure,"Unsupported preparation receive ledger version.");
    std::uint64_t legacyGrowth{};const auto pending=receivePending(manifest);
    const auto pendingTransfer=manifest.contains("receive_admission")?receiveNumber(manifest.at("receive_admission"),"transfer_index",UINT64_MAX):UINT64_MAX;
    for(std::size_t index=0U;index<stagingBytes.size();++index){auto& transfer=manifest["downloads"][index];const auto recorded=receiveNumber(transfer,"received_bytes");if(stagingBytes[index]<=recorded)continue;
        const auto growth=stagingBytes[index]-recorded;manifest["downloaded_bytes"]=receiveSum(receiveNumber(manifest,"downloaded_bytes"),growth);transfer["received_bytes"]=stagingBytes[index];
        if(index==pendingTransfer){if(growth>pending)fail(Domain::ErrorCodes::IntegrityFailure,"Retained download grew beyond its durable receive admission.");manifest["receive_admission"]["remaining_bytes"]=pending-growth;}
        if(index+1U==stagingBytes.size())legacyGrowth=growth;
    }
    finishPreparationReceive(manifest,false);
    if(legacy&&previousState=="preparing"&&!manifest.at("downloads").empty()){
        const auto& last=manifest.at("downloads").back();if(last.value("state",std::string{"downloading"})=="downloading"){
            const auto bound=last.value("type",std::string{})=="metadata"?65536ULL:ReceiveWindowBytes+65536ULL;
            const auto uncertainty=bound-(std::min)(legacyGrowth,bound);
            manifest["uncertain_download_bytes"]=receiveSum(receiveNumber(manifest,"uncertain_download_bytes"),uncertainty);
            manifest["legacy_receive_uncertainty"]={{"bytes",uncertainty},{"reason","An interrupted legacy preparing receipt has no durable receive admission; its last unsaved receive window is conservatively charged."}};
        }
    }
    manifest["receive_accounting_version"]=1U;if(!manifest.contains("uncertain_download_bytes"))manifest["uncertain_download_bytes"]=0ULL;
    manifest["budget_charged_bytes"]=preparationDownloadCharge(manifest);
}
std::uint64_t admitPreparationReceive(Json& manifest,std::size_t transfer,std::uint64_t proposed){
    if(!proposed||proposed>65536ULL||transfer>=manifest.at("downloads").size())fail(Domain::ErrorCodes::IntegrityFailure,"Invalid native preparation receive request.");
    auto pending=receivePending(manifest);if(pending&&receiveNumber(manifest.at("receive_admission"),"transfer_index",UINT64_MAX)!=transfer)fail(Domain::ErrorCodes::IntegrityFailure,"Preparation receive admission belongs to another transfer.");
    if(!pending){const auto charged=preparationDownloadCharge(manifest),budget=receiveNumber(manifest,"download_budget_bytes");
        if(charged>=budget)fail(Domain::ErrorCodes::LimitExceeded,"Preparation exhausted its cumulative download budget across metadata, phases, retries and resumes.");
        pending=(std::min)(ReceiveWindowBytes,budget-charged);manifest["receive_admission"]={{"transfer_index",transfer},{"reserved_bytes",pending},{"remaining_bytes",pending}};
    }
    return (std::min)(proposed,pending);
}
void recordPreparationReceive(Json& manifest,std::size_t transfer,std::uint64_t bytes){
    const auto pending=receivePending(manifest);if(!manifest.contains("receive_admission")||receiveNumber(manifest.at("receive_admission"),"transfer_index",UINT64_MAX)!=transfer||bytes>65536ULL||bytes>pending)fail(Domain::ErrorCodes::IntegrityFailure,"Native receive exceeded its durable preparation admission.");
    manifest["downloaded_bytes"]=receiveSum(receiveNumber(manifest,"downloaded_bytes"),bytes);auto& item=manifest["downloads"][transfer];item["received_bytes"]=receiveSum(receiveNumber(item,"received_bytes"),bytes);
    manifest["receive_admission"]["remaining_bytes"]=pending-bytes;
}
HttpResult http(std::string_view url,std::string_view method,std::string_view contentType,std::string_view body,
    std::size_t maximum,const Domain::OperationContext& context,HANDLE output,std::uint64_t streamMaximum,
    const std::function<void(std::uint64_t)>& received,HANDLE upload,std::uint64_t uploadBytes,std::string_view prefix,std::string_view suffix,
    const std::function<std::uint64_t(std::uint64_t)>& beforeRead) {
    check(context);const auto full=wide(url); URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);
    parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=parts.dwUserNameLength=parts.dwPasswordLength=static_cast<DWORD>(-1);
    if(!WinHttpCrackUrl(full.c_str(),static_cast<DWORD>(full.size()),0,&parts)||
        (parts.nScheme!=INTERNET_SCHEME_HTTP&&parts.nScheme!=INTERNET_SCHEME_HTTPS)||parts.dwUserNameLength||parts.dwPasswordLength)
        fail(Domain::ErrorCodes::InvalidRequest,"Invalid ComfyUI component HTTP URL.");
    std::wstring host{parts.lpszHostName,parts.dwHostNameLength},path{parts.lpszUrlPath,parts.dwUrlPathLength};
    if(parts.dwExtraInfoLength)path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);if(path.find(L'#')!=std::wstring::npos)fail(Domain::ErrorCodes::InvalidRequest,"HTTP URL fragment is invalid.");
    Internet session{WinHttpOpen(L"Forge-Conductor-ComfyUI",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC)};
    if(!session.get())fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot open native HTTP session.");
    Internet connection{WinHttpConnect(session.get(),host.c_str(),parts.nPort,0)};
    Internet raw{WinHttpOpenRequest(connection.get(),wide(method).c_str(),path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0)};
    if(!raw.get())fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot open native HTTP request.");
    auto request=std::make_shared<Async>(raw.get());raw.release();
    std::stop_callback cancel{context.cancellation,[request]{request->close();}};
    std::jthread guard{[request,deadline=context.deadline](std::stop_token stop){std::mutex mutex;std::condition_variable_any changed;std::unique_lock lock{mutex};changed.wait_until(lock,stop,deadline,[]{return false;});if(!stop.stop_requested())request->close();}};
    const auto operation=[&](DWORD code){check(context);if(code)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Native ComfyUI HTTP failed (WinHTTP "+std::to_string(code)+").");};
    DWORD disable=WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_AUTHENTICATION,redirect=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    operation(request->invoke([&](HINTERNET h){return WinHttpSetOption(h,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable));}));
    operation(request->invoke([&](HINTERNET h){return WinHttpSetOption(h,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect));}));
    operation(request->invoke([&](HINTERNET h){return WinHttpSetTimeouts(h,5000,5000,30000,30000);}));
    const auto total=upload?uploadBytes+prefix.size()+suffix.size():body.size();
    if(total>MAXDWORD)fail(Domain::ErrorCodes::PayloadTooLarge,"Upload exceeds the native HTTP 4 GiB body bound.");
    std::wstring headers=contentType.empty()?L"":wide("Content-Type: "+std::string{contentType}+"\r\n");
    operation(request->submit(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE,[&](HINTERNET h){return WinHttpSendRequest(h,headers.c_str(),static_cast<DWORD>(-1L),
        upload?WINHTTP_NO_REQUEST_DATA:(body.empty()?WINHTTP_NO_REQUEST_DATA:const_cast<char*>(body.data())),upload?0:static_cast<DWORD>(body.size()),static_cast<DWORD>(total),reinterpret_cast<DWORD_PTR>(request.get()));}));
    const auto write=[&](std::string_view data){if(!data.empty())operation(request->submit(WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE,[&](HINTERNET h){return WinHttpWriteData(h,data.data(),static_cast<DWORD>(data.size()),nullptr);}));};
    if(upload){write(prefix);LARGE_INTEGER zero{};SetFilePointerEx(upload,zero,nullptr,FILE_BEGIN);std::array<char,65536> buffer{};std::uint64_t sent{};
        while(sent<uploadBytes){check(context);DWORD count{};if(!ReadFile(upload,buffer.data(),static_cast<DWORD>((std::min)(uploadBytes-sent,static_cast<std::uint64_t>(buffer.size()))),&count,nullptr)||!count)fail(Domain::ErrorCodes::Conflict,"Upload source changed or cannot be read.");write({buffer.data(),count});sent+=count;}write(suffix);}
    operation(request->submit(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,[](HINTERNET h){return WinHttpReceiveResponse(h,nullptr);}));
    HttpResult result;DWORD size=sizeof(result.status);
    operation(request->invoke([&](HINTERNET h){return WinHttpQueryHeaders(h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&result.status,&size,WINHTTP_NO_HEADER_INDEX);}));
    std::array<wchar_t,8192> type{};size=static_cast<DWORD>(sizeof(type));
    const auto typeError=request->invoke([&](HINTERNET h){return WinHttpQueryHeaders(h,WINHTTP_QUERY_CONTENT_TYPE,WINHTTP_HEADER_NAME_BY_INDEX,type.data(),&size,WINHTTP_NO_HEADER_INDEX);});
    if(!typeError){result.contentType=utf8(std::wstring_view{type.data(),size/sizeof(wchar_t)});while(!result.contentType.empty()&&!result.contentType.back())result.contentType.pop_back();}
    size=static_cast<DWORD>(sizeof(type));const auto locationError=request->invoke([&](HINTERNET h){return WinHttpQueryHeaders(h,WINHTTP_QUERY_LOCATION,WINHTTP_HEADER_NAME_BY_INDEX,type.data(),&size,WINHTTP_NO_HEADER_INDEX);});
    if(!locationError){result.location=utf8(std::wstring_view{type.data(),size/sizeof(wchar_t)});while(!result.location.empty()&&!result.location.back())result.location.pop_back();}
    std::array<wchar_t,64> length{};size=static_cast<DWORD>(sizeof(length));std::optional<std::uint64_t> expectedLength;
    const auto lengthError=request->invoke([&](HINTERNET h){return WinHttpQueryHeaders(h,WINHTTP_QUERY_CONTENT_LENGTH,WINHTTP_HEADER_NAME_BY_INDEX,length.data(),&size,WINHTTP_NO_HEADER_INDEX);});
    if(!lengthError){std::wstring_view value{length.data(),size/sizeof(wchar_t)};while(!value.empty()&&!value.back())value.remove_suffix(1);
        std::uint64_t number{};if(value.empty())fail(Domain::ErrorCodes::MalformedMessage,"HTTP Content-Length is empty.");for(wchar_t c:value){if(c<L'0'||c>L'9'||number>(UINT64_MAX-static_cast<unsigned>(c-L'0'))/10U)fail(Domain::ErrorCodes::MalformedMessage,"HTTP Content-Length is invalid.");number=number*10U+static_cast<unsigned>(c-L'0');}expectedLength=number;}
    std::array<char,65536> buffer{};
    for(;;){auto requested=static_cast<std::uint64_t>(buffer.size());if(beforeRead){
            operation(request->submit(WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE,[](HINTERNET h){return WinHttpQueryDataAvailable(h,nullptr);}));const auto available=request->count();if(!available)break;
            requested=beforeRead(requested);if(!requested||requested>buffer.size())fail(Domain::ErrorCodes::IntegrityFailure,"Native receive admission has an invalid bound.");check(context);
        }
        operation(request->submit(WINHTTP_CALLBACK_STATUS_READ_COMPLETE,[&](HINTERNET h){return WinHttpReadData(h,buffer.data(),static_cast<DWORD>(requested),nullptr);}));const auto count=request->count();if(!count)break;
        // Count every byte delivered by WinHTTP, including error/redirect bodies
        // and the chunk that exceeds a payload limit, before publication.
        if(received)received(count);
        const auto limit=output?streamMaximum:maximum;if(count>limit-result.bytes)fail(Domain::ErrorCodes::PayloadTooLarge,"HTTP response exceeds the ComfyUI transfer budget.");
        if(output&&result.status>=200&&result.status<300){DWORD written{};if(!WriteFile(output,buffer.data(),count,&written,nullptr)||written!=count)fail(Domain::ErrorCodes::InternalFailure,"Cannot write streamed ComfyUI artifact.");}
        else {if(count>maximum-result.body.size())fail(Domain::ErrorCodes::PayloadTooLarge,"HTTP error/JSON response exceeds its bound.");result.body.append(buffer.data(),count);}
        result.bytes+=count;
    }
    check(context);if(expectedLength&&result.bytes!=*expectedLength)fail(Domain::ErrorCodes::IntegrityFailure,"HTTP body ended before its declared Content-Length.");
    if(output&&result.status>=200&&result.status<300&&!FlushFileBuffers(output))fail(Domain::ErrorCodes::InternalFailure,"Cannot flush streamed ComfyUI artifact.");return result;
}
HttpResult download(std::string_view url, std::size_t maximum, const Domain::OperationContext& context,
    HANDLE output, std::uint64_t streamMaximum, const std::function<void(std::uint64_t)>& received,
    const std::function<std::uint64_t(std::uint64_t)>& beforeRead) {
    std::string current{url};std::set<std::string> visited;
    for(unsigned redirect=0U;redirect<=5U;++redirect){
        if((!current.starts_with("http://")&&!current.starts_with("https://"))||current.find_first_of("\r\n")!=std::string::npos)
            fail(Domain::ErrorCodes::InvalidRequest,"Dependency downloads require an HTTP or HTTPS URL.");
        if(!visited.insert(current).second)fail(Domain::ErrorCodes::Conflict,"Dependency download redirect loop.");
        auto result=http(current,"GET","","",maximum,context,output,streamMaximum,received,nullptr,0ULL,{},{},beforeRead);
        if(result.status!=301U&&result.status!=302U&&result.status!=303U&&result.status!=307U&&result.status!=308U)return result;
        if(result.location.empty())fail(Domain::ErrorCodes::MalformedMessage,"Dependency redirect has no Location.");
        if(result.location.starts_with("http://")||result.location.starts_with("https://"))current=result.location;
        else if(result.location.starts_with("//"))current=current.substr(0U,current.find(':')+1U)+result.location;
        else if(result.location.front()=='/'){
            const auto slash=current.find('/',current.starts_with("https://")?8U:7U);current=current.substr(0,slash)+result.location;
        }else fail(Domain::ErrorCodes::InvalidRequest,"Dependency redirect must use an HTTP or HTTPS URL or origin path.");
    }
    fail(Domain::ErrorCodes::LimitExceeded,"Dependency download exceeded its redirect limit.");
}
std::string encode(std::string_view value){constexpr char digits[]="0123456789ABCDEF";std::string result;for(unsigned char c:value){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')result+=static_cast<char>(c);else{result+='%';result+=digits[c>>4];result+=digits[c&15];}}return result;}
struct Cdp::State final {
    struct Message final {std::string bytes;bool binary{};};
    HINTERNET session{},connection{},socket{};
    std::mutex api,mutex;std::condition_variable changed;
    std::array<char,65536> buffer{};std::string fragments,sendBuffer;
    DWORD received{},sent{},receiveError{},sendError{};
    WINHTTP_WEB_SOCKET_BUFFER_TYPE receivedType{};
    bool callbacks{},closed{},receivePending{},receiveDone{},sendPending{},sendDone{},binary{},fragmented{};
    ~State(){close();if(callbacks){std::unique_lock lock{mutex};changed.wait(lock,[&]{return closed;});}if(connection)WinHttpCloseHandle(connection);if(session)WinHttpCloseHandle(session);}
    void close(){std::lock_guard lock{api};auto handle=std::exchange(socket,nullptr);if(handle)WinHttpCloseHandle(handle);}
    static void CALLBACK callback(HINTERNET,DWORD_PTR context,DWORD status,void* value,DWORD length){
        if(!context)return;auto& self=*reinterpret_cast<State*>(context);std::lock_guard lock{self.mutex};
        if(status==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)self.closed=true;
        else if(status==WINHTTP_CALLBACK_STATUS_READ_COMPLETE){self.receiveDone=true;
            if(value&&length==sizeof(WINHTTP_WEB_SOCKET_STATUS)){const auto& event=*static_cast<WINHTTP_WEB_SOCKET_STATUS*>(value);self.received=event.dwBytesTransferred;self.receivedType=event.eBufferType;}
            else self.receiveError=ERROR_WINHTTP_INTERNAL_ERROR;
        }else if(status==WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE){self.sendDone=true;
            if(value&&length==sizeof(WINHTTP_WEB_SOCKET_STATUS))self.sent=static_cast<WINHTTP_WEB_SOCKET_STATUS*>(value)->dwBytesTransferred;
            else self.sendError=ERROR_WINHTTP_INTERNAL_ERROR;
        }else if(status==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR){
            if(value&&length>=sizeof(WINHTTP_WEB_SOCKET_ASYNC_RESULT)){const auto& event=*static_cast<WINHTTP_WEB_SOCKET_ASYNC_RESULT*>(value);
                if(event.Operation==WINHTTP_WEB_SOCKET_RECEIVE_OPERATION){self.receiveDone=true;self.receiveError=event.AsyncResult.dwError;}
                else if(event.Operation==WINHTTP_WEB_SOCKET_SEND_OPERATION){self.sendDone=true;self.sendError=event.AsyncResult.dwError;}
                else {self.receiveDone=self.sendDone=true;self.receiveError=self.sendError=event.AsyncResult.dwError;}
            }else {self.receiveDone=self.sendDone=true;self.receiveError=self.sendError=ERROR_WINHTTP_INTERNAL_ERROR;}
        }
        self.changed.notify_all();
    }
    void send(std::string command,const Domain::OperationContext& context){
        check(context);if(command.size()>MAXDWORD)fail(Domain::ErrorCodes::PayloadTooLarge,"Private CDP command exceeds the native send bound.");
        {std::lock_guard apiLock{api};if(!socket)fail(Domain::ErrorCodes::TransportClosed,"Private CDP socket closed.");
            {std::lock_guard lock{mutex};if(sendPending)fail(Domain::ErrorCodes::Conflict,"Private CDP send is already pending.");sendBuffer=std::move(command);sendPending=true;sendDone=false;sendError=sent=0U;}
            const auto error=WinHttpWebSocketSend(socket,WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,sendBuffer.data(),static_cast<DWORD>(sendBuffer.size()));
            if(error&&error!=ERROR_IO_PENDING)fail(Domain::ErrorCodes::TransportClosed,"Private CDP send failed (WinHTTP "+std::to_string(error)+").");
        }
        for(;;){check(context);std::unique_lock lock{mutex};if(sendDone||closed){if(closed||sendError||sent!=sendBuffer.size())fail(Domain::ErrorCodes::TransportClosed,"Private CDP send did not complete.");sendPending=false;sendBuffer.clear();return;}
            changed.wait_for(lock,std::chrono::milliseconds{50},[&]{return sendDone||closed;});}
    }
    std::optional<Message> read(std::size_t maximum,std::chrono::steady_clock::time_point until,const Domain::OperationContext& context){
        for(;;){check(context);
            {std::lock_guard apiLock{api};if(!socket)fail(Domain::ErrorCodes::TransportClosed,"Private ComfyUI socket closed.");bool submit{};
                {std::lock_guard lock{mutex};if(!receivePending){receivePending=true;receiveDone=false;receiveError=received=0U;submit=true;}}
                if(submit){const auto error=WinHttpWebSocketReceive(socket,buffer.data(),static_cast<DWORD>(buffer.size()),nullptr,nullptr);
                    if(error&&error!=ERROR_IO_PENDING)fail(Domain::ErrorCodes::TransportClosed,"Private ComfyUI receive failed (WinHTTP "+std::to_string(error)+").");}
            }
            std::unique_lock lock{mutex};if(!receiveDone&&!closed){const auto now=std::chrono::steady_clock::now();if(now>=until)return std::nullopt;
                changed.wait_until(lock,(std::min)(until,now+std::chrono::milliseconds{50}),[&]{return receiveDone||closed;});continue;}
            if(closed||receiveError||receivedType==WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)fail(Domain::ErrorCodes::TransportClosed,"Private ComfyUI socket disconnected.");
            receivePending=false;
            if(received>buffer.size()||fragments.size()>maximum||received>maximum-fragments.size())fail(Domain::ErrorCodes::PayloadTooLarge,"Private ComfyUI websocket message exceeds its delivery bound.");
            const auto binaryPart=receivedType==WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE||receivedType==WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE;
            const auto textPart=receivedType==WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE||receivedType==WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE;
            if((!binaryPart&&!textPart)||(fragmented&&binary!=binaryPart))fail(Domain::ErrorCodes::MalformedMessage,"Private ComfyUI socket changed its fragment type.");
            binary=binaryPart;fragments.append(buffer.data(),received);fragmented=true;
            if(receivedType==WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE||receivedType==WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE){Message result{std::move(fragments),binary};fragments.clear();fragmented=false;return result;}
        }
    }
};
Cdp::Cdp(std::string_view websocket,const Domain::OperationContext& context):state_{std::make_unique<State>()}{
    check(context);auto url=wide(websocket);if(!url.starts_with(L"ws://"))fail(Domain::ErrorCodes::InvalidRequest,"Private CDP requires a loopback websocket URL.");url.replace(0,2,L"http");
    URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=parts.dwUserNameLength=parts.dwPasswordLength=static_cast<DWORD>(-1);
    if(!WinHttpCrackUrl(url.c_str(),static_cast<DWORD>(url.size()),0,&parts)||parts.dwUserNameLength||parts.dwPasswordLength)fail(Domain::ErrorCodes::InvalidRequest,"Invalid private CDP websocket URL.");
    const std::wstring host{parts.lpszHostName,parts.dwHostNameLength};if(host!=L"127.0.0.1"&&host!=L"::1")fail(Domain::ErrorCodes::Unauthorized,"CDP requires the private loopback browser.");
    Internet session{WinHttpOpen(L"Forge-ComfyUI-CDP",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC)};
    if(!session.get())fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot open private ComfyUI socket session.");
    Internet connection{WinHttpConnect(session.get(),host.c_str(),parts.nPort,0)};
    std::wstring path{parts.lpszUrlPath,parts.dwUrlPathLength};if(parts.dwExtraInfoLength)path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
    Internet raw{WinHttpOpenRequest(connection.get(),L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0)};
    if(!raw.get())fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot create private ComfyUI socket request.");
    auto request=std::make_shared<Async>(raw.get());raw.release();
    std::stop_callback cancel{context.cancellation,[request]{request->close();}};
    std::jthread guard{[request,deadline=context.deadline](std::stop_token stop){std::mutex mutex;std::condition_variable_any changed;std::unique_lock lock{mutex};changed.wait_until(lock,stop,deadline,[]{return false;});if(!stop.stop_requested())request->close();}};
    const auto operation=[&](DWORD error){check(context);if(error)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot connect to private ComfyUI socket (WinHTTP "+std::to_string(error)+").");};
    operation(request->invoke([](HINTERNET handle){return WinHttpSetOption(handle,WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,nullptr,0)&&WinHttpSetTimeouts(handle,5000,5000,5000,1000);}));
    operation(request->submit(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE,[&](HINTERNET handle){return WinHttpSendRequest(handle,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,reinterpret_cast<DWORD_PTR>(request.get()));}));
    operation(request->submit(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,[](HINTERNET handle){return WinHttpReceiveResponse(handle,nullptr);}));
    DWORD status{},size=sizeof(status);operation(request->invoke([&](HINTERNET handle){return WinHttpQueryHeaders(handle,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX);}));
    if(status!=101U)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Private ComfyUI socket upgrade was rejected.");
    operation(request->invoke([&](HINTERNET handle){state_->socket=WinHttpWebSocketCompleteUpgrade(handle,0);return state_->socket!=nullptr;}));
    if(WinHttpSetStatusCallback(state_->socket,State::callback,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0)==WINHTTP_INVALID_STATUS_CALLBACK)
        fail(Domain::ErrorCodes::InternalFailure,"Cannot observe private ComfyUI socket completion.");
    DWORD_PTR binding=reinterpret_cast<DWORD_PTR>(state_.get());if(!WinHttpSetOption(state_->socket,WINHTTP_OPTION_CONTEXT_VALUE,&binding,sizeof(binding)))fail(Domain::ErrorCodes::InternalFailure,"Cannot bind private ComfyUI socket completion.");
    state_->callbacks=true;state_->session=session.release();state_->connection=connection.release();check(context);
}
Cdp::~Cdp()=default;
Json Cdp::call(std::string_view method,const Json& parameters,const Domain::OperationContext& context){
    try{const auto id=++id_;state_->send(Json{{"id",id},{"method",method},{"params",parameters}}.dump(),context);
        for(;;){const auto message=state_->read(16U*1024U*1024U,context.deadline,context);if(!message)continue;
            if(message->binary)fail(Domain::ErrorCodes::MalformedMessage,"Private CDP response must contain JSON text.");
            auto response=parse(message->bytes);if(response.value("id",0U)!=id)continue;
            if(response.contains("error"))fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Private browser operation failed: "+response.at("error").dump());return response.at("result");}
    }catch(...){state_->close();throw;}
}
Json Cdp::receive(const Domain::OperationContext& context){
    try{const auto message=state_->read(1024U*1024U,std::chrono::steady_clock::now()+std::chrono::milliseconds{100},context);
        if(!message)return nullptr;return message->binary?Json{{"type","binary_preview"},{"bytes",message->bytes.size()}}:parse(message->bytes,1024U*1024U);
    }catch(...){state_->close();throw;}
}
} // namespace ForgeConductor::NativeTools::Windows::ComfyDetail
