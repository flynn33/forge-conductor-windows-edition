#include <winsock2.h>
#include <ws2tcpip.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <bcrypt.h>
#include "Infrastructure/TestSupport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/NativeTools/Windows/WindowsComfyUiBackend.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include "NativeTools/Windows/ImageProviderCodec.h"
#include "NativeTools/Windows/ComfyUiNativeSupport.h"
#include "NativeTools/Windows/ComfyUiPackageContracts.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <span>
#include <thread>
#include <vector>

namespace ForgeConductor::Tests {
namespace {
namespace Fs = std::filesystem;
using Json = nlohmann::json;
Domain::PathText pathText(const Fs::path& path) {
    return take(Domain::PathText::create(take(Infrastructure::Windows::Detail::strictUtf16ToUtf8(path.native()))));
}
class Fixture final {
    struct EndpointReservation final {
        SOCKET handle{INVALID_SOCKET};bool initialized{};
        ~EndpointReservation(){if(handle!=INVALID_SOCKET)closesocket(handle);if(initialized)WSACleanup();}
        std::string acquire(){
            WSADATA data{};require(WSAStartup(MAKEWORD(2,2),&data)==0,"Fixture endpoint socket runtime did not start.");initialized=true;
            handle=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(handle!=INVALID_SOCKET,"Fixture endpoint reservation could not create a socket.");
            const BOOL exclusive=TRUE;require(setsockopt(handle,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive))!=SOCKET_ERROR,"Fixture endpoint reservation could not retain exclusive use.");
            sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            require(bind(handle,reinterpret_cast<const sockaddr*>(&address),sizeof(address))!=SOCKET_ERROR,"Fixture endpoint reservation could not bind loopback.");
            int length=sizeof(address);require(getsockname(handle,reinterpret_cast<sockaddr*>(&address),&length)!=SOCKET_ERROR&&address.sin_port!=0U,"Fixture endpoint reservation did not receive an OS-assigned port.");
            // Retain the bound socket without listening: independent fixtures
            // cannot share the provider lease or accidentally expose a provider.
            return "http://127.0.0.1:"+std::to_string(ntohs(address.sin_port));
        }
    } unavailableEndpoint_;
public:
    Fs::path root;
    std::unique_ptr<Infrastructure::Windows::WindowsWorkspaceAuthority> issuer;
    std::unique_ptr<NativeTools::Windows::WindowsComfyUiBackend> backend;
    Domain::ComfyUiConfig config;
    Domain::ProjectId project{parse<Domain::ProjectId>("10000000-0000-4000-8000-000000000001")};
    explicit Fixture(bool projectWorkspaceOnly = false) {
        Infrastructure::Windows::WindowsUuidGenerator uuid;
        root=Fs::temp_directory_path()/std::wstring{L"forge-comfy-test-"}/std::wstring{take(Infrastructure::Windows::Detail::strictUtf8ToUtf16(take(uuid.next()).value()))};
        Fs::create_directories(root/L"portable"/L"ComfyUI"/L"custom_nodes");
        Fs::create_directories(root/L"workspace");
        std::ofstream{root/L"portable"/L"ComfyUI"/L"main.py"} << "# fixture\n";
        issuer=std::make_unique<Infrastructure::Windows::WindowsWorkspaceAuthority>(std::vector<Infrastructure::Windows::WindowsWorkspaceAuthorityPolicy>{{
            parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000001"),project,parse<Domain::ClientId>("forge-conductor-manager"),
            {pathText(projectWorkspaceOnly ? root/L"workspace" : root)},Domain::FileAccess::Write,{Domain::FileAccess::Read,Domain::FileAccess::Write,Domain::FileAccess::Create},{},false,1U}});
        config.enabled=true;config.installationPath=pathText(root/L"portable").value();config.endpoint=unavailableEndpoint_.acquire();
        backend=std::make_unique<NativeTools::Windows::WindowsComfyUiBackend>(*issuer,pathText(root/L"runtime"));
    }
    ~Fixture(){backend->shutdown();std::error_code ignored;Fs::remove_all(root,ignored);}
    Domain::Result<std::string> call(std::string_view operation,const Json& arguments,const Domain::OperationContext& context=TestContext{}.active()) {
        return backend->perform(operation,arguments.dump(),config,take(issuer->authorityFor(project,context)),context);
    }
    void sealCurrentOwner() {
        FILETIME created{}, exited{}, kernel{}, user{};
        require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE,
            "Fixture process creation identity could not be measured.");
        std::array<wchar_t, 32768> image{};
        DWORD length = static_cast<DWORD>(image.size());
        require(QueryFullProcessImageNameW(GetCurrentProcess(), 0U, image.data(), &length) != FALSE,
            "Fixture process image identity could not be measured.");
        const Fs::path executable{std::wstring{image.data(), length}};
        std::ifstream source{executable, std::ios::binary};
        require(static_cast<bool>(source), "Fixture process image could not be sealed.");
        const std::vector<char> contents{std::istreambuf_iterator<char>{source}, std::istreambuf_iterator<char>{}};
        Infrastructure::Windows::BCryptSha256Hasher hasher;
        const auto sha256 = take(hasher.sha256(std::as_bytes(std::span{contents.data(), contents.size()}))).value();
        Fs::create_directories(root/L"runtime"/L"inputs");Fs::create_directories(root/L"runtime"/L"outputs");
        const std::string extraPaths{"{}"};std::ofstream{root/L"runtime"/L"extra-paths.yaml",std::ios::binary}<<extraPaths;
        const auto extraSha=take(hasher.sha256(std::as_bytes(std::span{extraPaths.data(),extraPaths.size()}))).value();
        std::ofstream{root/L"runtime"/L"active-environment.json"}<<Json{{"python",pathText(executable).value()}}.dump();
        Json interpreterPaths=Json::array();std::vector<Fs::path> pathConfigurations;
        for(const auto& entry:Fs::directory_iterator{executable.parent_path()})if(entry.is_regular_file()&&entry.path().extension()==L"._pth")pathConfigurations.push_back(entry.path());
        std::sort(pathConfigurations.begin(),pathConfigurations.end());for(const auto& path:pathConfigurations){std::ifstream input{path,std::ios::binary};const std::string bytes{std::istreambuf_iterator<char>{input},std::istreambuf_iterator<char>{}};
            interpreterPaths.push_back({{"path",pathText(path).value()},{"sha256",take(hasher.sha256(std::as_bytes(std::span{bytes.data(),bytes.size()}))).value()}});}
        const auto projection=Json::parse(take(call("status",Json::object()))).at("launch_configuration");
        const auto whitelist=projection.at("custom_node_whitelist");
        const Json ledger{{"pid", GetCurrentProcessId()},
            {"creation_time", (static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime},
            {"endpoint", config.endpoint}, {"python", pathText(executable).value()}, {"image_identity", {{"sha256", sha256}}},
            {"installation", pathText(root/L"portable"/L"ComfyUI").value()},
            {"model_storage_path",config.modelStoragePath.empty()?pathText(root/L"portable"/L"ComfyUI"/L"models").value():config.modelStoragePath},{"extra_paths_sha256",extraSha},{"interpreter_path_configuration",interpreterPaths},{"custom_node_whitelist",whitelist},
            {"original_extra_paths",projection.at("original_extra_paths")},{"model_category_paths",projection.at("model_category_paths")},
            {"output_directory", pathText(root/L"runtime"/L"outputs").value()}, {"input_directory", pathText(root/L"runtime"/L"inputs").value()}};
        std::ofstream destination{root/L"runtime"/L"runtime-owner.json", std::ios::binary};
        destination << ledger.dump();
        require(static_cast<bool>(destination), "Fixture provider owner ledger could not be saved.");
    }
};

class LoopbackWebSocketServer final {
public:
    LoopbackWebSocketServer(){
        WSADATA data{};require(WSAStartup(MAKEWORD(2,2),&data)==0,"Websocket fixture socket runtime did not start.");
        listener_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(listener_!=INVALID_SOCKET,"Websocket fixture listener could not be created.");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        require(bind(listener_,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==0&&listen(listener_,1)==0,"Websocket fixture listener could not bind.");
        int size=sizeof(address);require(getsockname(listener_,reinterpret_cast<sockaddr*>(&address),&size)==0,"Websocket fixture port is unavailable.");port_=ntohs(address.sin_port);
        worker_=std::jthread{[this](std::stop_token stop){serve(stop);}};
    }
    ~LoopbackWebSocketServer(){worker_.request_stop();{std::lock_guard lock{mutex_};if(client_!=INVALID_SOCKET){shutdown(client_,SD_BOTH);closesocket(client_);client_=INVALID_SOCKET;}}shutdown(listener_,SD_BOTH);closesocket(listener_);changed_.notify_all();worker_.join();WSACleanup();}
    std::string endpoint()const{return "ws://127.0.0.1:"+std::to_string(port_)+"/ws";}
    void frame(unsigned char opcode,bool final,std::string_view payload){
        std::unique_lock lock{mutex_};require(changed_.wait_for(lock,std::chrono::seconds{2},[&]{return connected_||!error_.empty();}),"Websocket fixture handshake did not finish.");require(error_.empty(),error_);require(client_!=INVALID_SOCKET,"Websocket fixture is already disconnected.");
        std::string header(1,static_cast<char>(opcode|(final?0x80U:0U)));if(payload.size()<126U)header+=static_cast<char>(payload.size());
        else if(payload.size()<=65535U){header+=static_cast<char>(126U);header+=static_cast<char>(payload.size()>>8U);header+=static_cast<char>(payload.size());}
        else {header+=static_cast<char>(127U);for(int shift=56;shift>=0;shift-=8)header+=static_cast<char>(static_cast<std::uint64_t>(payload.size())>>shift);}
        require(sendBytes(client_,header)&&sendBytes(client_,payload),"Websocket fixture frame could not be sent.");
    }
    Json command(){
        std::unique_lock lock{mutex_};require(changed_.wait_for(lock,std::chrono::seconds{2},[&]{return connected_||!error_.empty();}),"Websocket command fixture handshake did not finish.");require(error_.empty(),error_);
        const auto read=[&](std::size_t bytes){std::string result(bytes,'\0');std::size_t offset{};while(offset<bytes){const auto count=recv(client_,result.data()+offset,static_cast<int>(bytes-offset),0);require(count>0,"Websocket fixture command read did not complete.");offset+=static_cast<std::size_t>(count);}return result;};
        const auto header=read(2U);require(static_cast<unsigned char>(header[0])==0x81U&&(static_cast<unsigned char>(header[1])&0x80U)!=0U,"CDP fixture expected one masked UTF-8 command.");
        std::uint64_t bytes=static_cast<unsigned char>(header[1])&0x7fU;if(bytes==126U){const auto size=read(2U);bytes=(static_cast<unsigned char>(size[0])<<8U)|static_cast<unsigned char>(size[1]);}
        else if(bytes==127U){const auto size=read(8U);bytes=0U;for(unsigned char value:size)bytes=(bytes<<8U)|value;}
        require(bytes<=65536U,"CDP fixture command exceeds its test bound.");const auto mask=read(4U);auto payload=read(static_cast<std::size_t>(bytes));for(std::size_t index=0U;index<payload.size();++index)payload[index]^=mask[index%4U];return Json::parse(payload);
    }
    void disconnect(){std::lock_guard lock{mutex_};if(client_!=INVALID_SOCKET){shutdown(client_,SD_BOTH);closesocket(client_);client_=INVALID_SOCKET;}}
private:
    SOCKET listener_{INVALID_SOCKET},client_{INVALID_SOCKET};unsigned short port_{};std::jthread worker_;std::mutex mutex_;std::condition_variable_any changed_;bool connected_{};std::string error_;
    static bool sendBytes(SOCKET socket,std::string_view bytes){for(std::size_t offset=0U;offset<bytes.size();){const auto sent=send(socket,bytes.data()+offset,static_cast<int>((std::min)(bytes.size()-offset,std::size_t{65536U})),0);if(sent<=0)return false;offset+=static_cast<std::size_t>(sent);}return true;}
    static std::string acceptance(std::string key){
        key+="258EAFA5-E914-47DA-95CA-C5AB0DC85B11";BCRYPT_ALG_HANDLE algorithm{};require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA1_ALGORITHM,nullptr,0)>=0,"Websocket fixture SHA1 provider unavailable.");
        std::array<unsigned char,20> digest{};const auto status=BCryptHash(algorithm,nullptr,0,reinterpret_cast<PUCHAR>(key.data()),static_cast<ULONG>(key.size()),digest.data(),static_cast<ULONG>(digest.size()));BCryptCloseAlgorithmProvider(algorithm,0);require(status>=0,"Websocket fixture handshake hash failed.");
        constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";std::string encoded;
        for(std::size_t index=0U;index<digest.size();index+=3U){const auto remaining=digest.size()-index;const auto value=(static_cast<unsigned>(digest[index])<<16U)|(remaining>1U?static_cast<unsigned>(digest[index+1U])<<8U:0U)|(remaining>2U?digest[index+2U]:0U);encoded+=alphabet[(value>>18U)&63U];encoded+=alphabet[(value>>12U)&63U];encoded+=remaining>1U?alphabet[(value>>6U)&63U]:'=';encoded+=remaining>2U?alphabet[value&63U]:'=';}
        return encoded;
    }
    void serve(std::stop_token stop)noexcept{
        SOCKET accepted=INVALID_SOCKET;
        try{accepted=accept(listener_,nullptr,nullptr);if(accepted==INVALID_SOCKET)return;const DWORD timeout=2000U;setsockopt(accepted,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));setsockopt(accepted,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
            std::string request;std::array<char,4096> buffer{};while(request.find("\r\n\r\n")==std::string::npos&&request.size()<16384U){const auto count=recv(accepted,buffer.data(),static_cast<int>(buffer.size()),0);require(count>0,"Websocket fixture handshake was interrupted.");request.append(buffer.data(),static_cast<std::size_t>(count));}
            auto normalized=request;std::transform(normalized.begin(),normalized.end(),normalized.begin(),[](unsigned char value){return static_cast<char>(std::tolower(value));});const auto start=normalized.find("sec-websocket-key:");require(start!=std::string::npos,"Websocket fixture handshake key missing.");const auto end=request.find("\r\n",start);auto key=request.substr(start+18U,end-(start+18U));while(!key.empty()&&key.front()==' ')key.erase(0,1U);while(!key.empty()&&key.back()==' ')key.pop_back();require(key.size()<=128U,"Websocket fixture handshake key exceeds bound.");
            const auto response="HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "+acceptance(key)+"\r\n\r\n";require(sendBytes(accepted,response),"Websocket fixture handshake response failed.");
            std::unique_lock lock{mutex_};client_=accepted;accepted=INVALID_SOCKET;connected_=true;changed_.notify_all();changed_.wait(lock,stop,[]{return false;});
        }catch(const std::exception& failure){std::lock_guard lock{mutex_};error_=failure.what();changed_.notify_all();}
        if(accepted!=INVALID_SOCKET)closesocket(accepted);
    }
};

void nativeWebSocketPollingRetainsSilentAndFragmentedReads(){
    using NativeTools::Windows::ComfyDetail::Cdp;const auto context=TestContext{}.active();LoopbackWebSocketServer server;Cdp socket{server.endpoint(),context};
    const auto begin=std::chrono::steady_clock::now();require(socket.receive(context).is_null(),"Silent websocket poll did not return an empty event.");require(std::chrono::steady_clock::now()-begin<std::chrono::milliseconds{500},"Silent native websocket poll blocked beyond its bounded wait.");
    const Json event{{"type","progress"},{"data",{{"value",3},{"max",10}}}};server.frame(1U,true,event.dump());require(socket.receive(context)==event,"Event arriving after an idle poll was lost or consumed by another receive.");
    server.frame(1U,false,"{\"type\":\"progress\",\"data\":");const auto fragmentBegin=std::chrono::steady_clock::now();require(socket.receive(context).is_null(),"Incomplete websocket JSON was published before its final fragment.");require(std::chrono::steady_clock::now()-fragmentBegin<std::chrono::milliseconds{500},"Incomplete websocket fragment defeated the poll bound.");
    server.frame(0U,true,"{\"value\":4}}");require(socket.receive(context)==Json({{"type","progress"},{"data",{{"value",4}}}}),"Fragment state was lost across an idle poll.");
    server.frame(2U,false,std::string(4096U,'P'));require(socket.receive(context).is_null(),"Fragmented binary preview was published before completion.");server.frame(0U,true,std::string(8192U,'Q'));require(socket.receive(context)==Json({{"type","binary_preview"},{"bytes",12288U}}),"Binary preview fragments were not retained and bounded by their actual bytes.");
    server.disconnect();bool disconnected{};try{static_cast<void>(socket.receive(context));}catch(const NativeTools::Windows::ComfyDetail::Failure& failure){disconnected=failure.error.code==Domain::ErrorCodes::TransportClosed;}require(disconnected,"Remote websocket disconnect was not an explicit transport failure.");
}
void nativeWebSocketWaitsRespectCancellationDeadlinesAndPendingDestruction(){
    using NativeTools::Windows::ComfyDetail::Cdp;
    for(const bool cancelled:{true,false}){TestContext fixture;auto context=fixture.active();LoopbackWebSocketServer server;Cdp socket{server.endpoint(),context};if(!cancelled)context.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds{60};
        std::jthread cancellation{[&](std::stop_token stop){if(cancelled&&!stop.stop_requested()){std::this_thread::sleep_for(std::chrono::milliseconds{25});fixture.cancellation.request_stop();}}};const auto begin=std::chrono::steady_clock::now();bool refused{};
        try{static_cast<void>(socket.receive(context));}catch(const NativeTools::Windows::ComfyDetail::Failure& failure){refused=failure.error.code==(cancelled?Domain::ErrorCodes::Cancelled:Domain::ErrorCodes::DeadlineExceeded);}require(refused,"Silent websocket wait ignored cancellation or its operation deadline.");require(std::chrono::steady_clock::now()-begin<std::chrono::milliseconds{300},"Silent websocket cancellation/deadline was not bounded.");}
    for(unsigned attempt=0U;attempt<12U;++attempt){LoopbackWebSocketServer server;auto socket=std::make_unique<Cdp>(server.endpoint(),TestContext{}.active());require(socket->receive(TestContext{}.active()).is_null(),"Pending-close fixture did not retain a silent receive.");const auto begin=std::chrono::steady_clock::now();socket.reset();require(std::chrono::steady_clock::now()-begin<std::chrono::milliseconds{500},"Async websocket destruction blocked while a receive was pending.");}
}
void nativeCdpCallsPreserveResponseMatchingAndMessageBounds(){
    using NativeTools::Windows::ComfyDetail::Cdp;const auto context=TestContext{}.active();
    {LoopbackWebSocketServer server;Cdp socket{server.endpoint(),context};Json response;std::exception_ptr error;std::jthread caller{[&]{try{response=socket.call("Runtime.evaluate",{{"expression","2+2"}},context);}catch(...){error=std::current_exception();}}};const auto command=server.command();require(command.at("method")=="Runtime.evaluate"&&command.at("params").at("expression")=="2+2","Native CDP command changed its exact payload.");server.frame(1U,true,Json{{"method","Runtime.consoleAPICalled"},{"params",Json::object()}}.dump());std::this_thread::sleep_for(std::chrono::milliseconds{150});server.frame(1U,false,"{\"id\":"+command.at("id").dump()+",\"result\":");std::this_thread::sleep_for(std::chrono::milliseconds{120});server.frame(0U,true,"{\"value\":4}}");caller.join();if(error)std::rethrow_exception(error);require(response==Json({{"value",4}}),"Native CDP call lost delayed fragments or matched a foreign event.");}
    {LoopbackWebSocketServer server;Cdp socket{server.endpoint(),context};auto deadline=context;deadline.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds{70};const auto begin=std::chrono::steady_clock::now();bool expired{};try{static_cast<void>(socket.call("Runtime.evaluate",{},deadline));}catch(const NativeTools::Windows::ComfyDetail::Failure& failure){expired=failure.error.code==Domain::ErrorCodes::DeadlineExceeded;}require(expired&&std::chrono::steady_clock::now()-begin<std::chrono::milliseconds{300},"Silent CDP command wait ignored the operation deadline.");}
    {LoopbackWebSocketServer server;Cdp socket{server.endpoint(),context};std::exception_ptr error;std::jthread producer{[&]{try{server.frame(2U,true,std::string(1024U*1024U+1U,'B'));}catch(...){error=std::current_exception();}}};bool bounded{};const auto until=std::chrono::steady_clock::now()+std::chrono::seconds{3};try{while(std::chrono::steady_clock::now()<until)static_cast<void>(socket.receive(context));}catch(const NativeTools::Windows::ComfyDetail::Failure& failure){bounded=failure.error.code==Domain::ErrorCodes::PayloadTooLarge;}producer.join();if(error)std::rethrow_exception(error);require(bounded,"Native binary preview exceeded the preserved 1 MiB message bound.");}
    {LoopbackWebSocketServer server;Cdp socket{server.endpoint(),context};std::exception_ptr error;bool bounded{};std::jthread caller{[&]{try{static_cast<void>(socket.call("Runtime.evaluate",{},context));}catch(const NativeTools::Windows::ComfyDetail::Failure& failure){bounded=failure.error.code==Domain::ErrorCodes::PayloadTooLarge;}catch(...){error=std::current_exception();}}};static_cast<void>(server.command());server.frame(1U,false,std::string(16U*1024U*1024U,'X'));server.frame(0U,true,"X");caller.join();if(error)std::rethrow_exception(error);require(bounded,"Native CDP response exceeded the preserved 16 MiB message bound.");}
}

class LoopbackArtifactServer final {
public:
    std::atomic<unsigned> views{}, prompts{};
    std::atomic<bool> unexpectedRequest{};
    explicit LoopbackArtifactServer(std::vector<std::byte> bytes, bool truncateFirst = true, unsigned status = 200U, Json queue = nullptr,unsigned short requestedPort=0U,std::vector<std::byte> secondArtifact = {},Json schemas = Json::object(),std::vector<std::string> redirects = {})
        : bytes_{std::move(bytes)}, secondArtifact_{std::move(secondArtifact)}, truncateFirst_{truncateFirst}, status_{status}, queue_(std::move(queue)),schemas_(std::move(schemas)),redirects_(std::move(redirects)) {
        if(queue_.is_null())queue_={{"queue_running",Json::array()},{"queue_pending",Json::array()}};
        require(queue_.is_object(),"Fixture provider queue must be a JSON object.");
        require(schemas_.is_object(),"Fixture provider schemas must be a JSON object.");
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Fixture socket runtime did not start.");
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(listener_ != INVALID_SOCKET, "Fixture listener could not be created.");
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        endpoint.sin_port=htons(requestedPort);
        require(bind(listener_, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0 &&
            listen(listener_, SOMAXCONN) == 0, "Fixture loopback listener did not bind.");
        int size = sizeof(endpoint);
        require(getsockname(listener_, reinterpret_cast<sockaddr*>(&endpoint), &size) == 0,
            "Fixture listener port could not be read.");
        port_ = ntohs(endpoint.sin_port);
        worker_ = std::jthread{[this](std::stop_token stop) { serve(stop); }};
    }
    ~LoopbackArtifactServer() {
        worker_.request_stop();
        shutdown(listener_, SD_BOTH);
        closesocket(listener_);
        worker_.join();
        WSACleanup();
    }
    std::string endpoint() const { return "http://127.0.0.1:" + std::to_string(port_); }
private:
    SOCKET listener_{INVALID_SOCKET};
    unsigned short port_{};
    std::vector<std::byte> bytes_;
    std::vector<std::byte> secondArtifact_;
    bool truncateFirst_{};
    unsigned status_{};
    Json queue_;
    Json schemas_;
    std::vector<std::string> redirects_;
    std::jthread worker_;
    static bool sendBytes(SOCKET socket, const char* bytes, std::size_t length) noexcept {
        while (length) {
            const auto sent = send(socket, bytes, static_cast<int>(std::min<std::size_t>(length, 8192U)), 0);
            if (sent <= 0) return false;
            bytes += sent;
            length -= static_cast<std::size_t>(sent);
        }
        return true;
    }
    void serve(std::stop_token stop) noexcept {
        while (!stop.stop_requested()) {
            const auto client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            const DWORD timeout = 5000U;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            try {
                std::string request;
                std::array<char, 4096> buffer{};
                while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536U) {
                    const auto received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
                    if (received <= 0) break;
                    request.append(buffer.data(), static_cast<std::size_t>(received));
                }
                if (request.starts_with("GET /view?")) {
                    const auto number = ++views;
                    const auto& artifact = number==2U&&!secondArtifact_.empty()?secondArtifact_:bytes_;
                    const bool redirected=number<=redirects_.size();
                    const auto location=redirected?"Location: "+redirects_[number-1U]+"\r\n":std::string{};
                    const auto header = "HTTP/1.1 "+std::to_string(redirected?302U:status_)+" Fixture\r\n"+location+"Content-Type: video/mp4\r\nContent-Length: " +
                        std::to_string(artifact.size()) + "\r\nConnection: close\r\n\r\n";
                    const auto count = truncateFirst_ && number == 1U ? artifact.size() / 2U : artifact.size();
                    if (!sendBytes(client, header.data(), header.size()) ||
                        !sendBytes(client, reinterpret_cast<const char*>(artifact.data()), count)) unexpectedRequest = true;
                } else if(request.starts_with("GET /system_stats ")||request.starts_with("GET /queue ")||request.starts_with("GET /object_info ")||request.starts_with("GET /object_info/")) {
                    Json schemas=schemas_;
                    if(request.starts_with("GET /object_info/")){const auto end=request.find(' ',17U);const auto name=request.substr(17U,end-17U);schemas=Json::object();if(schemas_.contains(name))schemas[name]=schemas_.at(name);}
                    const auto body=request.starts_with("GET /queue ")?queue_.dump():request.starts_with("GET /object_info")?schemas.dump():std::string{"{}"};
                    const auto response="HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(body.size())+"\r\nConnection: close\r\n\r\n"+body;
                    if(!sendBytes(client,response.data(),response.size()))unexpectedRequest=true;
                } else {
                    if (request.starts_with("POST /prompt")) ++prompts;
                    unexpectedRequest = true;
                    const std::string denied{"HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"};
                    static_cast<void>(sendBytes(client, denied.data(), denied.size()));
                }
            } catch (...) { unexpectedRequest = true; }
            shutdown(client, SD_SEND);
            closesocket(client);
        }
    }
};

class PublicationBoundaryAuthority final:public Contracts::IWorkspaceAuthority {
public:
    Infrastructure::Windows::WindowsWorkspaceAuthority& delegate;
    Domain::PathText destination;
    std::stop_source* cancellation{};
    bool deny{},entered{};
    PublicationBoundaryAuthority(Infrastructure::Windows::WindowsWorkspaceAuthority& issuer,Domain::PathText target,std::stop_source* stop,bool reject)
        :delegate{issuer},destination{std::move(target)},cancellation{stop},deny{reject}{}
    Domain::Result<Contracts::WorkspaceAuthority> authorityFor(const Domain::ProjectId& project,const Domain::OperationContext& operation)noexcept override{return delegate.authorityFor(project,operation);}
    Domain::Result<Contracts::WorkspaceAuthority> narrow(const Contracts::WorkspaceAuthority& authority,const std::vector<Domain::PathText>& roots,const std::vector<Domain::FileAccess>& grants,bool shell,std::uint64_t generation,const Domain::OperationContext& operation)noexcept override{return delegate.narrow(authority,roots,grants,shell,generation,operation);}
    Domain::Result<Contracts::AuthorizedPath> authorize(const Contracts::WorkspaceAuthority& authority,const Domain::PathAuthorizationRequest& request,const Domain::OperationContext& operation)noexcept override {
        try{if(!entered && request.access==Domain::FileAccess::Read && request.requestedPath==destination && Fs::is_regular_file(Fs::path{take(Infrastructure::Windows::Detail::strictUtf8ToUtf16(destination.value()))})) {
                entered=true;if(cancellation)cancellation->request_stop();if(deny)return Domain::Result<Contracts::AuthorizedPath>::failure(Domain::makeError(Domain::ErrorCodes::Unauthorized,"Controlled publication verification authorization failure."));}
            return delegate.authorize(authority,request,operation);
        }catch(const std::exception& failure){return Domain::Result<Contracts::AuthorizedPath>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,failure.what()));}
    }
};

Json collectArguments(const Fixture& fixture, const char* filename = "preview.png") {
    const std::string owned{"ForgeConductor/project/job/preview"};
    return {{"prompt_id", "exact-prompt-id"}, {"namespace", owned},
        {"output_directory", pathText(fixture.root/L"workspace").value()}, {"expected_outputs", Json::array({"12"})},
        {"outputs", {{"12", {{"images", Json::array({{{"filename", filename}, {"subfolder", owned}, {"type", "output"}}})}}}}}};
}

void artifactCollectionAcceptsWindowsProviderSeparators() {
    NativeTools::Windows::Detail::ImageProviderPixels pixels{64U,64U,std::vector<std::byte>(64U*64U*4U,std::byte{42})};
    const auto bytes=take(NativeTools::Windows::Detail::encodeProviderImage(pixels,TestContext{}.active()));
    for(const auto& folder:std::array<std::string,6>{"ForgeConductor/project/job/preview","ForgeConductor\\project\\job\\preview","ForgeConductor\\project/job\\preview/child","ForgeConductor\\project\\job\\preview-other","ForgeConductor\\project\\job\\preview\\..\\other","ForgeConductor\\another-project\\job\\preview"}) {
        Fixture fixture{true};LoopbackArtifactServer server{bytes,false};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
        auto arguments=collectArguments(fixture);auto& descriptor=arguments["outputs"]["12"]["images"][0];descriptor["subfolder"]=folder;
        const auto result=Json::parse(take(fixture.call("collect",arguments)));
        const bool admitted=folder=="ForgeConductor/project/job/preview"||folder=="ForgeConductor\\project\\job\\preview"||folder=="ForgeConductor\\project/job\\preview/child";
        require(result.at("ok")==admitted,"Provider path separator normalization changed exact namespace admission.");
        if(admitted) {
            require(result.at("artifacts").size()==1U&&result.at("artifacts")[0].at("descriptor")==descriptor&&server.views==1U&&Fs::is_regular_file(fixture.root/L"workspace"/L"12_preview.png"),"Windows provider descriptor did not retain its exact original shape through verified publication.");
            std::string encodedFolder;for(const auto character:folder) {
                if(character=='\\')encodedFolder+="%5C";else if(character=='/')encodedFolder+="%2F";else encodedFolder+=character;
            }
            require(result.at("artifacts")[0].at("provider_view_url")==server.endpoint()+"/view?filename=preview.png&subfolder="+encodedFolder+"&type=output","Published provider link differs from the exact descriptor used to retrieve the verified artifact.");
        }
        else require(result.at("artifacts").empty()&&!result.at("unresolved").empty()&&server.views==0U&&!Fs::exists(fixture.root/L"workspace"/L"12_preview.png"),"An unrelated or traversing provider descriptor was transferred or published.");
        require(server.prompts==0U&&!server.unexpectedRequest,"Artifact collection created a generation or unexpected request.");
    }
}

void interruptedArtifactTransferReattachesAfterBackendRestart() {
    Fixture fixture{true};
    NativeTools::Windows::Detail::ImageProviderPixels pixels{64U, 64U, std::vector<std::byte>(64U*64U*4U, std::byte{42})};
    const auto bytes = take(NativeTools::Windows::Detail::encodeProviderImage(pixels, TestContext{}.active()));
    LoopbackArtifactServer server{bytes};
    fixture.config.endpoint = server.endpoint();
    fixture.sealCurrentOwner();
    const auto arguments = collectArguments(fixture);
    const auto operation = TestContext{}.active();
    const auto first = fixture.call("collect", arguments, operation);
    require(!first, "A truncated provider response was published as a complete artifact.");
    require(!Fs::exists(fixture.root/L"workspace"/L"12_preview.png"), "Partial artifact escaped staging.");
    require(server.views == 1U && server.prompts == 0U, "Collection retried or submitted generation unexpectedly.");
    const auto directory = fixture.root/L"runtime"/L"transfers";
    require(Fs::is_directory(directory), "Manager-owned transfer ledger could not be retained outside project roots.");
    const auto transferPath = Fs::directory_iterator{directory}->path();
    std::ifstream initialStream{transferPath};
    const auto interrupted = Json::parse(initialStream);
    initialStream.close();
    require(interrupted.at("state") == "interrupted" && interrupted.at("attempts").size() == 1U &&
        interrupted.at("downloaded_bytes").get<std::uint64_t>() > 0U, "Partial transfer lacks durable interrupted progress.");
    const auto partial = Fs::path{interrupted.at("attempts")[0].at("path").get<std::string>()};
    require(Fs::is_regular_file(partial), "Interrupted transfer evidence was discarded.");
    fixture.backend->shutdown();
    fixture.backend = std::make_unique<NativeTools::Windows::WindowsComfyUiBackend>(*fixture.issuer, pathText(fixture.root/L"runtime"));
    const auto completed = Json::parse(take(fixture.call("collect", arguments, operation)));
    require(completed.at("ok") == true && completed.at("artifacts").size() == 1U, "Exact artifact reattachment did not complete after restart.");
    require(completed.at("artifacts")[0].at("bytes") == bytes.size() && completed.at("artifacts")[0].at("media_type") == "image/png",
        "Provider Content-Type overrode actual media inspection or the byte count changed.");
    require(server.views == 2U && server.prompts == 0U && !server.unexpectedRequest, "Artifact reattachment submitted another generation or addressed an unrelated route.");
    std::ifstream completedStream{transferPath};
    const auto retained = Json::parse(completedStream);
    completedStream.close();
    require(retained.at("state") == "published" && retained.at("attempts").size() == 2U &&
        retained.at("attempts")[0].at("state") == "interrupted" && retained.at("attempts")[1].at("state") == "verified",
        "Retry replaced prior transfer evidence or failed to seal publication.");
    require(Fs::is_regular_file(partial) && retained.at("attempts")[0].at("path") != retained.at("attempts")[1].at("path"),
        "Retry reused or removed the interrupted staging file.");
}

void corruptArtifactRemainsUnpublished() {
    Fixture fixture{true};
    std::vector<std::byte> corrupt(128U, std::byte{42});
    const std::array<unsigned char, 8> signature{0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    for (std::size_t index{}; index < signature.size(); ++index) corrupt[index] = static_cast<std::byte>(signature[index]);
    LoopbackArtifactServer server{std::move(corrupt), false};
    fixture.config.endpoint = server.endpoint();
    fixture.sealCurrentOwner();
    require(!fixture.call("collect", collectArguments(fixture, "corrupt.png")), "Invalid PNG bytes with a valid signature bypassed decode verification.");
    require(!Fs::exists(fixture.root/L"workspace"/L"12_corrupt.png"), "Corrupt artifact was published before media verification.");
    require(server.views == 1U && server.prompts == 0U && !server.unexpectedRequest, "Corrupt artifact handling resubmitted generation.");
}

void artifactContentOverridesDescriptorAndHeader() {
    Fixture fixture{true};
    NativeTools::Windows::Detail::ImageProviderPixels pixels{64U, 64U, std::vector<std::byte>(64U*64U*4U, std::byte{42})};
    const auto bytes = take(NativeTools::Windows::Detail::encodeProviderImage(pixels, TestContext{}.active()));
    LoopbackArtifactServer server{bytes, false};
    fixture.config.endpoint = server.endpoint();
    fixture.sealCurrentOwner();
    auto arguments = collectArguments(fixture, "renamed.mp4");
    arguments["outputs"]["12"]["gifs"] = arguments["outputs"]["12"]["images"];
    arguments["outputs"]["12"].erase("images");
    const auto result = Json::parse(take(fixture.call("collect", arguments)));
    require(result.at("ok") == true && result.at("artifacts").size() == 1U, "Core/VHS descriptor key affected artifact admission.");
    const auto& artifact = result.at("artifacts")[0];
    require(artifact.at("media_type") == "image/png" && artifact.at("history_key") == "gifs" && artifact.at("provider_media_type") == "video/mp4",
        "Provider filename, Content-Type, or VHS key overrode verified actual content.");
    require(artifact.at("metadata").at("width") == 64U && artifact.at("metadata").at("height") == 64U,
        "Renamed image bytes were not decoded before publication.");
    require(server.prompts == 0U && server.views == 1U && !server.unexpectedRequest,
        "Content classification submitted generation or performed an unrelated request.");
}
void partialPublicationRetainsActualBatchManifestAndStaging() {
    NativeTools::Windows::Detail::ImageProviderPixels pixels{64U,64U,std::vector<std::byte>(64U*64U*4U,std::byte{42})};const auto bytes=take(NativeTools::Windows::Detail::encodeProviderImage(pixels,TestContext{}.active()));
    for(const bool cancel:{false,true}) {
        Fixture fixture;LoopbackArtifactServer server{bytes,false};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
        std::stop_source stop;PublicationBoundaryAuthority boundary{*fixture.issuer,pathText(fixture.root/L"workspace"/L"12_first.png"),cancel?&stop:nullptr,!cancel};
        fixture.backend=std::make_unique<NativeTools::Windows::WindowsComfyUiBackend>(boundary,pathText(fixture.root/L"runtime"));auto operation=TestContext{}.active();operation.cancellation=stop.get_token();
        auto arguments=collectArguments(fixture,"first.png");auto second=arguments.at("outputs").at("12").at("images")[0];second["filename"]="second.png";arguments["outputs"]["12"]["images"].push_back(second);
        const auto result=Json::parse(take(fixture.call("collect",arguments,operation)));require(boundary.entered && result.at("ok")==false && result.at("partial")==true && result.at("artifacts").empty() && result.at("error").at("code").get<std::string>()==(cancel?Domain::ErrorCodes::Cancelled:Domain::ErrorCodes::Unauthorized),"A late native publication failure lost its actual error or declared the partial batch complete.");
        require(result.at("published_count")==1U && result.at("verified_staging_count")==1U && result.at("publication_uncertain_count")==0U && result.at("publication_evidence").size()==2U,"Partial batch did not reconcile the actual moved file and retained second staging file.");
        const auto& published=result.at("publication_evidence")[0];const auto& staged=result.at("publication_evidence")[1];
        require(published.at("state")=="published_verified" && published.at("moved_by_this_attempt")==true && staged.at("state")=="verified_staging" && staged.at("publication_attempted")==false && Fs::is_regular_file(fixture.root/L"workspace"/L"12_first.png") && !Fs::exists(fixture.root/L"workspace"/L"12_second.png"),"Publication failure deleted a moved file, published the later file, or invented destination evidence.");
        const auto stagedPath=Fs::path{take(Infrastructure::Windows::Detail::strictUtf8ToUtf16(staged.at("staging_path").get<std::string>()))};require(Fs::is_regular_file(stagedPath),"Failed publication discarded verified inactive staging.");
        const auto manifestPath=Fs::path{take(Infrastructure::Windows::Detail::strictUtf8ToUtf16(result.at("publication_manifest").at("path").get<std::string>()))};const auto manifest=NativeTools::Windows::ComfyDetail::readJson(manifestPath);
        require(manifest.at("state")=="failed" && manifest.at("publication_evidence")==result.at("publication_evidence") && NativeTools::Windows::ComfyDetail::fileFacts(manifestPath,TestContext{}.active()).at("sha256")==result.at("publication_manifest").at("sha256"),"Partial publication manifest did not durably preserve exact reconciled evidence.");
        const auto recovered=Json::parse(take(fixture.call("collect",arguments)));require(recovered.at("ok")==true && recovered.at("artifacts").size()==2U && recovered.at("publication_manifest").at("path")!=result.at("publication_manifest").at("path") && NativeTools::Windows::ComfyDetail::readJson(manifestPath)==manifest && NativeTools::Windows::ComfyDetail::fileFacts(manifestPath,TestContext{}.active()).at("sha256")==result.at("publication_manifest").at("sha256"),"Exact generation collection retry overwrote its previously sealed partial-publication manifest.");
        require(server.views==4U && server.prompts==0U && !server.unexpectedRequest,"Collection error recovery resubmitted provider generation or repeated an unexpected transfer.");
    }
}
void wrappedSelectedModelIdentitySealsActualContent() {
    const Json schemas{{"ModelSource",{{"input",{{"required",{{"model",Json::array({Json::array({"installed.safetensors","UPPER.SAFETENSORS"})})}}}}},{"output",Json::array({"IMAGE"})},{"output_node",false}}}};
    for(const bool uppercase:{false,true}) {
        Fixture fixture;LoopbackArtifactServer server{{},false,200U,nullptr,0U,{},schemas};fixture.config.endpoint=server.endpoint();
        const auto models=fixture.root/L"portable"/L"ComfyUI"/L"models"/L"checkpoints";Fs::create_directories(models);const auto name=uppercase?"UPPER.SAFETENSORS":"installed.safetensors";const auto model=models/name;
        {std::ofstream file{model,std::ios::binary};file<<"first model fixture";}fixture.sealCurrentOwner();const auto modified=Fs::last_write_time(model);
        const Json graph{{"1",{{"class_type","ModelSource"},{"inputs",{{"model",{{"__value__",name}}}}}}}};const Json args{{"workflows",Json::array({graph})}};
        const auto original=Json::parse(take(fixture.call("identity",args)));require(original.at("models").size()==1U && original.at("models")[0].at("path")==pathText(model).value() && original.at("models")[0].contains("sha256"),"Supported wrapped model choice or uppercase extension was omitted from selected dependency identity.");
        {std::ofstream file{model,std::ios::binary|std::ios::trunc};file<<"other model fixture";}Fs::last_write_time(model,modified);const auto changed=Json::parse(take(fixture.call("identity",args)));
        require(changed.at("models")[0].at("bytes")==original.at("models")[0].at("bytes") && changed.at("models")[0].at("modified_time")==original.at("models")[0].at("modified_time") && changed.at("models")[0].at("sha256")!=original.at("models")[0].at("sha256") && changed.at("sha256")!=original.at("sha256"),"Model bytes changed under a wrapped input while dependency seal remained unchanged.");
        require(server.prompts==0U && !server.unexpectedRequest,"Selected model identity verification submitted inference or used an unknown provider route.");
    }
}
void schemaAndCatalogModelIdentitySealsAllFormatsAndExactCategories() {
    using NativeTools::Windows::ComfyDetail::writeJson;
    const auto modelSchema=[](Json required,Json optional=Json::object()) {
        return Json{{"input",{{"required",std::move(required)},{"optional",std::move(optional)}}},{"output",Json::array({"MODEL"})},{"output_node",false}};
    };
    {
        Fixture fixture{true};const Json schemas{{"ModelSource",modelSchema({{"configuration",Json::array({Json::array({"fixture.yaml"})})},{"prompt",Json::array({"STRING"})}},
            {{"classifier",Json::array({Json::array({"classifier"})})},{"audio",Json::array({Json::array({"input.bin"}),{{"audio_upload",true}}})}})}};
        LoopbackArtifactServer server{{},false,200U,nullptr,0U,{},schemas};fixture.config.endpoint=server.endpoint();
        const auto models=fixture.root/L"portable"/L"ComfyUI"/L"models";Fs::create_directories(models/L"configs");Fs::create_directories(models/L"classifiers");
        const auto configuration=models/L"configs"/L"fixture.yaml",classifier=models/L"classifiers"/L"classifier";
        std::ofstream{configuration,std::ios::binary}<<"first yaml";std::ofstream{classifier,std::ios::binary}<<"classifier bytes";
        std::ofstream{models/L"prompt.safetensors",std::ios::binary}<<"unrelated prompt bytes";std::ofstream{models/L"input.bin",std::ios::binary}<<"unrelated upload bytes";fixture.sealCurrentOwner();
        const Json graph{{"1",{{"class_type","ModelSource"},{"inputs",{{"configuration",{{"__value__","fixture.yaml"}}},{"classifier","classifier"},{"prompt","prompt.safetensors"},{"audio",{{"__value__","input.bin"}}}}}}}};
        const Json args{{"workflows",Json::array({graph})}};const auto before=Json::parse(take(fixture.call("identity",args)));require(before.at("models").size()==2U,"Actual required/optional file enums omitted YAML or extensionless model dependencies, or sealed a STRING/upload input as a model.");
        std::set<std::string> paths;for(const auto& model:before.at("models"))paths.insert(model.at("path").get<std::string>());
        require(paths==std::set<std::string>{pathText(configuration).value(),pathText(classifier).value()},"Schema-driven dependency discovery sealed files outside the actual selected model inputs.");
        const auto modified=Fs::last_write_time(configuration);std::ofstream{configuration,std::ios::binary|std::ios::trunc}<<"other yaml";Fs::last_write_time(configuration,modified);const auto changed=Json::parse(take(fixture.call("identity",args)));
        require(changed.at("sha256")!=before.at("sha256"),"Changed YAML bytes with unchanged size and timestamp did not invalidate dependency identity.");
        require(server.prompts==0U&&!server.unexpectedRequest,"Schema model identity discovery submitted generation or used an unexpected provider route.");
    }
    for(const bool selectedPresent:{true,false}) {
        Fixture fixture{true};const Json schemas{{"ModelSource",modelSchema({{"weights",Json::array({Json::array({"nested/fixture.weights"})})}})}};
        LoopbackArtifactServer server{{},false,200U,nullptr,0U,{},schemas};fixture.config.endpoint=server.endpoint();
        const auto models=fixture.root/L"portable"/L"ComfyUI"/L"models",manager=fixture.root/L"portable"/L"ComfyUI"/L"custom_nodes"/L"ComfyUI-Manager";
        Fs::create_directories(manager);writeJson(manager/L"model-list.json",{{"models",Json::array({{{"filename","fixture.weights"},{"save_path","checkpoints"},{"url","https://publisher.invalid/fixture.weights"}}})}});
        Fs::create_directories(models/L"vae"/L"nested");Fs::create_directories(models/L"checkpoints"/L"other");
        std::ofstream{models/L"vae"/L"nested"/L"fixture.weights",std::ios::binary}<<"wrong category";std::ofstream{models/L"checkpoints"/L"other"/L"fixture.weights",std::ios::binary}<<"wrong subfolder";
        const auto selected=models/L"checkpoints"/L"nested"/L"fixture.weights";if(selectedPresent){Fs::create_directories(selected.parent_path());std::ofstream{selected,std::ios::binary}<<"selected bytes";}fixture.sealCurrentOwner();
        const Json args{{"workflows",Json::array({{{"1",{{"class_type","ModelSource"},{"inputs",{{"weights",{{"__value__","nested/fixture.weights"}}}}}}}}})}};
        const auto result=fixture.call("identity",args);
        if(selectedPresent){const auto identity=Json::parse(take(result));require(identity.at("models").size()==1U&&identity.at("models")[0].at("path")==pathText(selected).value(),"Catalog-backed dependency identity admitted a basename match from the wrong category or subfolder.");}
        else requireError(result,Domain::ErrorCodes::IntegrityFailure,"A catalog-backed missing selected model was concealed by another category or subfolder with the same basename.");
        require(server.prompts==0U&&!server.unexpectedRequest,"Category-scoped model identity discovery submitted generation or used an unexpected provider route.");
    }
    {
        Fixture fixture{true};const Json schemas{{"DiffusersLoader",modelSchema({{"model_path",Json::array({Json::array({"fixture-directory"})})}})}};
        LoopbackArtifactServer server{{},false,200U,nullptr,0U,{},schemas};fixture.config.endpoint=server.endpoint();const auto directory=fixture.root/L"portable"/L"ComfyUI"/L"models"/L"diffusers"/L"fixture-directory";
        Fs::create_directories(directory/L"unet");writeJson(directory/L"model_index.json",{{"_class_name","FixturePipeline"}});std::ofstream{directory/L"unet"/L"weights",std::ios::binary}<<"fixture weights";
        const auto configuration=directory/L"unet"/L"config.json";std::ofstream{configuration,std::ios::binary}<<"{\"scale\":1}";fixture.sealCurrentOwner();
        const Json args{{"workflows",Json::array({{{"1",{{"class_type","DiffusersLoader"},{"inputs",{{"model_path",{{"__value__","fixture-directory"}}}}}}}}})}};
        const auto before=Json::parse(take(fixture.call("identity",args)));require(before.at("models").size()==3U,"A selected directory model did not seal every actual configuration and weight file.");
        std::set<std::string> paths;for(const auto& model:before.at("models"))paths.insert(model.at("path").get<std::string>());
        require(paths==std::set<std::string>{pathText(directory/L"model_index.json").value(),pathText(directory/L"unet"/L"weights").value(),pathText(configuration).value()},"Directory model identity did not preserve its complete selected content inventory.");
        const auto modified=Fs::last_write_time(configuration);std::ofstream{configuration,std::ios::binary|std::ios::trunc}<<"{\"scale\":2}";Fs::last_write_time(configuration,modified);const auto changed=Json::parse(take(fixture.call("identity",args)));
        require(changed.at("sha256")!=before.at("sha256"),"Changed directory model configuration bytes with unchanged metadata did not invalidate dependency identity.");
        require(server.prompts==0U&&!server.unexpectedRequest,"Directory model identity discovery submitted generation or used an unexpected provider route.");
    }
}
void clipEmbeddingReferencesSealCompleteContentOnlyWhenReferenced() {
    Fixture fixture{true};const Json schemas{
        {"CLIPTextEncode",{{"input",{{"required",{{"text",Json::array({"STRING",{{"multiline",true}}})},{"clip",Json::array({"CLIP"})}}}}},{"output",Json::array({"CONDITIONING"})},{"output_node",false}}},
        {"ClipSource",{{"input",{{"required",Json::object()}}},{"output",Json::array({"CLIP"})},{"output_node",false}}},
        {"CaptionSource",{{"input",{{"required",{{"text",Json::array({"STRING"})}}}}},{"output",Json::array({"CONDITIONING"})},{"output_node",false}}}};
    LoopbackArtifactServer server{{},false,200U,nullptr,0U,{},schemas};fixture.config.endpoint=server.endpoint();
    const auto embeddings=fixture.root/L"portable"/L"ComfyUI"/L"models"/L"embeddings";Fs::create_directories(embeddings/L"sub");
    const auto selected=embeddings/L"weighted-name.safetensors",other=embeddings/L"sub"/L"escaped(name).bin";
    std::ofstream{selected,std::ios::binary}<<"abc";std::ofstream{other,std::ios::binary}<<"first embedding";fixture.sealCurrentOwner();
    const auto arguments=[](std::string text,bool withClip=true){
        Json graph{{"1",{{"class_type",withClip?"CLIPTextEncode":"CaptionSource"},{"inputs",{{"text",{{"__value__",std::move(text)}}}}}}}};
        if(withClip){graph["1"]["inputs"]["clip"]=Json::array({"2",0});graph["2"]={{"class_type","ClipSource"},{"inputs",Json::object()}};}
        return Json{{"workflows",Json::array({std::move(graph)})}};
    };
    const auto plainArguments=arguments("a portrait"),foreignArguments=arguments("embedding:weighted-name",false);
    const auto referenceArguments=arguments("a portrait (embedding:weighted-name:1.25), embedding:sub/escaped\\(name\\),");
    const auto plainBefore=Json::parse(take(fixture.call("identity",plainArguments))),foreignBefore=Json::parse(take(fixture.call("identity",foreignArguments))),referencedBefore=Json::parse(take(fixture.call("identity",referenceArguments)));
    require(plainBefore.at("models").empty()&&foreignBefore.at("models").empty(),"An unreferenced embedding tree or a STRING without a CLIP encoder contract entered dependency identity.");
    require(referencedBefore.at("models").size()==2U,"A weighted/escaped CLIP embedding reference did not seal the complete installed embedding content inventory.");
    const auto modified=Fs::last_write_time(other);std::ofstream{other,std::ios::binary|std::ios::trunc}<<"other embedding";Fs::last_write_time(other,modified);
    const auto plainAfter=Json::parse(take(fixture.call("identity",plainArguments))),foreignAfter=Json::parse(take(fixture.call("identity",foreignArguments))),referencedAfter=Json::parse(take(fixture.call("identity",referenceArguments)));
    require(plainAfter.at("sha256")==plainBefore.at("sha256")&&foreignAfter.at("sha256")==foreignBefore.at("sha256"),"An embedding edit changed identity without an observed CLIP text reference.");
    require(referencedAfter.at("sha256")!=referencedBefore.at("sha256"),"Changed embedding bytes under an unchanged weighted/escaped CLIP prompt did not invalidate approval dependency identity.");
    std::ofstream{embeddings/L"new.pt",std::ios::binary}<<"new embedding";const auto added=Json::parse(take(fixture.call("identity",referenceArguments)));
    require(added.at("models").size()==3U&&added.at("sha256")!=referencedAfter.at("sha256"),"A newly available embedding was omitted from the referenced encoder's installed inventory seal.");
    require(server.prompts==0U&&!server.unexpectedRequest,"CLIP embedding dependency sealing submitted generation or used an unexpected provider route.");
}
void customNodeDependencyMetadataEditsInvalidateContentIdentity() {
    for(const auto* name:{L"setup.cfg",L"constraints.txt",L"requirements.in"}) {
        Fixture fixture{true};const auto directory=fixture.root/L"portable"/L"ComfyUI"/L"custom_nodes"/L"FixtureNode";Fs::create_directories(directory);const auto metadata=directory/name;
        std::ofstream{metadata,std::ios::binary}<<"fixture metadata 1";const auto modified=Fs::last_write_time(metadata);const auto before=Json::parse(take(fixture.call("identity",Json::object())));
        const auto manifest=NativeTools::Windows::ComfyDetail::readJson(Fs::path{take(Infrastructure::Windows::Detail::strictUtf8ToUtf16(before.at("manifest").at("path").get<std::string>()))});
        bool found=false;for(const auto& source:manifest.at("source_files"))if(source.at("path")==pathText(metadata).value())found=true;
        require(found,"Declared custom-node setup or included requirement metadata was omitted from the retained dependency source inventory.");
        std::ofstream{metadata,std::ios::binary|std::ios::trunc}<<"fixture metadata 2";Fs::last_write_time(metadata,modified);const auto changed=Json::parse(take(fixture.call("identity",Json::object())));
        require(changed.at("sha256")!=before.at("sha256")&&changed.at("source_file_count")==before.at("source_file_count"),"Changed custom-node dependency metadata with unchanged size and timestamp did not invalidate approval identity.");
    }
}
void identityUsesIndependentPhysicalYamlModelAndEmbeddingCategories() {
    Fixture fixture{true};const Json schemas{
        {"ModelSource",{{"input",{{"required",{{"weights",Json::array({Json::array({"fixture.weights"})})}}}}},{"output",Json::array({"CLIP"})},{"output_node",false}}},
        {"CLIPTextEncode",{{"input",{{"required",{{"text",Json::array({"STRING"})},{"clip",Json::array({"CLIP"})}}}}},{"output",Json::array({"CONDITIONING"})},{"output_node",false}}}};
    LoopbackArtifactServer server{{},false,200U,nullptr,0U,{},schemas};fixture.config.endpoint=server.endpoint();
    const auto checkpointRoot=fixture.root/L"shared-payload"/L"physical-weight-location",embeddingRoot=fixture.root/L"shared-payload"/L"physical-style-location";
    Fs::create_directories(checkpointRoot);Fs::create_directories(embeddingRoot/L"nested");const auto model=checkpointRoot/L"fixture.weights",embedding=embeddingRoot/L"nested"/L"style.pt";
    std::ofstream{model,std::ios::binary}<<"abc";std::ofstream{embedding,std::ios::binary}<<"first embedding";
    const auto manager=fixture.root/L"portable"/L"ComfyUI"/L"custom_nodes"/L"ComfyUI-Manager";Fs::create_directories(manager);
    NativeTools::Windows::ComfyDetail::writeJson(manager/L"model-list.json",{{"models",Json::array({{{"filename","fixture.weights"},{"save_path","checkpoints"},{"url","https://publisher.invalid/fixture.weights"}}})}});
    auto checkpoints=pathText(checkpointRoot).value(),embeddings=pathText(embeddingRoot).value();std::replace(checkpoints.begin(),checkpoints.end(),'\\','/');std::replace(embeddings.begin(),embeddings.end(),'\\','/');
    std::ofstream{fixture.root/L"portable"/L"ComfyUI"/L"extra_model_paths.yaml"}<<"mapped:\n    checkpoints: \""<<checkpoints<<"\"\n    embeddings: |\n        "<<embeddings<<"\n";
    fixture.sealCurrentOwner();const Json graph{{"1",{{"class_type","ModelSource"},{"inputs",{{"weights",{{"__value__","fixture.weights"}}}}}}},{"2",{{"class_type","CLIPTextEncode"},{"inputs",{{"clip",Json::array({"1",0})},{"text","embedding:nested/style"}}}}}};
    const Json args{{"workflows",Json::array({graph})}};const auto before=Json::parse(take(fixture.call("identity",args)));
    std::set<std::string> paths;for(const auto& value:before.at("models"))paths.insert(value.at("path").get<std::string>());
    require(paths==std::set<std::string>{pathText(model).value(),pathText(embedding).value()},"Independent YAML category paths were omitted or matched by physical folder basename rather than their declared model/embedding category.");
    const auto modified=Fs::last_write_time(embedding);std::ofstream{embedding,std::ios::binary|std::ios::trunc}<<"other embedding";Fs::last_write_time(embedding,modified);const auto changed=Json::parse(take(fixture.call("identity",args)));
    require(changed.at("sha256")!=before.at("sha256"),"Changed mapped embedding content with unchanged file metadata did not invalidate dependency identity.");
    require(!fixture.call("inspect",{{"path",pathText(model).value()}}),"Configured internal dependency discovery expanded the project's public file authority.");
    require(server.prompts==0U&&!server.unexpectedRequest,"Mapped model dependency identity submitted generation or used an unexpected provider route.");
}
void artifactBatchRejectsCanonicalDestinationAliasesBeforePublication() {
    const auto pixels=[](std::uint32_t width,std::uint32_t height,std::byte color){return NativeTools::Windows::Detail::ImageProviderPixels{width,height,std::vector<std::byte>(static_cast<std::size_t>(width)*height*4U,color)};};
    const auto first=take(NativeTools::Windows::Detail::encodeProviderImage(pixels(64U,64U,std::byte{42}),TestContext{}.active()));
    const auto second=take(NativeTools::Windows::Detail::encodeProviderImage(pixels(96U,32U,std::byte{96}),TestContext{}.active()));
    const std::string owned{"ForgeConductor/project/job/preview"};
    for(const auto variant:{0U,1U,2U,3U}) {
        Fixture fixture{true};LoopbackArtifactServer server{first,false,200U,nullptr,0U,second};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
        const auto secondName=variant==1U?"SHARED.PNG":variant==3U?"other.png":"shared.png";
        const Json primary{{"filename","shared.png"},{"subfolder",owned+"/first"},{"type","output"}};
        const Json additional=variant==2U?primary:Json{{"filename",secondName},{"subfolder",owned+"/second"},{"type","output"}};
        auto arguments=collectArguments(fixture,"shared.png");arguments["outputs"]["12"]={{"images",Json::array({primary,additional})}};
        const auto result=fixture.call("collect",arguments);
        if(variant==3U) {
            const auto collected=Json::parse(take(result));const auto& artifacts=collected.at("artifacts");
            require(collected.at("ok")==true && artifacts.size()==2U && artifacts[0].at("sha256")!=artifacts[1].at("sha256") &&
                artifacts[0].at("metadata").at("width")==64U && artifacts[1].at("metadata").at("width")==96U && artifacts[1].at("metadata").at("height")==32U,
                "Distinct destination control did not retain both actual provider contents and decoded metadata.");
            require(server.views==2U && Fs::exists(fixture.root/L"workspace"/L"12_shared.png") && Fs::exists(fixture.root/L"workspace"/L"12_other.png"),"Distinct file outputs were not both transferred and published.");
        } else {
            requireError(result,Domain::ErrorCodes::Conflict,"Same-name, case-alias or repeated descriptor destinations were silently coalesced during publication.");
            require(!Fs::exists(fixture.root/L"workspace"/L"12_shared.png") && !Fs::exists(fixture.root/L"workspace"/L"12_SHARED.PNG"),"A colliding artifact batch published a final filename before rejecting the alias.");
            require(server.views==1U,"Destination collision was not rejected before its second provider transfer.");
            for(const auto& item:Fs::directory_iterator(fixture.root/L"runtime"/L"transfers"))if(item.path().extension()==L".json"){
                const auto transfer=NativeTools::Windows::ComfyDetail::readJson(item.path());require(transfer.at("state")!="published","Rejected alias batch marked a transfer as published.");
            }
        }
        require(server.prompts==0U && !server.unexpectedRequest,"Artifact alias validation submitted a generation or contacted an unrelated provider route.");
    }
}
void mediaContainerMimeUsesObservedContentIdentities() {
    using NativeTools::Windows::ComfyDetail::mediaContentType;
    const auto decodeHex=[](std::string_view hex){std::vector<unsigned char> bytes;require(hex.size()%2U==0U,"Media prefix fixture hex is incomplete.");
        const auto nibble=[](char c){if(c>='0'&&c<='9')return static_cast<unsigned>(c-'0');if(c>='A'&&c<='F')return static_cast<unsigned>(c-'A'+10);throw TestFailure{"Invalid media prefix hex."};};
        for(std::size_t index=0U;index<hex.size();index+=2U)bytes.push_back(static_cast<unsigned char>((nibble(hex[index])<<4U)|nibble(hex[index+1U])));return bytes;};
    // Measured host prefixes/FFprobe identities from ffmpeg-container-contract-
    // 003d9ad0-8d1e-41a7-9bd0-c34d55937d9f.json; no inference in this test.
    const auto mkv=decodeHex("1A45DFA3A34286810142F7810142F2810442F381084282886D6174726F736B6142878104428581021853806701000000000019C2114D9B74C0BF847C95FC814D");
    const auto webm=decodeHex("1A45DFA39F4286810142F7810142F2810442F381084282847765626D4287810242858102185380670100000000001A0A114D9B74BA4DBB8B53AB841549A96653");
    const auto mov=decodeHex("00000014667479707174202000000200717420200000000877696465000017316D6461740000029F0605FFFF9BDC45E9BDE6D948B7962CD820D923EEEF783236");
    const auto mp4=decodeHex("000000206674797069736F6D0000020069736F6D69736F32617663316D7034310000000866726565000017316D6461740000029F0605FFFF9BDC45E9BDE6D948");
    const Json ebml{{"format_name","matroska,webm"},{"filename","misleading.mp4"}};
    const auto iso=[](const char* brand){return Json{{"format_name","mov,mp4,m4a,3gp,3g2,mj2"},{"filename","misleading.webm"},{"tags",{{"major_brand",brand}}}};};
    require(mediaContentType(mkv,ebml,true)=="video/x-matroska" && mediaContentType(webm,ebml,true)=="video/webm","Shared Matroska/WebM demuxer or misleading filename overrode the actual EBML DocType.");
    require(mediaContentType(mov,iso("qt  "),true)=="video/quicktime" && mediaContentType(mp4,iso("isom"),true)=="video/mp4","Shared MOV/MP4 demuxer overrode the actual major brand.");
    require(mediaContentType(mkv,ebml,false)=="audio/x-matroska" && mediaContentType(webm,ebml,false)=="audio/webm" && mediaContentType(mp4,iso("isom"),false)=="audio/mp4","Audio-only stream classification lost its measured container identity.");
    const std::span<const unsigned char> empty;
    require(mediaContentType(empty,iso("3gp6"),true)=="video/3gpp" && mediaContentType(empty,iso("3g2a"),true)=="video/3gpp2" && mediaContentType(empty,iso("3gp6"),false)=="audio/3gpp" && mediaContentType(empty,iso("3g2a"),false)=="audio/3gpp2","Identified registered 3GPP/3GPP2 brands were collapsed into MP4.");
    const auto rejected=[&](std::span<const unsigned char> bytes,const Json& format){bool failed=false;try{static_cast<void>(mediaContentType(bytes,format,true));}
        catch(const NativeTools::Windows::ComfyDetail::Failure& failure){failed=true;require(failure.error.code==Domain::ErrorCodes::IntegrityFailure,"Ambiguous media produced an unrelated error.");}
        require(failed,"Unrecognized, malformed or ambiguous container content received a guessed MIME type.");};
    rejected(std::span{mkv}.first(32U),ebml);rejected(std::span{webm}.first(32U),ebml);rejected(mp4,iso("qt  "));
    rejected(empty,iso("xxxx"));rejected(empty,{{"format_name","mov,mp4"}});rejected(empty,{{"format_name","unknownwebm"}});
    for(const auto* hex:{"1A45DFA3FF","1A45DFA300","1A45DFA386428283666F6F","1A45DFA38E4282847765626D4282847765626D","1A45DFA382EC804282847765626D","1A45DFA3874282857765626D","1A45DFA38700847765626D"})rejected(decodeHex(hex),ebml);
    const auto multiByte=decodeHex("1A45DFA34008428240047765626D");require(mediaContentType(multiByte,ebml,true)=="video/webm","Bounded EBML VINT sizes with multiple bytes were not parsed faithfully.");
    std::vector<unsigned char> delayed{0x1aU,0x45U,0xdfU,0xa3U,0xd9U,0xecU,0xd0U};delayed.insert(delayed.end(),80U,0U);const auto docType=decodeHex("4282847765626D");delayed.insert(delayed.end(),docType.begin(),docType.end());
    require(mediaContentType(delayed,ebml,true)=="video/webm","DocType beyond the old 32-byte prefix was not inspected.");
    rejected(std::span{delayed}.first(64U),ebml);auto oversized=delayed;oversized[4]=0x48U;oversized.insert(oversized.begin()+5,0x00U);oversized.resize(2048U,0U);rejected(oversized,ebml);
}
void inspectHashesAndRejectsOutsideScope() {
    Fixture fixture;const auto source=fixture.root/L"sample.bin";std::ofstream{source,std::ios::binary} << "abc";
    const auto facts=Json::parse(take(fixture.call("inspect",{{"path",pathText(source).value()}})));
    require(facts.at("bytes")==3U,"Binary inspection lost byte count.");
    require(facts.at("sha256")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","Inspection hash differs from pinned bytes.");
    require(!fixture.call("inspect",{{"path",pathText(fixture.root.parent_path()/L"outside.bin").value()}}),"Inspection escaped project authority.");
}
void independentFixturesReserveDistinctUnavailableEndpoints() {
    Fixture first,second;require(first.config.endpoint!=second.config.endpoint,"Independent fixtures retained the same provider port and lease.");
    auto context=TestContext{}.active();context.deadline=std::chrono::steady_clock::now()+std::chrono::seconds{5};
    for(auto* fixture:{&first,&second}){
        const auto status=Json::parse(take(fixture->call("status",Json::object(),context)));
        require(status.at("endpoint")==fixture->config.endpoint&&status.at("available")==false&&status.at("managed")==false&&!status.contains("owner"),
            "A bound non-listening fixture reservation claimed provider availability or process ownership.");
    }
}
void apiWorkflowPatchesAndExportConflicts() {
    Fixture fixture;Json graph{{"1",{{"class_type","ExampleNode"},{"inputs",{{"text","first"}}}}}};
    const auto changed=Json::parse(take(fixture.call("workflow",{{"action","modify"},{"workflow",graph},{"patches",Json::array({{{"node_id","1"},{"input","text"},{"value","second"}}})}})));
    require(changed.at("workflow").at("1").at("inputs").at("text")=="second","Typed API workflow patch was lost.");
    const auto output=pathText(fixture.root/L"export.json").value();
    static_cast<void>(take(fixture.call("workflow",{{"action","export"},{"workflow",graph},{"path",output}})));
    requireError(fixture.call("workflow",{{"action","export"},{"workflow",graph},{"path",output}}),Domain::ErrorCodes::Conflict,"Workflow export replaced an existing owner file.");
    require(!fixture.call("workflow",{{"action","modify"},{"workflow",graph},{"patches",Json::array({{{"node_id","1"},{"input","absent"},{"value",3}}})}}),"Patch inserted an absent input.");
}
void invalidOperationsAndShutdownRejectBeforeEffects() {
    Fixture fixture;
    requireError(fixture.call("request",{{"method","POST"},{"route","/interrupt"},{"body",Json::object()}}),Domain::ErrorCodes::InvalidRequest,"Global provider control was accepted.");
    requireError(fixture.call("control",{{"action","start"}}),Domain::ErrorCodes::Unauthorized,"Runtime launch ignored shell grant.");
    fixture.config.enabled=false;requireError(fixture.call("workflow",{{"action","inspect"},{"workflow",Json::object()}}),Domain::ErrorCodes::HostCapabilityUnavailable,"Disabled backend accepted a workflow.");
    fixture.config.enabled=true;fixture.backend->shutdown();requireError(fixture.call("inspect",{{"path",pathText(fixture.root/L"sample.bin").value()}}),Domain::ErrorCodes::HostCapabilityUnavailable,"Stopped backend performed file work.");
}
void starterCatalogPathsImportExactPreviewAndFinalGraphs() {
    Fixture fixture;const auto result=Json::parse(take(fixture.call("catalog",{{"kind","templates"}})));require(result.at("starters").at("workflows").size()==3U,"Staged starter catalog is incomplete.");std::array<wchar_t,32768> image{};const auto size=GetModuleFileNameW(nullptr,image.data(),static_cast<DWORD>(image.size()));require(size>0U&&size<image.size(),"Starter fixture cannot locate its staged resource manifest.");const auto resources=Fs::path{image.data()}.parent_path()/L"Resources"/L"ComfyUI";auto expected=NativeTools::Windows::ComfyDetail::readJson(resources/L"starter_manifest.json");
    for(auto& starter:expected.at("workflows"))for(const auto* field:{"preview_file","final_file"}){const auto source=resources/Fs::path{starter.at(field).get<std::string>()};starter[field]=pathText(source).value();const auto imported=Json::parse(take(fixture.call("workflow",{{"action","import"},{"path",starter.at(field)}})));require(Fs::path{starter.at(field).get<std::string>()}.is_absolute()&&imported.at("format")=="api"&&imported.at("workflow")==NativeTools::Windows::ComfyDetail::readJson(source),"Catalog starter path did not import its exact preview/final graph.");}
    require(result.at("starters")==expected,"Starter catalog absolute-path promotion changed policy, parameters, output or qualification fields.");require(!Fs::exists(fixture.root/L"runtime"/L"runtime.log"),"Starter catalog import launched media inference.");
}
void permitPreparation(Fixture& fixture) {
    fixture.backend->shutdown();fixture.backend.reset();
    fixture.issuer=std::make_unique<Infrastructure::Windows::WindowsWorkspaceAuthority>(std::vector<Infrastructure::Windows::WindowsWorkspaceAuthorityPolicy>{{
        parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000001"),fixture.project,parse<Domain::ClientId>("forge-conductor-manager"),
        {pathText(fixture.root)},Domain::FileAccess::Write,{Domain::FileAccess::Read,Domain::FileAccess::Write,Domain::FileAccess::Create,Domain::FileAccess::Execute},{},true,1U}});
    fixture.backend=std::make_unique<NativeTools::Windows::WindowsComfyUiBackend>(*fixture.issuer,pathText(fixture.root/L"runtime"));
    std::array<wchar_t,32768> image{};const auto size=GetModuleFileNameW(nullptr,image.data(),static_cast<DWORD>(image.size()));require(size>0U&&size<image.size(),"Preparation fixture cannot identify its native executable.");
    Fs::create_directories(fixture.root/L"portable"/L"python_embeded");Fs::copy_file(Fs::path{image.data()},fixture.root/L"portable"/L"python_embeded"/L"python.exe");
}
void rebindFixtureProviderAfterPreparation(Fixture& fixture) {
    using namespace NativeTools::Windows::ComfyDetail;
    const auto owner=readJson(fixture.root/L"runtime"/L"runtime-owner.json");auto environment=readJson(fixture.root/L"runtime"/L"active-environment.json");
    require(environment.at("python")==pathText(fixture.root/L"portable"/L"python_embeded"/L"python.exe").value()&&environment.at("python")!=owner.at("python"),"Preparation did not publish its actual baseline interpreter independently of the fixture's in-process provider.");
    const auto staleInterpreter=fixture.call("identity",{{"workflows",Json::array()}});
    requireError(staleInterpreter,Domain::ErrorCodes::Conflict,"Dependency identity ignored the preparation interpreter differing from the actual fixture provider image.");
    require(staleInterpreter.error().message=="Live ComfyUI runtime does not match the selected installation, interpreter, model storage, or private media directories.","Post-preparation fixture failed at an unrelated identity boundary.");
    // The loopback provider is this process; its package-contract child is the
    // copied executable. Preserve preparation metadata while rebinding only
    // the fixture's selected interpreter to its actual live image.
    environment["python"]=owner.at("python");writeJson(fixture.root/L"runtime"/L"active-environment.json",environment);
}
Fs::path preparationDirectory(const Fixture& fixture,const Domain::OperationContext& context) {
    const auto operation=take(Infrastructure::Windows::Detail::strictUtf8ToUtf16(context.operationId.value()));
    const auto directory=fixture.root/L"runtime"/L"preparations"/operation;Fs::create_directories(directory);return directory;
}
Json interruptedPreparation(const Fixture& fixture,const Domain::OperationContext& context) {
    return {{"schema_version",1},{"operation_id",context.operationId.value()},{"installation",pathText(fixture.root/L"portable"/L"ComfyUI").value()},
        {"state","preparing"},{"download_budget_bytes",fixture.config.downloadBudgetBytes},{"downloaded_bytes",0ULL},{"downloads",Json::array()},{"installed",Json::array()},{"rollback",Json::array()}};
}
void preparationResumeChargesRetainedPartialBytes() {
    Fixture fixture;permitPreparation(fixture);fixture.config.downloadBudgetBytes=100ULL;const auto context=TestContext{}.active();const auto directory=preparationDirectory(fixture,context);
    const auto partial=directory/L"interrupted.download";std::ofstream{partial,std::ios::binary}<<std::string(101U,'x');
    auto receipt=interruptedPreparation(fixture,context);receipt["downloaded_bytes"]=80ULL;receipt["downloads"].push_back({{"path",pathText(partial).value()},{"received_bytes",80ULL}});
    const auto modelDirectory=fixture.root/L"portable"/L"ComfyUI"/L"models"/L"checkpoints";Fs::create_directories(modelDirectory);const auto copy=modelDirectory/L"forge-copy-interrupted.disabled";std::ofstream{copy,std::ios::binary}<<std::string(27U,'c');
    receipt["activations"]=Json::array({{{"target",pathText(modelDirectory/L"unpublished.safetensors").value()},{"sha256",std::string(64U,'a')},{"state","planned"},{"staging_file",pathText(copy).value()},{"copied_bytes",0ULL}}});
    std::ofstream{directory/L"manifest.json"}<<receipt.dump();
    const auto result=Json::parse(take(fixture.call("prepare",Json::object(),context)));
    require(result.at("ok")==false&&result.at("error").at("code").get<std::string>()==Domain::ErrorCodes::LimitExceeded,"Resumed preparation ignored cumulative partial transfer bytes.");
    require(result.at("downloaded_bytes")==101ULL&&result.at("manifest").at("prior_attempts").size()==1U,"Resumed byte accounting replaced previous transfer evidence.");
    require(Fs::file_size(partial)==101ULL,"Over-budget resume discarded the interrupted transfer evidence.");
    require(result.at("manifest").at("activations").at(0).at("copied_bytes")==27ULL&&Fs::file_size(copy)==27ULL&&!Fs::exists(modelDirectory/L"unpublished.safetensors"),"Recovery did not measure retained hard-stop copy progress or published inactive staging.");
}
void preparationResumeCannotResetBudgetOrEscapeLedger() {
    Fixture fixture;permitPreparation(fixture);const auto context=TestContext{}.active();const auto directory=preparationDirectory(fixture,context);auto receipt=interruptedPreparation(fixture,context);
    receipt["download_budget_bytes"]=fixture.config.downloadBudgetBytes-1ULL;std::ofstream{directory/L"manifest.json"}<<receipt.dump();
    requireError(fixture.call("prepare",Json::object(),context),Domain::ErrorCodes::Conflict,"Resume changed the saved preparation budget.");
    receipt["download_budget_bytes"]=fixture.config.downloadBudgetBytes;receipt["downloads"].push_back({{"path",pathText(fixture.root/L"outside-transaction.download").value()},{"received_bytes",0ULL}});std::ofstream{directory/L"manifest.json"}<<receipt.dump();
    requireError(fixture.call("prepare",Json::object(),context),Domain::ErrorCodes::IntegrityFailure,"Resume trusted a download outside its owned transaction.");
}
void preparationRejectsTraversalBeforePublication() {
    Fixture fixture;permitPreparation(fixture);const auto context=TestContext{}.active();
    const Json dependency{{"kind","model"},{"url","https://example.com/model"},{"target","../escaped.safetensors"},{"sha256",std::string(64U,'a')}};
    const auto result=Json::parse(take(fixture.call("prepare",{{"dependencies",Json::array({dependency})}},context)));
    require(result.at("ok")==false&&result.at("error").at("code").get<std::string>()==Domain::ErrorCodes::InvalidRequest,"Preparation accepted an escaping dependency destination.");
    require(result.at("downloaded_bytes")==0ULL&&!Fs::exists(fixture.root/L"portable"/L"ComfyUI"/L"escaped.safetensors"),"Rejected dependency started a transfer or published outside models.");
}
void preparationReusesModelsAndRetainsBaselineOnReadinessFailure() {
    Fixture fixture;permitPreparation(fixture);const auto models=fixture.root/L"portable"/L"ComfyUI"/L"models"/L"checkpoints";Fs::create_directories(models);const auto model=models/L"existing.safetensors";std::ofstream{model,std::ios::binary}<<"abc";
    const Json dependency{{"kind","model"},{"url","https://example.com/unused"},{"target","checkpoints/existing.safetensors"},{"sha256","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}};
    const auto result=Json::parse(take(fixture.call("prepare",{{"dependencies",Json::array({dependency})}})));
    require(result.at("ok")==false&&result.at("downloaded_bytes")==0ULL,"Reused model readiness test transferred a replacement.");
    require(result.at("manifest").at("installed").size()==1U&&result.at("manifest").at("installed")[0].at("reused")==true,"Existing model reuse was not recorded.");
    require(Fs::is_regular_file(model)&&Fs::file_size(model)==3ULL,"Failed readiness removed a baseline model.");
}
void preparationChecksWrappedModelLiteralsAgainstLiveEnums() {
    for(const auto* scenario:{"installed","missing_publisher","unresolved_readiness"}){
        Fixture fixture;permitPreparation(fixture);const auto context=TestContext{}.active();
        const Json schemas{{"ModelSource",{{"input",{{"required",{{"ckpt_name",Json::array({Json::array({"installed.safetensors"})})}}}}}}}};
        LoopbackArtifactServer server{{},false,200U,{{"queue_running",Json::array()},{"queue_pending",Json::array({Json::array({1,"unrelated",Json::object()})})}},0U,{},schemas};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
        const std::string selected=std::string_view{scenario}=="installed"?"installed.safetensors":"missing.safetensors";
        const Json graph{{"1",{{"class_type","ModelSource"},{"inputs",{{"ckpt_name",{{"__value__",selected}}}}}}}};const Json arguments{{"workflow",graph}};
        if(std::string_view{scenario}=="unresolved_readiness"){auto receipt=interruptedPreparation(fixture,context);receipt["resolution"]={{"dependencies",Json::array()},{"requirements",Json::array()}};std::ofstream{preparationDirectory(fixture,context)/L"manifest.json"}<<receipt.dump();}
        const auto result=Json::parse(take(fixture.call("prepare",arguments,context)));require(result.at("downloaded_bytes")==0ULL&&server.prompts==0U&&!server.unexpectedRequest,"Wrapped model validation downloaded, submitted, or controlled unrelated provider work.");
        if(std::string_view{scenario}=="installed")require(result.at("ok")==true&&result.at("manifest").at("resolution").at("dependencies").empty()&&result.at("manifest").at("unresolved").empty(),"Wrapped installed enum was mistaken for a missing dependency.");
        else if(std::string_view{scenario}=="missing_publisher")require(result.at("ok")==false&&result.at("error").at("message").get<std::string>().find("Missing model missing.safetensors")!=std::string::npos,"Wrapped missing model bypassed publisher discovery.");
        else require(result.at("ok")==false&&result.at("manifest").at("unresolved").size()==1U&&result.at("manifest").at("unresolved")[0].at("input")=="ckpt_name","Wrapped missing enum bypassed post-preparation readiness.");
    }
}
void disabledAutomaticSetupRejectsMissingGraphDependenciesBeforePublisherMetadata() {
    const auto previousSize=GetEnvironmentVariableW(L"PATH",nullptr,0U);std::wstring previous(previousSize,L'\0');if(previousSize>0U){const auto copied=GetEnvironmentVariableW(L"PATH",previous.data(),previousSize);require(copied<previousSize,"Disabled-setup fixture cannot retain PATH.");previous.resize(copied);}
    struct RestorePath final{std::wstring previous;bool exists;~RestorePath(){SetEnvironmentVariableW(L"PATH",exists?previous.c_str():nullptr);}}restore{previous,previousSize>0U};
    for(const auto* scenario:{"installed","missing_node","missing_model","missing_media"}){
        Fixture fixture;permitPreparation(fixture);fixture.config.automaticSetup=false;const auto context=TestContext{}.active();require(SetEnvironmentVariableW(L"PATH",fixture.root.c_str())!=FALSE,"Disabled-setup fixture cannot isolate media component discovery.");
        const Json schemas{{"ModelSource",{{"input",{{"required",{{"ckpt_name",Json::array({Json::array({"installed.safetensors"})})}}}}}}}};
        LoopbackArtifactServer server{{},false,200U,{{"queue_running",Json::array()},{"queue_pending",Json::array({Json::array({1,"unrelated",Json::object()})})}},0U,{},schemas};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
        const auto manager=fixture.root/L"portable"/L"ComfyUI"/L"custom_nodes"/L"ComfyUI-Manager";Fs::create_directory(manager);const Json catalog{{"models",Json::array({{{"filename","missing.safetensors"},{"url","https://huggingface.co/fixture/never-contacted/resolve/main/missing.safetensors"},{"save_path","checkpoints"}}})}};std::ofstream{manager/L"model-list.json"}<<catalog.dump();
        const std::string name=std::string_view{scenario}=="missing_node"?"UninstalledFixtureNode":"ModelSource",selected=std::string_view{scenario}=="missing_model"?"missing.safetensors":"installed.safetensors";
        Json arguments{{"workflow",{{"1",{{"class_type",name},{"inputs",{{"ckpt_name",{{"__value__",selected}}}}}}}}}};if(std::string_view{scenario}=="missing_media")arguments["media_inspection_required"]=true;
        const auto result=Json::parse(take(fixture.call("prepare",arguments,context)));const auto& manifest=result.at("manifest");require(result.at("downloaded_bytes")==0ULL&&manifest.at("downloads").empty()&&manifest.at("metadata").empty()&&manifest.at("installed").empty()&&!manifest.contains("activations")&&server.prompts==0U&&!server.unexpectedRequest,"Disabled setup downloaded publisher metadata or mutated dependency/provider state.");
        std::ifstream retainedCatalog{manager/L"model-list.json"};const auto observed=Json::parse(retainedCatalog);retainedCatalog.close();require(observed==catalog,"Disabled setup changed the installed publisher catalog.");
        if(std::string_view{scenario}=="installed")require(result.at("ok")==true,"Disabled setup rejected compatible installed node/model enum reuse.");
        else require(result.at("ok")==false&&result.at("error").at("code").get<std::string>()==Domain::ErrorCodes::Unauthorized&&result.at("error").at("message").get<std::string>().find("before publisher discovery")!=std::string::npos,"Disabled setup reached an external publisher or an installation phase for missing graph dependencies.");
    }
}
void interruptedActivationRollbackRestoresSearchPath() {
    Fixture fixture;permitPreparation(fixture);const auto context=TestContext{}.active();const auto directory=preparationDirectory(fixture,context);
    const auto models=fixture.root/L"portable"/L"ComfyUI"/L"models"/L"checkpoints";Fs::create_directories(models);const auto added=models/L"new.safetensors";std::ofstream{added,std::ios::binary}<<"abc";
    const auto pth=fixture.root/L"portable"/L"python_embeded"/L"python313._pth";const std::string original{"python313.zip\r\n.\r\nimport site\r\n"},replacement{"interrupted-overlay\r\n"+original};std::ofstream{pth,std::ios::binary}<<replacement;
    auto receipt=interruptedPreparation(fixture,context);receipt["installed"].push_back({{"target",pathText(added).value()},{"kind","model"},{"sha256","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}});
    receipt["python_path_backup"]={{"path",pathText(pth).value()},{"original",original},{"replacement",replacement}};std::ofstream{directory/L"manifest.json"}<<receipt.dump();
    const auto result=Json::parse(take(fixture.call("prepare",Json::object(),context)));require(result.at("ok")==false,"Fixture provider unexpectedly passed readiness.");
    std::ifstream restored{pth,std::ios::binary};const std::string bytes{std::istreambuf_iterator<char>{restored},std::istreambuf_iterator<char>{}};require(bytes==original,"Interrupted overlay activation did not restore the exact prior search path.");
    require(!Fs::exists(added),"Failed preparation left its newly activated model in the live model directory.");bool retained=false;
    for(const auto& entry:result.at("manifest").at("rollback"))if(entry.contains("retained")){const auto path=Fs::path{entry.at("retained").get<std::string>()};retained=retained||(Fs::is_regular_file(path)&&Fs::file_size(path)==3ULL);}
    require(retained,"Rollback discarded this operation's newly activated dependency evidence.");
}
void preparationRejectsChangedInterruptedActivation() {
    Fixture fixture;permitPreparation(fixture);const auto context=TestContext{}.active();const auto directory=preparationDirectory(fixture,context);
    const auto models=fixture.root/L"portable"/L"ComfyUI"/L"models"/L"checkpoints";Fs::create_directories(models);const auto edited=models/L"edited.safetensors";std::ofstream{edited,std::ios::binary}<<"external edit";
    auto receipt=interruptedPreparation(fixture,context);receipt["installed"].push_back({{"target",pathText(edited).value()},{"kind","model"},{"sha256","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}});std::ofstream{directory/L"manifest.json"}<<receipt.dump();
    requireError(fixture.call("prepare",Json::object(),context),Domain::ErrorCodes::IntegrityFailure,"Preparation reconciled an externally edited interrupted activation as its owned file.");
    require(Fs::is_regular_file(edited)&&Fs::file_size(edited)==13ULL,"Recovery moved or removed an externally edited activation.");
}
void preparationEnforcesFreeSpaceBeforeStaging() {
    Fixture fixture;permitPreparation(fixture);ULARGE_INTEGER free{},total{},available{};require(GetDiskFreeSpaceExW(fixture.root.c_str(),&free,&total,&available)!=FALSE,"Fixture free-space measurement failed.");
    constexpr std::uint64_t maximumReserve=10000000000000ULL;
    if(free.QuadPart>=maximumReserve) return;
    fixture.config.freeSpaceReserveBytes=free.QuadPart+1ULL;
    const auto result=Json::parse(take(fixture.call("prepare",Json::object())));
    require(result.at("ok")==false&&result.at("error").at("code").get<std::string>()==Domain::ErrorCodes::LimitExceeded,"Preparation staged files below its saved free-space reserve.");
    require(result.at("downloaded_bytes")==0ULL,"Insufficient free-space preparation attempted a dependency transfer.");
}
void nativeTransferCountsErrorBodiesAndAcceptsHttpDependencies() {
    using namespace NativeTools::Windows::ComfyDetail;
    const std::vector<std::byte> payload(512U*1024U,std::byte{42});LoopbackArtifactServer server{payload,false,500U};std::uint64_t accounted{};
    const auto response=http(server.endpoint()+"/view?test", "GET", "", "", 1024U*1024U,TestContext{}.active(),nullptr,0ULL,[&](std::uint64_t count){accounted+=count;});
    require(response.status==500U&&accounted==payload.size()&&response.bytes==payload.size(),"Native accounting omitted a failed dependency response body.");
    accounted=0ULL;const auto downloadedError=download(server.endpoint()+"/view?test",1024U*1024U,TestContext{}.active(),nullptr,0ULL,[&](std::uint64_t count){accounted+=count;});
    require(downloadedError.status==500U&&accounted==payload.size()&&downloadedError.bytes==payload.size()&&server.views==2U&&!server.unexpectedRequest,"HTTP dependency download omitted its actual failed response or byte accounting.");
    LoopbackArtifactServer success{payload,false};accounted=0ULL;
    const auto downloaded=download(success.endpoint()+"/view?test",1024U*1024U,TestContext{}.active(),nullptr,0ULL,[&](std::uint64_t count){accounted+=count;});
    require(downloaded.status==200U&&downloaded.body==std::string(payload.size(),'*')&&downloaded.bytes==payload.size()&&accounted==payload.size()&&success.views==1U&&!success.unexpectedRequest,"HTTP dependency download did not return and account for the exact source bytes.");
    for(const auto* scheme:{"ftp://","file://","gopher://"}){
        bool rejected=false;try{static_cast<void>(download(std::string{scheme}+"127.0.0.1/view?test",1024U*1024U,TestContext{}.active(),nullptr,0ULL));}
            catch(const Failure& error){rejected=error.error.code==Domain::ErrorCodes::InvalidRequest;}
        require(rejected&&success.views==1U,"Dependency download admitted an unsupported URL scheme.");
    }
}
void nativeHttpDependencyRedirectsPreserveAccountingAndBounds() {
    using namespace NativeTools::Windows::ComfyDetail;
    const std::vector<std::byte> payload(32U,std::byte{42});LoopbackArtifactServer target{payload,false};
    for(const auto& location:{target.endpoint()+"/view?final",target.endpoint().substr(5U)+"/view?final"}){
        const auto prior=target.views.load();LoopbackArtifactServer source{payload,false,200U,nullptr,0U,{},Json::object(),{location}};std::uint64_t accounted{};
        const auto result=download(source.endpoint()+"/view?redirect",1024U,TestContext{}.active(),nullptr,0ULL,[&](std::uint64_t count){accounted+=count;});
        require(result.status==200U&&result.body==std::string(payload.size(),'*')&&accounted==2ULL*payload.size()&&source.views==1U&&target.views==prior+1U&&!source.unexpectedRequest&&!target.unexpectedRequest,"Absolute or scheme-relative HTTP redirect lost its protocol or received-byte accounting.");
    }
    {
        LoopbackArtifactServer source{payload,false,200U,nullptr,0U,{},Json::object(),{"/view?final"}};std::uint64_t accounted{};
        const auto result=download(source.endpoint()+"/view?redirect",1024U,TestContext{}.active(),nullptr,0ULL,[&](std::uint64_t count){accounted+=count;});
        require(result.status==200U&&result.body==std::string(payload.size(),'*')&&accounted==2ULL*payload.size()&&source.views==2U&&!source.unexpectedRequest,"Origin-path HTTP redirect changed the provider authority or omitted received bytes.");
    }
    for(const auto* variation:{"invalid_scheme","missing_location","loop","limit"}){
        const std::string_view mode{variation};std::vector<std::string> redirects;
        if(mode=="invalid_scheme")redirects={"ftp://127.0.0.1/view?invalid"};
        else if(mode=="loop")redirects={"/view?start"};
        else if(mode=="limit")for(unsigned index=1U;index<=6U;++index)redirects.push_back("/view?step="+std::to_string(index));
        LoopbackArtifactServer source{payload,false,mode=="missing_location"?302U:200U,nullptr,0U,{},Json::object(),std::move(redirects)};
        const auto expected=mode=="invalid_scheme"?Domain::ErrorCodes::InvalidRequest:mode=="missing_location"?Domain::ErrorCodes::MalformedMessage:mode=="loop"?Domain::ErrorCodes::Conflict:Domain::ErrorCodes::LimitExceeded;
        const auto requests=mode=="limit"?6U:1U;std::uint64_t accounted{};bool rejected=false;
        try{static_cast<void>(download(source.endpoint()+"/view?start",1024U,TestContext{}.active(),nullptr,0ULL,[&](std::uint64_t count){accounted+=count;}));}
            catch(const Failure& error){rejected=error.error.code==expected;}
        require(rejected&&source.views==requests&&accounted==requests*payload.size()&&!source.unexpectedRequest,"HTTP dependency redirect failure changed its bound, error or cumulative received-byte accounting.");
    }
}
void nativeTransferCountsTruncatedAndOverBudgetBodies() {
    using namespace NativeTools::Windows::ComfyDetail;
    const std::vector<std::byte> payload(512U*1024U,std::byte{42});LoopbackArtifactServer server{payload,true};std::uint64_t accounted{};bool truncated=false;
    try{static_cast<void>(http(server.endpoint()+"/view?test","GET","","",1024U*1024U,TestContext{}.active(),nullptr,0ULL,[&](std::uint64_t count){accounted+=count;}));}
        catch(const Failure& error){truncated=error.error.code==Domain::ErrorCodes::IntegrityFailure;}
    require(truncated&&accounted==payload.size()/2U,"Truncated transfer did not retain its actual received byte accounting.");
    accounted=0ULL;bool overBudget=false;
    try{static_cast<void>(http(server.endpoint()+"/view?test","GET","","",32U,TestContext{}.active(),nullptr,0ULL,[&](std::uint64_t count){accounted+=count;}));}
        catch(const Failure& error){overBudget=error.error.code==Domain::ErrorCodes::PayloadTooLarge;}
    require(overBudget&&accounted>32ULL,"Native response limit omitted the received chunk that exceeded its budget.");
}
Json receiveReceipt(std::uint64_t budget,std::string_view type="model") {
    return {{"download_budget_bytes",budget},{"downloaded_bytes",0ULL},{"uncertain_download_bytes",0ULL},{"receive_accounting_version",1U},
        {"downloads",Json::array({{{"type",type},{"state","downloading"},{"received_bytes",0ULL}}})}};
}
void nativeReceiveAdmissionsBoundErrorBodiesAndExactBudget() {
    using namespace NativeTools::Windows::ComfyDetail;
    const std::vector<std::byte> payload(512U*1024U,std::byte{42});LoopbackArtifactServer server{payload,false,500U};
    Fixture fixture;const auto path=fixture.root/L"native-receive-admission.json";auto manifest=receiveReceipt(payload.size(),"metadata");Json durable;std::uint64_t measured{},largestRead{};
    const auto response=http(server.endpoint()+"/view?test","GET","","",1024U*1024U,TestContext{}.active(),nullptr,0ULL,
        [&](std::uint64_t bytes){require(!durable.is_null()&&preparationDownloadCharge(durable)>=measured+bytes,"Native error bytes arrived before their durable admission.");recordPreparationReceive(manifest,0U,bytes);measured+=bytes;largestRead=(std::max)(largestRead,bytes);},
        nullptr,0ULL,{},{},[&](std::uint64_t requested){const bool fresh=!manifest.contains("receive_admission")||manifest.at("receive_admission").at("remaining_bytes")==0ULL;
            const auto admitted=admitPreparationReceive(manifest,0U,requested);if(fresh){writeJson(path,manifest);durable=readJson(path);}return admitted;});
    finishPreparationReceive(manifest,true);writeJson(path,manifest);require(readJson(path)==manifest,"Completed native receive totals did not replace their durable admission.");
    require(response.status==500U&&response.bytes==payload.size()&&manifest.at("downloaded_bytes")==payload.size()&&manifest.at("uncertain_download_bytes")==0ULL&&preparationDownloadCharge(manifest)==payload.size()&&!manifest.contains("receive_admission")&&largestRead<=65536ULL,"Completed native error response lost actual totals or charged an unnecessary EOF admission.");
    auto limited=receiveReceipt(31ULL);measured=0ULL;bool rejected=false;
    try{static_cast<void>(http(server.endpoint()+"/view?test","GET","","",1024U*1024U,TestContext{}.active(),nullptr,0ULL,
        [&](std::uint64_t bytes){recordPreparationReceive(limited,0U,bytes);measured+=bytes;},nullptr,0ULL,{},{},
        [&](std::uint64_t requested){return admitPreparationReceive(limited,0U,requested);}));}
    catch(const Failure& error){rejected=error.error.code==Domain::ErrorCodes::LimitExceeded;}
    finishPreparationReceive(limited,false);require(rejected&&measured==31ULL&&preparationDownloadCharge(limited)==31ULL,"Native preparation read exceeded its remaining saved budget.");
}
void preparationReceiveResumeRetainsUncertainAndMeasuredBytesOnce() {
    using namespace NativeTools::Windows::ComfyDetail;
    auto windows=receiveReceipt(500'000'000'000ULL);require(admitPreparationReceive(windows,0U,65536ULL)==65536ULL&&preparationDownloadCharge(windows)==16ULL*1024ULL*1024ULL,"Native preparation admitted an unbounded receive window.");
    for(unsigned chunk=0U;chunk<256U;++chunk)recordPreparationReceive(windows,0U,65536ULL);
    require(admitPreparationReceive(windows,0U,65536ULL)==65536ULL&&preparationDownloadCharge(windows)==32ULL*1024ULL*1024ULL,"Next preparation window refunded already received bytes.");
    finishPreparationReceive(windows,false);require(windows.at("downloaded_bytes")==16ULL*1024ULL*1024ULL&&windows.at("uncertain_download_bytes")==16ULL*1024ULL*1024ULL,"Unresolved next-window admission replaced measured progress.");
    Fixture fixture;const auto context=TestContext{}.active();const auto path=fixture.root/L"durable-receive.json";
    auto inMemory=receiveReceipt(1024ULL*1024ULL);static_cast<void>(admitPreparationReceive(inMemory,0U,65536ULL));writeJson(path,inMemory);
    // Discard unsaved progress, as happens after termination between receive and ledger flush.
    recordPreparationReceive(inMemory,0U,65536ULL);auto afterStop=readJson(path);reconcilePreparationReceives(afterStop,"preparing",std::array<std::uint64_t,1>{65536ULL});
    require(afterStop.at("downloaded_bytes")==65536ULL&&afterStop.at("uncertain_download_bytes")==1024ULL*1024ULL-65536ULL&&preparationDownloadCharge(afterStop)==1024ULL*1024ULL&&!afterStop.contains("receive_admission"),"Resume double-counted a retained staging prefix or refunded unconfirmed error-body bytes.");
    writeJson(path,afterStop);auto secondResume=readJson(path);reconcilePreparationReceives(secondResume,"failed",std::array<std::uint64_t,1>{65536ULL});
    require(secondResume.at("downloaded_bytes")==65536ULL&&preparationDownloadCharge(secondResume)==1024ULL*1024ULL&&secondResume.at("receive_uncertainties").size()==1U,"Repeated recovery charged the same receive admission again.");
    bool exhausted=false;try{static_cast<void>(admitPreparationReceive(secondResume,0U,1ULL));}catch(const Failure& error){exhausted=error.error.code==Domain::ErrorCodes::LimitExceeded;}require(exhausted,"Resume permitted another native read after its saved admission exhausted the budget.");
    auto errorBody=receiveReceipt(512ULL);static_cast<void>(admitPreparationReceive(errorBody,0U,512ULL));reconcilePreparationReceives(errorBody,"preparing",std::array<std::uint64_t,1>{0ULL});
    require(errorBody.at("downloaded_bytes")==0ULL&&errorBody.at("uncertain_download_bytes")==512ULL&&preparationDownloadCharge(errorBody)==512ULL,"Interrupted in-memory redirect/error body was refunded because no staging file grew.");
}
void preparationLegacyReceiveRecoveryPreservesExactTerminalReceipts() {
    using namespace NativeTools::Windows::ComfyDetail;
    for(const auto* state:{"failed","completed","preparing"})for(const auto* type:{"metadata","model"}){
        auto manifest=receiveReceipt(64ULL*1024ULL*1024ULL,type);manifest.erase("receive_accounting_version");manifest.erase("uncertain_download_bytes");manifest["downloaded_bytes"]=99ULL;manifest["downloads"][0]["received_bytes"]=99ULL;
        reconcilePreparationReceives(manifest,state,std::array<std::uint64_t,1>{99ULL});const auto bound=std::string_view{type}=="metadata"?65536ULL:16ULL*1024ULL*1024ULL+65536ULL;
        require(manifest.at("downloaded_bytes")==99ULL&&manifest.at("uncertain_download_bytes")== (std::string_view{state}=="preparing"?bound:0ULL),"Legacy receive migration changed exact failed/completed totals or ignored preparing uncertainty.");
        const auto once=preparationDownloadCharge(manifest);reconcilePreparationReceives(manifest,"preparing",std::array<std::uint64_t,1>{99ULL});require(preparationDownloadCharge(manifest)==once,"Legacy uncertainty was charged again on a subsequent resume.");
    }
    auto verified=receiveReceipt(100ULL);verified.erase("receive_accounting_version");verified["downloads"][0]["state"]="verified";verified["downloaded_bytes"]=100ULL;verified["downloads"][0]["received_bytes"]=100ULL;
    reconcilePreparationReceives(verified,"preparing",std::array<std::uint64_t,1>{100ULL});require(preparationDownloadCharge(verified)==100ULL,"Verified legacy download was charged unmeasured bytes during installed-content reuse.");
}
void preparationBackendPersistsInterruptedMetadataAdmissionBeforeEffects() {
    Fixture fixture;permitPreparation(fixture);fixture.config.downloadBudgetBytes=100ULL;ULARGE_INTEGER free{},total{},available{};
    require(GetDiskFreeSpaceExW(fixture.root.c_str(),&free,&total,&available)!=FALSE,"Cannot measure interrupted metadata fixture reserve.");fixture.config.freeSpaceReserveBytes=free.QuadPart+1ULL;
    const auto context=TestContext{}.active();const auto directory=preparationDirectory(fixture,context);auto receipt=interruptedPreparation(fixture,context);
    receipt["downloaded_bytes"]=80ULL;receipt["receive_accounting_version"]=1U;receipt["uncertain_download_bytes"]=0ULL;
    receipt["downloads"].push_back({{"type","metadata"},{"state","downloading"},{"path",pathText(directory/L"interrupted.metadata.json").value()},{"received_bytes",80ULL}});
    receipt["receive_admission"]={{"transfer_index",0U},{"reserved_bytes",20ULL},{"remaining_bytes",20ULL}};std::ofstream{directory/L"manifest.json"}<<receipt.dump();
    for(unsigned attempt=0U;attempt<2U;++attempt){const auto result=Json::parse(take(fixture.call("prepare",Json::object(),context)));
        require(result.at("ok")==false&&result.at("downloaded_bytes")==80ULL&&result.at("uncertain_download_bytes")==20ULL&&result.at("budget_charged_bytes")==100ULL&&result.at("manifest").at("receive_uncertainties").size()==1U&&!result.at("manifest").contains("receive_admission"),"Backend resume refunded or duplicated an interrupted metadata admission.");
        require(!Fs::exists(fixture.root/L"runtime"/L"runtime.log")&&!Fs::exists(directory/L"package-contract-0.json")&&result.at("manifest").at("downloads").size()==1U,"Interrupted metadata accounting reached provider startup, package execution, or another download before its reserve failure.");
    }
}
void modelIdentityNormalizesInstalledYamlPathSeparators() {
    Fixture fixture{true};const auto models=fixture.root/L"shared-models";Fs::create_directories(models/L"checkpoints");std::ofstream{models/L"checkpoints"/L"existing.safetensors",std::ios::binary}<<"abc";
    auto yamlPath=pathText(models).value();std::replace(yamlPath.begin(),yamlPath.end(),'\\','/');
    std::ofstream{fixture.root/L"portable"/L"ComfyUI"/L"extra_model_paths.yaml"}<<"shared_models:\n    base_path: "<<yamlPath<<"/\n    checkpoints: checkpoints/\n";
    const auto result=Json::parse(take(fixture.call("identity",{{"workflows",Json::array()}})));
    require(result.at("method")=="schema_sha256_source_content_sha256_package_RECORD_sha256_model_content_sha256_file_identity_size_mtime_media_component_content_sha256","Installed YAML model-root normalization did not retain dependency sealing.");
    require(!fixture.call("inspect",{{"path",pathText(models/L"checkpoints"/L"existing.safetensors").value()}}),"Installed YAML normalization expanded public project file authority.");
}
void runtimeFiltersManagerWithoutMutatingInstalledConfiguration() {
    Fixture fixture;permitPreparation(fixture);const auto home=fixture.root/L"portable"/L"ComfyUI";
    for(const auto* name:{L"ComfyUI-Manager",L"VideoHelperSuite",L"disabled.disabled",L"__pycache__"})Fs::create_directories(home/L"custom_nodes"/name);
    std::ofstream{home/L"custom_nodes"/L"local.py"}<<"# fixture node\n";
    std::ofstream{home/L"extra_model_paths.yaml"}<<"# operator configuration\n";
    Fs::create_directories(home.parent_path()/L"forge-managed"/L"custom_nodes"/L"ComfyMath");
    requireError(fixture.call("control",{{"action","start"}}),Domain::ErrorCodes::HostCapabilityUnavailable,"Native child fixture unexpectedly became a provider.");
    std::ifstream logged{fixture.root/L"runtime"/L"runtime.log"};const auto arguments=Json::parse(logged).get<std::vector<std::string>>();logged.close();
    require(std::find(arguments.begin(),arguments.end(),"--disable-api-nodes")==arguments.end(),"Runtime added an API-node or frontend network restriction.");
    const auto disabled=std::find(arguments.begin(),arguments.end(),"--disable-all-custom-nodes");
    const auto whitelist=std::find(arguments.begin(),arguments.end(),"--whitelist-custom-nodes");
    require(disabled!=arguments.end()&&whitelist!=arguments.end(),"Runtime did not filter custom-node prestartup and imports.");
    const std::vector<std::string> names{"ComfyMath","VideoHelperSuite","local.py"};
    require(std::vector<std::string>{std::next(whitelist),arguments.end()}==names,"Runtime changed supported custom nodes or admitted Manager/disabled nodes.");
    std::ifstream configuration{home/L"extra_model_paths.yaml"};const std::string saved{std::istreambuf_iterator<char>{configuration},std::istreambuf_iterator<char>{}};
    require(saved=="# operator configuration\n","Runtime filtering changed installed operator configuration.");
    std::ifstream ledger{fixture.root/L"runtime"/L"runtime-owner.json"};const auto owner=Json::parse(ledger);ledger.close();
    require(owner.at("manager_disabled")==true&&owner.at("custom_node_whitelist").get<std::vector<std::string>>()==names,"Runtime owner evidence omitted the effective custom-node filter.");
}
void runtimeModelStorageLaunchKeepsActualCategoriesAndAliases() {
    using namespace NativeTools::Windows::ComfyDetail;
    Fixture fixture;permitPreparation(fixture);const auto home=fixture.root/L"portable"/L"ComfyUI",storage=fixture.root/L"configured-models";Fs::create_directory(storage);fixture.config.modelStoragePath=pathText(storage).value();
    const std::string stock{"folder_names_and_paths[\"text_encoders\"] = ([os.path.join(models_dir, \"text_encoders\"), os.path.join(models_dir, \"clip\")], supported_pt_extensions)\nfolder_names_and_paths[\"diffusion_models\"] = ([os.path.join(models_dir, \"unet\"), os.path.join(models_dir, \"diffusion_models\")], supported_pt_extensions)\nfolder_names_and_paths[\"configs\"] = ([os.path.join(models_dir, \"configs\")], [\".yaml\"])\nfolder_names_and_paths[\"embeddings\"] = ([os.path.join(models_dir, \"embeddings\")], supported_pt_extensions)\nfolder_names_and_paths[\"diffusers\"] = ([os.path.join(models_dir, \"diffusers\")], [\"folder\"])\nfolder_names_and_paths[\"new_category\"] = ([os.path.join(models_dir, \"publisher_folder\")], supported_pt_extensions)\n"};
    std::ofstream{home/L"folder_paths.py"}<<stock;const auto original=fileFacts(home/L"folder_paths.py",TestContext{}.active());
    requireError(fixture.call("control",{{"action","start"}}),Domain::ErrorCodes::HostCapabilityUnavailable,"Native storage launch fixture unexpectedly became a provider.");
    const auto configuration=readJson(fixture.root/L"runtime"/L"extra-paths.yaml");const auto& categories=configuration.at("forge_models");
    require(categories.at("base_path")==pathText(storage).value()&&categories.at("text_encoders")=="text_encoders\nclip"&&categories.at("diffusion_models")=="unet\ndiffusion_models","Configured storage dropped or reordered actual text-encoder or diffusion-model aliases.");
    require(categories.at("configs")=="configs"&&categories.at("embeddings")=="embeddings"&&categories.at("diffusers")=="diffusers"&&categories.at("new_category")=="publisher_folder","Runtime launch still uses a fixed category list or invented category folder name.");
    require(!categories.contains("checkpoints")&&!categories.contains("custom_nodes")&&fileFacts(home/L"folder_paths.py",TestContext{}.active())==original,"Runtime category discovery invented absent contracts or changed the installed source.");
    std::ifstream logged{fixture.root/L"runtime"/L"runtime.log"};const auto arguments=Json::parse(logged).get<std::vector<std::string>>();logged.close();const auto flag=std::find(arguments.begin(),arguments.end(),"--extra-model-paths-config");
    require(flag!=arguments.end()&&std::next(flag)!=arguments.end()&&*std::next(flag)==pathText(fixture.root/L"runtime"/L"extra-paths.yaml").value(),"Captured provider launch did not use the source-backed model-storage configuration.");
}
void artifactOwnershipRejectsChangedRuntimeConfigurationBeforeTransfer() {
    for(const auto* changed:{"installation","model_storage","python","extra_paths"}){
        Fixture fixture{true};LoopbackArtifactServer server{std::vector<std::byte>(64U,std::byte{42}),false};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
        if(std::string_view{changed}=="installation"){const auto replacement=fixture.root/L"replacement";Fs::create_directories(replacement);std::ofstream{replacement/L"main.py"}<<"# replacement\n";fixture.config.installationPath=pathText(replacement).value();}
        else if(std::string_view{changed}=="model_storage")fixture.config.modelStoragePath=pathText(fixture.root/L"other-models").value();
        else if(std::string_view{changed}=="python")std::ofstream{fixture.root/L"runtime"/L"active-environment.json"}<<Json{{"python",pathText(fixture.root/L"other-python.exe").value()}}.dump();
        else std::ofstream{fixture.root/L"runtime"/L"extra-paths.yaml",std::ios::app}<<"\n# changed\n";
        const auto result=fixture.call("collect",collectArguments(fixture));
        require(!result,"Artifact collection accepted changed sealed runtime configuration.");
        require(server.views==0U&&server.prompts==0U&&!server.unexpectedRequest,"Ownership failure performed provider HTTP or submitted a generation.");
        require(!Fs::exists(fixture.root/L"workspace"/L"12_preview.png"),"Ownership failure published a provider artifact.");
    }
}
void runtimePathProjectionMatchesLoadedAliasDefaultAndEnvironmentOrder() {
    Fixture fixture;LoopbackArtifactServer server{{},false};fixture.config.endpoint=server.endpoint();const auto home=fixture.root/L"portable"/L"ComfyUI",storage=fixture.root/L"configured",shared=fixture.root/L"shared";fixture.config.modelStoragePath=pathText(storage).value();
    std::ofstream{home/L"folder_paths.py"}<<"folder_names_and_paths[\"text_encoders\"] = ([os.path.join(models_dir, \"text_encoders\"), os.path.join(models_dir, \"clip\")], supported_pt_extensions)\nfolder_names_and_paths[\"diffusion_models\"] = ([os.path.join(models_dir, \"unet\"), os.path.join(models_dir, \"diffusion_models\")], supported_pt_extensions)\nlegacy = {\"unet\": \"diffusion_models\", \"clip\": \"text_encoders\"}\n";
    const auto environment=L"FORGE_COMFY_PATH_FIXTURE";std::array<wchar_t,32768> previous{};SetLastError(ERROR_SUCCESS);const auto count=GetEnvironmentVariableW(environment,previous.data(),static_cast<DWORD>(previous.size()));const bool present=count>0U||GetLastError()!=ERROR_ENVVAR_NOT_FOUND;
    struct Restore {const wchar_t* name;std::wstring value;bool present;~Restore(){SetEnvironmentVariableW(name,present?value.c_str():nullptr);}} restore{environment,std::wstring{previous.data(),count},present};
    require(SetEnvironmentVariableW(environment,shared.c_str())!=FALSE,"Fixture path environment could not be set.");
    std::ofstream{home/L"extra_model_paths.yaml"}<<"first:\n    base_path: '${FORGE_COMFY_PATH_FIXTURE}/base'\n    is_default: true\n    clip: |\n        first\n        second\n    unet: '$FORGE_COMFY_PATH_FIXTURE/literal'\nsecond:\n    base_path: "<<Json(pathText(home/L"models").value()).dump()<<"\n    is_default: yes\n    clip: text_encoders\nthird:\n    text_encoders: "<<Json(pathText(fixture.root/L"manual").value()).dump()<<"\n    embeddings: 'quote''folder'\n";
    const auto configuration=Json::parse(take(fixture.call("status",Json::object()))).at("launch_configuration");const auto& paths=configuration.at("model_category_paths");
    const auto expected=Json::array({pathText(home/L"models"/L"text_encoders").value(),pathText(shared/L"base"/L"second").value(),pathText(shared/L"base"/L"first").value(),pathText(home/L"models"/L"clip").value(),pathText(fixture.root/L"manual").value(),pathText(storage/L"text_encoders").value(),pathText(storage/L"clip").value()});
    require(paths.at("text_encoders")==expected&&!paths.contains("clip")&&!paths.contains("unet"),"Native projection changed actual alias/default/newline or operator-before-Forge ordering.");
    require(paths.at("diffusion_models")[0]==pathText(shared/L"base"/L"$FORGE_COMFY_PATH_FIXTURE"/L"literal").value()&&paths.at("diffusion_models").back()==pathText(storage/L"diffusion_models").value(),"Native projection expanded category paths instead of only base_path or changed alias order.");
    require(paths.at("embeddings")==Json::array({pathText(home/L"quote'folder").value()}),"Native projection did not decode YAML doubled single quotes.");
    for(const auto* malformed:{"group:\n    checkpoints: [first, second]\n","group:\n    checkpoints: >\n        first\n","group:\n    checkpoints:\n        nested: wrong\n","group:\n    checkpoints: first\n    checkpoints: second\n"}){
        std::ofstream{home/L"extra_model_paths.yaml"}<<malformed;const auto rejected=Json::parse(take(fixture.call("status",Json::object())));require(rejected.contains("installation_error")&&!rejected.contains("launch_configuration"),"Unsupported YAML configuration was silently projected into loaded paths.");
    }
}
void runtimeOriginalPathConfigurationAndResolvedEnvironmentFenceLiveProvider() {
    for(const auto* changed:{"created","edited","deleted","environment","legacy","active_environment"}){
        Fixture fixture;const auto home=fixture.root/L"portable"/L"ComfyUI",extra=home/L"extra_model_paths.yaml";LoopbackArtifactServer server{std::vector<std::byte>(64U,std::byte{42}),false,200U,{{"queue_running",Json::array()},{"queue_pending",Json::array({Json::array({1,"unrelated",Json::object()})})}}};fixture.config.endpoint=server.endpoint();
        const auto environment=L"FORGE_COMFY_LAUNCH_PATH_FIXTURE";std::array<wchar_t,32768> previous{};SetLastError(ERROR_SUCCESS);const auto count=GetEnvironmentVariableW(environment,previous.data(),static_cast<DWORD>(previous.size()));const bool present=count>0U||GetLastError()!=ERROR_ENVVAR_NOT_FOUND;
        struct Restore {const wchar_t* name;std::wstring value;bool present;~Restore(){SetEnvironmentVariableW(name,present?value.c_str():nullptr);}} restore{environment,std::wstring{previous.data(),count},present};
        require(SetEnvironmentVariableW(environment,(fixture.root/L"first-models").c_str())!=FALSE,"Launch fixture environment was unavailable.");
        if(std::string_view{changed}=="environment")std::ofstream{extra}<<"shared:\n    base_path: ${FORGE_COMFY_LAUNCH_PATH_FIXTURE}\n    checkpoints: checkpoints\n";
        else if(std::string_view{changed}!="created")std::ofstream{extra}<<"# original operator YAML\n";
        fixture.sealCurrentOwner();require(Json::parse(take(fixture.call("status",Json::object()))).at("managed")==true,"Unchanged launch configuration did not retain exact provider ownership.");
        if(std::string_view{changed}=="deleted")Fs::remove(extra);
        else if(std::string_view{changed}=="environment")require(SetEnvironmentVariableW(environment,(fixture.root/L"second-models").c_str())!=FALSE,"Launch fixture environment could not change.");
        else if(std::string_view{changed}=="legacy"){auto ledger=NativeTools::Windows::ComfyDetail::readJson(fixture.root/L"runtime"/L"runtime-owner.json");ledger.erase("original_extra_paths");ledger.erase("model_category_paths");std::ofstream{fixture.root/L"runtime"/L"runtime-owner.json"}<<ledger.dump();}
        else if(std::string_view{changed}=="active_environment")std::ofstream{fixture.root/L"runtime"/L"active-environment.json"}<<"{}";
        else std::ofstream{extra,std::ios::app}<<"# changed YAML\n";
        const auto status=Json::parse(take(fixture.call("status",Json::object())));require(status.at("managed")==false&&status.contains("ownership_error"),"Changed/missing launch evidence or environment was reported as loaded readiness.");
        const auto code=std::string_view{changed}=="active_environment"?Domain::ErrorCodes::InvalidRequest:Domain::ErrorCodes::Conflict;
        requireError(fixture.call("collect",collectArguments(fixture)),code,"Changed original paths permitted artifact transfer.");
        requireError(fixture.call("identity",{{"workflows",Json::array()}}),code,"Live dependency identity adopted roots the provider never loaded.");
        const auto deniedStop=fixture.call("control",{{"action","stop"}});
        requireError(deniedStop,Domain::ErrorCodes::Unauthorized,"The default fixture unexpectedly admitted runtime control without Execute permission.");
        require(deniedStop.error().message=="ComfyUI setup and runtime control require owner-enabled Execute permission.","Runtime control failed before reaching the expected fixture authority boundary.");
        permitPreparation(fixture);
        requireError(fixture.call("control",{{"action","stop"}}),Domain::ErrorCodes::Conflict,"Configuration drift or malformed active environment bypassed the exact busy-queue stop fence.");
        require(server.views==0U&&server.prompts==0U&&!server.unexpectedRequest&&!Fs::exists(fixture.root/L"workspace"/L"12_preview.png"),"Launch configuration drift performed media transfer/submission/publication.");
    }
}
void archiveExpansionAccountsAllocationAndRejectsLinksAndInvalidSizes() {
    using namespace NativeTools::Windows::ComfyDetail;
    const std::string inventory{"drwxrwxrwx  0 0      0           0 Oct 09 16:58 directory/\n-rw-rw-rw-  0 0      0          17 Oct 09 16:58 payload with spaces.txt\n-rw-r--r--  0 user   group    4097 Apr 30  2025 nested/file\n"};
    require(archiveRequiredBytes(inventory,4096ULL,16384ULL)==16384ULL,"Archive reservation omitted directory/file allocation or filenames with spaces.");
    const auto rejects=[](std::string_view listing,std::uint64_t maximum,std::string_view code){bool rejected=false;
        try{static_cast<void>(archiveRequiredBytes(listing,4096ULL,maximum));}catch(const Failure& error){rejected=error.error.code==code;}
        require(rejected,"Archive reservation accepted a link, malformed size, overflow, or insufficient bound.");};
    rejects("hrw-rw-rw-  0 0      0           0 Oct 09 16:58 hardlink.txt link to payload with spaces.txt\n",65536ULL,Domain::ErrorCodes::PathOutsideAuthority);
    rejects("lrw-rw-rw-  0 0      0           0 Oct 09 16:59 symbolic.txt -> //?/C:/fixture/target.txt\n",65536ULL,Domain::ErrorCodes::PathOutsideAuthority);
    rejects("-rw-rw-rw-  0 0      0       17bad Oct 09 16:58 payload\n",65536ULL,Domain::ErrorCodes::MalformedMessage);
    rejects("-rw-rw-rw-  0 0      0          -1 Oct 09 16:58 payload\n",65536ULL,Domain::ErrorCodes::MalformedMessage);
    rejects("-rw-rw-rw-  0 0      0 18446744073709551615 Oct 09 16:58 payload\n",UINT64_MAX,Domain::ErrorCodes::LimitExceeded);
    rejects(inventory,16383ULL,Domain::ErrorCodes::LimitExceeded);
}
void configurationReplacementRetainsOriginalWhenAtomicCommitFails() {
    using namespace NativeTools::Windows::ComfyDetail;
    Fixture fixture;const auto target=fixture.root/L"python313._pth";const std::string original{"python313.zip\r\n.\r\nimport site\r\n"},replacement{"overlay\r\n"+original};
    std::ofstream{target,std::ios::binary}<<original;
    Handle locked{CreateFileW(target.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
    require(static_cast<bool>(locked),"Fixture could not fence configuration replacement.");bool rejected=false;
    try{replaceContents(target,replacement,TestContext{}.active());}catch(const Failure& error){rejected=error.error.code==Domain::ErrorCodes::Conflict;}
    require(rejected,"Atomic configuration replacement ignored an existing file identity lock.");locked.reset();
    std::ifstream baseline{target,std::ios::binary};const std::string retained{std::istreambuf_iterator<char>{baseline},{}};baseline.close();
    require(retained==original,"Failed atomic commit truncated or changed the working interpreter configuration.");
    std::size_t staged{};for(const auto& item:Fs::directory_iterator{fixture.root})if(item.path().extension()==L".new")++staged;
    require(staged==1U,"Failed configuration commit discarded its staged recovery evidence.");
    replaceContents(target,replacement,TestContext{}.active());std::ifstream published{target,std::ios::binary};const std::string content{std::istreambuf_iterator<char>{published},{}};
    require(content==replacement,"Atomic configuration commit did not publish exact replacement bytes.");
}
void artifactFreeSpaceReserveRejectsBeforeProviderTransfer() {
    Fixture fixture{true};LoopbackArtifactServer server{std::vector<std::byte>(64U,std::byte{42}),false};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
    ULARGE_INTEGER free{},total{},available{};require(GetDiskFreeSpaceExW(fixture.root.c_str(),&free,&total,&available)!=FALSE,"Artifact fixture cannot measure available disk space.");
    constexpr std::uint64_t maximumReserve=10000000000000ULL,margin=1024ULL*1024ULL*1024ULL;if(free.QuadPart>maximumReserve-margin)return;
    fixture.config.freeSpaceReserveBytes=free.QuadPart+margin;
    requireError(fixture.call("collect",collectArguments(fixture)),Domain::ErrorCodes::LimitExceeded,"Artifact collection ignored its configured free-space reserve.");
    require(server.views==0U&&server.prompts==0U&&!server.unexpectedRequest,"Insufficient artifact space performed a provider transfer or generation.");
    require(!Fs::exists(fixture.root/L"workspace"/L"12_preview.png"),"Insufficient artifact space published a file.");
}
void largeApiWorkflowRemainsExportableAfterBoundedImportFailure() {
    Fixture fixture{true};const Json graph{{"1",{{"class_type","ExampleNode"},{"inputs",{{"text",std::string(160U*1024U,'x')},{"literal_array",Json::array({1,2,3})}}}}}};
    requireError(fixture.call("workflow",{{"action","import"},{"workflow",graph}}),Domain::ErrorCodes::PayloadTooLarge,"Large API graph exceeded the inline import bound.");
    std::ifstream session{fixture.root/L"runtime"/L"workflow-session.json"};const auto retained=Json::parse(session);session.close();
    require(retained.at("source")==graph,"Bounded import discarded or changed the retained API graph.");
    const auto path=pathText(fixture.root/L"workspace"/L"large-workflow.json").value();const auto encoded=take(fixture.call("workflow",{{"action","export"},{"path",path}}));const auto exported=Json::parse(encoded);
    require(encoded.size()<=128U*1024U&&exported.at("inline_graphs_omitted")==true&&exported.at("path")==path&&!exported.contains("workflow"),"Large retained graph export did not return a bounded file receipt.");
    std::ifstream output{fixture.root/L"workspace"/L"large-workflow.json",std::ios::binary};const std::string bytes{std::istreambuf_iterator<char>{output},{}};
    require(bytes==graph.dump(2),"Large workflow export changed the serialized original graph or literal array.");
}
void collectionRejectsInactiveAndMalformedManagedComponentsBeforeExecution() {
    const auto previousSize=GetEnvironmentVariableW(L"PATH",nullptr,0U);std::wstring previous(previousSize,L'\0');
    if(previousSize>0U){const auto copied=GetEnvironmentVariableW(L"PATH",previous.data(),previousSize);require(copied<previousSize,"Component fixture could not retain PATH.");previous.resize(copied);}
    struct RestorePath {std::wstring previous;bool exists;~RestorePath(){SetEnvironmentVariableW(L"PATH",exists?previous.c_str():nullptr);}} restore{previous,previousSize>0U};
    for(const auto* variation:{"inactive","object","string","primitive_entry"}){
        Fixture fixture;permitPreparation(fixture);std::vector<std::byte> media(32U,std::byte{});media[4]=std::byte{'f'};media[5]=std::byte{'t'};media[6]=std::byte{'y'};media[7]=std::byte{'p'};
        LoopbackArtifactServer server{media,false};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
        const auto inactive=fixture.root/L"runtime"/L"components"/L"ffmpeg-failed.forge-attempt.disabled"/L"bin";Fs::create_directories(inactive);
        for(const auto* executable:{L"ffprobe.exe",L"ffmpeg.exe"})Fs::copy_file(fixture.root/L"portable"/L"python_embeded"/L"python.exe",inactive/executable);
        require(SetEnvironmentVariableW(L"PATH",inactive.c_str())!=FALSE,"Component fixture could not isolate PATH.");
        if(std::string_view{variation}!="inactive"){
            std::ifstream input{fixture.root/L"runtime"/L"active-environment.json"};auto environment=Json::parse(input);input.close();
            environment["dependencies"]=std::string_view{variation}=="object"?Json::object():std::string_view{variation}=="string"?Json("malformed dependencies"):Json::array({7});
            std::ofstream{fixture.root/L"runtime"/L"active-environment.json"}<<environment.dump();
        }
        const auto result=fixture.call("collect",collectArguments(fixture,"fixture.mp4"));requireError(result,Domain::ErrorCodes::IntegrityFailure,"Collection accepted an inactive component or malformed active dependency inventory.");
        if(std::string_view{variation}=="inactive")require(result.error().message.find("inactive")!=std::string::npos,"Inactive component was selected and executed instead of rejected.");
        require(!Fs::exists(fixture.root/L"runtime"/L"inspection"),"Untrusted managed component was executed before installation admission.");
        for(const auto& entry:Fs::directory_iterator{fixture.root/L"workspace"})require(entry.path().extension()!=L".mp4","Untrusted managed component produced a published artifact.");
        require(server.views==1U&&server.prompts==0U&&!server.unexpectedRequest,"Component admission submitted generation or retrieved unrelated provider data.");
    }
}
void preparationResumeRehashesAndReusesVerifiedArchives() {
    using NativeTools::Windows::ComfyDetail::fileFacts;using NativeTools::Windows::ComfyDetail::runProcess;
    std::array<wchar_t,32768> tarPath{};const auto tarSize=SearchPathW(nullptr,L"tar.exe",nullptr,static_cast<DWORD>(tarPath.size()),tarPath.data(),nullptr);
    require(tarSize>0U&&tarSize<tarPath.size(),"Preparation archive fixture requires installed Windows tar.");
    for(const auto* variation:{"unchanged","edited","exact_budget_expanded","http_unchanged"}){
        Fixture fixture;permitPreparation(fixture);const auto context=TestContext{}.active();const auto transaction=preparationDirectory(fixture,context);const auto archive=transaction/L"verified.download";
        const bool exactBudget=std::string_view{variation}=="exact_budget_expanded";const auto source=fixture.root/L"publisher-source";Fs::create_directories(source/L"component"/L"bin");std::ofstream{source/L"component"/L"bin"/L"ffprobe.exe",std::ios::binary}<<(exactBudget?std::string(256U*1024U,'x'):std::string{"sealed fixture component"});
        const auto zipped=runProcess(Fs::path{tarPath.data()},{"-a","-cf",pathText(transaction/L"fixture.zip").value(),"-C",pathText(source).value(),"component"},transaction,transaction/L"archive-create.log",context);
        require(zipped.at("exit_code")==0,"Preparation fixture could not create its native publisher ZIP.");Fs::rename(transaction/L"fixture.zip",archive);const auto facts=fileFacts(archive,context);
        if(exactBudget){fixture.config.downloadBudgetBytes=facts.at("bytes").get<std::uint64_t>();require(fixture.config.downloadBudgetBytes<256ULL*1024ULL,"Native ZIP fixture did not compress its retained content below the transfer budget.");}
        const std::string url{std::string_view{variation}=="http_unchanged"?"http://publisher.invalid/unused-verified.zip":"https://publisher.invalid/unused-verified.zip"};const Json dependency{{"kind","component"},{"url",url},{"target","fixture-component"},{"archive",true},{"sha256",facts.at("sha256")},{"bytes",facts.at("bytes")}};
        auto receipt=interruptedPreparation(fixture,context);receipt["downloaded_bytes"]=facts.at("bytes");receipt["downloads"].push_back({{"url",url},{"path",pathText(archive).value()},{"sha256_expected",facts.at("sha256")},{"bytes",facts.at("bytes")},{"received_bytes",facts.at("bytes")},{"state","verified"}});
        std::ofstream{transaction/L"manifest.json"}<<receipt.dump();if(std::string_view{variation}=="edited")std::ofstream{archive,std::ios::binary|std::ios::trunc}<<"externally altered archive";
        const auto result=Json::parse(take(fixture.call("prepare",{{"dependencies",Json::array({dependency})}},context)));
        require(result.at("ok")==false&&result.at("downloaded_bytes")==facts.at("bytes")&&result.at("manifest").at("downloads").size()==1U,"Archive resume repeated a transfer or changed cumulative downloaded bytes.");
        if(std::string_view{variation}!="edited"){
            require(result.at("manifest").at("downloads")[0].at("reused_after_verification")==true&&result.at("manifest").at("installed").size()==1U,"Verified retained publisher archive was not reused and activated before readiness.");
            require(result.at("manifest").at("installed")[0].at("tree").contains("bin\\ffprobe.exe")&&fileFacts(archive,context).at("sha256")==facts.at("sha256"),"Archive reuse lost its original file or expanded content seal.");
            if(exactBudget){require(result.at("manifest").at("installed")[0].at("tree").at("bin\\ffprobe.exe").at("bytes")==256ULL*1024ULL,"Archive expansion was still constrained by received-byte budget.");bool rolledBack=false;for(const auto& item:result.at("manifest").at("rollback"))if(item.value("path",std::string{})==result.at("manifest").at("installed")[0].at("target").get<std::string>())rolledBack=item.value("moved",false);require(rolledBack,"Exact-budget archive tree could not be verified and rolled back independently of its transfer limit.");}
        }else require(result.at("error").at("code").get<std::string>()==Domain::ErrorCodes::IntegrityFailure&&result.at("manifest").at("installed").empty(),"Resume accepted an externally altered verified archive.");
    }
}
void dependencyDirectorySealSupportsConfiguredLongPaths() {
    Fixture fixture;auto root=fixture.root/L"long-path-component";while(root.native().size()<240U)root/=std::wstring(45U,L'x');const auto leaf=root/L"nested-component-bin"/L"ffprobe.exe";
    require(leaf.native().size()>260U,"Long-path dependency fixture did not cross Windows' legacy path boundary.");const Fs::path extended{L"\\\\?\\"+leaf.native()};Fs::create_directories(extended.parent_path());std::ofstream{extended,std::ios::binary}<<"abc";
    const auto facts=NativeTools::Windows::ComfyDetail::directoryFacts(root,1024ULL,TestContext{}.active());
    require(facts.size()==1U&&facts.contains("nested-component-bin\\ffprobe.exe")&&facts.at("nested-component-bin\\ffprobe.exe").at("bytes")==3ULL,"Native dependency seal cannot inventory configured paths beyond 260 characters.");
}
void ffmpegResolverUsesDatedPublisherAssetContractAndDigest() {
    using NativeTools::Windows::ComfyDetail::resolveFfmpegRelease;using NativeTools::Windows::ComfyDetail::Failure;
    const std::string tag{"autobuild-2026-10-09-14-16"},name{"ffmpeg-N-127259-gb91a82d6dd-win64-gpl.zip"},sha{"9bcf8b949efb0e746bb85d840d0c08918e8b6b7d55c641f4065f4aedeffd9a12"};
    const std::string url{"https://github.com/BtbN/FFmpeg-Builds/releases/download/"+tag+"/"+name};
    const Json asset{{"name",name},{"id",625178772ULL},{"size",200234231ULL},{"digest","sha256:"+sha},{"browser_download_url",url}};
    const Json dated{{"tag_name",tag},{"id",407985839ULL},{"prerelease",false},{"draft",false},{"assets",Json::array({asset})}};
    const Json rolling{{"tag_name","latest"},{"id",408007421ULL},{"assets",Json::array({{{"name","ffmpeg-master-latest-win64-gpl.zip"},{"id",625244710ULL},{"size",200232791ULL},{"digest","sha256:dafe64a1c4ece573b8e5f076bf8914ea997be9533be4bb52e6c5a33adfdd8d1b"},{"browser_download_url","https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip"}}})}};
    const auto selected=resolveFfmpegRelease(Json::array({rolling,dated}));
    require(selected.at("url")==url&&selected.at("sha256")==sha&&selected.at("bytes")==200234231ULL&&selected.at("target")=="ffmpeg-"+tag,"FFmpeg resolver did not select the actual versioned Windows publisher asset.");
    require(selected.at("provenance").at("release_id")==407985839ULL&&selected.at("provenance").at("asset_id")==625178772ULL&&selected.at("provenance").at("asset_name")==name,"FFmpeg resolver discarded its pinned publisher identity.");
    for(const auto* changed:{"checksum","origin","prerelease","architecture","moving"}){auto rejected=dated;
        if(std::string_view{changed}=="checksum")rejected["assets"][0]["digest"]="sha256:invalid";
        else if(std::string_view{changed}=="origin")rejected["assets"][0]["browser_download_url"]="https://other.invalid/"+name;
        else if(std::string_view{changed}=="prerelease")rejected["prerelease"]=true;
        else if(std::string_view{changed}=="architecture")rejected["assets"][0]["name"]="ffmpeg-N-127259-gb91a82d6dd-linux64-gpl.tar.xz";
        else rejected=rolling;
        bool failed=false;try{static_cast<void>(resolveFfmpegRelease(Json::array({rejected})));}catch(const Failure& error){failed=error.error.code==Domain::ErrorCodes::HostCapabilityUnavailable;}
        require(failed,"FFmpeg resolver accepted unsealed, unowned, prerelease, incompatible or moving publisher assets.");
    }
}
void directoryContentSealsDistinguishPublisherEditsAndGeneratedFiles() {
    using NativeTools::Windows::ComfyDetail::directoryFacts;using NativeTools::Windows::ComfyDetail::retainsDirectoryFiles;
    Fixture fixture;const auto node=fixture.root/L"node";Fs::create_directories(node);std::ofstream{node/L"nodes.py"}<<"publisher source\n";
    const auto publisher=directoryFacts(node,1024ULL*1024ULL,TestContext{}.active());
    Fs::create_directories(node/L"__pycache__");Fs::create_directories(node/L".git");std::ofstream{node/L"__pycache__"/L"node.pyc"}<<"compiled";std::ofstream{node/L".git"/L"HEAD"}<<"git";std::ofstream{node/L"other.pyc"}<<"compiled";std::ofstream{node/L".forge-dependency.json"}<<"{}";
    require(directoryFacts(node,1024ULL*1024ULL,TestContext{}.active())==publisher,"Dependency seal included its own marker, Git state or generated Python cache.");
    std::ofstream{node/L"runtime-config.json"}<<"{\"generated\":true}";const auto generated=directoryFacts(node,1024ULL*1024ULL,TestContext{}.active());
    require(generated!=publisher&&retainsDirectoryFiles(publisher,generated),"Startup-generated configuration changed publisher-file verification.");
    std::ofstream{node/L"nodes.py",std::ios::trunc}<<"external edit\n";
    require(!retainsDirectoryFiles(publisher,directoryFacts(node,1024ULL*1024ULL,TestContext{}.active())),"Dependency seal accepted a changed publisher source file.");
}
void directoryRecoveryRetainsGeneratedFilesAndRejectsUnsealedOrEditedContent() {
    using NativeTools::Windows::ComfyDetail::directoryFacts;
    for(const auto* change:{"unchanged","source_edit","unsealed"}){
        Fixture fixture;permitPreparation(fixture);const auto context=TestContext{}.active();const auto transaction=preparationDirectory(fixture,context);const auto node=fixture.root/L"portable"/L"forge-managed"/L"custom_nodes"/L"FixtureNode";
        Fs::create_directories(node);std::ofstream{node/L"nodes.py"}<<"publisher source\n";std::ofstream{node/L"runtime-config.json"}<<"{\"generated\":true}";
        const auto tree=directoryFacts(node,1024ULL*1024ULL,context);const std::string sha(64U,'a');Json marker{{"sha256",sha},{"tree",tree}};Json installed{{"target",pathText(node).value()},{"kind","custom_node"},{"sha256",sha},{"tree",tree}};
        if(std::string_view{change}=="unsealed"){marker.erase("tree");installed.erase("tree");}
        std::ofstream{node/L".forge-dependency.json"}<<marker.dump();auto receipt=interruptedPreparation(fixture,context);receipt["installed"].push_back(installed);std::ofstream{transaction/L"manifest.json"}<<receipt.dump();
        if(std::string_view{change}=="source_edit")std::ofstream{node/L"nodes.py",std::ios::trunc}<<"external edit\n";
        const auto result=fixture.call("prepare",Json::object(),context);
        if(std::string_view{change}!="unchanged"){requireError(result,Domain::ErrorCodes::IntegrityFailure,"Directory recovery accepted edited or unsealed publisher content.");require(Fs::is_regular_file(node/L"nodes.py"),"Directory recovery moved externally edited or unsealed content.");}
        else{const auto failed=Json::parse(take(result));require(failed.at("ok")==false&&!Fs::exists(node),"Failed readiness did not roll back its unchanged sealed directory.");bool retained=false;
            for(const auto& rollback:failed.at("manifest").at("rollback"))if(rollback.contains("retained")){const Fs::path path{rollback.at("retained").get<std::string>()};if(Fs::is_directory(path))retained=directoryFacts(path,1024ULL*1024ULL,context)==tree;}
            require(retained,"Directory rollback lost the sealed generated startup configuration or publisher files.");}
    }
}
void directoryReuseChecksInstalledContentBeforeReadiness() {
    using NativeTools::Windows::ComfyDetail::directoryFacts;
    for(const auto* change:{"unchanged","source_edit","unsealed"}){
        Fixture fixture;permitPreparation(fixture);const auto node=fixture.root/L"portable"/L"forge-managed"/L"custom_nodes"/L"FixtureNode";Fs::create_directories(node);std::ofstream{node/L"nodes.py"}<<"publisher source\n";
        const std::string sha(64U,'a'),revision(40U,'b');Json marker{{"sha256",sha},{"tree",directoryFacts(node,1024ULL*1024ULL,TestContext{}.active())}};if(std::string_view{change}=="unsealed")marker.erase("tree");std::ofstream{node/L".forge-dependency.json"}<<marker.dump();
        if(std::string_view{change}=="source_edit")std::ofstream{node/L"nodes.py",std::ios::trunc}<<"external edit\n";
        const auto failed=Json::parse(take(fixture.call("prepare",{{"dependencies",Json::array({{{"kind","custom_node"},{"url","https://publisher.invalid/source.zip"},{"sha256",sha},{"revision",revision},{"target","FixtureNode"},{"archive",true}}})}})));
        require(failed.at("ok")==false&&failed.at("downloaded_bytes")==0ULL&&Fs::is_regular_file(node/L"nodes.py"),"Existing dependency verification downloaded or moved baseline content.");
        if(std::string_view{change}=="unchanged")require(failed.at("manifest").at("installed").size()==1U&&failed.at("manifest").at("installed")[0].at("reused")==true,"Unchanged installed tree was not reused.");
        else require(failed.at("error").at("code").get<std::string>()==Domain::ErrorCodes::IntegrityFailure,"Reused dependency accepted edited or unsealed content.");
    }
}
void selectedModelIdentityDetectsSameSizeAndTimestampContentChanges() {
    Fixture fixture{true};const auto directory=fixture.root/L"portable"/L"ComfyUI"/L"models"/L"checkpoints";Fs::create_directories(directory);const auto model=directory/L"tinyweights.safetensors";
    std::ofstream{model,std::ios::binary}<<"abc";const auto modified=Fs::last_write_time(model);const Json arguments{{"workflows",Json::array({{{"1",{{"inputs",{{"ckpt_name","tinyweights.safetensors"}}}}}}})}};
    const auto first=Json::parse(take(fixture.call("identity",arguments)));require(first.at("models").size()==1U,"Selected model fixture did not produce a content identity.");
    std::ofstream{model,std::ios::binary|std::ios::trunc}<<"xyz";Fs::last_write_time(model,modified);const auto second=Json::parse(take(fixture.call("identity",arguments)));
    const auto& before=first.at("models")[0];const auto& after=second.at("models")[0];
    require(before.at("bytes")==after.at("bytes")&&before.at("modified_time")==after.at("modified_time")&&before.at("file_id")==after.at("file_id")&&before.at("volume_serial")==after.at("volume_serial"),"Model-content fixture changed its retained metadata identity.");
    require(before.at("sha256")!=after.at("sha256")&&first.at("sha256")!=second.at("sha256"),"Selected model identity accepted changed content with identical size, timestamp and file identity.");
}
void privateInputSealFencesNamespaceAndDetectsChangedProviderBytes() {
    Fixture fixture{true};LoopbackArtifactServer server{{},false};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
    const auto namespaceValue="ForgeConductor/"+fixture.project.value()+"/"+TestContext{}.active().operationId.value();
    const auto folder=fixture.root/L"runtime"/L"inputs"/Fs::path{namespaceValue};Fs::create_directories(folder);const auto copy=folder/L"0_input.png";
    std::ofstream{copy,std::ios::binary}<<"abc";const Json descriptor{{"type","input"},{"subfolder",namespaceValue},{"name","0_input.png"}};
    const Json args{{"descriptor",descriptor},{"namespace",namespaceValue}};const auto first=Json::parse(take(fixture.call("seal_input",args)));
    std::ofstream{copy,std::ios::binary|std::ios::trunc}<<"xyz";const auto second=Json::parse(take(fixture.call("seal_input",args)));
    require(first.at("bytes")==second.at("bytes")&&first.at("sha256")!=second.at("sha256"),"Private input content changes escaped its exact byte seal.");
    auto wrong=args;wrong["descriptor"]["subfolder"]="ForgeConductor/another-project/job";
    requireError(fixture.call("seal_input",wrong),Domain::ErrorCodes::IntegrityFailure,"Provider copy seal accepted another namespace.");
    wrong=args;wrong["descriptor"]["name"]="../0_input.png";
    requireError(fixture.call("seal_input",wrong),Domain::ErrorCodes::IntegrityFailure,"Provider copy seal accepted a traversing filename.");
    wrong=args;wrong["descriptor"]["type"]="output";
    requireError(fixture.call("seal_input",wrong),Domain::ErrorCodes::IntegrityFailure,"Provider copy seal accepted an output descriptor.");
    require(server.views==0U&&server.prompts==0U&&!server.unexpectedRequest,"Private copy inspection addressed a provider generation or download route.");
}
void statusBoundsUnrelatedQueueGraphsAndRetainsBusyCounts() {
    Json pending=Json::array();for(unsigned index=0;index<80U;++index)pending.push_back(Json::array({index,"unrelated-"+std::to_string(index),{{"1",{{"inputs",{{"text",std::string(8192U,'x')}}}}}},Json::object(),Json::array()}));
    Fixture fixture;LoopbackArtifactServer server{{},false,200U,{{"queue_running",Json::array()},{"queue_pending",pending}}};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
    const auto ownerPath=fixture.root/L"runtime"/L"runtime-owner.json";std::ifstream retained{ownerPath};auto owner=Json::parse(retained);retained.close();
    owner["custom_node_whitelist"]=Json::array();for(unsigned index=0;index<512U;++index)owner["custom_node_whitelist"].push_back(std::to_string(index)+std::string(236U,'x'));
    std::ofstream{ownerPath,std::ios::trunc}<<owner.dump();
    const auto status=Json::parse(take(fixture.call("status",Json::object())));const auto& queue=status.at("queue");
    require(status.at("available")==true&&queue.at("queue_pending_count")==80U&&queue.at("queue_pending_omitted")==16U&&queue.at("queue_pending").size()==64U,"Bounded provider status lost its busy queue evidence or truncation counts.");
    require(status.dump().size()<32U*1024U&&queue.at("queue_pending")[0].contains("graph_sha256")&&!queue.at("queue_pending")[0].contains("inputs"),"Public status exposed unrelated full graphs beyond its delivery bound.");
    require(status.at("owner").at("custom_node_whitelist_count")==512U&&status.at("owner").contains("custom_node_whitelist_sha256")&&!status.at("owner").contains("custom_node_whitelist"),"Public status copied the full installation whitelist beyond its delivery bound.");
    require(!server.unexpectedRequest&&server.prompts==0U&&server.views==0U,"Status discovery submitted generation or retrieved an artifact.");
}
void generalImageDeliveryDecodesLargeFramesWithoutWeakeningLegacyLimits() {
    using Microsoft::WRL::ComPtr;const auto checked=[](HRESULT value,const char* action){require(SUCCEEDED(value),action);};
    const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);require(initialized==RPC_E_CHANGED_MODE||SUCCEEDED(initialized),"Fixture COM initialization failed.");
    struct Apartment {bool initialized;~Apartment(){if(initialized)CoUninitialize();}} apartment{SUCCEEDED(initialized)};
    ComPtr<IWICImagingFactory> codec;checked(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&codec)),"Fixture WIC factory failed.");
    ComPtr<IStream> stream;checked(CreateStreamOnHGlobal(nullptr,TRUE,&stream),"Fixture PNG stream failed.");
    ComPtr<IWICBitmapEncoder> encoder;checked(codec->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"Fixture PNG encoder failed.");checked(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"Fixture PNG encoder initialization failed.");
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> options;checked(encoder->CreateNewFrame(&frame,&options),"Fixture PNG frame failed.");checked(frame->Initialize(options.Get()),"Fixture PNG frame initialization failed.");
    constexpr UINT width=4096U,height=1536U;checked(frame->SetSize(width,height),"Fixture image dimensions failed.");auto format=GUID_WICPixelFormat32bppBGRA;checked(frame->SetPixelFormat(&format),"Fixture image format failed.");require(format==GUID_WICPixelFormat32bppBGRA,"Fixture PNG format changed.");
    std::vector<BYTE> pixels(static_cast<std::size_t>(width)*height*4U);std::uint32_t seed=0x12345678U;
    for(auto& byte:pixels){seed^=seed<<13U;seed^=seed>>17U;seed^=seed<<5U;byte=static_cast<BYTE>(seed);}
    checked(frame->WritePixels(height,width*4U,static_cast<UINT>(pixels.size()),pixels.data()),"Fixture PNG pixels failed.");checked(frame->Commit(),"Fixture frame commit failed.");checked(encoder->Commit(),"Fixture encoder commit failed.");
    STATSTG facts{};checked(stream->Stat(&facts,STATFLAG_NONAME),"Fixture PNG length failed.");require(facts.cbSize.QuadPart>16ULL*1024ULL*1024ULL&&facts.cbSize.QuadPart<64ULL*1024ULL*1024ULL,"Fixture PNG does not exercise the legacy byte ceiling.");
    LARGE_INTEGER start{};checked(stream->Seek(start,STREAM_SEEK_SET,nullptr),"Fixture PNG seek failed.");std::vector<std::byte> bytes(static_cast<std::size_t>(facts.cbSize.QuadPart));ULONG read{};
    checked(stream->Read(bytes.data(),static_cast<ULONG>(bytes.size()),&read),"Fixture PNG read failed.");require(read==bytes.size(),"Fixture PNG read was incomplete.");
    requireError(NativeTools::Windows::Detail::decodeProviderImage(bytes,TestContext{}.active()),Domain::ErrorCodes::PayloadTooLarge,"General media implementation weakened the legacy image byte/dimension contract.");
    Fixture fixture{true};LoopbackArtifactServer server{std::move(bytes),false};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
    const auto collected=Json::parse(take(fixture.call("collect",collectArguments(fixture,"large.png"))));const auto& artifact=collected.at("artifacts")[0];
    require(collected.at("ok")==true&&artifact.at("media_type")=="image/png"&&artifact.at("metadata").at("width")==width&&artifact.at("metadata").at("height")==height&&artifact.at("metadata").at("decoded")==true,"General ComfyUI image lost its original decoded dimensions or failed publication.");
    require(artifact.at("preview").at("width").get<unsigned>()<=768U&&artifact.at("preview").at("height").get<unsigned>()<=768U&&!server.unexpectedRequest,"Large image preview exceeded its bound or addressed an unrelated route.");
    require(artifact.at("preview").at("base64").get_ref<const std::string&>().size()<=24U*1024U,"High-detail generic preview cannot fit the native MCP image delivery budget.");
}
void mediaComponentIdentitySealsActiveFilesAndIgnoresRetainedExtraction() {
    Fixture fixture{true};const auto disabled=fixture.root/L"runtime"/L"components"/L"failed.forge-retained.disabled"/L"bin";Fs::create_directories(disabled);std::ofstream{disabled/L"ffmpeg.exe"}<<"inactive";
    const auto before=Json::parse(take(fixture.call("identity",Json::object())));std::ifstream baseline{Fs::path{before.at("manifest").at("path").get<std::string>()}};const auto original=Json::parse(baseline);baseline.close();
    for(const auto& component:original.at("media_components"))require(component.at("path")!=pathText(disabled/L"ffmpeg.exe").value(),"Dependency identity selected retained failed extraction.");
    const auto active=fixture.root/L"runtime"/L"components"/L"active-fixture";Fs::create_directories(active/L"bin");const auto executable=active/L"bin"/L"ffmpeg.exe";std::ofstream{executable}<<"abc";
    const auto tree=NativeTools::Windows::ComfyDetail::directoryFacts(active,1024ULL*1024ULL,TestContext{}.active());const std::string sha(64U,'a');
    std::ofstream{active/L".forge-dependency.json"}<<Json{{"sha256",sha},{"tree",tree}}.dump();
    std::ofstream{fixture.root/L"runtime"/L"active-environment.json"}<<Json{{"dependencies",Json::array({{{"kind","component"},{"target",pathText(active).value()},{"sha256",sha},{"tree",tree}}})}}.dump();
    const auto admitted=Json::parse(take(fixture.call("identity",Json::object())));require(before.at("sha256")!=admitted.at("sha256"),"Activated media component did not enter dependency identity.");
    std::ifstream manifest{Fs::path{admitted.at("manifest").at("path").get<std::string>()}};const auto sealed=Json::parse(manifest);bool found=false;
    for(const auto& component:sealed.at("media_components"))if(component.at("path")==pathText(executable).value())found=component.at("bytes")==3U&&component.contains("sha256");
    require(found,"Active media executable has no live content seal.");std::ofstream{executable,std::ios::trunc}<<"xyz";
    requireError(fixture.call("identity",Json::object()),Domain::ErrorCodes::IntegrityFailure,"Dependency identity accepted an edited active media executable.");
}
void rollbackRetainsDirectoryEditedAfterRecoveryVerification() {
    Fixture fixture;permitPreparation(fixture);const auto context=TestContext{}.active();const auto transaction=preparationDirectory(fixture,context);const auto node=fixture.root/L"portable"/L"forge-managed"/L"custom_nodes"/L"FixtureNode";
    Fs::create_directories(node);const auto source=node/L"nodes.py";std::ofstream{source}<<"publisher source\n";const auto tree=NativeTools::Windows::ComfyDetail::directoryFacts(node,1024ULL*1024ULL,context);const std::string sha(64U,'a');
    std::ofstream{node/L".forge-dependency.json"}<<Json{{"sha256",sha},{"tree",tree}}.dump();auto receipt=interruptedPreparation(fixture,context);receipt["installed"].push_back({{"target",pathText(node).value()},{"kind","custom_node"},{"sha256",sha},{"tree",tree}});receipt["snapshot"]={{"runtime",{{"available",true},{"managed",true}}}};std::ofstream{transaction/L"manifest.json"}<<receipt.dump();
    std::ofstream{fixture.root/L"portable"/L"python_embeded"/L"fixture-edit-on-inventory.json"}<<Json{{"path",pathText(source).value()},{"contents","external edit after recovery\n"}}.dump();
    const auto failed=Json::parse(take(fixture.call("prepare",Json::object(),context)));require(failed.at("ok")==false&&Fs::is_directory(node),"Rollback moved a directory changed after recovery verified its source.");
    std::ifstream edited{source};const std::string content{std::istreambuf_iterator<char>{edited},{}};require(content=="external edit after recovery\n","Rollback fixture did not execute its external edit.");bool unreconciled=false;
    for(const auto& item:failed.at("manifest").at("rollback"))if(item.contains("path")&&item.at("path")==pathText(node).value())unreconciled=item.value("unreconciled",false)&&!item.value("moved",true);
    require(unreconciled,"Rollback did not report externally edited activation as retained and unreconciled.");
    bool suppressed=false;for(const auto& item:failed.at("manifest").at("rollback"))if(item.contains("prior_runtime_restored"))suppressed=item.at("prior_runtime_restored")==false&&item.contains("reason");
    require(suppressed,"Rollback restarted the prior provider with retained edited active node content.");std::ifstream log{fixture.root/L"runtime"/L"runtime.log"};require(Json::parse(log).is_array(),"Unreconciled active rollback launched a second provider process.");
}
void inactiveUnsealedWheelStagingDoesNotSuppressPriorRuntimeRecovery() {
    Fixture fixture;permitPreparation(fixture);const auto context=TestContext{}.active();const auto transaction=preparationDirectory(fixture,context);const auto overlay=fixture.root/L"portable"/L"forge-managed"/Fs::path{context.operationId.value()}/L"packages-unsealed";
    Fs::create_directories(overlay);std::ofstream{overlay/L"partial-package.txt"}<<"inactive wheel staging";auto receipt=interruptedPreparation(fixture,context);receipt["package_overlay"]=pathText(overlay).value();receipt["snapshot"]={{"runtime",{{"available",true},{"managed",true}}}};std::ofstream{transaction/L"manifest.json"}<<receipt.dump();
    const auto failed=Json::parse(take(fixture.call("prepare",Json::object(),context)));bool attempted=false;
    for(const auto& item:failed.at("manifest").at("rollback"))if(item.contains("prior_runtime_restored"))attempted=item.at("prior_runtime_restored")==false&&item.contains("error");
    require(attempted&&Fs::is_regular_file(overlay/L"partial-package.txt"),"Inactive unsealed wheel staging blocked prior-runtime recovery or lost partial evidence.");
    std::ifstream log{fixture.root/L"runtime"/L"runtime.log"};const std::string launches{std::istreambuf_iterator<char>{log},{}};const std::string token{"--windows-standalone-build"};const auto first=launches.find(token);
    require(first!=std::string::npos&&launches.find(token,first+token.size())!=std::string::npos,"Inactive staging test did not observe the prior-runtime recovery launch.");
}
void preparationRetainsActiveRecoveryWhenProviderIdleIsUnconfirmed() {
    for(unsigned scenario=0U;scenario<3U;++scenario){
        Fixture fixture;permitPreparation(fixture);std::unique_ptr<LoopbackArtifactServer> provider;
        if(scenario!=1U){provider=std::make_unique<LoopbackArtifactServer>(std::vector<std::byte>{},false,200U,
            scenario==0U?Json{{"queue_running",Json::array({Json::array({1,"unrelated",Json::object()})})},{"queue_pending",Json::array()}}:Json{{"queue_running",Json::array()},{"queue_pending",Json::array()}});fixture.config.endpoint=provider->endpoint();}
        fixture.sealCurrentOwner();if(scenario==2U){const auto differentModels=fixture.root/L"other-models";Fs::create_directories(differentModels);fixture.config.modelStoragePath=pathText(differentModels).value();}
        const auto context=TestContext{}.active();const auto transaction=preparationDirectory(fixture,context);const auto models=fixture.root/L"portable"/L"ComfyUI"/L"models"/L"checkpoints";Fs::create_directories(models);
        const auto model=models/L"new.safetensors";std::ofstream{model,std::ios::binary}<<"abc";
        const auto pth=fixture.root/L"portable"/L"python_embeded"/L"python313._pth";const std::string original{"python313.zip\r\n.\r\n"},replacement{"active-overlay\r\n"+original};std::ofstream{pth,std::ios::binary}<<replacement;
        auto receipt=interruptedPreparation(fixture,context);receipt["installed"].push_back({{"target",pathText(model).value()},{"kind","model"},{"sha256","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}});
        receipt["python_path_backup"]={{"path",pathText(pth).value()},{"original",original},{"replacement",replacement}};receipt["snapshot"]={{"runtime",{{"available",true},{"managed",true}}}};std::ofstream{transaction/L"manifest.json"}<<receipt.dump();
        const auto failed=Json::parse(take(fixture.call("prepare",Json::object(),context)));
        require(failed.at("ok")==false&&failed.at("error").at("code").get<std::string>()==Domain::ErrorCodes::Conflict,"Preparation resumed active files without a confirmed idle provider.");
        std::ifstream preserved{pth,std::ios::binary};const std::string bytes{std::istreambuf_iterator<char>{preserved},{}};require(bytes==replacement&&Fs::is_regular_file(model)&&Fs::file_size(model)==3ULL,"Unconfirmed provider recovery restored or moved active files.");
        bool idleRejected=false,pathRetained=false,modelRetained=false,restartSuppressed=false;
        for(const auto& item:failed.at("manifest").at("rollback")){
            if(item.contains("runtime_idle_confirmed"))idleRejected=idleRejected||item.at("runtime_idle_confirmed")==false;
            if(item.value("path",std::string{})==pathText(pth).value())pathRetained=item.value("unreconciled",false)&&!item.value("restored",true);
            if(item.value("path",std::string{})==pathText(model).value())modelRetained=item.value("unreconciled",false)&&!item.value("moved",true);
            if(item.contains("prior_runtime_restored"))restartSuppressed=item.at("prior_runtime_restored")==false&&item.contains("reason");
        }
        require(idleRejected&&pathRetained&&modelRetained&&restartSuppressed,"Unconfirmed recovery did not report retained active changes and suppress prior-runtime restoration.");
        require(!Fs::exists(fixture.root/L"runtime"/L"runtime.log")&&failed.at("downloaded_bytes")==0ULL,"Unconfirmed recovery launched a provider or started a transfer.");
        if(provider)require(provider->prompts==0U&&provider->views==0U&&!provider->unexpectedRequest,"Unconfirmed recovery affected unrelated provider work.");
    }
}
void preparationRejectsActivationWhenOwnedProcessIsLiveButUnavailable() {
    Fixture fixture;permitPreparation(fixture);fixture.sealCurrentOwner();
    Fs::create_directories(fixture.root/L"portable"/L"ComfyUI"/L"models"/L"checkpoints");
    const Json dependency{{"kind","model"},{"url","https://example.com/not-downloaded"},{"target","checkpoints/new.safetensors"},{"sha256",std::string(64U,'a')}};
    const auto failed=Json::parse(take(fixture.call("prepare",{{"dependencies",Json::array({dependency})}})));
    require(failed.at("ok")==false&&failed.at("error").at("code").get<std::string>()==Domain::ErrorCodes::Conflict,"Unavailable live owned process allowed dependency activation.");
    require(failed.at("downloaded_bytes")==0ULL&&!Fs::exists(fixture.root/L"portable"/L"ComfyUI"/L"models"/L"checkpoints"/L"new.safetensors")&&!Fs::exists(fixture.root/L"runtime"/L"runtime.log"),"Unavailable provider preparation transferred files or launched another runtime.");
}
void preparationRollbackRetainsUnchangedActivationWithBusyProvider() {
    Fixture fixture;permitPreparation(fixture);{LoopbackArtifactServer reservePort{{},false};fixture.config.endpoint=reservePort.endpoint();}
    const auto context=TestContext{}.active();const auto transaction=preparationDirectory(fixture,context);const auto models=fixture.root/L"portable"/L"ComfyUI"/L"models"/L"checkpoints";Fs::create_directories(models);const auto model=models/L"new.safetensors";std::ofstream{model,std::ios::binary}<<"abc";
    auto receipt=interruptedPreparation(fixture,context);receipt["installed"].push_back({{"target",pathText(model).value()},{"kind","model"},{"sha256","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}});receipt["snapshot"]={{"runtime",{{"available",true},{"managed",true}}}};std::ofstream{transaction/L"manifest.json"}<<receipt.dump();
    const auto python=fixture.root/L"portable"/L"python_embeded";std::ofstream{python/L"fixture-pause-fail-inventory.txt"}<<"wait for live provider";
    std::unique_ptr<LoopbackArtifactServer> provider;std::exception_ptr observationFailure;
    std::jthread runtimeArrival{[&](std::stop_token stop){try{
        for(unsigned attempt=0U;attempt<1000U&&!stop.stop_requested();++attempt){if(Fs::is_regular_file(python/L"fixture-inventory-reached.txt")){
            const auto port=static_cast<unsigned short>(std::stoul(fixture.config.endpoint.substr(fixture.config.endpoint.rfind(':')+1U)));
            provider=std::make_unique<LoopbackArtifactServer>(std::vector<std::byte>{},false,200U,Json{{"queue_running",Json::array({Json::array({1,"unrelated",Json::object()})})},{"queue_pending",Json::array()}},port);fixture.sealCurrentOwner();std::ofstream{python/L"fixture-inventory-release.txt"}<<"provider is busy";return;}
            std::this_thread::sleep_for(std::chrono::milliseconds{10});}
        require(false,"Preparation did not reach the external inventory barrier.");
    }catch(...){observationFailure=std::current_exception();std::ofstream{python/L"fixture-inventory-release.txt"}<<"fixture failed";}}};
    const auto response=fixture.call("prepare",Json::object(),context);runtimeArrival.request_stop();runtimeArrival.join();if(observationFailure)std::rethrow_exception(observationFailure);
    const auto failed=Json::parse(take(response));require(failed.at("ok")==false&&failed.at("error").at("code").get<std::string>()==Domain::ErrorCodes::HostCapabilityUnavailable,"Busy rollback fixture did not fail at external package inventory.");
    require(Fs::is_regular_file(model)&&NativeTools::Windows::ComfyDetail::fileFacts(model,context).at("sha256")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","Failed provider stop moved an unchanged active dependency during rollback.");
    bool retained=false,suppressed=false;for(const auto& item:failed.at("manifest").at("rollback")){if(item.value("path",std::string{})==pathText(model).value())retained=item.value("unreconciled",false)&&!item.value("moved",true);if(item.contains("prior_runtime_restored"))suppressed=item.at("prior_runtime_restored")==false&&item.contains("reason");}
    require(provider&&retained&&suppressed&&provider->prompts==0U&&provider->views==0U&&!provider->unexpectedRequest&&!Fs::exists(fixture.root/L"runtime"/L"runtime.log"),"Busy rollback failed to retain evidence or disrupted unrelated provider work.");
}
void malformedProviderQueueCannotAuthorizeStopOrPreparationMutation() {
    const std::array<Json,5> invalid{{Json::object(),Json{{"queue_pending",Json::array()}},Json{{"queue_running",Json::array()}},Json{{"queue_running","unknown"},{"queue_pending",Json::array()}},Json{{"queue_running",Json::array()},{"queue_pending",nullptr}}}};
    for(const auto& queue:invalid){
        Fixture fixture;permitPreparation(fixture);LoopbackArtifactServer provider{{},false,200U,queue};fixture.config.endpoint=provider.endpoint();fixture.sealCurrentOwner();const auto context=TestContext{}.active();
        const auto status=Json::parse(take(fixture.call("status",Json::object(),context)));require(status.at("available")==false&&status.at("owner_alive")==true&&status.at("error").at("code").get<std::string>()==Domain::ErrorCodes::MalformedMessage,"Malformed queue reported provider readiness or lost its format error.");
        requireError(fixture.call("request",{{"method","GET"},{"route","/queue"}},context),Domain::ErrorCodes::MalformedMessage,"Shared provider boundary accepted incomplete or non-array queue fields.");
        requireError(fixture.call("cancel",{{"prompt_id","owned-prompt"},{"graph",Json::object()}},context),Domain::ErrorCodes::MalformedMessage,"Malformed queue falsely confirmed exact remote cancellation.");
        requireError(fixture.call("control",{{"action","stop"}},context),Domain::ErrorCodes::Conflict,"Malformed queue authorized stopping the live owned fixture process.");
        const auto transaction=preparationDirectory(fixture,context),models=fixture.root/L"portable"/L"ComfyUI"/L"models"/L"checkpoints";Fs::create_directories(models);const auto model=models/L"new.safetensors";std::ofstream{model,std::ios::binary}<<"abc";
        const auto pth=fixture.root/L"portable"/L"python_embeded"/L"python313._pth";const std::string original{"python313.zip\r\n"},replacement{"active-overlay\r\n"+original};std::ofstream{pth,std::ios::binary}<<replacement;
        const auto input=fixture.root/L"runtime"/L"inputs"/L"sealed-input.bin";std::ofstream{input,std::ios::binary}<<"input evidence";const auto inputFacts=NativeTools::Windows::ComfyDetail::fileFacts(input,context);
        auto receipt=interruptedPreparation(fixture,context);receipt["installed"].push_back({{"target",pathText(model).value()},{"kind","model"},{"sha256","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}});receipt["python_path_backup"]={{"path",pathText(pth).value()},{"original",original},{"replacement",replacement}};std::ofstream{transaction/L"manifest.json"}<<receipt.dump();
        const auto failed=Json::parse(take(fixture.call("prepare",Json::object(),context)));std::ifstream preserved{pth,std::ios::binary};const std::string bytes{std::istreambuf_iterator<char>{preserved},{}};
        require(failed.at("ok")==false&&failed.at("error").at("code").get<std::string>()==Domain::ErrorCodes::Conflict&&bytes==replacement&&Fs::is_regular_file(model)&&NativeTools::Windows::ComfyDetail::fileFacts(input,context)==inputFacts,"Malformed queue permitted search-path/dependency/input mutation during preparation.");
        require(provider.prompts==0U&&provider.views==0U&&!provider.unexpectedRequest&&!Fs::exists(fixture.root/L"runtime"/L"runtime.log"),"Malformed queue handling mutated provider work or launched another process.");
    }
}
void automaticSetupDisabledReusesBaselineAndBlocksMissingPackagesBeforePublisherAccess() {
    for(const bool recovered:{false,true})for(const bool compatible:{false,true}){
        Fixture fixture;permitPreparation(fixture);fixture.config.automaticSetup=false;const auto context=TestContext{}.active();const auto transaction=preparationDirectory(fixture,context);
        const auto node=fixture.root/L"portable"/L"forge-managed"/L"custom_nodes"/L"FixtureNode";Fs::create_directories(node);std::ofstream{node/L"nodes.py"}<<"publisher source\n";std::ofstream{node/L"requirements.txt"}<<"fixture-package>=1\n";
        const auto tree=NativeTools::Windows::ComfyDetail::directoryFacts(node,1024ULL*1024ULL,context);const std::string sha(64U,'a');std::ofstream{node/L".forge-dependency.json"}<<Json{{"sha256",sha},{"tree",tree}}.dump();
        const Json dependency{{"kind","custom_node"},{"url","https://example.com/not-downloaded"},{"target","FixtureNode"},{"sha256",sha},{"revision",std::string(40U,'b')},{"archive",true}};
        Json arguments{{"dependencies",Json::array({dependency})}};if(recovered){arguments=Json::object();auto receipt=interruptedPreparation(fixture,context);receipt["state"]="failed";
            receipt["installed"].push_back({{"target",pathText(node).value()},{"kind","custom_node"},{"sha256",sha},{"tree",tree},{"reused",true}});receipt["resolution"]={{"dependencies",Json::array({dependency})},{"requirements",Json::array({"fixture-package>=1"})}};std::ofstream{transaction/L"manifest.json"}<<receipt.dump();}
        Json packages=Json::object();if(compatible)packages["fixture-package"]={{"version","1.0"},{"requires",Json::array()}};
        const Json responses{{"inventory",{{"python_version","3.13.11"},{"tags",Json::array({"cp313-cp313-win_amd64"})},{"packages",packages}}},
            {"requirements",Json::array({Json{{"name","fixture-package"},{"requirement","fixture-package>=1"},{"specifier",">=1"},{"extras",Json::array()}}})},
            {"check",{{"compatible",compatible},{"conflicts",compatible?Json::array():Json::array({"fixture-package>=1 (selected None)"})}}}};
        std::ofstream{fixture.root/L"portable"/L"python_embeded"/L"fixture-package-contracts.json"}<<responses.dump();
        const auto failed=Json::parse(take(fixture.call("prepare",arguments,context)));const auto& manifest=failed.at("manifest");require(failed.at("ok")==false,"Fixture external provider unexpectedly passed readiness.");
        const auto expected=compatible?Domain::ErrorCodes::HostCapabilityUnavailable:Domain::ErrorCodes::Unauthorized;require(failed.at("error").at("code").get<std::string>()==expected,"Automatic-setup policy blocked compatible baseline reuse or permitted a missing package.");
        require(failed.at("downloaded_bytes")==0ULL&&manifest.at("downloads").empty()&&manifest.at("metadata").empty()&&!manifest.contains("package_overlay")&&!Fs::exists(transaction/L"packages-install.log"),"Disabled automatic setup reached publisher transfer or package activation.");
        require(NativeTools::Windows::ComfyDetail::directoryFacts(node,1024ULL*1024ULL,context)==tree&&manifest.at("installed").size()==1U&&manifest.at("installed")[0].at("reused")==true,"Disabled automatic setup mutated or failed to reuse the sealed installed node.");
        if(compatible)require(manifest.at("package_resolution").at("baseline_compatible")==true&&manifest.at("package_resolution").at("selected").empty(),"Compatible baseline package requirement did not pass preparation without selecting wheels.");
        else require(!Fs::exists(fixture.root/L"runtime"/L"runtime.log"),"Missing-package rejection attempted a provider or package process activation.");
    }
    Fixture fixture;permitPreparation(fixture);fixture.config.automaticSetup=false;const auto context=TestContext{}.active();const auto transaction=preparationDirectory(fixture,context);Fs::create_directories(transaction/L"wheels");const auto wheel=transaction/L"wheels"/L"existing.whl";std::ofstream{wheel,std::ios::binary}<<"abc";
    const Json dependency{{"kind","package"},{"url","https://example.com/not-downloaded"},{"target","existing.whl"},{"sha256","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}};
    const auto failed=Json::parse(take(fixture.call("prepare",{{"dependencies",Json::array({dependency})}},context)));
    require(failed.at("ok")==false&&failed.at("error").at("code").get<std::string>()==Domain::ErrorCodes::Unauthorized&&failed.at("downloaded_bytes")==0ULL&&!failed.at("manifest").contains("package_overlay")&&!Fs::exists(transaction/L"packages-install.log")&&Fs::file_size(wheel)==3ULL,"Disabled automatic setup activated a retained explicit wheel.");
}
void installedDependencySealsAreIndependentOfTransferBudget() {
    for(const bool recovered:{false,true}){
        Fixture fixture;permitPreparation(fixture);fixture.config.downloadBudgetBytes=1ULL;const auto context=TestContext{}.active();const auto transaction=preparationDirectory(fixture,context);
        const auto node=fixture.root/L"portable"/L"forge-managed"/L"custom_nodes"/L"FixtureNode";Fs::create_directories(node);std::ofstream{node/L"nodes.py",std::ios::binary}<<std::string(128U*1024U,'x');
        const auto tree=NativeTools::Windows::ComfyDetail::directoryFacts(node,1024ULL*1024ULL,context);const std::string sha(64U,'a');std::ofstream{node/L".forge-dependency.json"}<<Json{{"sha256",sha},{"tree",tree}}.dump();
        Json arguments{{"dependencies",Json::array({{{"kind","custom_node"},{"url","https://example.com/not-downloaded"},{"target","FixtureNode"},{"sha256",sha},{"revision",std::string(40U,'b')}}})}};
        if(recovered){arguments=Json::object();auto receipt=interruptedPreparation(fixture,context);receipt["installed"].push_back({{"target",pathText(node).value()},{"kind","custom_node"},{"sha256",sha},{"tree",tree}});std::ofstream{transaction/L"manifest.json"}<<receipt.dump();}
        const auto failed=Json::parse(take(fixture.call("prepare",arguments,context)));require(failed.at("ok")==false&&failed.at("error").at("code").get<std::string>()==Domain::ErrorCodes::HostCapabilityUnavailable&&failed.at("downloaded_bytes")==0ULL,"Installed tree verification consumed or was limited by transfer budget.");
        if(!recovered)require(Fs::is_directory(node)&&failed.at("manifest").at("installed")[0].at("reused")==true,"Small transfer limit prevented existing sealed-node reuse.");
        else{bool retained=false;for(const auto& item:failed.at("manifest").at("rollback"))if(item.value("path",std::string{})==pathText(node).value()&&item.value("moved",false))retained=NativeTools::Windows::ComfyDetail::directoryFacts(Fs::path{item.at("retained").get<std::string>()},1024ULL*1024ULL,context)==tree;require(retained,"Recovered activation rollback used the download limit for content verification.");}
    }
}
void registryNodeDiscoveryAlsoResolvesSelectedModelInSamePreparation() {
    using namespace NativeTools::Windows::ComfyDetail;
    Fixture fixture;permitPreparation(fixture);const auto context=TestContext{}.active();const auto transaction=preparationDirectory(fixture,context);
    LoopbackArtifactServer server{{},false,200U,{{"queue_running",Json::array()},{"queue_pending",Json::array({Json::array({1,"unrelated",Json::object()})})}}};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
    const auto manager=fixture.root/L"portable"/L"ComfyUI"/L"custom_nodes"/L"ComfyUI-Manager";Fs::create_directory(manager);
    writeJson(manager/L"model-list.json",{{"models",Json::array({{{"filename","fixture.gguf"},{"url","https://huggingface.co/fixture/publisher/resolve/main/fixture.gguf"},{"save_path","text_encoders/t5"}}})}});
    auto receipt=interruptedPreparation(fixture,context);receipt["metadata"]=Json::object();Infrastructure::Windows::BCryptSha256Hasher hasher;unsigned index{};
    const auto retain=[&](const std::string& url,const Json& data){const auto key=take(hasher.sha256(std::as_bytes(std::span{url.data(),url.size()}))).value();const auto path=transaction/(L"publisher-"+std::to_wstring(index++)+L".metadata.json");writeJson(path,{{"status",200U},{"data",data}});receipt["metadata"][key]={{"path",pathText(path).value()},{"sha256",fileFacts(path,context).at("sha256")},{"url",url}};};
    retain("https://api.comfy.org/comfy-nodes/UninstalledRegistryNode/node",{{"id","fixture-registry-package"},{"status","NodeStatusActive"},{"publisher",{{"status","PublisherStatusActive"}}},{"repository","https://github.com/fixture/publisher"},{"latest_version",{{"version","1.0.0"}}}});
    retain("https://api.comfy.org/nodes/fixture-registry-package/install?version=1.0.0",{{"id","fixture-version-id"},{"version","1.0.0"},{"status","NodeVersionStatusActive"},{"deprecated",false},{"downloadUrl","https://cdn.comfy.org/fixture-package.zip"},{"dependencies",Json::array()}});
    const std::string revision(40U,'b'),sha(64U,'a');retain("https://huggingface.co/api/models/fixture/publisher",{{"sha",revision}});
    retain("https://huggingface.co/api/models/fixture/publisher/tree/"+revision,Json::array({{{"path","fixture.gguf"},{"size",3ULL},{"lfs",{{"oid",sha}}}}}));writeJson(transaction/L"manifest.json",receipt);
    const Json graph{{"1",{{"class_type","UninstalledRegistryNode"},{"inputs",{{"any_model_selector",{{"__value__","fixture.gguf"}}}}}}}};
    const auto result=Json::parse(take(fixture.call("prepare",{{"workflow",graph}},context)));const auto& manifest=result.at("manifest");
    require(result.at("ok")==false&&result.at("error").at("code").get<std::string>()==Domain::ErrorCodes::Conflict,"Busy-provider fixture unexpectedly activated a resolved dependency.");
    const auto& resolved=manifest.at("resolution").at("dependencies");require(resolved.size()==2U&&resolved[0].at("kind")=="custom_node"&&resolved[0].at("registry_version")=="1.0.0"&&resolved[1].at("kind")=="model"&&resolved[1].at("target")=="text_encoders/t5/fixture.gguf"&&resolved[1].at("sha256")==sha&&resolved[1].at("provenance").at("revision")==revision,"Registry success skipped same-operation model discovery or lost its actual format/storage/publisher pin.");
    require(manifest.at("downloads").empty()&&result.at("downloaded_bytes")==0ULL&&server.prompts==0U&&!server.unexpectedRequest&&!Fs::exists(fixture.root/L"portable"/L"forge-managed"),"Pinned-discovery fixture contacted another publisher, downloaded content, or changed active installation files.");
}
void declaredModelSelectorsCheckFilesAndPreserveAllUploadContracts() {
    for(const bool installed:{false,true}){
        Fixture fixture;permitPreparation(fixture);fixture.config.automaticSetup=false;const auto context=TestContext{}.active();
        Json required{{"weights",Json::array({Json::array({"fixture.gguf"})})},{"prompt",Json::array({"STRING"})}};
        for(const auto* flag:{"image_upload","audio_upload","video_upload"})required[flag]=Json::array({Json::array({"root-file.bin"}),Json{{flag,true}}});
        const Json schemas{{"GenericSource",{{"input",{{"required",required}}}}}};LoopbackArtifactServer server{{},false,200U,{{"queue_running",Json::array()},{"queue_pending",Json::array({Json::array({1,"unrelated",Json::object()})})}},0U,{},schemas};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
        const auto manager=fixture.root/L"portable"/L"ComfyUI"/L"custom_nodes"/L"ComfyUI-Manager";Fs::create_directory(manager);std::ofstream{manager/L"model-list.json"}<<Json{{"models",Json::array({{{"filename","fixture.gguf"},{"url","https://huggingface.co/fixture/never-contacted/resolve/main/fixture.gguf"},{"save_path","text_encoders"}},{{"filename","input.bin"},{"url","https://huggingface.co/fixture/never-contacted/resolve/main/input.bin"},{"save_path","other"}}})}}.dump();
        if(installed){const auto models=fixture.root/L"portable"/L"ComfyUI"/L"models"/L"text_encoders";Fs::create_directories(models);std::ofstream{models/L"fixture.gguf"}<<"abc";}
        Json inputs{{"weights",{{"__value__","fixture.gguf"}}},{"prompt","input.bin"}};for(const auto* flag:{"image_upload","audio_upload","video_upload"})inputs[flag]={{"__value__","forge/input.bin"}};
        const auto result=Json::parse(take(fixture.call("prepare",{{"workflow",{{"1",{{"class_type","GenericSource"},{"inputs",inputs}}}}}},context)));const auto& manifest=result.at("manifest");
        if(installed)require(result.at("ok")==true&&manifest.at("resolution").at("dependencies").empty()&&manifest.at("unresolved").empty()&&manifest.at("warnings").size()==3U,"Installed generic model or declared private image/audio/video upload contract failed readiness.");
        else require(result.at("ok")==false&&result.at("error").at("code").get<std::string>()==Domain::ErrorCodes::Unauthorized,"Static model enum was treated as proof that its absent file exists.");
        require(result.at("downloaded_bytes")==0ULL&&manifest.at("downloads").empty()&&manifest.at("metadata").empty()&&server.prompts==0U&&!server.unexpectedRequest,"Selector/upload inspection reached publishers or generation.");
    }
}
Json missingPublisherReceipt(Fixture& fixture,const Domain::OperationContext& context,const std::string& className){
    using namespace NativeTools::Windows::ComfyDetail;const auto transaction=preparationDirectory(fixture,context);auto receipt=interruptedPreparation(fixture,context);receipt["metadata"]=Json::object();Infrastructure::Windows::BCryptSha256Hasher hasher;unsigned index{};
    const auto retain=[&](const std::string& url,const Json& data){const auto key=take(hasher.sha256(std::as_bytes(std::span{url.data(),url.size()}))).value();const auto path=transaction/(L"publisher-reuse-"+std::to_wstring(index++)+L".metadata.json");writeJson(path,{{"status",200U},{"data",data}});receipt["metadata"][key]={{"path",pathText(path).value()},{"sha256",fileFacts(path,context).at("sha256")},{"url",url}};};
    retain("https://api.comfy.org/comfy-nodes/"+className+"/node",{{"id","fixture-registry-package"},{"status","NodeStatusActive"},{"publisher",{{"id","fixture-publisher"},{"status","PublisherStatusActive"}}},{"repository","https://github.com/fixture/publisher"},{"latest_version",{{"version","2.0.0"}}}});
    retain("https://api.comfy.org/nodes/fixture-registry-package/install?version=2.0.0",{{"id","fixture-newer-version-id"},{"version","2.0.0"},{"status","NodeVersionStatusActive"},{"deprecated",false},{"downloadUrl","https://cdn.comfy.org/fixture-newer-package.zip"},{"dependencies",Json::array()}});return receipt;
}
Fs::path installedPublisherFixtureSource(Fixture& fixture,const std::string& variation,const Domain::OperationContext& context){
    using namespace NativeTools::Windows::ComfyDetail;const auto source=fixture.root/L"portable"/L"ComfyUI"/L"custom_nodes"/L"Different Source Name";Fs::create_directory(source);std::ofstream{source/L"nodes.py"}<<"# existing publisher source\n";std::ofstream{source/L"requirements.txt"}<<"fixture-lazy>=1\n";const std::string commit(40U,'b');
    if(variation=="registry"||variation=="no_tracking"){
        std::ofstream{source/L"pyproject.toml"}<<"[project]\nname='fixture-registry-package'\nversion='1.0.0'\ndependencies=[]\n[project.urls]\nRepository='https://github.com/fixture/publisher'\n[tool.comfy]\nPublisherId='fixture-publisher'\n";
        if(variation=="registry")std::ofstream{source/L".tracking"}<<"nodes.py\nrequirements.txt\npyproject.toml\n";
    }else if(variation=="marker"||variation=="marker_edit"){
        const auto tree=directoryFacts(source,1024ULL*1024ULL,context);writeJson(source/L".forge-dependency.json",{{"sha256",std::string(64U,'a')},{"tree",tree},{"revision",commit},{"url","https://codeload.github.com/fixture/publisher/zip/"+commit},{"provenance",{{"kind","identified_publisher_commit"},{"repository","https://github.com/fixture/publisher"},{"revision",commit}}}});
        if(variation=="marker_edit")std::ofstream{source/L"nodes.py",std::ios::trunc}<<"# edited publisher content\n";
    }else{
        Fs::create_directory(source/L".git");const auto origin=variation=="wrong_origin"?"https://github.com/other/publisher.git":variation=="detached"?"git@github.com:Fixture/Publisher.git":variation=="packed"?"ssh://git@github.com/fixture/publisher.git":"https://github.com/Fixture/Publisher.git";std::ofstream{source/L".git"/L"config"}<<"[core]\n repositoryformatversion = 0\n[remote \"origin\"]\n url = "<<origin<<"\n";
        if(variation=="detached")std::ofstream{source/L".git"/L"HEAD"}<<commit<<"\n";
        else{std::ofstream{source/L".git"/L"HEAD"}<<"ref: refs/heads/main\n";if(variation=="packed")std::ofstream{source/L".git"/L"packed-refs"}<<"# pack-refs with: peeled fully-peeled sorted\n"<<std::string(40U,'a')<<" refs/remotes/origin/other\n"<<commit<<" refs/heads/main\n^"<<std::string(40U,'c')<<"\n";else{Fs::create_directories(source/L".git"/L"refs"/L"heads");std::ofstream{source/L".git"/L"refs"/L"heads"/L"main"}<<commit<<"\n";}}
    }
    writeJson(fixture.root/L"portable"/L"python_embeded"/L"fixture-package-contracts.json",{{"inventory",{{"python_version","3.13.11"},{"tags",Json::array({"py3-none-any"})},{"packages",{{"fixture-lazy",{{"version","1.0"},{"requires",Json::array()}}}}}}},{"requirements",Json::array({{{"name","fixture-lazy"},{"requirement","fixture-lazy>=1"},{"extras",Json::array()},{"specifier",">=1"}}})},{"resolve",{{"selected",Json::object()},{"requirements",Json::array({"fixture-lazy>=1"})},{"versions",{{"fixture-lazy","1.0"}}}}},{"check",{{"compatible",true},{"conflicts",Json::array()}}},{"project_requirements",Json::array()},{"node_project_identity",{{"id","fixture-registry-package"},{"version","1.0.0"},{"repository","https://github.com/fixture/publisher"},{"publisher_id","fixture-publisher"}}}});return source;
}
void missingImportedNodesReuseExactInstalledPublisherSource(){
    using namespace NativeTools::Windows::ComfyDetail;for(const auto* variation:{"loose","detached","packed","registry","marker"}){
        Fixture fixture;permitPreparation(fixture);const auto context=TestContext{}.active();Json graph{{"1",{{"class_type","MissingPublisherNode"},{"inputs",Json::object()}}}};if(std::string_view{variation}=="loose")graph["2"]=graph.at("1");const auto source=installedPublisherFixtureSource(fixture,variation,context);const auto before=directoryFacts(source,1024ULL*1024ULL,context);
        LoopbackArtifactServer server{{},false,200U,{{"queue_running",Json::array()},{"queue_pending",Json::array({Json::array({1,"unrelated",Json::object()})})}}};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();writeJson(preparationDirectory(fixture,context)/L"manifest.json",missingPublisherReceipt(fixture,context,"MissingPublisherNode"));
        const auto result=Json::parse(take(fixture.call("prepare",{{"workflow",graph}},context)));const auto& manifest=result.at("manifest");require(result.at("ok")==false&&result.at("error").at("code").get<std::string>()==Domain::ErrorCodes::HostCapabilityUnavailable&&manifest.at("unresolved").size()==graph.size(),"Still-missing imported class was falsely declared ready after installed publisher reuse.");
        const auto& resolution=manifest.at("resolution");require(resolution.at("dependencies").empty()&&resolution.at("installed_node_directories")==Json::array({pathText(source).value()})&&resolution.at("installed_node_sources").size()==1U,"Import-failed node source was replaced, duplicated or omitted from its durable binding.");const auto& binding=resolution.at("installed_node_sources")[0];require(binding.at("path")==pathText(source).value()&&binding.at("kind")=="directory"&&binding.at("tree")==before&&!binding.at("identity_files").empty(),"Installed source binding lost its exact path/content/metadata evidence.");
        const auto& identity=binding.at("publisher_identity");if(std::string_view{variation}=="registry")require(identity.at("kind")=="installed_registry_version"&&identity.at("version")=="1.0.0"&&identity.at("package_id")=="fixture-registry-package","Existing Registry version was upgraded to the advertised latest package.");else if(std::string_view{variation}=="marker")require(identity.at("kind")=="forge_manifest"&&identity.at("revision")==std::string(40U,'b'),"Existing Forge publisher manifest lost its installed pin.");else require(identity.at("kind")=="installed_git_revision"&&identity.at("revision")==std::string(40U,'b')&&identity.at("repository")=="https://github.com/fixture/publisher","Installed Git loose/detached/packed revision did not match the identified repository exactly.");
        require(manifest.at("requirement_files").size()==1U&&manifest.at("package_resolution").at("baseline_compatible")==true&&manifest.at("package_resolution").at("selected").empty()&&manifest.at("installed").empty()&&manifest.at("downloads").empty()&&result.at("downloaded_bytes")==0ULL&&!manifest.contains("package_overlay")&&!Fs::exists(fixture.root/L"portable"/L"forge-managed")&&directoryFacts(source,1024ULL*1024ULL,context)==before&&server.prompts==0U&&!server.unexpectedRequest,"Installed publisher package repair skipped declarations, queried downloads, changed source, or interrupted unrelated work.");
    }
}
void installedPublisherReuseRefusesAmbiguousOrUnprovenSources(){
    using namespace NativeTools::Windows::ComfyDetail;for(const auto* variation:{"wrong_origin","no_tracking","duplicate","marker_edit","registry_origin_conflict"}){
        Fixture fixture;permitPreparation(fixture);const auto context=TestContext{}.active();const auto source=installedPublisherFixtureSource(fixture,std::string_view{variation}=="duplicate"?"loose":std::string_view{variation}=="registry_origin_conflict"?"registry":variation,context);if(std::string_view{variation}=="duplicate")Fs::copy(source,source.parent_path()/L"Second Publisher Copy",Fs::copy_options::recursive);if(std::string_view{variation}=="registry_origin_conflict"){Fs::create_directory(source/L".git");std::ofstream{source/L".git"/L"config"}<<"[remote \"origin\"]\nurl=https://github.com/other/publisher.git\n";}
        LoopbackArtifactServer server{{},false,200U,{{"queue_running",Json::array()},{"queue_pending",Json::array({Json::array({1,"unrelated",Json::object()})})}}};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();writeJson(preparationDirectory(fixture,context)/L"manifest.json",missingPublisherReceipt(fixture,context,"MissingPublisherNode"));
        const auto result=Json::parse(take(fixture.call("prepare",{{"workflow",{{"1",{{"class_type","MissingPublisherNode"},{"inputs",Json::object()}}}}}},context)));const auto& manifest=result.at("manifest");const auto code=result.at("error").at("code").get<std::string>();require(result.at("ok")==false&&code==(std::string_view{variation}=="marker_edit"?Domain::ErrorCodes::IntegrityFailure:Domain::ErrorCodes::Conflict),"Unproven/ambiguous installed source was reused or activated.");
        if(std::string_view{variation}=="wrong_origin"||std::string_view{variation}=="no_tracking")require(manifest.at("resolution").at("installed_node_sources").empty()&&manifest.at("resolution").at("dependencies").size()==1U,"Wrong Git publisher or untracked project metadata was treated as an installed publisher pin.");else require(!manifest.contains("resolution"),"Ambiguous/edited publisher sources reached durable archive admission.");
        require(manifest.at("downloads").empty()&&manifest.at("installed").empty()&&result.at("downloaded_bytes")==0ULL&&!Fs::exists(fixture.root/L"portable"/L"forge-managed")&&server.prompts==0U&&!server.unexpectedRequest,"Publisher refusal changed the installation, downloaded a package or interrupted unrelated work.");
    }
}
void retainedPublisherBindingRejectsChangedSourceOrPin(){
    using namespace NativeTools::Windows::ComfyDetail;for(const auto* variation:{"source","ref","tracking"}){
        Fixture fixture;permitPreparation(fixture);const auto context=TestContext{}.active();const auto source=installedPublisherFixtureSource(fixture,std::string_view{variation}=="tracking"?"registry":"loose",context);LoopbackArtifactServer server{{},false,200U,{{"queue_running",Json::array()},{"queue_pending",Json::array({Json::array({1,"unrelated",Json::object()})})}}};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();writeJson(preparationDirectory(fixture,context)/L"manifest.json",missingPublisherReceipt(fixture,context,"MissingPublisherNode"));const Json request{{"workflow",{{"1",{{"class_type","MissingPublisherNode"},{"inputs",Json::object()}}}}}};
        const auto first=Json::parse(take(fixture.call("prepare",request,context)));require(first.at("manifest").at("resolution").at("installed_node_sources").size()==1U&&first.at("manifest").at("unresolved").size()==1U,"Publisher restart fixture did not retain its exact installed source binding.");const auto resolution=first.at("manifest").at("resolution");
        const auto changed=std::string_view{variation}=="source"?source/L"nodes.py":std::string_view{variation}=="ref"?source/L".git"/L"refs"/L"heads"/L"main":source/L".tracking";std::ofstream{changed,std::ios::trunc}<<(std::string_view{variation}=="ref"?std::string(40U,'c')+"\n":"changed retained source evidence\n");fixture.backend->shutdown();fixture.backend=std::make_unique<NativeTools::Windows::WindowsComfyUiBackend>(*fixture.issuer,pathText(fixture.root/L"runtime"));
        requireError(fixture.call("prepare",request,context),Domain::ErrorCodes::IntegrityFailure,"Resume silently refreshed an edited source/Git ref/Registry tracking seal.");const auto retained=readJson(preparationDirectory(fixture,context)/L"manifest.json");require(retained.at("resolution")==resolution&&retained.at("installed").empty()&&retained.at("downloads").empty()&&retained.at("downloaded_bytes")==0ULL&&!Fs::exists(fixture.root/L"portable"/L"forge-managed")&&server.prompts==0U&&!server.unexpectedRequest,"Changed retained publisher binding changed the saved resolution, reached preparation effects or submitted another generation.");
    }
}
void preparationIncludesConstraintsAndSetupRequirementsWithFileSeals() {
    using namespace NativeTools::Windows::ComfyDetail;
    Fixture fixture;permitPreparation(fixture);fixture.config.automaticSetup=false;const auto context=TestContext{}.active();const auto node=fixture.root/L"portable"/L"forge-managed"/L"custom_nodes"/L"DeclaredNode";Fs::create_directories(node);
    std::ofstream{node/L"nodes.py"}<<"# fixture";std::ofstream{node/L"requirements.txt"}<<"-r nested.txt\n-c constraints.txt\n";std::ofstream{node/L"nested.txt"}<<"fixture-package>=1\n";std::ofstream{node/L"constraints.txt"}<<"fixture-package<2\n";std::ofstream{node/L"setup.cfg"}<<"[options]\ninstall_requires =\n    fixture-package>=1\n";
    Json files=Json::array();for(const auto* name:{L"requirements.txt",L"nested.txt",L"constraints.txt"}){const auto path=node/name;const auto facts=fileFacts(path,context);files.push_back({{"path",pathText(path).value()},{"sha256",facts.at("sha256")},{"bytes",facts.at("bytes")}});}
    const auto tree=directoryFacts(node,1024ULL*1024ULL,context);const std::string sha(64U,'a');writeJson(node/L".forge-dependency.json",{{"sha256",sha},{"tree",tree}});
    const Json responses{{"inventory",{{"python_version","3.13.11"},{"tags",Json::array({"cp313-cp313-win_amd64"})},{"packages",{{"fixture-package",{{"version","1.0"},{"requires",Json::array()}}}}}}},
        {"requirements_file",{{"requirements",Json::array({"fixture-package>=1"})},{"constraints",Json::array({"fixture-package<2"})},{"files",files}}},{"setup_requirements",Json::array({"fixture-package>=1"})},
        {"requirements",Json::array({{{"name","fixture-package"},{"requirement","fixture-package>=1"},{"specifier",">=1"},{"extras",Json::array()}}})},{"check",{{"compatible",true},{"conflicts",Json::array()}}}};
    writeJson(fixture.root/L"portable"/L"python_embeded"/L"fixture-package-contracts.json",responses);
    const auto result=Json::parse(take(fixture.call("prepare",{{"dependencies",Json::array({{{"kind","custom_node"},{"url","https://fixture.invalid/never-contacted"},{"target","DeclaredNode"},{"sha256",sha},{"revision",std::string(40U,'b')}}})}},context)));const auto& manifest=result.at("manifest");
    require(result.at("ok")==false&&result.at("error").at("code").get<std::string>()==Domain::ErrorCodes::HostCapabilityUnavailable&&manifest.at("requirement_files")[0].at("files")==files&&manifest.at("requirement_files")[0].at("constraints")==Json::array({"fixture-package<2"})&&manifest.at("setup_requirements")[0].at("requirements")==Json::array({"fixture-package>=1"}),"Native preparation did not retain declared includes, constraints, setup requirements and their source seals.");
    require(manifest.at("package_resolution").at("baseline_compatible")==true&&manifest.at("package_resolution").at("selected").empty()&&result.at("downloaded_bytes")==0ULL&&manifest.at("metadata").empty()&&!manifest.contains("package_overlay")&&directoryFacts(node,1024ULL*1024ULL,context)==tree,"Compatible declared requirements performed publisher or package mutations.");
}
void installedWorkflowNodeDeclaredPackagesAreInspectedWithoutNodeReplacement() {
    using namespace NativeTools::Windows::ComfyDetail;
    for(const auto& [compatible,automatic]:std::array<std::pair<bool,bool>,3U>{{{false,false},{true,false},{true,true}}}){
        Fixture fixture;permitPreparation(fixture);fixture.config.automaticSetup=automatic;const auto context=TestContext{}.active();const auto node=fixture.root/L"portable"/L"ComfyUI"/L"custom_nodes"/L"ExistingNode";Fs::create_directory(node);std::ofstream{node/L"requirements.txt"}<<"fixture-lazy>=1\n";const auto facts=fileFacts(node/L"requirements.txt",context);
        const Json schemas{{"LazyNode",{{"python_module","custom_nodes.ExistingNode"},{"input",{{"required",Json::object()}}}}}};LoopbackArtifactServer server{{},false,200U,{{"queue_running",Json::array()},{"queue_pending",Json::array({Json::array({1,"unrelated",Json::object()})})}},0U,{},schemas};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
        Json packages=Json::object();if(compatible)packages["fixture-lazy"]={{"version","1.0"},{"requires",Json::array()}};
        writeJson(fixture.root/L"portable"/L"python_embeded"/L"fixture-package-contracts.json",{{"inventory",{{"python_version","3.13.11"},{"tags",Json::array({"py3-none-any"})},{"packages",packages}}},{"requirements_file",{{"requirements",Json::array({"fixture-lazy>=1"})},{"constraints",Json::array()},{"files",Json::array({{{"path",pathText(node/L"requirements.txt").value()},{"sha256",facts.at("sha256")},{"bytes",facts.at("bytes")}}})}}},{"requirements",Json::array({{{"name","fixture-lazy"},{"requirement","fixture-lazy>=1"},{"extras",Json::array()},{"specifier",">=1"}}})},{"check",{{"compatible",compatible},{"conflicts",compatible?Json::array():Json::array({"fixture-lazy>=1 (selected None)"})}}}});
        if(automatic){const auto contract=fixture.root/L"portable"/L"python_embeded"/L"fixture-package-contracts.json";auto responses=readJson(contract);responses["resolve"]={{"selected",Json::object()},{"requirements",Json::array({"fixture-lazy>=1"})},{"versions",{{"fixture-lazy","1.0"}}}};writeJson(contract,responses);}
        const auto result=Json::parse(take(fixture.call("prepare",{{"workflow",{{"1",{{"class_type","LazyNode"},{"inputs",Json::object()}}}}}},context)));const auto& manifest=result.at("manifest");
        require(result.at("ok")==compatible&&manifest.at("requirement_files").size()==1U&&manifest.at("resolution").at("dependencies").empty()&&manifest.at("resolution").at("installed_node_directories")==Json::array({pathText(node).value()}),"Installed workflow node's declared packages were skipped or its source was replaced.");
        if(!compatible)require(result.at("error").at("code").get<std::string>()==Domain::ErrorCodes::Unauthorized,"Missing installed-node package passed disabled automatic-setup policy.");
        require(result.at("downloaded_bytes")==0ULL&&manifest.at("metadata").empty()&&manifest.at("downloads").empty()&&manifest.at("installed").empty()&&!manifest.contains("package_overlay")&&fileFacts(node/L"requirements.txt",context).at("sha256")==facts.at("sha256")&&server.prompts==0U&&!server.unexpectedRequest,"Installed-node dependency inspection changed source, queried publishers or interrupted unrelated provider work.");
    }
}
void preparationBindsConfiguredCustomNodeDirectoriesAndSingleFilesWithoutParentRequirements() {
    using namespace NativeTools::Windows::ComfyDetail;
    for(const auto* variation:{"directory","file","ambiguous"}){
        Fixture fixture;permitPreparation(fixture);fixture.config.automaticSetup=false;const auto context=TestContext{}.active();const auto home=fixture.root/L"portable"/L"ComfyUI",external=fixture.root/L"external nodes";Fs::create_directory(external);
        const auto module="Publisher Nodes.v1";const auto node=external/std::wstring{L"Publisher Nodes.v1"},file=external/L"Publisher Nodes.v1.py";
        std::ofstream{home/L"extra_model_paths.yaml"}<<"operator:\n    custom_nodes: "<<Json(pathText(external).value()).dump()<<"\n";
        if(std::string_view{variation}!="file"){Fs::create_directory(node);std::ofstream{node/L"nodes.py"}<<"# publisher fixture\n";std::ofstream{node/L"requirements.txt"}<<"fixture-lazy>=1\n";}
        if(std::string_view{variation}!="directory")std::ofstream{file}<<"# single publisher fixture\n";
        std::ofstream{external/L"requirements.txt"}<<"must-never-infer-parent==99\n";
        const Json schemas{{"BoundNode",{{"python_module",std::string{"custom_nodes."}+module},{"input",{{"required",Json::object()}}}}}};
        LoopbackArtifactServer server{{},false,200U,{{"queue_running",Json::array()},{"queue_pending",Json::array({Json::array({1,"unrelated",Json::object()})})}},0U,{},schemas};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
        writeJson(fixture.root/L"portable"/L"python_embeded"/L"fixture-package-contracts.json",{{"inventory",{{"python_version","3.13.11"},{"tags",Json::array({"py3-none-any"})},{"packages",{{"fixture-lazy",{{"version","1.0"},{"requires",Json::array()}}}}}}},{"requirements",Json::array({{{"name","fixture-lazy"},{"requirement","fixture-lazy>=1"},{"extras",Json::array()},{"specifier",">=1"}}})},{"check",{{"compatible",true},{"conflicts",Json::array()}}}});
        const Json request{{"workflow",{{"1",{{"class_type","BoundNode"},{"inputs",Json::object()}}}}}};const auto result=Json::parse(take(fixture.call("prepare",request,context)));const auto& manifest=result.at("manifest");
        if(std::string_view{variation}=="ambiguous"){require(result.at("ok")==false&&result.at("error").at("code").get<std::string>()==Domain::ErrorCodes::Conflict&&!manifest.contains("resolution"),"Directory/single-file module ambiguity reached package resolution.");}
        else {require(result.at("ok")==true&&manifest.at("resolution").at("installed_node_sources").size()==1U,"Configured external custom node did not receive an exact durable source binding.");const auto& binding=manifest.at("resolution").at("installed_node_sources")[0];const bool directory=std::string_view{variation}=="directory";
            require(binding.at("path")==pathText(directory?node:file).value()&&binding.at("kind")== (directory?"directory":"file"),"Live module binding did not select its exact literal directory/file path.");
            if(directory)require(manifest.at("requirement_files").size()==1U&&manifest.at("requirement_files")[0].at("files")[0].at("path")==pathText(node/L"requirements.txt").value(),"External node directory declarations were not inspected.");
            else require(!manifest.contains("requirement_files")&&!manifest.contains("package_resolution")&&manifest.at("resolution").at("installed_node_directories").empty()&&binding.at("sha256")==fileFacts(file,context).at("sha256"),"Single-file custom node inferred its parent's declarations or lost its content seal.");
            rebindFixtureProviderAfterPreparation(fixture);
            const auto first=Json::parse(take(fixture.call("identity",{{"workflows",Json::array()}})));std::ofstream{directory?node/L"nodes.py":file,std::ios::app}<<"# changed publisher\n";const auto second=Json::parse(take(fixture.call("identity",{{"workflows",Json::array()}})));require(first.at("sha256")!=second.at("sha256"),"External node source edit did not invalidate dependency identity.");
            fixture.backend->shutdown();fixture.backend=std::make_unique<NativeTools::Windows::WindowsComfyUiBackend>(*fixture.issuer,pathText(fixture.root/L"runtime"));requireError(fixture.call("prepare",request,context),Domain::ErrorCodes::IntegrityFailure,"Resumed preparation adopted edited external directory/file source.");
        }
        require(manifest.at("downloads").empty()&&manifest.at("installed").empty()&&result.at("downloaded_bytes")==0ULL&&server.prompts==0U&&!server.unexpectedRequest,"Installed configured node reuse performed a publisher transfer/activation or disrupted unrelated work.");
    }
}
void preparationRejectsLegacyUnsealedCustomNodeSourceRecovery() {
    using namespace NativeTools::Windows::ComfyDetail;Fixture fixture;permitPreparation(fixture);const auto context=TestContext{}.active();const auto node=fixture.root/L"portable"/L"ComfyUI"/L"custom_nodes"/L"legacy node";Fs::create_directory(node);std::ofstream{node/L"nodes.py"}<<"# current source is not prior source proof\n";
    auto receipt=interruptedPreparation(fixture,context);receipt["state"]="failed";receipt["resolution"]={{"dependencies",Json::array()},{"installed_node_directories",Json::array({pathText(node).value()})}};writeJson(preparationDirectory(fixture,context)/L"manifest.json",receipt);
    const auto source=fileFacts(node/L"nodes.py",context);requireError(fixture.call("prepare",Json::object(),context),Domain::ErrorCodes::Conflict,"Legacy path-only resolution was labeled as verified prior source content.");require(fileFacts(node/L"nodes.py",context).at("sha256")==source.at("sha256")&&!Fs::exists(fixture.root/L"runtime"/L"runtime.log"),"Legacy source reconciliation refusal mutated source/runtime.");
}
void sourcePreparationReusesSealedBuildsAndRetainsInactiveFailures() {
    using namespace NativeTools::Windows::ComfyDetail;std::array<wchar_t,32768> tarPath{};const auto tarSize=SearchPathW(nullptr,L"tar.exe",nullptr,static_cast<DWORD>(tarPath.size()),tarPath.data(),nullptr);require(tarSize>0U&&tarSize<tarPath.size(),"Source preparation fixture requires installed Windows tar.");
    for(const auto* variation:{"completed_reuse","legacy_completed_reuse","foreign_operation_scope","source_edit","wheel_edit","archive_edit","failed_hook","missing_fresh_result","equivalent_version","incompatible_version"}){
        Fixture fixture;permitPreparation(fixture);const auto context=TestContext{}.active();const auto transaction=preparationDirectory(fixture,context),publisher=fixture.root/L"publisher";Fs::create_directories(publisher/L"fixture_source-1.0");std::ofstream{publisher/L"fixture_source-1.0"/L"pyproject.toml"}<<"[build-system]\nrequires = []\nbuild-backend = 'fixture_backend'\nbackend-path = ['.']\n";std::ofstream{publisher/L"fixture_source-1.0"/L"payload.txt"}<<std::string(256U*1024U,'x');
        const auto archive=transaction/L"retained-source.zip";require(runProcess(Fs::path{tarPath.data()},{"-a","-cf",pathText(archive).value(),"-C",pathText(publisher).value(),"fixture_source-1.0"},transaction,transaction/L"fixture-source-create.log",context).at("exit_code")==0,"Native source fixture could not create its retained publisher archive.");const auto archiveFacts=fileFacts(archive,context);fixture.config.downloadBudgetBytes=archiveFacts.at("bytes").get<std::uint64_t>();require(fixture.config.downloadBudgetBytes<256ULL*1024ULL,"Source fixture did not exercise independent expansion above its exhausted download budget.");
        Fs::create_directories(publisher/L"wheel"/L"fixture_source");std::ofstream{publisher/L"wheel"/L"fixture_source"/L"payload.txt"}<<"native orchestration fixture";const auto wheel=transaction/L"fixture-wheel.zip";require(runProcess(Fs::path{tarPath.data()},{"-a","-cf",pathText(wheel).value(),"-C",pathText(publisher/L"wheel").value(),"fixture_source"},transaction,transaction/L"fixture-wheel-create.log",context).at("exit_code")==0,"Native source fixture could not create its generated-wheel archive.");
        const Json descriptor{{"name","fixture-source"},{"version","1.0"},{"filename","fixture_source-1.0.zip"},{"url","https://files.pythonhosted.org/fixture/fixture_source-1.0.zip"},{"sha256",archiveFacts.at("sha256")},{"bytes",archiveFacts.at("bytes")}};auto receipt=interruptedPreparation(fixture,context);receipt["downloaded_bytes"]=archiveFacts.at("bytes");receipt["downloads"].push_back({{"url",descriptor.at("url")},{"path",pathText(archive).value()},{"sha256_expected",archiveFacts.at("sha256")},{"sha256",archiveFacts.at("sha256")},{"bytes",archiveFacts.at("bytes")},{"received_bytes",archiveFacts.at("bytes")},{"state","verified"}});writeJson(transaction/L"manifest.json",receipt);
        const auto node=fixture.root/L"portable"/L"ComfyUI"/L"custom_nodes"/L"ExistingSourceNode";Fs::create_directory(node);std::ofstream{node/L"requirements.txt"}<<"fixture-source==1.0\n";const Json schemas{{"SourceNode",{{"python_module","custom_nodes.ExistingSourceNode"},{"input",{{"required",Json::object()}}}}}};LoopbackArtifactServer server{{},false,200U,{{"queue_running",Json::array()},{"queue_pending",Json::array({Json::array({1,"unrelated",Json::object()})})}},0U,{},schemas};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
        const auto pth=fixture.root/L"portable"/L"python_embeded"/L"python313._pth";std::ofstream{pth}<<"python313.zip\n.\nimport site\n";const auto pthBefore=fileFacts(pth,context),environmentBefore=fileFacts(fixture.root/L"runtime"/L"active-environment.json",context);const Json inventory{{"python_version","3.13.11"},{"tags",Json::array({"py3-none-any"})},{"packages",Json::object()}};
        Json responses{{"inventory",inventory},{"requirements",Json::array({{{"name","fixture-source"},{"requirement","fixture-source==1.0"},{"extras",Json::array()},{"specifier","==1.0"}}})},{"check",{{"compatible",true},{"conflicts",Json::array()}}},{"build_configuration",{{"requirements",Json::array()},{"backend","fixture_backend"},{"backend_path",Json::array()}}},{"build_requires",{{"requirements",Json::array()}}},{"build_metadata",{{"requires_wheel",true}}},{"wheel_metadata",{{"name","fixture-source"},{"version","1.0"},{"requires_dist",Json::array()},{"requires_python",""}}},{"source_fixture",{{"descriptor",descriptor},{"wheel",pathText(wheel).value()},{"fail_hook",std::string_view{variation}=="failed_hook"},{"changed_inventory",{{"python_version","3.13.11"},{"tags",Json::array({"py3-none-any"})},{"packages",{{"external-change",{{"version","1.0"},{"requires",Json::array()}}}}}}}}}};writeJson(fixture.root/L"portable"/L"python_embeded"/L"fixture-package-contracts.json",responses);
        responses["version_equal"]={{"equal",std::string_view{variation}!="incompatible_version"}};if(std::string_view{variation}=="equivalent_version"){responses["source_fixture"]["descriptor"]["version"]="1.0.0";}if(std::string_view{variation}=="incompatible_version")responses["wheel_metadata"]["version"]="1.1";if(std::string_view{variation}=="missing_fresh_result"){responses["source_fixture"]["omit_result"]=true;for(unsigned index=0U;index<16U;++index)writeJson(transaction/(L"package-contract-"+std::to_wstring(index)+L".result.json"),{{"requirements",Json::array()}});}writeJson(fixture.root/L"portable"/L"python_embeded"/L"fixture-package-contracts.json",responses);if(std::string_view{variation}=="archive_edit")std::ofstream{archive,std::ios::binary|std::ios::trunc}<<"changed archive";
        const auto identityBefore=std::string_view{variation}=="completed_reuse"?Json::parse(take(fixture.call("identity",Json::object(),context))):Json::object();
        const Json arguments{{"workflow",{{"1",{{"class_type","SourceNode"},{"inputs",Json::object()}}}}}};const auto first=Json::parse(take(fixture.call("prepare",arguments,context)));const auto& manifest=first.at("manifest");require(first.at("ok")==false&&first.at("downloaded_bytes")==archiveFacts.at("bytes")&&manifest.at("downloads").size()==1U&&manifest.at("metadata").empty()&&server.prompts==0U&&!server.unexpectedRequest,"Native source preparation repeated a publisher transfer, reset its exhausted budget or submitted generation work.");
        require(fileFacts(pth,context)==pthBefore&&fileFacts(fixture.root/L"runtime"/L"active-environment.json",context)==environmentBefore&&!manifest.contains("python_path_backup")&&manifest.at("installed").empty(),"Inactive source build or failed wheel install changed the live interpreter/provider environment.");
        if(std::string_view{variation}=="archive_edit"){require(first.at("error").at("code").get<std::string>()==Domain::ErrorCodes::IntegrityFailure&&!manifest.at("source_builds").begin().value().contains("source"),"Source preparation accepted a changed retained publisher archive or ran its backend.");continue;}
        const auto& build=manifest.at("source_builds").begin().value();require(build.contains("source"),"Native source preparation failed before extraction completed: "+first.at("error").dump()+" build="+build.dump());require(build.at("expanded_reserve_bytes").get<std::uint64_t>()>fixture.config.downloadBudgetBytes&&Fs::is_regular_file(Fs::path{build.at("source").get<std::string>()}/L"payload.txt"),"Source extraction incorrectly used its exhausted transfer budget as an installed-content limit.");const auto sourcePath=Fs::path{build.at("source").get<std::string>()},attemptPath=Fs::path{build.at("attempt").get<std::string>()};require(sourcePath==attemptPath/L"project"&&contained(fixture.root/L"portable"/L"forge-builds.disabled"/wide(context.operationId.value()),attemptPath)&&!contained(transaction,attemptPath)&&sourcePath.native().size()<MAX_PATH&&(attemptPath/L"final-build-packages.disabled").native().size()<MAX_PATH,"Source hooks did not use the exact inactive installation workspace with bounded child working directories.");
        if(std::string_view{variation}=="failed_hook"){require(build.at("state")=="discovering_build_requirements"&&manifest.at("baseline_package_inventory_changed")==true&&manifest.at("baseline_package_inventory_unverified")==true&&!manifest.contains("package_overlay"),"Failed source hook concealed changed baseline inventory or entered runtime wheel activation.");continue;}
        if(std::string_view{variation}=="missing_fresh_result"){require(build.at("state")=="discovering_build_requirements"&&manifest.at("baseline_package_inventory_unverified")==false&&!manifest.contains("package_overlay"),"Successful child without a fresh hook result consumed a stale result or entered runtime installation.");const auto observed=readJson(fixture.root/L"portable"/L"python_embeded"/L"fixture-missing-result-query.json");const Fs::path resultPath{observed.at("result_path").get<std::string>()};require(contained(transaction,resultPath)&&!Fs::exists(resultPath)&&resultPath.filename().native().size()>60U&&readJson(transaction/L"package-contract-0.result.json")==Json{{"requirements",Json::array()}},"Fresh-result admission overwrote prior evidence or used the reset per-attempt result filename.");continue;}
        if(std::string_view{variation}=="incompatible_version"){require(first.at("error").at("code").get<std::string>()==Domain::ErrorCodes::IntegrityFailure&&build.at("state")=="building_wheel"&&!manifest.contains("package_overlay"),"Source construction accepted an incompatible generated wheel version or began runtime installation.");continue;}
        require(build.at("state")=="completed"&&manifest.at("baseline_package_inventory_unverified")==false&&build.at("source_tree").contains("payload.txt"),"Native source construction did not retain its source/build/wheel seals and exact post-hook inventory audit: variation="+std::string{variation}+" error="+first.at("error").dump().substr(0U,4096U)+" build="+build.dump().substr(0U,12288U));
        const Fs::path overlay{build.at("overlay").get<std::string>()};const auto clone=overlay/L".forge-python";const auto& interpreters=manifest.at("build_interpreters");require(interpreters.size()==1U,"Source construction staged an unexpected build interpreter inventory.");const auto& staged=interpreters.begin().value();
        require(staged.at("embedded")==true&&staged.at("python")==pathText(clone/L"python.exe").value()&&staged.at("provider_path_configuration")==Json::array({pthBefore})&&staged.at("components").size()==3U,"Source construction did not bind its exact cloned executable and baseline path configuration.");
        Json expectedOverlay=Json::object();for(const auto& component:staged.at("components")){const auto& facts=component.contains("copy")?component.at("copy"):component.at("generated");const Fs::path path{facts.at("path").get<std::string>()};require(contained(clone,path)&&fileFacts(path,context)==facts,"Retained build interpreter component lost its actual content seal.");const auto relative=pathText(Fs::relative(path,overlay)).value();require(!expectedOverlay.contains(relative),"Retained build interpreter repeats a component path.");expectedOverlay[relative]={{"sha256",facts.at("sha256")},{"bytes",facts.at("bytes")}};}
        require(build.at("overlay_tree")==expectedOverlay&&directoryFacts(overlay,64ULL*1024ULL*1024ULL,context)==expectedOverlay&&fileFacts(clone/L"python.exe",context).at("sha256")==fileFacts(fixture.root/L"portable"/L"python_embeded"/L"python.exe",context).at("sha256"),"Source build overlay included unsealed dependencies or lost its exact staged interpreter content.");
        const auto hooks=readJson(fixture.root/L"portable"/L"python_embeded"/L"fixture-hook-runtime.json");require(hooks.size()==3U,"Source construction did not execute all three native hook fixtures.");for(const auto* mode:{"build_requires","build_metadata","build_wheel"})require(hooks.at(mode).at("executable")==staged.at("python")&&hooks.at(mode).at("overlay")==build.at("overlay")&&hooks.at(mode).at("source")==build.at("source"),"A source hook bypassed the actual staged interpreter or its exact inactive project/overlay.");
        if(std::string_view{variation}=="equivalent_version"){require(build.at("descriptor").at("version")=="1.0.0"&&build.at("wheel").at("filename")=="fixture_source-1.0-py3-none-any.whl","Equivalent generated-wheel version changed its publisher source pin or wheel identity.");continue;}auto builtWheel=Fs::path{build.at("wheel").at("local_wheel").get<std::string>()};if(std::string_view{variation}=="source_edit")std::ofstream{Fs::path{build.at("source").get<std::string>()}/L"payload.txt",std::ios::trunc}<<"changed source";if(std::string_view{variation}=="wheel_edit")std::ofstream{builtWheel,std::ios::binary|std::ios::trunc}<<"changed wheel";
        if(std::string_view{variation}=="completed_reuse")require(Json::parse(take(fixture.call("identity",Json::object(),context))).at("sha256")==identityBefore.at("sha256"),"Inactive source-build files entered the live dependency identity before activation.");
        if(std::string_view{variation}=="legacy_completed_reuse"){auto retained=manifest;auto& oldBuild=retained["source_builds"].begin().value();const auto legacy=transaction/L"legacy-source-build";Fs::rename(attemptPath,legacy);oldBuild["attempt"]=pathText(legacy).value();oldBuild["source"]=pathText(legacy/L"project").value();oldBuild["overlay"]=pathText(legacy/L"build-packages.disabled").value();builtWheel=legacy/L"wheels"/builtWheel.filename();oldBuild["wheel"]["local_wheel"]=pathText(builtWheel).value();writeJson(transaction/L"manifest.json",retained);}
        if(std::string_view{variation}=="foreign_operation_scope"){auto retained=manifest;const auto foreign=fixture.root/L"portable"/L"forge-builds.disabled"/L"20000000-0000-4000-8000-000000000002"/L"foreign.whl";Fs::create_directories(foreign.parent_path());Fs::copy_file(builtWheel,foreign);retained["source_builds"].begin().value()["wheel"]["local_wheel"]=pathText(foreign).value();writeJson(transaction/L"manifest.json",retained);}
        const auto resumed=Json::parse(take(fixture.call("prepare",arguments,context)));const auto& resumedBuild=resumed.at("manifest").at("source_builds").begin().value();require(resumed.at("ok")==false&&resumed.at("downloaded_bytes")==archiveFacts.at("bytes")&&resumed.at("manifest").at("downloads").size()==1U&&resumed.at("manifest").at("prior_attempts").size()==2U,"Source resume lost an attempt receipt or charged reusable source content again.");
        if(std::string_view{variation}=="completed_reuse"||std::string_view{variation}=="legacy_completed_reuse")require(resumedBuild.at("reused")==true&&fileFacts(builtWheel,context).at("sha256")==build.at("wheel").at("digests").at("sha256"),"Resume rebuilt or rejected a sealed compatible completed source wheel.");else require(resumed.at("error").at("code").get<std::string>()==Domain::ErrorCodes::IntegrityFailure&&!resumedBuild.value("reused",false),"Resume accepted changed source/wheel content or another operation's build scope.");
    }
}
void sourcePreparationRecoveryRequiresExactSavedBaselineInventory() {
    using namespace NativeTools::Windows::ComfyDetail;
    for(const bool restored:{false,true}){Fixture fixture;permitPreparation(fixture);const auto context=TestContext{}.active();const auto transaction=preparationDirectory(fixture,context);LoopbackArtifactServer server{{},false,200U,{{"queue_running",Json::array()},{"queue_pending",Json::array({Json::array({1,"unrelated",Json::object()})})}}};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
        const Json baseline{{"python_version","3.13.11"},{"tags",Json::array({"py3-none-any"})},{"packages",Json::object()}};auto changed=baseline;changed["packages"]["changed-build-tool"]={{"version","1.0"},{"requires",Json::array()}};writeJson(fixture.root/L"portable"/L"python_embeded"/L"fixture-package-contracts.json",{{"inventory",restored?baseline:changed}});auto receipt=interruptedPreparation(fixture,context);receipt["baseline_package_inventory_unverified"]=true;receipt["snapshot"]={{"packages",baseline},{"runtime",{{"available",true},{"managed",true}}}};writeJson(transaction/L"manifest.json",receipt);const auto result=Json::parse(take(fixture.call("prepare",Json::object(),context)));require(result.at("ok")==restored&&result.at("downloaded_bytes")==0ULL&&result.at("manifest").at("downloads").empty(),"Interrupted build inventory recovery downloaded content or admitted an unverified runtime.");
        if(restored)require(result.at("manifest").at("baseline_package_inventory_unverified")==false&&result.at("manifest").at("baseline_package_inventory_reconciled")==true,"Exact restored baseline did not reconcile its interrupted source-hook inventory.");else{require(result.at("error").at("code").get<std::string>()==Domain::ErrorCodes::Conflict&&result.at("manifest").at("baseline_package_inventory_unverified")==true&&!Fs::exists(fixture.root/L"runtime"/L"runtime.log"),"Resume adopted an altered source-hook inventory or restarted an unverified provider.");bool suppressed=false;for(const auto& item:result.at("manifest").at("rollback"))suppressed=suppressed||(item.contains("prior_runtime_restored")&&item.at("prior_runtime_restored")==false);require(suppressed,"Unverified source-hook inventory did not report suppressed prior-runtime restoration.");}}
}
void activeInstalledPublisherIdentityUsesLatestBindingAndCurrentMetadata() {
    using namespace NativeTools::Windows::ComfyDetail;Fixture fixture;const auto context=TestContext{}.active();const auto home=fixture.root/L"portable"/L"ComfyUI";const auto source=home/L"custom_nodes"/L"PinnedNode";Fs::create_directories(source/L".git"/L"refs"/L"heads");std::ofstream{source/L"nodes.py"}<<"# unchanged source\n";const auto asset=source/L"payload.json";std::ofstream{asset}<<"{\"value\":1}";const auto pin=source/L".git"/L"refs"/L"heads"/L"main";std::ofstream{pin}<<std::string(40U,'a')<<"\n";
    const auto facts=fileFacts(pin,context);const Json binding{{"path",pathText(source).value()},{"kind","directory"},{"tree",directoryFacts(source,1024ULL*1024ULL,context)},{"publisher_identity",{{"kind","installed_git_revision"},{"revision",std::string(40U,'a')}}},{"identity_files",Json::array({{{"path",pathText(pin).value()},{"sha256",facts.at("sha256")},{"bytes",facts.at("bytes")}}})}};
    const auto history=fixture.root/L"runtime"/L"preparations"/L"prior"/L"manifest.json",current=fixture.root/L"runtime"/L"preparations"/L"current"/L"manifest.json";Fs::create_directories(history.parent_path());Fs::create_directories(current.parent_path());auto old=binding;old["identity_files"][0]["sha256"]=std::string(64U,'b');old["publisher_identity"]["revision"]=std::string(40U,'b');writeJson(history,{{"resolution",{{"installed_node_sources",Json::array({old})}}}});writeJson(current,{{"resolution",{{"installed_node_sources",Json::array({binding})}}}});writeJson(fixture.root/L"runtime"/L"active-environment.json",{{"manifests",Json::array({pathText(history).value(),pathText(current).value()})}});
    const auto first=Json::parse(take(fixture.call("identity",{{"workflows",Json::array()}})));const auto retained=readJson(Fs::path{first.at("manifest").at("path").get<std::string>()});require(retained.at("custom_node_publisher_metadata").size()==1U&&retained.at("custom_node_publisher_metadata")[0].at("publisher_identity")==binding.at("publisher_identity"),"Active publisher identity required stale historical pins or lost the latest binding.");
    const auto modified=Fs::last_write_time(pin);std::ofstream{pin,std::ios::trunc}<<std::string(40U,'c')<<"\n";Fs::last_write_time(pin,modified);const auto second=Json::parse(take(fixture.call("identity",{{"workflows",Json::array()}})));require(first.at("sha256")!=second.at("sha256"),"Git pin-only content edit retained final dependency identity.");Fs::remove(pin);const auto third=Json::parse(take(fixture.call("identity",{{"workflows",Json::array()}})));const auto absent=readJson(Fs::path{third.at("manifest").at("path").get<std::string>()});require(second.at("sha256")!=third.at("sha256")&&absent.at("custom_node_publisher_metadata")[0].at("identity_files")[0].at("present")==false,"Deleted publisher metadata was silently omitted from the current identity.");
    const auto assetModified=Fs::last_write_time(asset);std::ofstream{asset,std::ios::trunc}<<"{\"value\":2}";Fs::last_write_time(asset,assetModified);const auto fourth=Json::parse(take(fixture.call("identity",{{"workflows",Json::array()}})));require(third.at("sha256")!=fourth.at("sha256"),"Same-size/mtime non-code node asset edit retained final dependency identity.");Fs::remove(asset);const auto fifth=Json::parse(take(fixture.call("identity",{{"workflows",Json::array()}})));require(fourth.at("sha256")!=fifth.at("sha256"),"Node asset deletion retained final dependency identity.");Fs::remove_all(source);const auto sixth=Json::parse(take(fixture.call("identity",{{"workflows",Json::array()}})));const auto missing=readJson(Fs::path{sixth.at("manifest").at("path").get<std::string>()});require(fifth.at("sha256")!=sixth.at("sha256")&&missing.at("custom_node_publisher_metadata")[0].at("source_content").at("present")==false,"Deleted bound source was silently omitted from final identity.");std::ofstream{source}<<"malformed replacement";requireError(fixture.call("identity",{{"workflows",Json::array()}}),Domain::ErrorCodes::IntegrityFailure,"Malformed bound source kind was accepted as a directory identity.");
}
void providerLiveEnumUsesActualCategoryPathBeforePublisherPreferredSubfolder() {
    using namespace NativeTools::Windows::ComfyDetail;
    for(const auto* variation:{"flat","qualified","wrong_category","absent"}){
        Fixture fixture;permitPreparation(fixture);fixture.config.automaticSetup=false;const auto context=TestContext{}.active();const auto home=fixture.root/L"portable"/L"ComfyUI",manager=home/L"custom_nodes"/L"ComfyUI-Manager";Fs::create_directories(manager/L"glob");const auto category=home/L"models"/L"diffusion_models";Fs::create_directories(category);Fs::create_directories(home/L"models"/L"vae");
        const std::string filename{"wan_fixture.weights"},selected=std::string_view{variation}=="qualified"?"Wan2.2/"+filename:filename;
        if(std::string_view{variation}!="absent")std::ofstream{std::string_view{variation}=="wrong_category"?home/L"models"/L"vae"/wide(filename):category/wide(filename)}<<"installed exact enum model\n";
        std::ofstream{home/L"folder_paths.py"}<<"folder_names_and_paths[\"diffusion_models\"] = ([os.path.join(models_dir, \"diffusion_models\")], supported_pt_extensions)\nfolder_names_and_paths[\"vae\"] = ([os.path.join(models_dir, \"vae\")], supported_pt_extensions)\n";
        writeJson(manager/L"model-list.json",{{"models",Json::array({{{"filename",filename},{"save_path","diffusion_models/Wan2.2"},{"url","https://huggingface.co/fixture/never-contacted/resolve/main/wan_fixture.weights"}}})}});
        const Json schemas{{"UNETLoader",{{"input",{{"required",{{"unet_name",Json::array({Json::array({selected})})}}}}}}}};LoopbackArtifactServer server{{},false,200U,{{"queue_running",Json::array()},{"queue_pending",Json::array({Json::array({1,"unrelated",Json::object()})})}},0U,{},schemas};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
        const Json graph{{"1",{{"class_type","UNETLoader"},{"inputs",{{"unet_name",selected}}}}}};const auto result=Json::parse(take(fixture.call("prepare",{{"workflow",graph}},context)));const bool reusable=std::string_view{variation}=="flat";
        require(result.at("ok")==reusable,"Live enum model reuse substituted catalog placement, a wrong category, or an absent qualified file.");if(reusable){rebindFixtureProviderAfterPreparation(fixture);const auto identity=Json::parse(take(fixture.call("identity",{{"workflows",Json::array({graph})}})));require(identity.at("models").size()==1U&&identity.at("models")[0].at("path")==pathText(category/wide(filename)).value(),"Actual flat enum was reused without sealing the exact installed category file.");}else require(result.at("error").at("code").get<std::string>()==Domain::ErrorCodes::Unauthorized,"Unavailable exact enum model bypassed disabled setup before publisher discovery.");
        require(result.at("downloaded_bytes")==0ULL&&result.at("manifest").at("downloads").empty()&&result.at("manifest").at("metadata").empty()&&server.prompts==0U&&!server.unexpectedRequest,"Actual enum reuse inspected a publisher, downloaded a duplicate, or submitted inference.");
    }
}
void providerCategoryPathsReuseOnlyExactMappedModelFile() {
    using namespace NativeTools::Windows::ComfyDetail;
    for(const bool installed:{false,true}){
        Fixture fixture;permitPreparation(fixture);fixture.config.automaticSetup=false;const auto context=TestContext{}.active();const auto home=fixture.root/L"portable"/L"ComfyUI",manager=home/L"custom_nodes"/L"ComfyUI-Manager",actual=fixture.root/L"independent-model-folder",decoy=fixture.root/L"shared"/L"vae";Fs::create_directories(manager/L"glob");Fs::create_directories(actual/L"nested");Fs::create_directories(decoy/L"nested");std::ofstream{decoy/L"nested"/L"fixture.weights"}<<"decoy";
        std::ofstream{home/L"folder_paths.py"}<<"folder_names_and_paths[\"checkpoints\"] = ([os.path.join(models_dir, \"checkpoints\")], supported_pt_extensions)\n";
        std::ofstream{manager/L"glob"/L"manager_server.py"}<<"model_dir_name_map = {\n\"checkpoint\": \"checkpoints\",\n}\n";
        writeJson(manager/L"model-list.json",{{"models",Json::array({{{"filename","fixture.weights"},{"type","CHECKPOINT"},{"save_path","default"},{"url","https://huggingface.co/fixture/never-contacted/resolve/main/fixture.weights"}}})}});
        auto actualPath=pathText(actual).value(),sharedPath=pathText(fixture.root/L"shared").value();std::replace(actualPath.begin(),actualPath.end(),'\\','/');std::replace(sharedPath.begin(),sharedPath.end(),'\\','/');std::ofstream{home/L"extra_model_paths.yaml"}<<"shared:\n    base_path: "<<sharedPath<<"/\n    checkpoints: "<<actualPath<<"/\n    vae: vae/\n";if(installed)std::ofstream{actual/L"nested"/L"fixture.weights"}<<"real";
        const Json schemas{{"CategoryNode",{{"input",{{"required",{{"weights",Json::array({Json::array({"nested/fixture.weights"})})}}}}}}}};LoopbackArtifactServer server{{},false,200U,{{"queue_running",Json::array()},{"queue_pending",Json::array({Json::array({1,"unrelated",Json::object()})})}},0U,{},schemas};fixture.config.endpoint=server.endpoint();fixture.sealCurrentOwner();
        const auto result=Json::parse(take(fixture.call("prepare",{{"workflow",{{"1",{{"class_type","CategoryNode"},{"inputs",{{"weights",{{"__value__","nested/fixture.weights"}}}}}}}}}},context)));const auto& manifest=result.at("manifest");
        if(installed)require(result.at("ok")==true&&manifest.at("resolution").at("dependencies").empty(),"Configured absolute category path with a different physical basename was not reused.");
        else require(result.at("ok")==false&&result.at("error").at("code").get<std::string>()==Domain::ErrorCodes::Unauthorized,"A wrong-category same-name model satisfied the exact requested checkpoint inventory.");
        require(result.at("downloaded_bytes")==0ULL&&manifest.at("metadata").empty()&&manifest.at("downloads").empty()&&server.prompts==0U&&!server.unexpectedRequest,"Mapped-model reuse inspected publishers or submitted inference.");
    }
}
void publisherMetadataRetryRetainsNegativeEvidenceAndCumulativeLedger() {
    using NativeTools::Windows::ComfyDetail::publisherMetadataDocument;using NativeTools::Windows::ComfyDetail::retainedPublisherMetadata;
    using NativeTools::Windows::ComfyDetail::writeJson;using NativeTools::Windows::ComfyDetail::readJson;using NativeTools::Windows::ComfyDetail::fileFacts;using NativeTools::Windows::ComfyDetail::Failure;
    for(const unsigned status:{200U,404U,429U,500U,503U}){
        Fixture fixture;const auto context=TestContext{}.active();const auto transaction=preparationDirectory(fixture,context),path=transaction/L"previous.metadata.json";
        const std::string body=status==200U?"{\"version\":\"pinned\"}":"<html>Publisher request temporarily failed.</html>";Infrastructure::Windows::BCryptSha256Hasher hasher;const auto sha=take(hasher.sha256(std::as_bytes(std::span{body.data(),body.size()}))).value();
        const auto document=publisherMetadataDocument(status,body,sha,body.size());writeJson(path,document);const std::string url{"https://publisher.invalid/metadata"};const Json receipt{{"path",pathText(path).value()},{"sha256",fileFacts(path,context).at("sha256")},{"url",url}};
        Json manifest{{"downloaded_bytes",999ULL},{"downloads",Json::array({{{"path",pathText(path).value()},{"received_bytes",999ULL},{"http_status",status},{"body_sha256",sha}}})},{"metadata",{{"key",receipt}}}};const auto attempts=manifest.at("downloads");
        const auto cached=retainedPublisherMetadata(manifest,"key",url,transaction,context);require(manifest.at("downloaded_bytes")==999ULL&&manifest.at("downloads")==attempts&&readJson(path)==document&&fileFacts(path,context).at("sha256")==receipt.at("sha256"),"Metadata retry reset counters or overwrote prior response evidence.");
        if(status==200U)require(cached==document&&manifest.at("metadata").contains("key")&&!manifest.contains("metadata_retries"),"Pinned successful metadata was not reusable.");
        else require(cached.is_null()&&!manifest.at("metadata").contains("key")&&manifest.at("metadata_retries")[0].at("previous")==receipt&&document.at("status")==status&&document.at("body_sha256")==sha&&document.at("data").at("diagnostic")==body,"Negative publisher response became permanently cached or lost HTTP evidence.");
    }
    bool rejected=false;try{static_cast<void>(publisherMetadataDocument(200U,"<html>invalid successful JSON</html>",std::string(64U,'a'),36ULL));}catch(const Failure& error){rejected=error.error.code==Domain::ErrorCodes::MalformedMessage;}require(rejected,"Successful publisher metadata accepted malformed JSON.");
}
void nativeDependencyCopyStreamsAndPreservesInactiveFailureEvidence() {
    using NativeTools::Windows::ComfyDetail::Handle;using NativeTools::Windows::ComfyDetail::Failure;using NativeTools::Windows::ComfyDetail::copyFileContents;using NativeTools::Windows::ComfyDetail::fileFacts;
    for(const auto* scenario:{"exact","reserve","cancel","oversized"}){
        Fixture fixture;const auto source=fixture.root/L"verified.download",staged=fixture.root/L"copy.disabled",active=fixture.root/L"active.safetensors";std::string contents(3U*65536U+17U,'x');for(std::size_t index=0;index<contents.size();++index)contents[index]=static_cast<char>(index%251U);std::ofstream{source,std::ios::binary}<<contents;
        Handle input{CreateFileW(source.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)},output{CreateFileW(staged.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};require(input&&output,"Native dependency-copy fixture could not open its sealed handles.");
        TestContext test;const auto context=test.active();const auto before=fileFacts(input.get(),context);std::uint64_t observations{};bool failed=false;Json copied;
        try{copied=copyFileContents(input.get(),output.get(),std::string_view{scenario}=="oversized"?contents.size()-1U:contents.size(),context,[&](std::uint64_t bytes){require(bytes<=65536ULL,"Native dependency copy exceeded its bounded chunk size.");++observations;if(std::string_view{scenario}=="reserve"&&observations==2ULL)NativeTools::Windows::ComfyDetail::fail(Domain::ErrorCodes::LimitExceeded,"Fixture free-space reserve reached.");if(std::string_view{scenario}=="cancel"&&observations==2ULL)test.cancellation.request_stop();});}
        catch(const Failure& error){const auto expected=std::string_view{scenario}=="cancel"?Domain::ErrorCodes::Cancelled:Domain::ErrorCodes::LimitExceeded;failed=error.error.code==expected;}
        require(!Fs::exists(active)&&fileFacts(input.get(),TestContext{}.active()).at("sha256")==before.at("sha256"),"Failed copy published an active filename or altered its verified source.");
        if(std::string_view{scenario}=="exact")require(!failed&&observations==4ULL&&copied.at("sha256")==before.at("sha256")&&copied.at("bytes")==contents.size(),"Multi-chunk native dependency copy was not byte exact.");
        else{const auto expectedBytes=std::string_view{scenario}=="oversized"?0ULL:65536ULL;require(failed&&fileFacts(output.get(),TestContext{}.active()).at("bytes")==expectedBytes,"Reserve/cancellation/size failure wrote a forbidden chunk or discarded inactive prefix evidence.");}
    }
}
Json qualifyCrossVolumePreparation(const Fs::path& destinationBase) {
    Fixture fixture;permitPreparation(fixture);const auto context=TestContext{}.active();
    require(destinationBase.is_absolute()&&destinationBase.has_root_name()&&destinationBase!=destinationBase.root_path(),"Cross-volume fixture needs an absolute non-root destination namespace.");
    Fs::create_directories(destinationBase);std::array<wchar_t,MAX_PATH> sourceVolume{},destinationVolume{};
    require(GetVolumeNameForVolumeMountPointW(fixture.root.root_path().c_str(),sourceVolume.data(),static_cast<DWORD>(sourceVolume.size()))&&GetVolumeNameForVolumeMountPointW(destinationBase.root_path().c_str(),destinationVolume.data(),static_cast<DWORD>(destinationVolume.size())),"Cross-volume fixture cannot inspect volume identities.");
    require(std::wstring_view{sourceVolume.data()}!=std::wstring_view{destinationVolume.data()},"Cross-volume fixture source and destination resolve to the same volume.");
    Infrastructure::Windows::WindowsUuidGenerator ids;const auto identifier=take(ids.next()).value();const auto destinationRoot=(destinationBase/Fs::path{identifier}).lexically_normal();
    require(destinationRoot.parent_path()==destinationBase.lexically_normal()&&!Fs::exists(destinationRoot),"Cross-volume fixture refuses an existing or escaping destination.");Fs::create_directory(destinationRoot);
    struct Cleanup final{Fs::path path,base;~Cleanup(){if(path.parent_path()==base&&path!=base&&!path.filename().empty()){std::error_code ignored;Fs::remove_all(path,ignored);}}}cleanup{destinationRoot,destinationBase.lexically_normal()};
    fixture.config.modelStoragePath=pathText(destinationRoot).value();fixture.backend->shutdown();fixture.backend.reset();
    fixture.issuer=std::make_unique<Infrastructure::Windows::WindowsWorkspaceAuthority>(std::vector<Infrastructure::Windows::WindowsWorkspaceAuthorityPolicy>{{
        parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000001"),fixture.project,parse<Domain::ClientId>("forge-conductor-manager"),
        {pathText(fixture.root),pathText(destinationRoot)},Domain::FileAccess::Write,{Domain::FileAccess::Read,Domain::FileAccess::Write,Domain::FileAccess::Create,Domain::FileAccess::Execute},{},true,1U}});
    fixture.backend=std::make_unique<NativeTools::Windows::WindowsComfyUiBackend>(*fixture.issuer,pathText(fixture.root/L"runtime"));
    const auto transaction=preparationDirectory(fixture,context),source=transaction/L"synthetic.download";std::string bytes(4U*1024U*1024U+17U,'x');for(std::size_t index=0;index<bytes.size();++index)bytes[index]=static_cast<char>(index%251U);std::ofstream{source,std::ios::binary}<<bytes;
    const auto facts=NativeTools::Windows::ComfyDetail::fileFacts(source,context);const std::string url{"https://fixture.invalid/synthetic-model-never-downloaded"};auto receipt=interruptedPreparation(fixture,context);receipt["downloaded_bytes"]=facts.at("bytes");receipt["downloads"].push_back({{"url",url},{"path",pathText(source).value()},{"sha256_expected",facts.at("sha256")},{"sha256",facts.at("sha256")},{"bytes",facts.at("bytes")},{"received_bytes",facts.at("bytes")},{"state","verified"}});std::ofstream{transaction/L"manifest.json"}<<receipt.dump();
    const auto started=std::chrono::steady_clock::now();const auto result=Json::parse(take(fixture.call("prepare",{{"dependencies",Json::array({{{"kind","model"},{"url",url},{"target","checkpoints/synthetic.safetensors"},{"sha256",facts.at("sha256")},{"bytes",facts.at("bytes")}}})}},context)));
    const auto& activation=result.at("manifest").at("activations").at(0);require(result.at("ok")==false&&activation.at("state")=="activated"&&activation.contains("staging_file")&&activation.at("copied_bytes")==facts.at("bytes")&&activation.at("staging_identity").at("sha256")==facts.at("sha256"),"Actual cross-volume backend activation did not stream and seal the complete dependency.");
    require(result.at("downloaded_bytes")==facts.at("bytes")&&NativeTools::Windows::ComfyDetail::fileFacts(source,context).at("sha256")==facts.at("sha256"),"Cross-volume reuse redownloaded or changed its sealed source.");
    const auto target=destinationRoot/L"checkpoints"/L"synthetic.safetensors";require(!Fs::exists(target),"Failed fake-runtime readiness left the synthetic model active.");Json retained;
    for(const auto& item:result.at("manifest").at("rollback"))if(item.value("path",std::string{})==pathText(target).value()&&item.value("moved",false)){const auto path=Fs::path{item.at("retained").get<std::string>()};require(NativeTools::Windows::ComfyDetail::fileFacts(path,context).at("sha256")==facts.at("sha256"),"Cross-volume rollback content changed.");retained=item;}
    require(!retained.is_null(),"Cross-volume rollback did not retain verified inactive evidence.");
    return {{"qualification","synthetic_native_backend_cross_volume_preparation"},{"source_volume",take(Infrastructure::Windows::Detail::strictUtf16ToUtf8(sourceVolume.data()))},{"destination_volume",take(Infrastructure::Windows::Detail::strictUtf16ToUtf8(destinationVolume.data()))},{"source_bytes",facts.at("bytes")},{"source_sha256",facts.at("sha256")},{"activation",activation},{"rollback",retained},{"downloaded_bytes",result.at("downloaded_bytes")},{"readiness_error",result.at("error")},{"elapsed_ms",std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count()},{"temporary_destination",pathText(destinationRoot).value()},{"cleanup","Temporary source and destination namespaces are removed by scoped fixture destructors."},{"generation_posts",0}};
}
Json qualifyInstalledPackageContracts(const Fs::path& python) {
    using namespace NativeTools::Windows::ComfyDetail;
    require(python.is_absolute()&&Fs::is_regular_file(python),"Offline package-contract qualification needs an explicit installed interpreter.");Fixture fixture;const auto context=TestContext{}.active();const auto directory=fixture.root/L"offline-contracts";Fs::create_directory(directory);unsigned sequence{};Json cases=Json::array();
    const auto execute=[&](const Json& query){const auto stem=std::to_wstring(sequence++);const auto data=directory/(stem+L".json");writeJson(data,query);const auto output=runProcess(python,{"-I","-B","-c",std::string{PackageContracts},pathText(data).value()},python.parent_path(),directory/(stem+L".log"),context);require(output.at("exit_code")==0,"Installed offline packaging contract failed: "+output.at("output").get<std::string>());return Json::parse(output.at("output").get<std::string>());};
    const auto actualBefore=execute({{"mode","inventory"}});Json environment{{"python_version",actualBefore.at("python_version")},{"tags",actualBefore.at("tags")},{"packages",Json::object()}};
    const auto publisher=[](std::string_view name,const Json& versions){Json releases=Json::object(),metadata=Json::object();for(const auto& [version,declaredRequirements]:versions.items()){const auto filename=std::string{name}+"-"+version+"-py3-none-any.whl";releases[version]=Json::array({{{"filename",filename},{"packagetype","bdist_wheel"},{"url","https://files.pythonhosted.org/fixture/"+filename},{"digests",{{"sha256",std::string(64U,'a')}}},{"size",3ULL}}});metadata[version]={{"info",{{"name",name},{"version",version},{"requires_dist",declaredRequirements}}}};}return Json{{"index",{{"releases",releases}}},{"versions",metadata}};};
    const Json publishers{{"a",publisher("a",{{"2.0",Json::array({"b>=2","abandoned>=1"})},{"1.0",Json::array({"b<2"})}})},{"b",publisher("b",{{"2.0",Json::array()},{"1.0",Json::array()}})},{"c",publisher("c",{{"1.0",Json::array({"b<2"})}})},{"abandoned",publisher("abandoned",{{"1.0",Json::array()}})}};
    const auto resolve=[&](const Json& requirements,const Json& constraints,const Json& baseline,const Json& sources,bool isolatedBuild=false){auto effective=environment;effective["packages"]=baseline;return execute({{"mode","resolve"},{"requirements",requirements},{"constraints",constraints},{"environment",effective},{"publishers",sources},{"isolated_build",isolatedBuild}});};
    const auto backtracked=resolve(Json::array({"a>=1","c>=1"}),Json::array(),Json::object(),publishers);require(backtracked.at("selected").at("a").at("version")=="1.0"&&backtracked.at("selected").at("b").at("version")=="1.0"&&backtracked.at("selected").at("c").at("version")=="1.0"&&!backtracked.at("selected").contains("abandoned")&&std::find(backtracked.at("requirements").begin(),backtracked.at("requirements").end(),Json("abandoned>=1"))==backtracked.at("requirements").end(),"Installed resolver did not backtrack and remove abandoned dependency information.");cases.push_back({{"name","backtracking_removes_abandoned_dependency"},{"result",backtracked}});
    const auto conflict=resolve(Json::array({"b>=2"}),Json::array(),{{"b",{{"version","1.0"},{"requires",Json::array()}}}},publishers);require(conflict.at("error")=="conflict","Resolver replaced an incompatible pinned baseline package.");cases.push_back({{"name","baseline_version_pinned"},{"result",conflict}});
    require(actualBefore.at("packages").contains("setuptools")&&actualBefore.at("packages").at("setuptools").at("version")!="1.0","Installed source-build qualification needs setuptools newer than its synthetic older candidate.");
    const Json buildPublisher{{"setuptools",publisher("setuptools",{{"1.0",Json::array()}})}};const auto olderRuntime=resolve(Json::array({"setuptools==1.0"}),Json::array(),actualBefore.at("packages"),buildPublisher);require(olderRuntime.at("error")=="conflict","Runtime resolver replaced the installed setuptools baseline.");
    const auto olderBuild=resolve(Json::array({"setuptools==1.0"}),Json::array(),actualBefore.at("packages"),buildPublisher,true);require(olderBuild.at("selected").at("setuptools").at("version")=="1.0"&&olderBuild.at("selected").size()==1U,"Isolated build requirements could not select an older tool independently of unrelated installed runtime dependencies.");
    const auto reusedBuild=resolve(Json::array({"setuptools=="+actualBefore.at("packages").at("setuptools").at("version").get<std::string>()}),Json::array(),actualBefore.at("packages"),Json::object(),true);require(reusedBuild.at("selected").empty(),"Isolated build resolution downloaded a replacement for its compatible installed build tool.");cases.push_back({{"name","isolated_older_build_tool_and_compatible_baseline_reuse"},{"installed_version",actualBefore.at("packages").at("setuptools").at("version")},{"runtime_result",olderRuntime},{"build_result",olderBuild},{"reuse_result",reusedBuild}});
    const auto constrained=resolve(Json::array({"a>=1"}),Json::array({"a<2"}),Json::object(),publishers);require(constrained.at("selected").at("a").at("version")=="1.0","Resolver ignored the included version constraint.");cases.push_back({{"name","constraint_selects_compatible_candidate"},{"result",constrained}});
    const auto needed=resolve(Json::array({"a>=1"}),Json::array(),Json::object(),Json::object());require(needed.at("need").at("name")=="a"&&!needed.at("need").contains("version"),"Resolver did not request native publisher index metadata.");cases.push_back({{"name","native_index_request"},{"result",needed}});
    const auto inactive=execute({{"mode","requirements"},{"requirements",Json::array({"ignored @ https://fixture.invalid/source.zip ; sys_platform == 'linux'","active>=1"})}});require(inactive.size()==1U&&inactive[0].at("name")=="active","Inactive platform-specific direct requirement blocked the installed Windows package contract.");cases.push_back({{"name","inactive_platform_requirement"},{"result",inactive}});
    std::ofstream{directory/L"requirements.txt"}<<"-r nested.txt\n-c constraints.txt\n";std::ofstream{directory/L"nested.txt"}<<"a>=1\n";std::ofstream{directory/L"constraints.txt"}<<"a<2\n";const auto included=execute({{"mode","requirements_file"},{"root",pathText(directory).value()},{"path",pathText(directory/L"requirements.txt").value()}});require(included.at("requirements")==Json::array({"a>=1"})&&included.at("constraints")==Json::array({"a<2"})&&included.at("files").size()==3U,"Installed pip did not preserve local includes and constraints.");for(const auto& file:included.at("files"))require(fileFacts(Fs::path{file.at("path").get<std::string>()},context).at("sha256")==file.at("sha256"),"Installed requirement parser returned a different included-file seal.");cases.push_back({{"name","includes_constraints_actual_file_seals"},{"result",included}});
    const auto project=directory/L"node-project.toml";
    std::ofstream{project}<<"[project]\nname = ' Fixture-Registry-Package '\nversion = '1.2.3'\n[project.urls]\nRepository = 'https://github.com/fixture/publisher'\n[tool.comfy]\nPublisherId = 'fixture-publisher'\n";
    const auto projectIdentity=execute({{"mode","node_project_identity"},{"path",pathText(project).value()}});
    require(projectIdentity==Json{{"id","fixture-registry-package"},{"version","1.2.3"},{"repository","https://github.com/fixture/publisher"},{"publisher_id","fixture-publisher"}},"Installed TOML parser did not retain the literal Registry project identity.");
    std::ofstream{project,std::ios::trunc}<<"[project]\nname = 'fixture-registry-package'\nversion = {dynamic = 'unresolved'}\n";bool refused{};
    try{static_cast<void>(execute({{"mode","node_project_identity"},{"path",pathText(project).value()}}));}catch(const std::exception& error){refused=std::string_view{error.what()}.find("Registry source identity must contain bounded literal strings")!=std::string_view::npos;}
    require(refused,"Installed Registry project identity accepted a nonliteral version.");
    cases.push_back({{"name","registry_project_literal_identity_and_nonliteral_refusal"},{"result",projectIdentity},{"nonliteral_version_refused",refused}});
    const auto actualAfter=execute({{"mode","inventory"}});require(actualBefore==actualAfter,"Offline package-contract qualification changed the installed interpreter inventory.");Infrastructure::Windows::BCryptSha256Hasher hasher;const auto source=take(hasher.sha256(std::as_bytes(std::span{PackageContracts.data(),PackageContracts.size()}))).value();
    return {{"qualification","installed_interpreter_offline_package_contracts"},{"interpreter",pathText(python).value()},{"interpreter_identity",fileFacts(python,context)},{"package_contract_sha256",source},{"cases",cases},{"package_inventory_unchanged",true},{"publisher_metadata","Synthetic JSON only; no publisher requests or downloads."},{"generation_posts",0U}};
}
Fs::path lifetimeFixtureExecutable() {
    std::array<wchar_t,32768> image{};const auto count=GetModuleFileNameW(nullptr,image.data(),static_cast<DWORD>(image.size()));
    require(count>0U&&count<image.size(),"Process lifetime fixture cannot identify its executable.");return Fs::path{image.data()};
}
Json lifetimeFixtureIdentity() {
    FILETIME creation{},exit{},kernel{},user{};require(GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user)!=FALSE,"Lifetime helper cannot seal its process creation time.");
    return {{"pid",GetCurrentProcessId()},{"creation_time",(static_cast<std::uint64_t>(creation.dwHighDateTime)<<32)|creation.dwLowDateTime},
        {"image",NativeTools::Windows::ComfyDetail::pathText(lifetimeFixtureExecutable())}};
}
int runLifetimeFixtureProcess(const std::string_view mode,const Fs::path& directory) {
    using namespace NativeTools::Windows::ComfyDetail;
    if(mode=="leaf") {writeJson(directory/L"leaf.json",lifetimeFixtureIdentity());Sleep(20000U);return 0;}
    const auto executable=lifetimeFixtureExecutable();
    if(mode=="branch") {
        auto leaf=startProcess(executable,{"--fixture-process-lifetime","leaf",pathText(directory).value()},directory,directory/L"leaf.log");
        writeJson(directory/L"branch.json",lifetimeFixtureIdentity());return WaitForSingleObject(leaf.process.get(),20000U)==WAIT_OBJECT_0?0:72;
    }
    if(mode=="transient") {
        auto context=TestContext{}.active();context.deadline=std::chrono::steady_clock::now()+std::chrono::seconds{25};
        return static_cast<int>(runProcess(executable,{"--fixture-process-lifetime","branch",pathText(directory).value()},directory,directory/L"branch.log",context).at("exit_code").get<DWORD>());
    }
    if(mode=="durable") {
        auto branch=startProcess(executable,{"--fixture-process-lifetime","branch",pathText(directory).value()},directory,directory/L"branch.log");
        return WaitForSingleObject(branch.process.get(),20000U)==WAIT_OBJECT_0?0:73;
    }
    return 74;
}
void transientProcessTreeEndsWithHardOwnerExitAndDurableProcessSurvives() {
    using namespace NativeTools::Windows::ComfyDetail;
    for(const bool transient:{true,false}) {
        Fixture fixture;const auto directory=fixture.root/L"owned-process-lifetime";Fs::create_directory(directory);const auto executable=lifetimeFixtureExecutable();
        auto owner=startProcess(executable,{"--fixture-process-lifetime",transient?"transient":"durable",pathText(directory).value()},directory,directory/L"owner.log");
        Handle branch,leaf;
        struct Cleanup final {
            Process& owner;Handle& branch;Handle& leaf;
            ~Cleanup(){if(owner.job)TerminateJobObject(owner.job.get(),75U);for(const auto handle:{owner.process.get(),branch.get(),leaf.get()})if(handle)WaitForSingleObject(handle,5000U);}
        } cleanup{owner,branch,leaf};
        const auto ready=std::chrono::steady_clock::now()+std::chrono::seconds{8};
        while(!Fs::is_regular_file(directory/L"branch.json")||!Fs::is_regular_file(directory/L"leaf.json")) {
            require(WaitForSingleObject(owner.process.get(),0U)==WAIT_TIMEOUT,"Process lifetime owner exited before its exact descendant boundary.");
            require(std::chrono::steady_clock::now()<ready,"Process lifetime descendants did not publish bounded readiness evidence.");Sleep(10U);
        }
        const auto observe=[&](const Fs::path& path) {
            const auto evidence=readJson(path);const auto pid=evidence.at("pid").get<DWORD>();require(pid!=GetCurrentProcessId()&&pid!=owner.pid,"Lifetime fixture returned an unrelated owner identity.");
            Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid)};require(static_cast<bool>(process),"Cannot observe the exact owned descendant process.");
            FILETIME creation{},exit{},kernel{},user{};require(GetProcessTimes(process.get(),&creation,&exit,&kernel,&user)!=FALSE&&
                ((static_cast<std::uint64_t>(creation.dwHighDateTime)<<32)|creation.dwLowDateTime)==evidence.at("creation_time").get<std::uint64_t>(),"Owned descendant process creation identity changed.");
            std::array<wchar_t,32768> image{};DWORD bytes=static_cast<DWORD>(image.size());require(QueryFullProcessImageNameW(process.get(),0U,image.data(),&bytes)!=FALSE&&
                _wcsicmp(image.data(),executable.c_str())==0&&evidence.at("image")==pathText(executable).value(),"Lifetime descendant was not the exact native fixture image.");
            BOOL member=FALSE;require(IsProcessInJob(process.get(),owner.job.get(),&member)!=FALSE&&member!=FALSE&&WaitForSingleObject(process.get(),0U)==WAIT_TIMEOUT,
                "Lifetime descendant was not live inside its exact fixture-owned process tree.");return process;
        };
        branch=observe(directory/L"branch.json");leaf=observe(directory/L"leaf.json");require(GetProcessId(branch.get())!=GetProcessId(leaf.get()),"Lifetime fixture did not create an actual second-generation descendant.");
        require(TerminateProcess(owner.process.get(),71U)!=FALSE&&WaitForSingleObject(owner.process.get(),5000U)==WAIT_OBJECT_0,"Cannot abruptly terminate the exact lifetime owner process.");
        DWORD exitCode{};require(GetExitCodeProcess(owner.process.get(),&exitCode)!=FALSE&&exitCode==71U,"Lifetime test did not observe the requested hard owner exit.");
        if(transient)require(WaitForSingleObject(branch.get(),5000U)==WAIT_OBJECT_0&&WaitForSingleObject(leaf.get(),5000U)==WAIT_OBJECT_0,
            "A transient component or its descendant survived hard owner shutdown and could overlap resumed preparation.");
        else require(WaitForSingleObject(branch.get(),250U)==WAIT_TIMEOUT&&WaitForSingleObject(leaf.get(),250U)==WAIT_TIMEOUT,
            "Default durable provider process lifetime changed when its owner exited.");
        require(TerminateJobObject(owner.job.get(),75U)!=FALSE&&WaitForSingleObject(branch.get(),5000U)==WAIT_OBJECT_0&&WaitForSingleObject(leaf.get(),5000U)==WAIT_OBJECT_0,
            "Fixture-owned descendants were not fully cleaned up after process lifetime verification.");
    }
}
void embeddedBuildInterpreterPreservesProviderAndRetainsSealedCopies() {
    using namespace NativeTools::Windows::ComfyDetail;
    Fixture fixture; permitPreparation(fixture); const auto context=TestContext{}.active();
    const auto provider=fixture.root/L"portable"/L"python_embeded"/L"python.exe";
    const auto configuration=provider.parent_path()/L"python313._pth",component=provider.parent_path()/L"python313.dll";
    std::ofstream{configuration}<<"python313.zip\n.\nimport site\n"; std::ofstream{component}<<"synthetic interpreter component\n";
    const auto providerBefore=fileFacts(provider,context),configurationBefore=fileFacts(configuration,context),componentBefore=fileFacts(component,context);
    const auto overlay=fixture.root/L"inactive-overlay";Fs::create_directory(overlay);const auto directory=overlay/L".forge-python";
    std::uint64_t reservedBytes{};
    const auto authorize=[&](const Fs::path& path,Domain::FileAccess){require(contained(provider.parent_path(),path)||contained(overlay,path),"Embedded build staging left its fixture provider or overlay.");};
    const auto staged=stageBuildInterpreter(provider,overlay,directory,context,authorize,[&](std::uint64_t bytes){reservedBytes+=bytes;});
    const auto expectedBytes=providerBefore.at("bytes").get<std::uint64_t>()+componentBefore.at("bytes").get<std::uint64_t>();
    require(staged.at("embedded")==true&&staged.at("python")==pathText(directory/L"python.exe").value()&&
        staged.at("component_bytes")==expectedBytes&&staged.at("copied_bytes")==expectedBytes&&reservedBytes>=expectedBytes,
        "Embedded build staging did not retain its measured component bytes or exact inactive executable.");
    require(fileFacts(directory/L"python.exe",context).at("sha256")==providerBefore.at("sha256")&&
        fileFacts(directory/L"python313.dll",context).at("sha256")==componentBefore.at("sha256")&&
        fileFacts(provider,context)==providerBefore&&fileFacts(configuration,context)==configurationBefore&&fileFacts(component,context)==componentBefore,
        "Embedded build staging changed installed files or copied different interpreter content.");
    const auto originalTree=directoryFacts(directory,64ULL*1024ULL*1024ULL,context);reservedBytes=0U;
    const auto reused=stageBuildInterpreter(provider,overlay,directory,context,authorize,[&](std::uint64_t bytes){reservedBytes+=bytes;});
    require(reused.at("copied_bytes")==0ULL&&reservedBytes==0ULL&&directoryFacts(directory,64ULL*1024ULL*1024ULL,context)==originalTree,
        "Verified embedded build interpreter reuse rewrote files or reported another copy.");
    std::ofstream{directory/L"python313.dll",std::ios::trunc}<<"edited retained component\n";bool rejected{};
    try{static_cast<void>(stageBuildInterpreter(provider,overlay,directory,context,authorize,[](std::uint64_t){}));}
    catch(const Failure& error){rejected=error.error.code==Domain::ErrorCodes::IntegrityFailure;}
    require(rejected&&fileFacts(component,context)==componentBefore,"Edited inactive interpreter content was overwritten or accepted.");
    const auto interruptedOverlay=fixture.root/L"interrupted-overlay";Fs::create_directory(interruptedOverlay);unsigned chunks{};rejected=false;
    require(providerBefore.at("bytes").get<std::uint64_t>()>131072ULL,"Embedded copy fixture needs at least two actual native copy chunks.");
    try{static_cast<void>(stageBuildInterpreter(provider,interruptedOverlay,interruptedOverlay/L".forge-python",context,
        [&](const Fs::path& path,Domain::FileAccess){require(contained(provider.parent_path(),path)||contained(interruptedOverlay,path),"Interrupted build copy left its scopes.");},
        [&](std::uint64_t bytes){if(bytes>0U&&bytes<=65536ULL&&++chunks==2U)fail(Domain::ErrorCodes::LimitExceeded,"Fixture free-space reserve reached.");}));}
    catch(const Failure& error){rejected=error.error.code==Domain::ErrorCodes::LimitExceeded;}
    require(rejected&&Fs::file_size(interruptedOverlay/L".forge-python"/L"python.exe")==65536ULL&&
        fileFacts(provider,context)==providerBefore&&fileFacts(configuration,context)==configurationBefore,
        "Interrupted interpreter staging lost its bounded partial copy or modified the installed provider.");
}
Json qualifyInstalledSourceHooks(const Fs::path& python) {
    using namespace NativeTools::Windows::ComfyDetail;
    require(python.is_absolute()&&Fs::is_regular_file(python),"Offline source-hook qualification needs an explicit installed interpreter.");
    Fixture fixture;const auto context=TestContext{}.active();const auto directory=fixture.root/L"source-contracts";Fs::create_directory(directory);unsigned sequence{};Json cases=Json::array();
    const auto execute=[&](Json query){const auto stem=std::to_wstring(sequence++);const auto data=directory/(stem+L".json"),result=directory/(stem+L".result.json");
        query["result_path"]=pathText(result).value();writeJson(data,query);auto interpreter=python;Json staged=nullptr;
        const auto mode=query.at("mode").get<std::string>();if(mode=="build_requires"||mode=="build_metadata"||mode=="build_wheel"){
            const Fs::path overlay{query.at("overlay").get<std::string>()};staged=stageBuildInterpreter(python,overlay,overlay/L".forge-python",context,
                [&](const Fs::path& path,Domain::FileAccess){require(contained(python.parent_path(),path)||contained(overlay,path),"Offline build interpreter staging left its explicit source or fixture overlay.");},[](std::uint64_t){});
            interpreter=Fs::path{staged.at("python").get<std::string>()};
        }
        const auto observed=runProcess(interpreter,{"-I","-B","-c",std::string{PackageContracts},pathText(data).value()},interpreter.parent_path(),directory/(stem+L".log"),context);
        require(observed.at("exit_code")==0,"Installed offline source hook failed: "+observed.at("output").get<std::string>());
        require(Fs::is_regular_file(result),"Fixed source hook did not write its structured result independently of backend stdout.");return Json{{"result",readJson(result)},{"process",observed},{"build_interpreter",staged}};};
    const auto before=execute({{"mode","inventory"}}).at("result");
    const std::string backend=R"FORGESOURCE(
import os,csv,io,json,zipfile,hashlib,base64,subprocess,sys
NAME='forge_source_fixture'
DIST=NAME+'-1.0.dist-info'
METADATA='Metadata-Version: 2.3\nName: forge-source-fixture\nVersion: 1.0\nRequires-Python: >=3.10\nRequires-Dist: fixture-runtime>=1; sys_platform == "win32"\nRequires-Dist: fixture-extra>=2; extra == "review"\n\n'
PAYLOAD="fixture_value = 'from sealed synthetic source'\n"
def get_requires_for_build_wheel(config_settings=None):
    assert os.environ.get('PIP_NO_INDEX')=='1'
    import forge_overlay_probe
    assert forge_overlay_probe.value=='sealed_overlay'
    observations=[]
    for flags in (['-B'],['-I','-B']):
        child=subprocess.run([sys.executable]+flags+['-c','import json,os,sys,forge_overlay_probe;print(json.dumps({"value":forge_overlay_probe.value,"pth":os.environ.get("FORGE_BUILD_PTH"),"pid":os.getpid(),"executable":sys.executable}))'],capture_output=True,text=True)
        assert child.returncode==0,child.stderr
        result=json.loads(child.stdout); assert result['value']=='sealed_overlay' and result['pth']==str(result['pid'])
        observations.append({'flags':flags,'exit_code':child.returncode,'result':result})
    with open('build-child-observations.json','w',encoding='utf-8') as f: json.dump(observations,f)
    print('NOISY_FIXTURE_BUILD_REQUIRES')
    return ['fixture-build-dep>=1']
def build_wheel(wheel_directory,config_settings=None,metadata_directory=None):
    assert os.environ.get('PIP_NO_INDEX')=='1'
    print('NOISY_FIXTURE_BUILD_WHEEL')
    files={NAME+'/__init__.py':PAYLOAD,DIST+'/METADATA':METADATA,DIST+'/WHEEL':'Wheel-Version: 1.0\nGenerator: ForgeSyntheticFixture\nRoot-Is-Purelib: true\nTag: py3-none-any\n'}
    record=io.StringIO(); writer=csv.writer(record,lineterminator='\n')
    for name,content in files.items():
        raw=content.encode(); sha=base64.urlsafe_b64encode(hashlib.sha256(raw).digest()).rstrip(b'=').decode(); writer.writerow([name,'sha256='+sha,len(raw)])
    writer.writerow([DIST+'/RECORD','','']); files[DIST+'/RECORD']=record.getvalue()
    filename=NAME+'-1.0-py3-none-any.whl'
    with zipfile.ZipFile(os.path.join(wheel_directory,filename),'w',compression=zipfile.ZIP_STORED) as wheel:
        for name,content in files.items(): wheel.writestr(name,content)
    return filename
)FORGESOURCE";
    const std::string metadataHook=R"FORGESOURCE(
def prepare_metadata_for_build_wheel(metadata_directory,config_settings=None):
    print('NOISY_FIXTURE_BUILD_METADATA')
    path=os.path.join(metadata_directory,DIST); os.makedirs(path,exist_ok=True)
    with open(os.path.join(path,'METADATA'),'w',encoding='utf-8',newline='') as stream: stream.write(METADATA)
    return DIST
)FORGESOURCE";
    const Json expectedMetadata{{"name","forge-source-fixture"},{"version","1.0"},{"requires_python",">=3.10"},
        {"requires_dist",Json::array({"fixture-runtime>=1; sys_platform == \"win32\"","fixture-extra>=2; extra == \"review\""})}};
    for(const bool prepared:{true,false}) {
        const auto source=directory/(prepared?L"prepared-metadata":L"missing-metadata-hook"),overlay=source/L"inactive-build-deps",output=source/L"generated";
        Fs::create_directories(overlay);std::ofstream{overlay/L"forge_overlay_probe.py"}<<"value='sealed_overlay'\n";
        std::ofstream{overlay/L"forge_fixture.pth"}<<"import os; os.environ['FORGE_BUILD_PTH']=str(os.getpid())\n";
        std::ofstream{source/L"pyproject.toml"}<<"[build-system]\nrequires = []\nbuild-backend = 'fixture_backend'\nbackend-path = ['.']\n";
        std::ofstream{source/L"fixture_backend.py"}<<backend<<(prepared?metadataHook:std::string{});
        const auto configuration=execute({{"mode","build_configuration"},{"source",pathText(source).value()},{"name","forge-source-fixture"}}).at("result");
        require(configuration.at("requirements").empty() && configuration.at("backend")=="fixture_backend" &&
            configuration.at("backend_path")==Json::array({pathText(source).value()}),"Installed PEP517 configuration lost the in-tree backend contract or invented build dependencies.");
        Json query=configuration;query["source"]=pathText(source).value();query["overlay"]=pathText(overlay).value();query["mode"]="build_requires";
        const auto required=execute(query);require(required.at("result").at("requirements")==Json::array({"fixture-build-dep>=1"}) &&
            required.at("process").at("output").get_ref<const std::string&>().find("NOISY_FIXTURE_BUILD_REQUIRES")!=std::string::npos,
            "Noisy installed build-requirements hook lost its actual result or bounded stdout evidence.");
        const auto children=readJson(source/L"build-child-observations.json");require(children.size()==2U&&children[0].at("exit_code")==0&&children[1].at("exit_code")==0&&
            required.at("build_interpreter").at("embedded")==true,"Installed portable source hook did not use the native staged interpreter for its actual child commands.");
        query["mode"]="build_metadata";query["output"]=pathText(output).value();const auto metadata=execute(query);
        if(prepared){require(metadata.at("result").contains("metadata_directory") &&
                metadata.at("process").at("output").get_ref<const std::string&>().find("NOISY_FIXTURE_BUILD_METADATA")!=std::string::npos,
                "Installed metadata hook failed to separate noisy stdout from its structured result.");
            require(execute({{"mode","directory_metadata"},{"path",metadata.at("result").at("metadata_directory")}}).at("result")==expectedMetadata,
                "Prepared metadata did not retain exact name, version, Python requirement and conditional dependencies.");
            query["metadata_directory"]=metadata.at("result").at("metadata_directory");
        }else require(metadata.at("result")==Json{{"requires_wheel",true}},"Missing optional metadata hook did not request the supported wheel fallback.");
        query["mode"]="build_wheel";const auto built=execute(query);const auto wheel=Fs::path{built.at("result").at("wheel").get<std::string>()};
        require(wheel==output/L"forge_source_fixture-1.0-py3-none-any.whl" && Fs::is_regular_file(wheel) &&
            built.at("process").at("output").get_ref<const std::string&>().find("NOISY_FIXTURE_BUILD_WHEEL")!=std::string::npos,
            "Installed wheel hook lost its exact output filename, actual file or noisy process evidence.");
        const auto actualMetadata=execute({{"mode","wheel_metadata"},{"path",pathText(wheel).value()}}).at("result");
        require(actualMetadata==expectedMetadata,
            "Built wheel metadata differs from its requested synthetic package or conditional requirements.");
        const auto equivalentVersion=execute({{"mode","version_equal"},{"actual",actualMetadata.at("version")},{"expected","1.0.0"}}).at("result");
        require(equivalentVersion==Json{{"equal",true}},"Installed wheel metadata rejected its equivalent pinned source release spelling.");
        const std::string inspectWheel=R"FORGEWHEEL(import json,sys,zipfile
with zipfile.ZipFile(sys.argv[1]) as wheel:
 print(json.dumps({'files':sorted(wheel.namelist()),'payload':wheel.read('forge_source_fixture/__init__.py').decode(),'corrupt_entry':wheel.testzip()}))
)FORGEWHEEL";
        const auto inspected=runProcess(python,{"-I","-B","-c",inspectWheel,pathText(wheel).value()},python.parent_path(),source/L"inspect-wheel.log",context);
        require(inspected.at("exit_code")==0,"Installed interpreter could not inspect the generated fixture wheel.");const auto content=Json::parse(inspected.at("output").get<std::string>());
        require(content.at("files")==Json::array({"forge_source_fixture-1.0.dist-info/METADATA","forge_source_fixture-1.0.dist-info/RECORD","forge_source_fixture-1.0.dist-info/WHEEL","forge_source_fixture/__init__.py"}) &&
            content.at("payload")=="fixture_value = 'from sealed synthetic source'\n" && content.at("corrupt_entry").is_null(),
            "Actual built wheel contents differ from the sealed synthetic source or contain corrupt ZIP data.");
        std::ofstream{source/L"pyproject.toml",std::ios::trunc}<<"[build-system]\nrequires = ['fixture-declared-backend>=1']\nbuild-backend = 'fixture_backend'\nbackend-path = ['.']\n";
        require(execute({{"mode","build_configuration"},{"source",pathText(source).value()},{"name","forge-source-fixture"}}).at("result").at("requirements")==Json::array({"fixture-declared-backend>=1"}),
            "Installed build-system parser omitted a declared standard build requirement.");
        cases.push_back({{"name",prepared?"noisy_prepared_metadata_and_wheel":"missing_metadata_hook_build_wheel_fallback"},{"configuration",configuration},
            {"dynamic_build_requirements",required.at("result")},{"metadata_result",metadata.at("result")},{"wheel_identity",fileFacts(wheel,context)},{"wheel_metadata",actualMetadata},
            {"source_version_pin","1.0.0"},{"source_version_comparison",equivalentVersion},{"wheel_content",content},
            {"build_interpreter",required.at("build_interpreter")},{"child_observations",children}});
    }
    {
        const auto source=directory/L"incompatible-source-version",overlay=source/L"inactive-build-deps",output=source/L"generated";
        Fs::create_directories(overlay);std::ofstream{source/L"pyproject.toml"}<<"[build-system]\nrequires = []\nbuild-backend = 'fixture_backend'\nbackend-path = ['.']\n";
        auto changed=backend;for(const auto& replacement:{std::pair{std::string{"DIST=NAME+'-1.0.dist-info'"},std::string{"DIST=NAME+'-1.1.dist-info'"}},
            std::pair{std::string{"Version: 1.0\\nRequires-Python:"},std::string{"Version: 1.1\\nRequires-Python:"}},
            std::pair{std::string{"filename=NAME+'-1.0-py3-none-any.whl'"},std::string{"filename=NAME+'-1.1-py3-none-any.whl'"}}}){
            const auto location=changed.find(replacement.first);require(location!=std::string::npos,"Synthetic mismatched metadata fixture lost its exact version field.");changed.replace(location,replacement.first.size(),replacement.second);
        }
        std::ofstream{source/L"fixture_backend.py"}<<changed;
        auto query=execute({{"mode","build_configuration"},{"source",pathText(source).value()},{"name","forge-source-fixture"}}).at("result");
        query["mode"]="build_wheel";query["source"]=pathText(source).value();query["overlay"]=pathText(overlay).value();query["output"]=pathText(output).value();
        const auto wheel=Fs::path{execute(query).at("result").at("wheel").get<std::string>()};const auto metadata=execute({{"mode","wheel_metadata"},{"path",pathText(wheel).value()}}).at("result");
        require(metadata.at("version")=="1.1" && wheel.filename()==L"forge_source_fixture-1.1-py3-none-any.whl","Installed mismatched source fixture did not produce the actual different wheel version.");
        const auto comparison=execute({{"mode","version_equal"},{"actual",metadata.at("version")},{"expected","1.0.0"}}).at("result");
        require(comparison==Json{{"equal",false}},"Installed source version comparison accepted a different built wheel release.");
        cases.push_back({{"name","different_built_wheel_release_rejected"},{"source_version_pin","1.0.0"},{"wheel_identity",fileFacts(wheel,context)},
            {"wheel_metadata",metadata},{"source_version_comparison",comparison}});
    }
    {
        const auto source=directory/L"overlay-console-launcher",overlay=source/L"inactive-build-deps",wheel=source/L"forge_console_fixture-1.0-py3-none-any.whl";
        Fs::create_directories(overlay);std::ofstream{overlay/L"forge_overlay_probe.py"}<<"value='sealed_overlay'\n";
        std::ofstream{overlay/L"forge_fixture.pth"}<<"import os; os.environ['FORGE_BUILD_PTH']=str(os.getpid())\n";
        const std::string createWheel=R"FORGECONSOLE(import base64,csv,hashlib,io,sys,zipfile
name='forge_console_fixture'; dist=name+'-1.0.dist-info'
cli='import json,os,sys,forge_overlay_probe\ndef main():\n print(json.dumps({"value":forge_overlay_probe.value,"pth":os.environ.get("FORGE_BUILD_PTH"),"pid":os.getpid(),"executable":sys.executable}))\n'
files={name+'/__init__.py':'',name+'/cli.py':cli,dist+'/METADATA':'Metadata-Version: 2.1\nName: forge-console-fixture\nVersion: 1.0\n\n',dist+'/WHEEL':'Wheel-Version: 1.0\nGenerator: ForgeSyntheticFixture\nRoot-Is-Purelib: true\nTag: py3-none-any\n',dist+'/entry_points.txt':'[console_scripts]\nforge-build-console-fixture = forge_console_fixture.cli:main\n'}
record=io.StringIO(); writer=csv.writer(record,lineterminator='\n')
for path,content in files.items():
 raw=content.encode(); sha=base64.urlsafe_b64encode(hashlib.sha256(raw).digest()).rstrip(b'=').decode(); writer.writerow([path,'sha256='+sha,len(raw)])
writer.writerow([dist+'/RECORD','','']); files[dist+'/RECORD']=record.getvalue()
with zipfile.ZipFile(sys.argv[1],'w',compression=zipfile.ZIP_STORED) as archive:
 for path,content in files.items(): archive.writestr(path,content)
)FORGECONSOLE";
        const auto created=runProcess(python,{"-I","-B","-c",createWheel,pathText(wheel).value()},python.parent_path(),source/L"create-console-wheel.log",context);
        require(created.at("exit_code")==0&&Fs::is_regular_file(wheel),"Installed interpreter could not create the local console-script fixture wheel.");
        const auto staged=stageBuildInterpreter(python,overlay,overlay/L".forge-python",context,
            [&](const Fs::path& path,Domain::FileAccess){require(contained(python.parent_path(),path)||contained(overlay,path),"Console fixture interpreter staging left its installed source or inactive overlay.");},[](std::uint64_t){});
        const Fs::path buildPython{staged.at("python").get<std::string>()};require(staged.at("embedded")==true&&buildPython!=python,"Console fixture did not stage the installed portable interpreter.");
        const auto installed=runProcess(buildPython,{"-I","-B","-m","pip","--isolated","--disable-pip-version-check","install","--no-index","--no-deps","--no-compile","--no-cache-dir","--only-binary",":all:","--target",pathText(overlay).value(),pathText(wheel).value()},buildPython.parent_path(),source/L"install-console-wheel.log",context);
        require(installed.at("exit_code")==0,"Offline inactive console wheel installation failed: "+installed.at("output").get<std::string>());
        const auto scriptScheme=runProcess(buildPython,{"-I","-B","-c","import json,sys; from pip._internal.locations import get_scheme; scheme=get_scheme('',home=sys.argv[1]); print(json.dumps({'scripts':scheme.scripts,'data':scheme.data}))",pathText(overlay).value()},buildPython.parent_path(),source/L"console-script-scheme.log",context);
        require(scriptScheme.at("exit_code")==0,"Installed pip could not report its actual target home script scheme.");const auto scheme=NativeTools::Windows::ComfyDetail::parse(scriptScheme.at("output").get<std::string>());const Fs::path scripts{scheme.at("scripts").get<std::string>()};require(contained(overlay,scripts),"Installed pip target script scheme left the inactive fixture overlay.");
        const auto launcher=scripts/L"forge-build-console-fixture.exe";require(Fs::is_regular_file(launcher),"Offline pip did not generate the fixture's Windows console launcher at its actual home scheme: "+pathText(launcher).value());
        writeJson(source/L"console-launcher-expected.json",{{"launcher",pathText(launcher).value()}});
        const std::string consoleBackend=R"FORGECONSOLE(import json,os,shutil,subprocess,sys
def get_requires_for_build_wheel(config_settings=None):
    assert os.environ.get('PIP_NO_INDEX')=='1'
    launcher=shutil.which('forge-build-console-fixture')
    with open('console-launcher-expected.json',encoding='utf-8') as stream: expected=json.load(stream)['launcher']
    assert launcher and os.path.normcase(os.path.realpath(launcher))==os.path.normcase(os.path.realpath(expected)),launcher
    child=subprocess.run([launcher],capture_output=True,text=True)
    assert child.returncode==0,child.stderr
    result=json.loads(child.stdout)
    assert result['value']=='sealed_overlay' and result['pth']==str(result['pid'])
    assert os.path.normcase(os.path.realpath(result['executable']))==os.path.normcase(os.path.realpath(sys.executable)),result
    with open('console-child-observation.json','w',encoding='utf-8') as stream: json.dump({'launcher':launcher,'exit_code':child.returncode,'result':result},stream)
    print('NOISY_FIXTURE_CONSOLE_LAUNCHER')
    return []
)FORGECONSOLE";
        std::ofstream{source/L"pyproject.toml"}<<"[build-system]\nrequires = []\nbuild-backend = 'fixture_backend'\nbackend-path = ['.']\n";std::ofstream{source/L"fixture_backend.py"}<<consoleBackend;
        auto query=execute({{"mode","build_configuration"},{"source",pathText(source).value()},{"name","forge-console-fixture"}}).at("result");query["mode"]="build_requires";query["source"]=pathText(source).value();query["overlay"]=pathText(overlay).value();const auto required=execute(query);const auto child=readJson(source/L"console-child-observation.json");
        require(required.at("result").at("requirements").empty()&&required.at("process").at("output").get_ref<const std::string&>().find("NOISY_FIXTURE_CONSOLE_LAUNCHER")!=std::string::npos&&child.at("exit_code")==0&&child.at("result").at("value")=="sealed_overlay"&&child.at("result").at("pth")==std::to_string(child.at("result").at("pid").get<unsigned long>())&&
            !_wcsicmp(Fs::path{child.at("result").at("executable").get<std::string>()}.c_str(),buildPython.c_str()),"Actual PEP517 console child lost the clone interpreter, overlay-only imports or its own .pth execution.");
        cases.push_back({{"name","offline_overlay_console_launcher_staged_interpreter"},{"fixture_wheel",fileFacts(wheel,context)},{"inactive_installation",installed},{"script_scheme",scheme},{"script_scheme_process",scriptScheme},{"launcher_identity",fileFacts(launcher,context)},{"build_interpreter",staged},{"dynamic_build_requirements",required.at("result")},{"child_observation",child}});
    }
    const auto after=execute({{"mode","inventory"}}).at("result");require(before==after,"Offline source-hook qualification changed the installed interpreter package inventory.");
    Infrastructure::Windows::BCryptSha256Hasher hasher;const auto source=take(hasher.sha256(std::as_bytes(std::span{PackageContracts.data(),PackageContracts.size()}))).value();
    return {{"qualification","installed_interpreter_offline_source_hooks"},{"interpreter",pathText(python).value()},{"interpreter_identity",fileFacts(python,context)},
        {"package_contract_sha256",source},{"cases",cases},{"package_inventory_unchanged",true},
        {"dependency_installations",1U},{"provider_dependency_installations",0U},{"inactive_fixture_wheel_installations",1U},{"generation_posts",0U},{"fixture_scope","Synthetic source, wheels and one offline pip --no-index --no-deps installation exist only inside the scoped temporary fixture. No publisher access or installed-provider package mutation is invoked."}};
}
}
}
int main(int argc,char** argv){using namespace ForgeConductor::Tests;
    if(argc==4&&std::string_view{argv[1]}=="--fixture-process-lifetime"){
        try{return runLifetimeFixtureProcess(argv[2],Fs::path{ForgeConductor::NativeTools::Windows::ComfyDetail::wide(argv[3])});}
        catch(const std::exception& error){std::cerr<<error.what();return 76;}
        catch(const ForgeConductor::NativeTools::Windows::ComfyDetail::Failure& error){std::cerr<<error.error.message;return 77;}
    }
    // Native child fixture for preparation process boundaries; packaging
    // semantics are qualified separately against the actual installed Python.
    if((argc==5&&std::string_view{argv[1]}=="-I"&&std::string_view{argv[2]}=="-c")||(argc==6&&std::string_view{argv[1]}=="-I"&&std::string_view{argv[2]}=="-B"&&std::string_view{argv[3]}=="-c")){
        std::ifstream input{Fs::path{argv[argc-1]}};const auto query=Json::parse(input);const auto emit=[&](const Json& result){if(query.contains("result_path"))ForgeConductor::NativeTools::Windows::ComfyDetail::writeJson(Fs::path{query.at("result_path").get<std::string>()},result);else std::cout<<result.dump();};
        std::array<wchar_t,32768> executable{};const auto count=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));if(count==0U||count>=executable.size())return 80;
        auto directory=Fs::path{executable.data()}.parent_path();auto contracts=directory/L"fixture-package-contracts.json";const auto mode=query.at("mode").get<std::string>();
        if(!Fs::is_regular_file(contracts)&&(mode=="build_requires"||mode=="build_metadata"||mode=="build_wheel")){
            using namespace ForgeConductor::NativeTools::Windows::ComfyDetail;
            const auto transaction=Fs::absolute(Fs::path{wide(argv[argc-1])}).parent_path().lexically_normal(),fixtureRoot=transaction.parent_path().parent_path().parent_path();const auto buildRoot=fixtureRoot/L"portable"/L"forge-builds.disabled"/transaction.filename();const Fs::path overlay{wide(query.at("overlay").get<std::string>())},source{wide(query.at("source").get<std::string>())},result{wide(query.at("result_path").get<std::string>())};
            if(transaction.parent_path().filename()!=L"preparations"||transaction.parent_path().parent_path().filename()!=L"runtime"||result.parent_path()!=transaction||!contained(buildRoot,source)||!contained(buildRoot,overlay)||!Fs::equivalent(directory,overlay/L".forge-python"))return 82;
            // Staging copies interpreter components only. Read canned responses
            // from this exact fixture's baseline; inventory audits and failure
            // markers must continue to observe that baseline directory.
            directory=fixtureRoot/L"portable"/L"python_embeded";contracts=directory/L"fixture-package-contracts.json";if(!Fs::is_regular_file(contracts))return 83;
            const auto observation=directory/L"fixture-hook-runtime.json";auto observed=Fs::is_regular_file(observation)?readJson(observation):Json::object();observed[mode]={{"executable",ForgeConductor::NativeTools::Windows::ComfyDetail::pathText(Fs::path{executable.data()})},{"overlay",ForgeConductor::NativeTools::Windows::ComfyDetail::pathText(overlay)},{"source",ForgeConductor::NativeTools::Windows::ComfyDetail::pathText(source)}};writeJson(observation,observed);
        }
        if(Fs::is_regular_file(contracts)){std::ifstream contract{contracts};const auto responses=Json::parse(contract);
            if(query.contains("requirements")&&query.at("requirements").empty()&&(mode=="requirements"||mode=="check")){emit(mode=="requirements"?Json::array():Json{{"compatible",true},{"conflicts",Json::array()}});return 0;}
            if(responses.contains("source_fixture")){const auto& source=responses.at("source_fixture");const auto& descriptor=source.at("descriptor");const auto name=descriptor.at("name").get<std::string>(),version=descriptor.at("version").get<std::string>();
                if(mode=="resolve"){if(query.value("isolated_build",false)){emit({{"selected",Json::object()},{"requirements",query.at("requirements")},{"versions",Json::object()}});return 0;}const auto publisher=query.at("publishers").value(name,Json::object());const auto built=publisher.value("built",Json::object());if(!built.contains(version)){emit({{"build",descriptor}});return 0;}const auto& wheel=built.at(version);emit({{"selected",{{name,{{"name",name},{"version",version},{"filename",wheel.at("filename")},{"url",wheel.at("url")},{"sha256",wheel.at("digests").at("sha256")},{"bytes",wheel.at("size")},{"local_wheel",wheel.at("local_wheel")}}}}},{"requirements",query.at("requirements")},{"versions",{{name,version}}}});return 0;}
                if(mode=="build_wheel"){const auto wheel=Fs::path{query.at("output").get<std::string>()}/L"fixture_source-1.0-py3-none-any.whl";Fs::copy_file(Fs::path{source.at("wheel").get<std::string>()},wheel);std::cout<<"Fixture source backend build log.";emit({{"wheel",pathText(wheel).value()}});return 0;}
                if(mode=="build_requires"&&source.value("fail_hook",false)){std::ofstream{directory/L"fixture-hook-changed-inventory.txt"}<<"changed";std::cout<<"Fixture source hook failed after changing the observed inventory.";return 79;}
                if(mode=="build_requires"&&source.value("omit_result",false)){ForgeConductor::NativeTools::Windows::ComfyDetail::writeJson(directory/L"fixture-missing-result-query.json",query);return 0;}
                if(mode=="inventory"&&Fs::is_regular_file(directory/L"fixture-hook-changed-inventory.txt")){emit(source.at("changed_inventory"));return 0;}}
            if(responses.contains(mode)){emit(responses.at(mode));return 0;}
            if(mode=="requirements_file"&&responses.contains("requirements")){const auto path=Fs::path{query.at("path").get<std::string>()};const auto facts=ForgeConductor::NativeTools::Windows::ComfyDetail::fileFacts(path,TestContext{}.active());Json requirements=Json::array();for(const auto& item:responses.at("requirements"))requirements.push_back(item.at("requirement"));std::cout<<Json{{"requirements",requirements},{"constraints",Json::array()},{"files",Json::array({{{"path",pathText(path).value()},{"sha256",facts.at("sha256")},{"bytes",facts.at("bytes")}}})}}.dump();return 0;}}
        if(query.at("mode")!="inventory")return 79;
        if(Fs::is_regular_file(directory/L"fixture-pause-fail-inventory.txt")){std::ofstream{directory/L"fixture-inventory-reached.txt"}<<"inventory reached";
            for(unsigned attempt=0U;attempt<1000U;++attempt){if(Fs::is_regular_file(directory/L"fixture-inventory-release.txt")){std::cout<<"Fixture external package inventory failed.";return 79;}std::this_thread::sleep_for(std::chrono::milliseconds{10});}return 81;}
        const auto editPath=Fs::path{executable.data()}.parent_path()/L"fixture-edit-on-inventory.json";
        if(Fs::is_regular_file(editPath)){std::ifstream change{editPath};const auto edit=Json::parse(change);std::ofstream{Fs::path{edit.at("path").get<std::string>()},std::ios::trunc}<<edit.at("contents").get<std::string>();}
        std::cout<<Json{{"python_version","3.13.11"},{"tags",Json::array({"cp313-cp313-win_amd64"})},{"packages",Json::object()}}.dump();return 0;
    }
    if(argc==3&&std::string_view{argv[1]}=="--cross-volume-preparation"){try{std::cout<<qualifyCrossVolumePreparation(Fs::path{argv[2]}).dump(2);return 0;}catch(const std::exception& error){std::cerr<<error.what();return 1;}}
    if(argc==3&&std::string_view{argv[1]}=="--package-contracts-python"){try{std::cout<<qualifyInstalledPackageContracts(Fs::path{argv[2]}).dump(2);return 0;}catch(const std::exception& error){std::cerr<<error.what();return 1;}}
    if(argc==3&&std::string_view{argv[1]}=="--source-contracts-python"){try{std::cout<<qualifyInstalledSourceHooks(Fs::path{argv[2]}).dump(2);return 0;}catch(const std::exception& error){std::cerr<<error.what();return 1;}}
    std::string_view suite;if(argc==3&&std::string_view{argv[1]}=="--suite"){suite=argv[2];if(suite!="core"&&suite!="preparation"&&suite!="preparation-lifecycle"&&suite!="preparation-transfer"&&suite!="automatic-setup"&&suite!="package-resolution"){std::cerr<<"Unknown ComfyUI backend test suite.";return 2;}}
    else if(argc>1){Json arguments=Json::array();for(int index=1;index<argc;++index)arguments.push_back(argv[index]);std::cout<<arguments.dump();return 78;}
    TestRegistry tests{{"streaming-inspection-authority",inspectHashesAndRejectsOutsideScope},{"typed-workflow-export-collision",apiWorkflowPatchesAndExportConflicts},{"invalid-control-disabled-shutdown",invalidOperationsAndShutdownRejectBeforeEffects},
    {"starter-catalog-absolute-preview-final-typed-import",starterCatalogPathsImportExactPreviewAndFinalGraphs},
    {"independent-fixture-exclusive-unavailable-provider-ports",independentFixturesReserveDistinctUnavailableEndpoints},
    {"embedded-build-interpreter-copy-reuse-partial-reserve-baseline",embeddedBuildInterpreterPreservesProviderAndRetainsSealedCopies},
    {"native-websocket-silent-poll-delayed-fragment-binary-disconnect",nativeWebSocketPollingRetainsSilentAndFragmentedReads},
    {"native-websocket-cancel-deadline-pending-close-lifetime",nativeWebSocketWaitsRespectCancellationDeadlinesAndPendingDestruction},
    {"native-websocket-cdp-matching-delayed-response-message-bounds",nativeCdpCallsPreserveResponseMatchingAndMessageBounds},
    {"transient-hard-owner-exit-descendants-durable-provider-lifetime",transientProcessTreeEndsWithHardOwnerExitAndDurableProcessSurvives},
    {"interrupted-artifact-backend-restart",interruptedArtifactTransferReattachesAfterBackendRestart},
    {"artifact-core-windows-provider-subfolder-separators",artifactCollectionAcceptsWindowsProviderSeparators},
    {"corrupt-artifact-publication-suppressed",corruptArtifactRemainsUnpublished},
    {"artifact-content-header-filename-vhs",artifactContentOverridesDescriptorAndHeader},
    {"media-content-mime-iso-brands-ebml-vint-doc-type",mediaContainerMimeUsesObservedContentIdentities},
    {"artifact-batch-canonical-destination-aliases-no-publication",artifactBatchRejectsCanonicalDestinationAliasesBeforePublication},
    {"artifact-partial-publication-manifest-reconciliation",partialPublicationRetainsActualBatchManifestAndStaging},
    {"wrapped-selected-model-content-sha-identity",wrappedSelectedModelIdentitySealsActualContent},
    {"schema-catalog-model-formats-category-directory-content-identity",schemaAndCatalogModelIdentitySealsAllFormatsAndExactCategories},
    {"clip-text-embedding-reference-content-inventory-identity",clipEmbeddingReferencesSealCompleteContentOnlyWhenReferenced},
    {"custom-node-setup-included-requirement-content-identity",customNodeDependencyMetadataEditsInvalidateContentIdentity},
    {"yaml-physical-model-embedding-category-content-identity",identityUsesIndependentPhysicalYamlModelAndEmbeddingCategories},
    {"preparation-resume-cumulative-partial-budget",preparationResumeChargesRetainedPartialBytes},
    {"preparation-resume-budget-ledger-authority",preparationResumeCannotResetBudgetOrEscapeLedger},
    {"preparation-target-traversal-no-publication",preparationRejectsTraversalBeforePublication},
    {"preparation-model-reuse-baseline-readiness-failure",preparationReusesModelsAndRetainsBaselineOnReadinessFailure},
    {"preparation-wrapped-model-discovery-enum-readiness",preparationChecksWrappedModelLiteralsAgainstLiveEnums},
    {"automatic-setup-disabled-graph-no-publisher-metadata",disabledAutomaticSetupRejectsMissingGraphDependenciesBeforePublisherMetadata},
    {"preparation-interrupted-activation-exact-rollback",interruptedActivationRollbackRestoresSearchPath},
    {"preparation-external-activation-edit-unreconciled",preparationRejectsChangedInterruptedActivation},
    {"preparation-free-reserve-before-staging",preparationEnforcesFreeSpaceBeforeStaging},
    {"native-transfer-error-body-accounting-http-download",nativeTransferCountsErrorBodiesAndAcceptsHttpDependencies},
    {"native-http-dependency-redirect-accounting-bounds",nativeHttpDependencyRedirectsPreserveAccountingAndBounds},
    {"native-transfer-truncation-over-budget-accounting",nativeTransferCountsTruncatedAndOverBudgetBodies},
    {"native-transfer-durable-receive-error-body-exact-budget",nativeReceiveAdmissionsBoundErrorBodiesAndExactBudget},
    {"preparation-durable-receive-crash-resume-no-double-charge",preparationReceiveResumeRetainsUncertainAndMeasuredBytesOnce},
    {"preparation-legacy-receive-terminal-totals-uncertainty",preparationLegacyReceiveRecoveryPreservesExactTerminalReceipts},
    {"preparation-backend-resumed-metadata-admission-before-effects",preparationBackendPersistsInterruptedMetadataAdmissionBeforeEffects},
    {"ffmpeg-versioned-publisher-asset-digest-contract",ffmpegResolverUsesDatedPublisherAssetContractAndDigest},
    {"component-inactive-path-malformed-inventory-no-execute",collectionRejectsInactiveAndMalformedManagedComponentsBeforeExecution},
    {"preparation-retained-verified-archive-rehash-reuse",preparationResumeRehashesAndReusesVerifiedArchives},
    {"directory-seal-configured-long-paths",dependencyDirectorySealSupportsConfiguredLongPaths},
    {"installed-yaml-model-root-native-normalization",modelIdentityNormalizesInstalledYamlPathSeparators},
    {"runtime-manager-filter-retains-custom-nodes-config",runtimeFiltersManagerWithoutMutatingInstalledConfiguration},
    {"runtime-manager-storage-source-categories-aliases",runtimeModelStorageLaunchKeepsActualCategoriesAndAliases},
    {"runtime-manager-yaml-alias-default-environment-order",runtimePathProjectionMatchesLoadedAliasDefaultAndEnvironmentOrder},
    {"runtime-manager-original-yaml-resolved-environment-fence",runtimeOriginalPathConfigurationAndResolvedEnvironmentFenceLiveProvider},
    {"active-installed-node-latest-publisher-metadata-identity",activeInstalledPublisherIdentityUsesLatestBindingAndCurrentMetadata},
    {"artifact-ownership-config-fence-before-transfer",artifactOwnershipRejectsChangedRuntimeConfigurationBeforeTransfer},
    {"archive-reserve-allocation-links-size-bounds",archiveExpansionAccountsAllocationAndRejectsLinksAndInvalidSizes},
    {"configuration-atomic-replace-preserves-baseline",configurationReplacementRetainsOriginalWhenAtomicCommitFails},
    {"artifact-reserve-before-http-no-publication",artifactFreeSpaceReserveRejectsBeforeProviderTransfer},
    {"large-api-workflow-retained-bounded-export",largeApiWorkflowRemainsExportableAfterBoundedImportFailure},
    {"directory-seal-publisher-generated-cache-content",directoryContentSealsDistinguishPublisherEditsAndGeneratedFiles},
    {"directory-recovery-generated-edit-unsealed",directoryRecoveryRetainsGeneratedFilesAndRejectsUnsealedOrEditedContent},
    {"directory-reuse-content-integrity-baseline",directoryReuseChecksInstalledContentBeforeReadiness},
    {"model-content-change-same-size-mtime-file-id",selectedModelIdentityDetectsSameSizeAndTimestampContentChanges},
    {"private-input-copy-seal-namespace-content",privateInputSealFencesNamespaceAndDetectsChangedProviderBytes},
    {"status-bounded-unrelated-queue-busy-counts",statusBoundsUnrelatedQueueGraphsAndRetainsBusyCounts},
    {"general-image-large-dimensions-streamed-bytes-legacy-fence",generalImageDeliveryDecodesLargeFramesWithoutWeakeningLegacyLimits},
    {"active-media-component-content-inactive-extraction-fence",mediaComponentIdentitySealsActiveFilesAndIgnoresRetainedExtraction},
    {"rollback-rechecks-edited-directory-after-recovery",rollbackRetainsDirectoryEditedAfterRecoveryVerification},
    {"rollback-inactive-wheel-staging-prior-runtime-recovery",inactiveUnsealedWheelStagingDoesNotSuppressPriorRuntimeRecovery},
    {"preparation-active-recovery-busy-unavailable-unverified-provider",preparationRetainsActiveRecoveryWhenProviderIdleIsUnconfirmed},
    {"preparation-activation-live-owner-unavailable-provider",preparationRejectsActivationWhenOwnedProcessIsLiveButUnavailable},
    {"preparation-busy-provider-unchanged-activation-rollback",preparationRollbackRetainsUnchangedActivationWithBusyProvider},
    {"malformed-queue-no-readiness-stop-cancel-active-mutation",malformedProviderQueueCannotAuthorizeStopOrPreparationMutation},
    {"automatic-setup-disabled-baseline-reuse-missing-wheel-policy",automaticSetupDisabledReusesBaselineAndBlocksMissingPackagesBeforePublisherAccess},
    {"preparation-existing-tree-transfer-budget-independent",installedDependencySealsAreIndependentOfTransferBudget},
    {"package-resolution-registry-and-model-same-operation",registryNodeDiscoveryAlsoResolvesSelectedModelInSamePreparation},
    {"package-resolution-static-file-selectors-upload-contracts",declaredModelSelectorsCheckFilesAndPreserveAllUploadContracts},
    {"package-resolution-includes-constraints-setup-source-seals",preparationIncludesConstraintsAndSetupRequirementsWithFileSeals},
    {"package-resolution-installed-node-declarations-no-replacement",installedWorkflowNodeDeclaredPackagesAreInspectedWithoutNodeReplacement},
    {"package-resolution-configured-node-directory-single-file-binding",preparationBindsConfiguredCustomNodeDirectoriesAndSingleFilesWithoutParentRequirements},
    {"package-resolution-legacy-unsealed-node-source-recovery",preparationRejectsLegacyUnsealedCustomNodeSourceRecovery},
    {"package-resolution-live-enum-flat-model-before-publisher-subfolder",providerLiveEnumUsesActualCategoryPathBeforePublisherPreferredSubfolder},
    {"package-resolution-import-failed-node-installed-publisher-pin-reuse",missingImportedNodesReuseExactInstalledPublisherSource},
    {"package-resolution-installed-publisher-ambiguous-unproven-refusal",installedPublisherReuseRefusesAmbiguousOrUnprovenSources},
    {"package-resolution-installed-publisher-source-pin-restart-seals",retainedPublisherBindingRejectsChangedSourceOrPin},
    {"package-resolution-exact-yaml-category-reuse",providerCategoryPathsReuseOnlyExactMappedModelFile},
    {"package-resolution-source-build-budget-seals-inactive-failures",sourcePreparationReusesSealedBuildsAndRetainsInactiveFailures},
    {"package-resolution-source-build-interrupted-baseline-recovery",sourcePreparationRecoveryRequiresExactSavedBaselineInventory},
    {"publisher-negative-cache-retry-http-evidence-ledger",publisherMetadataRetryRetainsNegativeEvidenceAndCumulativeLedger},
    {"native-dependency-copy-chunks-reserve-cancel-size-seal",nativeDependencyCopyStreamsAndPreservesInactiveFailureEvidence}};
    try{for(const auto& [name,run]:tests){const auto packageResolution=name.starts_with("package-resolution");const auto automaticSetup=name.starts_with("automatic-setup");const auto transfer=name.starts_with("native-transfer")||name.starts_with("native-dependency")||name.starts_with("publisher")||name.starts_with("preparation-durable-receive")||name.starts_with("preparation-legacy-receive")||name.starts_with("preparation-backend-resumed-metadata");const auto lifecycle=name.starts_with("runtime-manager")||name.starts_with("rollback")||name.starts_with("directory-recovery")||name.starts_with("directory-reuse")||name.starts_with("preparation-active")||name.starts_with("preparation-activation")||name.starts_with("preparation-busy-provider")||name.starts_with("malformed-queue");const auto preparation=name.starts_with("preparation")||name.starts_with("rollback")||name.starts_with("directory")||automaticSetup||name.starts_with("publisher")||name.starts_with("native-transfer")||name.starts_with("native-dependency")||name.starts_with("ffmpeg")||name.starts_with("archive")||name.starts_with("configuration-atomic")||name.starts_with("malformed-queue")||name.starts_with("runtime-manager");if((suite=="package-resolution"&&!packageResolution)||(suite=="automatic-setup"&&!automaticSetup)||(suite=="preparation-transfer"&&!transfer)||(suite=="preparation-lifecycle"&&!lifecycle)||(suite=="preparation"&&(!preparation||packageResolution||automaticSetup||transfer||lifecycle))||(suite=="core"&&(preparation||packageResolution||automaticSetup||transfer)))continue;const auto started=std::chrono::steady_clock::now();run();std::cout<<"PASS "<<name<<" ("<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count()<<"ms)"<<std::endl;}}catch(const std::exception& error){std::cerr<<"FAIL comfy backend: "<<error.what()<<'\n';return 1;}return 0;}
