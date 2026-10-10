#include "Infrastructure/TestSupport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/NativeTools/Windows/WindowsComfyUiService.h"
#include "NativeTools/Windows/ImageProviderCodec.h"
#include "NativeTools/Windows/ProviderOperationLease.h"
#include "NativeTools/Windows/ComfyUiNativeSupport.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <barrier>
#include <optional>
#include <set>
#include <thread>

namespace ForgeConductor::Tests {
namespace {
using Json=nlohmann::json;
using Service=NativeTools::Windows::WindowsComfyUiService;
using Infrastructure::Windows::WindowsWorkspaceAuthority;
using Infrastructure::Windows::WindowsWorkspaceAuthorityPolicy;
Domain::OperationContext context(){return TestContext{}.active();}
Domain::PathText pathText(const std::filesystem::path& p){return take(Domain::PathText::create(take(Infrastructure::Windows::Detail::strictUtf16ToUtf8(p.native()))));}
std::string digest(std::span<const std::byte> b){Infrastructure::Windows::BCryptSha256Hasher h;return take(h.sha256(b)).value();}
std::vector<std::byte> read(const std::filesystem::path& p){std::ifstream s(p,std::ios::binary);require(static_cast<bool>(s),"Fixture read failed.");std::vector<char>b{std::istreambuf_iterator<char>{s},{}};return {reinterpret_cast<const std::byte*>(b.data()),reinterpret_cast<const std::byte*>(b.data()+b.size())};}
void write(const std::filesystem::path& p,std::span<const std::byte> raw){std::ofstream s(p,std::ios::binary|std::ios::trunc);require(static_cast<bool>(s),"Fixture write failed.");s.write(reinterpret_cast<const char*>(raw.data()),static_cast<std::streamsize>(raw.size()));require(static_cast<bool>(s),"Fixture write was incomplete.");}
std::vector<std::byte> png(){NativeTools::Windows::Detail::ImageProviderPixels pixels{64U,64U,std::vector<std::byte>(64U*64U*4U,std::byte{42})};return take(NativeTools::Windows::Detail::encodeProviderImage(pixels,context()));}
void writeEnvelope(const std::filesystem::path& file,Json envelope,bool reseal){if(reseal){auto payload=envelope.at("payload").dump();envelope["sha256"]=digest(std::as_bytes(std::span{payload.data(),payload.size()}));}auto body=envelope.dump();write(file,std::as_bytes(std::span{body.data(),body.size()}));}
Json envelope(const std::filesystem::path& file){auto raw=read(file);return Json::parse(reinterpret_cast<const char*>(raw.data()),reinterpret_cast<const char*>(raw.data()+raw.size()));}
template<class Predicate>void await(Predicate condition,std::string_view failure){auto end=std::chrono::steady_clock::now()+std::chrono::seconds{3};while(!condition() && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds{5});require(condition(),failure);}
std::size_t mcpEnvelopeBytes(const Json& payload){
    const auto body=payload.dump();Json content=Json::array();
    if(body.size()<=32U*1024U)content.push_back({{"type","text"},{"text",body}});
    else{
        std::vector<std::string> parts;for(std::size_t start=0U;start<body.size();){auto end=(std::min)(body.size(),start+12U*1024U);while(end<body.size()&&(static_cast<unsigned char>(body[end])&0xc0U)==0x80U)--end;parts.push_back(body.substr(start,end-start));start=end;}
        for(std::size_t index=0U;index<parts.size();++index){auto fragment=Json{{"kind","forge_tool_result_fragment"},{"version",1U},{"index",index},{"count",parts.size()},{"total_bytes",body.size()},{"part",parts[index]},
            {"instruction","Concatenate part from every fragment in index order, then parse the complete JSON tool result. Do not repeat the tool call."}}.dump();require(fragment.size()<=32U*1024U,"Projected evidence exceeded the installed MCP fragment contract.");content.push_back({{"type","text"},{"text",fragment}});}
    }
    return Json{{"jsonrpc","2.0"},{"id",1U},{"result",{{"content",content},{"isError",false},{"structuredContent",payload}}}}.dump().size();
}
void requirePublicBudget(const Json& payload){require(payload.dump().size()<=128U*1024U,"Public projection exceeded 128 KiB.");require(mcpEnvelopeBytes(payload)<=1024U*1024U,"Public projection exceeded the serialized MCP response limit.");require(Json{{"body",payload.dump()}}.dump().size()<=1024U*1024U,"Public projection exceeded the escaped Manager response limit.");}
class PausingFiles final:public Contracts::IAtomicFileStore {
public:
    Infrastructure::Windows::WindowsAtomicFileStore delegate;
    std::atomic<bool> blockSecond{},releaseSecond{},failFirst{},failSecond{};
    std::atomic<unsigned> blocked{};
    std::mutex mutex;std::map<std::string,unsigned> writes;
    Domain::Result<std::vector<std::byte>> read(const Contracts::AuthorizedPath& path,std::size_t maximum,const Domain::OperationContext& c)noexcept override{return delegate.read(path,maximum,c);}
    Domain::Result<void> replace(const Contracts::AuthorizedPath& path,std::span<const std::byte> content,bool backup,const Domain::OperationContext& c)noexcept override {
        try {
            unsigned ordinal{};{std::lock_guard lock{mutex};ordinal=++writes[path.canonicalPath().value()];}
            if(ordinal==1U && failFirst.exchange(false))return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,"Controlled initial receipt persistence failure."));
            if(ordinal==2U && failSecond.exchange(false))return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,"Controlled worker-launch persistence failure."));
            if(ordinal==2U && blockSecond.load()) {
                ++blocked;
                while(!releaseSecond.load()) {
                    if(c.isCancellationRequested())return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::Cancelled,"Controlled receipt gate cancelled."));
                    if(c.isExpired(std::chrono::steady_clock::now()))return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::DeadlineExceeded,"Controlled receipt gate expired."));
                    std::this_thread::sleep_for(std::chrono::milliseconds{2});
                }
            }
            return delegate.replace(path,content,backup,c);
        }catch(const std::exception& e){return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,e.what()));}
    }
};
class Configuration final:public Contracts::IConfigurationStore {
public:
    std::mutex mutex;
    Domain::AppConfig config;
    Configuration(){config.comfyUi.enabled=true;config.comfyUi.endpoint="http://127.0.0.1:49172";}
    Domain::Result<Domain::AppConfig> load(const Domain::OperationContext&)noexcept override{std::lock_guard lock{mutex};return Domain::Result<Domain::AppConfig>::success(config);}
    Domain::Result<Domain::AppConfig> reload(const Domain::OperationContext& c)noexcept override{return load(c);}
    Domain::Result<Domain::AppConfig> update(const Domain::AppConfigPatch& p,const Domain::OperationContext&)noexcept override{std::lock_guard lock{mutex};auto r=Domain::applyConfigPatch(config,p);if(r)config=r.value();return r;}
    void shutdown()noexcept override{}
};
class RevocableAuthority final:public Contracts::IWorkspaceAuthority {
public:
    explicit RevocableAuthority(WindowsWorkspaceAuthority& delegate):delegate_(delegate){}
    std::atomic<bool> revoked{};
    Domain::Result<Contracts::WorkspaceAuthority> authorityFor(const Domain::ProjectId& project,const Domain::OperationContext& c)noexcept override{
        if(revoked.load())return Domain::Result<Contracts::WorkspaceAuthority>::failure(Domain::makeError(Domain::ErrorCodes::Unauthorized,"Fixture authority was revoked."));return delegate_.authorityFor(project,c);}
    Domain::Result<Contracts::WorkspaceAuthority> narrow(const Contracts::WorkspaceAuthority& authority,const std::vector<Domain::PathText>& roots,const std::vector<Domain::FileAccess>& grants,bool shell,std::uint64_t generation,const Domain::OperationContext& c)noexcept override{return delegate_.narrow(authority,roots,grants,shell,generation,c);}
    Domain::Result<Contracts::AuthorizedPath> authorize(const Contracts::WorkspaceAuthority& authority,const Domain::PathAuthorizationRequest& request,const Domain::OperationContext& c)noexcept override{return delegate_.authorize(authority,request,c);}
private:
    WindowsWorkspaceAuthority& delegate_;
};
class Backend final:public Contracts::IComfyUiBackend {
public:
    std::mutex mutex;
    std::map<std::string,Json> graphs;
    std::map<std::string,std::filesystem::path> providerInputs;
    std::atomic<unsigned> posts{},queueReads{},queueWrites{},cancellations{},controlCalls{},prepareCalls{};
    std::atomic<bool> loseAck{},running{},wrongGraph{},missingOutput{},changedIdentity{},foreignQueue{},rejectPrompt{},executionError{},partialAdmission{},prepareFails{};
    std::atomic<bool> largeEvidence{},largeSummary{};
    std::atomic<bool> largeIdentity{},changedIdentitySuffix{};
    std::atomic<bool> largeCatalog{},oversizedCatalog{};
    std::atomic<bool> normalizeLiteralWrappers{};
    std::atomic<bool> partialCollection{},blockCollection{},collectionEntered{};
    std::atomic<bool> blockIdentity{},identityEntered{};
    std::atomic<unsigned> normalizedCancellations{};
    Json localCatalog=Json::array();
    Json modelFolders=Json::array({"checkpoints","text_encoders","diffusion_models"});
    Json modelChoices={{"checkpoints",Json::array({"installed.safetensors","Wan/draft.safetensors","Wan/final.safetensors"})},
        {"text_encoders",Json::array({"encoder.safetensors"})},{"diffusion_models",Json::array({"wan-5b.gguf"})}};
    std::vector<std::string> modelRequests;
    Json preparationArguments=Json::object();
    std::string artifactMediaType{"image/png"};
    Json artifactMetadata{{"width",64U},{"height",64U},{"decoded",true}};
    Json collectionMedia=Json::array();
    std::filesystem::path output;
    Backend(std::filesystem::path root):output(std::move(root)){}
    Domain::Result<std::string> perform(std::string_view op,std::string_view encoded,const Domain::ComfyUiConfig&,
        const Contracts::WorkspaceAuthority&,const Domain::OperationContext& operation)noexcept override {
        try {
            auto args=Json::parse(encoded);Json result=Json::object();std::lock_guard lock{mutex};
            if(op=="status")result={{"available",true}};
            else if(op=="control"){++controlCalls;result={{"ok",true}};}
            else if(op=="prepare"){++prepareCalls;preparationArguments=args;result=prepareFails.load()?Json{{"ok",false},{"downloaded_bytes",1234567U},{"error",{{"code",Domain::ErrorCodes::HostCapabilityUnavailable},{"message","Controlled missing dependency."}}},{"unresolved",Json::array({"missing-node"})}}:Json{{"ok",true},{"downloaded_bytes",0U}};
                if(largeEvidence.load())result["manifest"]={{"retained_log",std::string(600U*1024U,'L')}};if(largeSummary.load())result["summary"]=std::string(100000U,'"');}
            else if(op=="identity"){
                identityEntered=true;
                while(blockIdentity.load()&&!operation.isCancellationRequested()&&!operation.isExpired(std::chrono::steady_clock::now()))std::this_thread::sleep_for(std::chrono::milliseconds{2});
                result={{"runtime","test-fixture"},{"changed",changedIdentity.load()}};
                if(largeIdentity.load()){result["z_inventory"]=std::string(24U*1024U,'I');result["z_revision"]=changedIdentitySuffix.load()?"changed-publisher-revision":"original-publisher-revision";}
            }
            else if(op=="upload"){
                Json seals=Json::array(),bindings=Json::array();for(const auto& input:args.at("inputs")){
                    const auto raw=read(std::filesystem::path(input.at("path").get<std::string>()));const auto folder=args.at("namespace").get<std::string>();
                    const auto name=std::to_string(seals.size())+"_uploaded.png",inputName=input.value("input",std::string{"image"}),value=folder+"/"+name;
                    const Json descriptor{{"name",name},{"subfolder",folder},{"type","input"}};const auto destination=output/"private-provider-inputs"/folder/name;
                    std::filesystem::create_directories(destination.parent_path());write(destination,raw);providerInputs[descriptor.dump()]=destination;
                    seals.push_back({{"path",input.at("path")},{"sha256",digest(raw)},{"bytes",raw.size()},{"descriptor",descriptor},{"node_id",input.at("node_id")},
                        {"input",inputName},{"value",value},{"provider_sha256",digest(raw)},{"provider_bytes",raw.size()}});
                    bindings.push_back({{"node_id",input.at("node_id")},{"input",inputName},{"value",value}});
                }result={{"inputs",seals},{"bindings",bindings}};
            }
            else if(op=="seal_input"){
                const auto& descriptor=args.at("descriptor");if(descriptor.at("subfolder")!=args.at("namespace") || descriptor.at("type")!="input" || !providerInputs.contains(descriptor.dump()))
                    return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::IntegrityFailure,"Controlled unowned provider input."));
                const auto raw=read(providerInputs.at(descriptor.dump()));result={{"sha256",digest(raw)},{"bytes",raw.size()}};
            }
            else if(op=="cancel"){
                ++cancellations;
                if(normalizeLiteralWrappers.load()) {
                    const auto id=args.at("prompt_id").get<std::string>();
                    if(!graphs.contains(id) || !NativeTools::Windows::ComfyDetail::promptGraphMatches(args.at("graph"),graphs.at(id)))
                        return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::IntegrityFailure,"Controlled normalized cancellation graph differs from its exact sealed prompt."));
                    ++normalizedCancellations;
                }
                result={{"remote_cancel_confirmed",false},{"remote_execution_may_continue",true}};
            }
            else if(op=="catalog"){
                auto offset=args.value("offset",0U),limit=args.value("limit",32U);Json rows=Json::array();for(std::size_t index=offset;index<localCatalog.size()&&rows.size()<limit;++index)rows.push_back(localCatalog[index]);
                auto next=offset+rows.size();result={{"ok",true},{"kind",args.at("kind")},{"items",rows},{"offset",offset},{"total",localCatalog.size()},{"has_more",next<localCatalog.size()},{"next_offset",next<localCatalog.size()?Json(next):Json(nullptr)}};
            }
            else if(op=="inspect" || op=="seal"){
                auto raw=read(std::filesystem::path(args.at("path").get<std::string>()));result={{"sha256",digest(raw)},{"bytes",raw.size()}};
            } else if(op=="collect") {
                auto dest=std::filesystem::path(args.at("output_directory").get<std::string>());std::filesystem::create_directories(dest);
                auto file=dest/"actual.png";
                auto raw=png();write(file,raw);
                result={{"artifacts",Json::array({{{"node_id","2"},{"path",pathText(file).value()},{"media_type",artifactMediaType},{"sha256",digest(raw)},{"bytes",raw.size()},{"metadata",artifactMetadata},
                    {"provider_view_url","http://127.0.0.1:49172/view?filename=actual.png&subfolder=ForgeConductor%2Fproject%2Fjob%2Fpreview&type=output"}}})}};
                if(!collectionMedia.empty()) {
                    result["artifacts"]=Json::array();
                    for(const auto& media:collectionMedia) {
                        const auto outputFile=dest/("actual-"+media.at("node_id").get<std::string>()+".fixture");write(outputFile,raw);
                        result["artifacts"].push_back({{"node_id",media.at("node_id")},{"path",pathText(outputFile).value()},{"media_type",media.at("media_type")},
                            {"sha256",digest(raw)},{"bytes",raw.size()},{"metadata",media.at("metadata")}});
                    }
                }
                if(largeEvidence.load()){result["artifacts"][0]["metadata"]["raw"]=std::string(256U*1024U,'R');result["artifacts"][0]["preview"]={{"base64",std::string(180U*1024U,'A')}};}
                if(largeSummary.load())result["artifacts"][0]["metadata"]["publisher_notes"]=std::string(50000U,'"');
                if(partialCollection.load()) {
                    collectionEntered=true;
                    while(blockCollection.load() && !operation.isCancellationRequested() && !operation.isExpired(std::chrono::steady_clock::now())) std::this_thread::sleep_for(std::chrono::milliseconds{2});
                    const auto receipt=dest/"transfer.json";const auto evidence=Json{{"node_id","2"},{"destination",pathText(file).value()},{"transfer_receipt",pathText(receipt).value()},
                        {"sha256",digest(raw)},{"bytes",raw.size()},{"state","published_verified"}};const auto publicationJson=evidence.dump();
                    write(receipt,std::span<const std::byte>{reinterpret_cast<const std::byte*>(publicationJson.data()),publicationJson.size()});
                    result={{"ok",false},{"partial",true},{"artifacts",Json::array()},{"publication_evidence",Json::array({evidence})},
                        {"error",{{"code",operation.isCancellationRequested()?Domain::ErrorCodes::Cancelled:Domain::ErrorCodes::IntegrityFailure},{"message","Controlled failure after first artifact publication."}}}};
                    if(largeEvidence.load())result["retained_log"]=std::string(128U*1024U,'L');
                }
            } else if(op=="request") {
                auto route=args.at("route").get<std::string>();
                if(route=="/models" || route.starts_with("/models/")) {
                    modelRequests.push_back(route);result=route=="/models"?modelFolders:modelChoices.value(route.substr(8U),Json::array());
                }else if(route=="/object_info"){
                    for(unsigned index=0U;index<4U;++index)result["CatalogNode"+std::to_string(index)]={{"category",index%2U?"video/output":"image/output"},{"description",std::string(oversizedCatalog.load()?70000U:largeCatalog.load()?30000U:8U,'"')},{"input",Json::object()}};
                }else if(route.starts_with("/object_info/")) {
                    auto type=route.substr(13U);
                    if(type=="Source")result={{type,{{"input",{{"required",{{"value",Json::array({"STRING"})}}}}},{"output",Json::array({"IMAGE"})},{"output_node",false}}}};
                    else if((type=="PreviewDependency" || type=="FinalDependency") && prepareCalls.load()>0U)result={{type,{{"input",{{"required",{{"value",Json::array({"STRING"})}}}}},{"output",Json::array({"IMAGE"})},{"output_node",false}}}};
                    else if(type=="CustomArray")result={{type,{{"input",{{"required",{{"payload",Json::array({"*"})}}}}},{"output",Json::array({"IMAGE"})},{"output_node",false}}}};
                    else if(type=="UploadImage")result={{type,{{"input",{{"required",{{"image",Json::array({Json::array({"root-image.png"}),Json{{"image_upload",true}}})}}}}},{"output",Json::array({"IMAGE"})},{"output_node",false}}}};
                    else if(type=="OptionalUploadImage")result={{type,{{"input",{{"required",{{"value",Json::array({"STRING"})}}},{"optional",{{"reference",Json::array({"STRING",Json{{"image_upload",true}}})}}}}},{"output",Json::array({"IMAGE"})},{"output_node",false}}}};
                    else if(type=="LoadAudio" || type=="LoadVideo" || type=="OptionalLoadAudio" || type=="OptionalLoadVideo") {
                        const bool audio=type.find("Audio")!=std::string::npos,optional=type.starts_with("Optional");const auto flag=audio?"audio_upload":"video_upload",input=optional?"reference":audio?"audio":"file";
                        Json definitions=optional?Json{{"required",{{"value",Json::array({"STRING"})}}},{"optional",{{input,Json::array({"STRING",Json{{flag,true}}})}}}}:
                            Json{{"required",{{input,Json::array({Json::array({audio?"root-audio.wav":"root-video.mp4"}),Json{{flag,true}}})}}}};
                        result={{type,{{"input",definitions},{"output",Json::array({audio?"AUDIO":"VIDEO"})},{"output_node",false}}}};
                    }
                    else if(type=="ModelSource")result={{type,{{"input",{{"required",{{"model",Json::array({Json::array({"installed.safetensors"})})}}}}},{"output",Json::array({"IMAGE"})},{"output_node",false}}}};
                    else if(type=="SaveImage" || type=="CustomSaver")result={{type,{{"input",{{"required",{{"images",Json::array({type=="CustomSaver"?"*":"IMAGE"})},{"filename_prefix",Json::array({"STRING"})}}}}},{"output",Json::array()},{"output_node",true}}}};
                    else if(type=="SaveAnimatedPNG")result={{type,{{"input",{{"required",{{"images",Json::array({"IMAGE"})},{"filename_prefix",Json::array({"STRING"})},{"fps",Json::array({"FLOAT",Json{{"min",0.01},{"max",1000.0}}})},{"compress_level",Json::array({"INT",Json{{"min",0},{"max",9}}})}}}}},{"output",Json::array()},{"output_node",true}}}};
                    else if(type=="SaveAnimatedWEBP")result={{type,{{"input",{{"required",{{"images",Json::array({"IMAGE"})},{"filename_prefix",Json::array({"STRING"})},{"fps",Json::array({"FLOAT",Json{{"min",0.01},{"max",1000.0}}})},{"lossless",Json::array({"BOOLEAN"})},{"quality",Json::array({"INT",Json{{"min",0},{"max",100}}})},{"method",Json::array({Json::array({"default","fastest","slowest"})})}}}}},{"output",Json::array()},{"output_node",true}}}};
                    else if(type=="VideoSource")result={{type,{{"input",{{"required",{{"value",Json::array({"STRING"})}}}}},{"output",Json::array({"VIDEO"})},{"output_node",false}}}};
                    else if(type=="TypedSaver")result={{type,{{"input",{{"required",{{"video",Json::array({"VIDEO"})},{"filename_prefix",Json::array({"STRING"})}}}}},{"output",Json::array()},{"output_node",true}}}};
                    else if(type=="EnumSaver")result={{type,{{"input",{{"required",{{"images",Json::array({"IMAGE"})},{"format",Json::array({Json::array({"video/h264-mp4","image/gif"})})},{"filename_prefix",Json::array({"STRING"})}}}}},{"output",Json::array()},{"output_node",true}}}};
                } else if(route=="/prompt") {
                    ++posts;auto body=args.at("body");
                    if(rejectPrompt.load())return Domain::Result<std::string>::success(Json{{"http_status",400U},{"error",{{"type","prompt_outputs_failed_validation"},{"message","Controlled actual node error."}}},{"node_errors",{{"2",{{"errors",Json::array({{{"type","invalid_input"},{"message","Controlled actual input error."}}})}}}}}}.dump());
                    graphs[body.at("prompt_id").get<std::string>()]=body.at("prompt");
                    if(normalizeLiteralWrappers.load())for(auto& [id,node]:graphs.at(body.at("prompt_id").get<std::string>()).items()){
                        static_cast<void>(id);for(auto& [input,value]:node["inputs"].items()){
                            static_cast<void>(input);if(value.is_object() && value.contains("__value__"))value=value.at("__value__");
                        }
                    }
                    if(loseAck.load())return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::TransportClosed,"Controlled lost acknowledgement."));
                    result={{"prompt_id",body.at("prompt_id")},{"node_errors",partialAdmission.load()?Json{{"2",{{"errors",Json::array({{{"type","invalid_output"}}})}}}}:Json::object()}};
                } else if(route.starts_with("/history/")) {
                    auto id=route.substr(9U);
                    if(!running.load() && graphs.contains(id)) {
                        auto graph=graphs[id];if(wrongGraph.load())graph["1"]["inputs"]["value"]="foreign";
                        Json outputs{{"2",{{"images",Json::array()}}}};for(const auto& media:collectionMedia)outputs[media.at("node_id").get<std::string>()]={{"images",Json::array()}};
                        result[id]={{"prompt",Json::array({0U,id,graph})},{"outputs",missingOutput.load()?Json::object():outputs},
                            {"status",{{"completed",true},{"status_str",executionError.load()?"error":"success"},{"messages",executionError.load()?Json::array({"Controlled custom-node exception"}):Json::array()}}}};
                    }
                } else if(route=="/queue") {
                    ++queueReads;if(args.value("method",std::string{})=="POST")++queueWrites;
                    Json queue=Json::array();for(const auto& [id,g]:graphs)queue.push_back(Json::array({0U,id,g}));
                    result={{"queue_running",running.load()?queue:Json::array()},{"queue_pending",foreignQueue.load()?Json::array({Json::array({17U,"unrelated-provider-job",Json{{"foreign",{{"class_type","External"},{"inputs",Json::object()}}}}})}):Json::array()}};
                }
            }
            return Domain::Result<std::string>::success(result.dump());
        }catch(const std::exception& e){return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,e.what()));}
    }
    void shutdown()noexcept override{}
};
class Fixture {
public:
    std::filesystem::path root;
    PausingFiles files;
    Infrastructure::Windows::WindowsUuidGenerator uuids;
    Infrastructure::Windows::SystemClock clock;
    Infrastructure::Windows::BCryptSha256Hasher hasher;
    Configuration configuration;
    std::shared_ptr<Backend> backend;
    std::unique_ptr<WindowsWorkspaceAuthority> issuer,storageIssuer;
    std::unique_ptr<RevocableAuthority> revocable;
    std::unique_ptr<Contracts::WorkspaceAuthority> authority,storage;
    std::unique_ptr<Service> service;
    std::mutex chatMutex;
    Json messages=Json::array({{{"text","Create a test image"},{"message_index",0U},{"selected_version",0U}}});
    Json results=Json::array();std::size_t position{1U};
    Json observationExtras=Json::object();
    std::string conversation="native-owned-chat";
    std::vector<std::string> requestedBoundConversations;
    Fixture(){
        root=std::filesystem::temp_directory_path()/("ForgeConductor.ComfyUi."+take(uuids.next()).value());
        std::filesystem::create_directories(root/"workspace");std::filesystem::create_directories(root/"private");root=std::filesystem::canonical(root);
        backend=std::make_shared<Backend>(root/"workspace");
        issuer=std::make_unique<WindowsWorkspaceAuthority>(std::vector<WindowsWorkspaceAuthorityPolicy>{{parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000001"),project(),parse<Domain::ClientId>("comfy-manager"),{pathText(root/"workspace")},Domain::FileAccess::Write,{Domain::FileAccess::Read,Domain::FileAccess::Write,Domain::FileAccess::Create,Domain::FileAccess::Execute},{},true,1U}});
        storageIssuer=std::make_unique<WindowsWorkspaceAuthority>(std::vector<WindowsWorkspaceAuthorityPolicy>{{parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000002"),project(),parse<Domain::ClientId>("comfy-storage"),{pathText(root/"private")},Domain::FileAccess::Write,{Domain::FileAccess::Read,Domain::FileAccess::Write,Domain::FileAccess::Create},{},false,1U}});
        authority=std::make_unique<Contracts::WorkspaceAuthority>(take(issuer->authorityFor(project(),context())));storage=std::make_unique<Contracts::WorkspaceAuthority>(take(storageIssuer->authorityFor(project(),context())));reopen();
    }
    ~Fixture(){service.reset();std::error_code ignored;std::filesystem::remove_all(root,ignored);}
    static Domain::ProjectId project(){return parse<Domain::ProjectId>("10000000-0000-4000-8000-000000000001");}
    void reopen(){service.reset();if(!revocable)revocable=std::make_unique<RevocableAuthority>(*issuer);service=std::make_unique<Service>(*revocable,files,configuration,*storageIssuer,*storage,pathText(root/"private"),uuids,clock,hasher,backend);
        service->setConversationObserver([this](const Domain::ProjectId&,std::string_view boundConversation,const Domain::OperationContext&){std::lock_guard lock{chatMutex};requestedBoundConversations.emplace_back(boundConversation);Json observed{{"conversation_id",conversation},{"user_messages",messages},{"native_tool_results",results}};observed.update(observationExtras);return Domain::Result<std::string>::success(observed.dump());});}
    Json graph(){return {{"1",{{"class_type","Source"},{"inputs",{{"value","draft"}}}}},{"2",{{"class_type","SaveImage"},{"inputs",{{"images",Json::array({"1",0U})},{"filename_prefix","owner"}}}}}};}
    Json args(){return {{"stage","preview"},{"media_kind","image"},{"preview_workflow",graph()},{"final_workflow",graph()},{"output_directory",pathText(root/"workspace").value()},{"timeout_sec",5U}};}
    Domain::Result<std::string> invoke(std::string_view name,const Json& args){return service->execute(name,args.dump(),*authority,context());}
    Json execute(std::string_view name,const Json& args){return Json::parse(take(invoke(name,args)));}
    Json wait(const Json& job){auto result=execute("comfy_job_status",{{"job_id",job.at("job_id")},{"wait_sec",10U}});
        std::lock_guard lock{chatMutex};results.push_back({{"name","comfy_job_status"},{"plugin_identifier","mcp/forge-conductor"},{"message_index",position++},{"selected_version",0U},{"text_bodies",Json::array({result.dump()})}});return result;}
    void reply(std::string text){std::lock_guard lock{chatMutex};messages.push_back({{"text",std::move(text)},{"message_index",position++},{"selected_version",0U}});}
};
void rediscoverPreviewConversation(Fixture& f){
    std::lock_guard lock{f.chatMutex};
    f.observationExtras={{"bound_conversation",{{"conversation_id",f.conversation},{"user_messages",f.messages},{"native_tool_results",f.results}}}};
    f.conversation="native-preview-delivery-chat";
    f.messages=Json::array({{{"text","Show the existing saved preview and its proposed final settings."},{"message_index",0U},{"selected_version",0U}}});
    f.results=Json::array();f.position=1U;
}
void deliverPreview(Fixture& f,const Json& preview,std::string_view plugin="mcp/forge-conductor",std::string_view name="comfy_job_status"){
    std::lock_guard lock{f.chatMutex};
    f.results.push_back({{"name",name},{"plugin_identifier",plugin},{"message_index",f.position++},{"selected_version",0U},{"text_bodies",Json::array({preview.dump()})}});
}
void approvalAndExactlyOnce(){
    Fixture f;auto job=f.execute("comfy_run",f.args());auto preview=f.wait(job);
    require(preview.at("state")=="awaiting_preview_approval" && preview.at("requires_operator_approval")==true,"Preview did not await native operator approval.");
    require(f.backend->posts==1U,"Preview submitted more than once.");
    Json final{{"stage","final"},{"plan_id",job.at("plan_id")}};
    requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Unauthorized,"Model initiated final without a native user reply.");
    auto forged=final;forged["approved"]=true;
    requireError(f.invoke("comfy_run",forged),Domain::ErrorCodes::InvalidRequest,"Model approval flag was admitted.");
    f.reply(" APPROVED! ");auto completed=f.wait(f.execute("comfy_run",final));
    require(completed.at("state")=="completed" && completed.at("stage")=="final" && f.backend->posts==2U,"Approved final did not finish exactly once.");
    f.execute("comfy_run",final);f.execute("comfy_run",final);require(f.backend->posts==2U,"Duplicate final calls resubmitted inference.");
}
void pendingApprovalSurvivesReopen(){
    Fixture f;auto job=f.execute("comfy_run",f.args());f.wait(job);f.reopen();
    auto pending=f.wait(job);require(pending.at("state")=="awaiting_preview_approval" && f.backend->posts==1U,"Reopen lost pending approval or replayed inference.");
    f.reply("yes");auto done=f.wait(f.execute("comfy_run",{{"stage","final"},{"plan_id",job.at("plan_id")}}));
    require(done.at("state")=="completed" && f.backend->posts==2U,"Reopened plan did not accept actual subsequent user approval.");
}
void changedConversationAndDependencies(){
    Fixture f;auto job=f.execute("comfy_run",f.args());f.wait(job);f.reply("render final");Json final{{"stage","final"},{"plan_id",job.at("plan_id")}};
    f.conversation="foreign-chat";requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Conflict,"Foreign conversation approval was admitted.");
    f.conversation="native-owned-chat";f.backend->changedIdentity=true;requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Conflict,"Changed dependency identity bypassed preview approval.");
    f.backend->changedIdentity=false;{std::lock_guard lock{f.chatMutex};f.messages[0]["selected_version"]=1U;}
    requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Conflict,"Edited native conversation prefix was admitted.");require(f.backend->posts==1U,"Refused approvals produced inference.");
}
void lostAcknowledgementReattach(){
    Fixture f;f.backend->loseAck=true;auto job=f.execute("comfy_run",f.args());auto unknown=f.wait(job);
    require(unknown.at("state")=="unknown" && f.backend->posts==1U,"Lost acknowledgement did not retain uncertain single submission.");
    f.reopen();auto status=f.wait(job);require(status.at("state")=="unknown" && f.backend->posts==1U,"Status/reconstruction replayed generation.");
    f.backend->loseAck=false;auto resumed=f.wait(f.execute("comfy_job_resume",{{"job_id",job.at("job_id")}}));
    require(resumed.at("state")=="awaiting_preview_approval" && f.backend->posts==1U,"Exact job resume did not retrieve the existing preview without replay.");
}
void normalizedLiteralGraphMatcherIsNarrow(){
    Fixture f;auto submitted=f.graph();submitted["1"]={{"class_type","CustomArray"},{"inputs",{{"payload",{{"__value__",Json::array({1U,true,"literal"})}}}}}};
    submitted["3"]={{"class_type","Source"},{"inputs",{{"value",{{"__value__","unreachable"}}}}}};
    auto observed=submitted;observed["1"]["inputs"]["payload"]=submitted["1"]["inputs"]["payload"]["__value__"];
    const auto matches=[&](const Json& graph){return NativeTools::Windows::ComfyDetail::promptGraphMatches(submitted,graph);};
    require(matches(submitted) && matches(observed),"Exact or selectively normalized provider graph lost its sealed ownership identity.");
    require(!NativeTools::Windows::ComfyDetail::promptGraphMatches(observed,submitted),"Matcher allowed a provider to insert a wrapper into an unwrapped submitted graph.");
    const std::vector<std::function<void(Json&)>> changes{
        [](Json& graph){graph["1"]["inputs"]["payload"][0]=2U;},
        [](Json& graph){graph["1"]["class_type"]="ForeignArray";},
        [](Json& graph){graph["1"]["inputs"]["extra"]="foreign";},
        [](Json& graph){graph["1"]["inputs"].erase("payload");},
        [](Json& graph){graph["1"]["_meta"]={{"title","changed"}};},
        [](Json& graph){graph.erase("3");},
        [](Json& graph){graph["4"]={{"class_type","Source"},{"inputs",{{"value","foreign"}}}};},
        [](Json& graph){graph["3"]["inputs"]["value"]="changed unreachable";}
    };
    for(const auto& change:changes){auto altered=observed;change(altered);require(!matches(altered),"Literal normalization matcher admitted an unrelated graph change.");}
    auto nested=submitted;nested["1"]["inputs"]["payload"]={{"nested",{{"__value__",Json::array({1U,2U})}}}};auto stripped=nested;stripped["1"]["inputs"]["payload"]["nested"]=Json::array({1U,2U});
    require(!NativeTools::Windows::ComfyDetail::promptGraphMatches(nested,stripped),"Matcher normalized a wrapper outside the exact input value.");
    auto extra=submitted;extra["1"]["inputs"]["payload"]["retained"]="must not disappear";
    require(!NativeTools::Windows::ComfyDetail::promptGraphMatches(extra,observed),"Matcher silently discarded an extra wrapper field.");
}
void providerLiteralArrayPreflightMatchesInstalledContract(){
    Fixture f;auto graph=f.graph();graph["1"]={{"class_type","CustomArray"},{"inputs",Json::object()}};
    const std::vector<Json> invalid{
        {{"value",Json::array({1U,true,"literal"})},{"code","literal_array_requires_wrapper"}},
        {{"value",Json::array({1U,2U})},{"code","literal_array_requires_wrapper"}},
        {{"value",{{"__value__",Json::array({"1",0U})}}},{"code","unsupported_literal_link_shape"}},
        {{"value",{{"__value__",Json::array({"1",0.0})}}},{"code","unsupported_literal_link_shape"}},
        {{"value",{{"__value__",Json::array({"1",true})}}},{"code","unsupported_literal_link_shape"}},
        {{"value",{{"__value__",Json::array({1U,2U})},{"extra","discarded by provider"}}},{"code","unsupported_literal_wrapper"}}
    };
    for(const auto& example:invalid){graph["1"]["inputs"]["payload"]=example.at("value");const auto check=f.execute("comfy_validate",{{"workflow",graph}});
        require(check.at("ready")==false && std::any_of(check.at("issues").begin(),check.at("issues").end(),[&](const auto& issue){return issue.at("code")==example.at("code");}),"Preflight did not identify the installed provider's literal-array boundary.");}
    for(const auto& literal:std::vector<Json>{Json::array({1U,2U}),Json::array({1U,true,"literal"})}){graph["1"]["inputs"]["payload"]={{"__value__",literal}};require(f.execute("comfy_validate",{{"workflow",graph}}).at("ready")==true,"Supported wrapped non-link literal array was rejected by preflight.");}
    require(f.backend->posts==0U && f.execute("comfy_job_list",Json::object()).at("total")==0U,"Literal-array preflight created durable work or submitted a generation.");
}
void normalizedLiteralArraysSurviveHistoryRecoveryAndCancellation(){
    Fixture f;f.backend->normalizeLiteralWrappers=true;f.backend->loseAck=true;auto args=f.args();auto graph=f.graph();
    const auto literal=Json::array({1U,true,"literal"});graph["1"]={{"class_type","CustomArray"},{"inputs",{{"payload",{{"__value__",literal}}}}}};args["preview_workflow"]=graph;args["final_workflow"]=graph;
    const auto admitted=f.execute("comfy_run",args),uncertain=f.wait(admitted);require(uncertain.at("state")=="unknown" && f.backend->posts==1U,"Normalized literal-array lost acknowledgement did not retain one uncertain submission.");
    const auto receipt=std::filesystem::path(uncertain.at("receipt_path").get<std::string>());const auto saved=envelope(receipt).at("payload");
    const auto sealedGraph=saved.at("graph").dump();
    require(saved.at("graph")["1"]["inputs"]["payload"]==Json{{"__value__",literal}} && saved.at("graph_sha256")==digest(std::as_bytes(std::span{sealedGraph.data(),sealedGraph.size()})),"Provider normalization rewrote the exact submitted graph or its durable hash.");
    {std::lock_guard lock{f.backend->mutex};require(f.backend->graphs.at(saved.at("prompt_id").get<std::string>())["1"]["inputs"]["payload"]==literal,"Fixture did not retain actual provider normalization before losing acknowledgement.");}
    f.reopen();f.backend->loseAck=false;const auto preview=f.wait(f.execute("comfy_job_resume",{{"job_id",admitted.at("job_id")}}));
    require(preview.at("state")=="awaiting_preview_approval" && f.backend->posts==1U,"Normalized history was refused or recovery submitted another preview.");
    f.reply("approved");require(f.wait(f.execute("comfy_run",{{"stage","final"},{"plan_id",admitted.at("plan_id")}})).at("state")=="completed" && f.backend->posts==2U,"Approved normalized literal-array plan failed exact final execution.");
    Fixture foreign;foreign.backend->normalizeLiteralWrappers=true;foreign.backend->wrongGraph=true;auto foreignArgs=foreign.args();foreignArgs["preview_workflow"]=graph;foreignArgs["final_workflow"]=graph;
    const auto refused=foreign.wait(foreign.execute("comfy_run",foreignArgs));require(refused.at("state")=="unknown" && refused.at("artifacts").empty() && refused.at("publication_suppressed")==true && refused.at("remote_execution_may_continue")==true &&
        refused.at("error").at("code").get<std::string>()==Domain::ErrorCodes::IntegrityFailure && foreign.backend->posts==1U,"Allowed literal normalization concealed a foreign history field: "+refused.dump());
    foreign.reopen();const auto refusedAgain=foreign.wait(foreign.execute("comfy_job_resume",{{"job_id",refused.at("job_id")}}));
    require(refusedAgain.at("state")=="unknown" && refusedAgain.at("artifacts").empty() && refusedAgain.at("publication_suppressed")==true &&
        refusedAgain.at("error").at("code").get<std::string>()==Domain::ErrorCodes::IntegrityFailure && foreign.backend->posts==1U,"Restart/resume admitted foreign normalized history or resubmitted the sealed prompt: "+refusedAgain.dump());
    Fixture cancelled;cancelled.backend->normalizeLiteralWrappers=true;cancelled.backend->running=true;auto cancelArgs=cancelled.args();cancelArgs["preview_workflow"]=graph;cancelArgs["final_workflow"]=graph;
    const auto active=cancelled.execute("comfy_run",cancelArgs);await([&]{return cancelled.backend->posts==1U && cancelled.backend->queueReads>=2U;},"Normalized provider queue was not observed before cancellation.");
    cancelled.execute("comfy_job_cancel",{{"job_id",active.at("job_id")}});const auto stopped=cancelled.wait(active);
    require(stopped.at("state")=="cancelled" && stopped.at("publication_suppressed")==true && stopped.at("artifacts").empty() && cancelled.backend->normalizedCancellations==1U && cancelled.backend->posts==1U,"Exact cancellation rejected normalized owned queue work or resubmitted it.");
}
void wrongHistoryAndPartialOutput(){
    Fixture f;f.backend->wrongGraph=true;auto bad=f.wait(f.execute("comfy_run",f.args()));
    require(bad.at("error").at("code").get<std::string>()==Domain::ErrorCodes::IntegrityFailure && bad.at("artifacts").empty(),"Foreign graph history produced an artifact.");
    f.backend->wrongGraph=false;f.backend->missingOutput=true;auto omitted=f.wait(f.execute("comfy_run",f.args()));
    require(omitted.at("error").at("code").get<std::string>()==Domain::ErrorCodes::IntegrityFailure && omitted.at("artifacts").empty(),"Partial output was declared complete.");
}
void disabledAndPreflightBoundaries(){
    Fixture f;f.configuration.config.comfyUi.enabled=false;auto status=f.execute("comfy_status",Json::object());require(!status.at("available") && !status.at("configured"),"Disabled status claimed availability.");
    requireError(f.invoke("comfy_run",f.args()),Domain::ErrorCodes::HostCapabilityUnavailable,"Disabled automation rendered.");
    f.configuration.config.comfyUi.enabled=true;auto graph=f.graph();graph["2"]["inputs"]["images"]=Json::array({"absent",0U});
    auto preflight=f.execute("comfy_validate",{{"workflow",graph}});require(preflight.at("ready")==false && preflight.at("server_validation_performed")==false && f.backend->posts==0U,"Preflight submitted work or hid an invalid link.");
    graph=f.graph();graph["2"]["inputs"]["images"]=Json::array({"1",2U});require(!f.execute("comfy_validate",{{"workflow",graph}}).at("ready").get<bool>(),"Preflight admitted a source output index absent from its actual node schema.");
    graph=f.graph();graph["1"]["class_type"]="MissingNode";require(!f.execute("comfy_validate",{{"workflow",graph}}).at("ready").get<bool>(),"Preflight admitted an unavailable custom node.");
    graph=f.graph();graph["1"]={{"class_type","ModelSource"},{"inputs",{{"model","missing.safetensors"}}}};require(!f.execute("comfy_validate",{{"workflow",graph}}).at("ready").get<bool>(),"Preflight admitted a model absent from the discovered choices.");
    graph=f.graph();graph["1"]={{"class_type","CustomArray"},{"inputs",{{"payload",{{"__value__",Json::array({1U,true,"literal"})}}}}}};
    require(f.execute("comfy_validate",{{"workflow",graph}}).at("ready").get<bool>() && f.backend->posts==0U,"Preflight treated an explicit literal array as a graph link or submitted inference.");
    graph=f.graph();graph["1"]={{"class_type","UploadImage"},{"inputs",{{"image","ForgeConductor/private/uploaded.png"}}}};
    auto uploaded=f.execute("comfy_validate",{{"workflow",graph}});require(uploaded.at("ready").get<bool>() && uploaded.at("warnings").size()==1U && uploaded.at("warnings")[0].at("code")=="provider_file_validation_required" && !uploaded.at("server_validation_performed").get<bool>(),"A private image upload was refused by the root-folder widget enumeration or falsely reported server validation.");
}
void cancellationAndList(){
    Fixture f;f.backend->running=true;auto job=f.execute("comfy_run",f.args());
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds{3};while(f.backend->posts==0U && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds{5});
    auto cancelled=f.execute("comfy_job_cancel",{{"job_id",job.at("job_id")}});f.wait(job);
    require(cancelled.at("publication_suppressed")==true,"Cancellation did not suppress publication.");
    auto listing=f.execute("comfy_job_list",Json::object());require(listing.at("jobs").size()==1U,"Job list did not recover admitted project job.");
    for(const auto offset:{100001U,1000000U}){
        const auto beyond=f.execute("comfy_job_list",{{"offset",offset},{"limit",1U}});requirePublicBudget(beyond);
        require(beyond.at("jobs").empty() && beyond.at("unreadable_jobs").empty() && beyond.at("has_more")==false && beyond.at("next_offset").is_null() && beyond.at("total")==1U,
            "An advertised job-list offset above the old bound did not return an empty terminal page.");
    }
    requireError(f.invoke("comfy_job_list",{{"offset",1000001U}}),Domain::ErrorCodes::InvalidRequest,"Job-list offset exceeded its advertised maximum.");
    f.backend->running=false;auto recovered=f.wait(f.execute("comfy_job_resume",{{"job_id",job.at("job_id")}}));
    require(recovered.at("state")=="awaiting_preview_approval" && f.backend->posts==1U,"Cancellation resume submitted another generation.");
}
void simultaneousFinalIsOneSubmission(){
    Fixture f;auto job=f.execute("comfy_run",f.args());f.wait(job);f.reply("yes");
    const Json final{{"stage","final"},{"plan_id",job.at("plan_id")}};
    std::barrier start{3};std::optional<Domain::Result<std::string>> first,second;
    std::thread one{[&]{start.arrive_and_wait();first.emplace(f.invoke("comfy_run",final));}};
    std::thread two{[&]{start.arrive_and_wait();second.emplace(f.invoke("comfy_run",final));}};
    start.arrive_and_wait();one.join();two.join();
    require(first.has_value() && second.has_value() && *first && *second,"Concurrent approved final calls did not return the same admitted plan.");
    const auto oneReceipt=Json::parse(take(std::move(*first))),twoReceipt=Json::parse(take(std::move(*second)));
    require(oneReceipt.at("job_id")==twoReceipt.at("job_id") && oneReceipt.at("stage")=="final" && twoReceipt.at("stage")=="final","Concurrent final calls admitted different plans.");
    auto completed=f.wait(job);require(completed.at("state")=="completed" && f.backend->posts==2U,"Concurrent final calls submitted more than one approved final.");
}
void approvalNeedsEarlierDeliveredPreview(){
    Fixture f;auto job=f.execute("comfy_run",f.args());
    auto preview=f.execute("comfy_job_status",{{"job_id",job.at("job_id")},{"wait_sec",10U}});
    require(preview.at("state")=="awaiting_preview_approval","Fixture preview did not finish.");
    f.reply("approved");const Json final{{"stage","final"},{"plan_id",job.at("plan_id")}};
    requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Unauthorized,"Approval before a saved preview tool result was admitted.");
    {std::lock_guard lock{f.chatMutex};f.results.push_back({{"name","comfy_job_status"},{"plugin_identifier","mcp/forge-conductor"},{"message_index",f.position++},{"selected_version",0U},{"text_bodies",Json::array({preview.dump()})}});}
    requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Unauthorized,"A later preview result retroactively approved an earlier reply.");
    f.reply("approved");{std::lock_guard lock{f.chatMutex};f.messages.back()["forge_generated"]=true;}
    requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Unauthorized,"Forge-generated continuity text was used as approval.");
    require(f.backend->posts==1U,"Undelivered or generated approval caused final inference.");
}
void resumedPreviewDeliveryPreservesApprovalBoundaries(){
    Fixture f;const auto job=f.execute("comfy_run",f.args());
    const auto preview=f.execute("comfy_job_status",{{"job_id",job.at("job_id")},{"wait_sec",10U}});
    require(preview.at("state")=="awaiting_preview_approval","Fixture preview did not reach pending approval before restart.");
    f.reopen();const auto resumed=f.execute("comfy_job_resume",{{"job_id",job.at("job_id")}});
    require(resumed.at("state")=="awaiting_preview_approval" && resumed.at("artifacts")==preview.at("artifacts") && f.backend->posts==1U,"Pending preview resume lost verified artifacts or submitted inference.");
    const Json final{{"stage","final"},{"plan_id",job.at("plan_id")}};
    {std::lock_guard lock{f.chatMutex};f.results.push_back({{"name","comfy_job_resume"},{"plugin_identifier","mcp/foreign"},{"message_index",f.position++},{"selected_version",0U},{"text_bodies",Json::array({resumed.dump()})}});}
    f.reply("yes");requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Unauthorized,"A foreign resume result supplied native preview delivery evidence.");
    {std::lock_guard lock{f.chatMutex};f.results.push_back({{"name","comfy_job_resume"},{"plugin_identifier","mcp/forge-conductor-fallback"},{"message_index",f.position++},{"selected_version",0U},{"text_bodies",Json::array({resumed.dump()})}});}
    requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Unauthorized,"A resume result retroactively approved an earlier operator reply.");
    f.reply(" Render final! ");const auto complete=f.wait(f.execute("comfy_run",final));
    require(complete.at("state")=="completed" && f.backend->posts==2U,"A subsequent real reply to the recovered preview did not render exactly one final.");

    Fixture latest;auto firstArgs=latest.args(),secondArgs=latest.args();const auto firstDirectory=latest.root/"workspace"/"first",secondDirectory=latest.root/"workspace"/"second";
    std::filesystem::create_directories(firstDirectory);std::filesystem::create_directories(secondDirectory);firstArgs["output_directory"]=pathText(firstDirectory).value();secondArgs["output_directory"]=pathText(secondDirectory).value();
    const auto first=latest.execute("comfy_run",firstArgs);latest.wait(first);const auto second=latest.execute("comfy_run",secondArgs);
    require(latest.execute("comfy_job_status",{{"job_id",second.at("job_id")},{"wait_sec",10U}}).at("state")=="awaiting_preview_approval","Second pending preview did not complete.");
    latest.reopen();const auto delivered=latest.execute("comfy_job_resume",{{"job_id",second.at("job_id")}});
    {std::lock_guard lock{latest.chatMutex};latest.results.push_back({{"name","comfy_job_resume"},{"plugin_identifier","mcp/forge-conductor"},{"message_index",latest.position++},{"selected_version",0U},{"text_bodies",Json::array({delivered.dump()})}});}
    latest.reply("approved");requireError(latest.invoke("comfy_run",{{"stage","final"},{"plan_id",first.at("plan_id")}}),Domain::ErrorCodes::Unauthorized,"Resume-delivered latest preview failed to supersede an older pending plan.");
    require(latest.wait(latest.execute("comfy_run",{{"stage","final"},{"plan_id",second.at("plan_id")}})).at("state")=="completed" && latest.backend->posts==3U,"Latest resumed preview did not bind approval to its saved final revision.");
}
void revisionReplyRequiresFreshPreviewDespiteLaterApproval(){
    Fixture f;const auto original=f.execute("comfy_run",f.args());const auto preview=f.wait(original);
    f.reply("make it blue");f.reply("yes");const Json originalFinal{{"stage","final"},{"plan_id",original.at("plan_id")}};
    requireError(f.invoke("comfy_run",originalFinal),Domain::ErrorCodes::Unauthorized,"A revision request followed by yes approved the unchanged saved final graph.");
    const auto invalidated=f.wait(original);
    require(invalidated.at("requires_new_preview")==true && invalidated.at("requires_operator_approval")==false && invalidated.at("approval_reply_choices").empty() &&
        invalidated.at("artifacts")==preview.at("artifacts") && invalidated.at("preview_revision_evidence")==Json{{"message_index",2U},{"selected_version",0U}} && f.backend->posts==1U,
        "Revision invalidation did not retain bounded evidence/artifacts or kept inviting approval of the old preview.");
    f.reopen();const auto recovered=f.execute("comfy_job_resume",{{"job_id",original.at("job_id")}});
    require(recovered.at("requires_new_preview")==true && recovered.at("requires_operator_approval")==false,"Manager restart or pending preview resume cleared a requested revision.");
    {std::lock_guard lock{f.chatMutex};f.results.push_back({{"name","comfy_job_resume"},{"plugin_identifier","mcp/forge-conductor"},{"message_index",f.position++},{"selected_version",0U},{"text_bodies",Json::array({recovered.dump()})}});}
    f.reply("approved");requireError(f.invoke("comfy_run",originalFinal),Domain::ErrorCodes::Unauthorized,"Redisplaying an invalidated preview erased the intervening revision request.");
    require(f.backend->posts==1U,"A later approval or recovered status submitted final work for the invalidated plan.");
    auto revisedArgs=f.args();revisedArgs["preview_workflow"]["1"]["inputs"]["value"]="blue";revisedArgs["final_workflow"]["1"]["inputs"]["value"]="blue";
    const auto revised=f.execute("comfy_run",revisedArgs);const auto revisedPreview=f.wait(revised);
    require(revisedPreview.at("requires_new_preview")==false && revisedPreview.at("requires_operator_approval")==true && f.backend->posts==2U,"A fresh revised preview inherited the prior plan's invalidation.");
    f.reply("yes");f.reply("\xc2\xa0\xe2\x80\x9c" "APPROVED" "\xe2\x80\x9d\xe2\x80\xaf");
    f.reply("Auto Continuity: recovered saved context");{std::lock_guard lock{f.chatMutex};f.messages.back()["forge_generated"]=true;}
    const Json revisedFinal{{"stage","final"},{"plan_id",revised.at("plan_id")}};const auto complete=f.wait(f.execute("comfy_run",revisedFinal));
    require(complete.at("state")=="completed" && f.backend->posts==3U,"Exact duplicate approvals, surrounding Unicode punctuation or excluded Forge continuity blocked the fresh revised final.");
    f.execute("comfy_run",revisedFinal);require(f.backend->posts==3U,"Duplicate final invocation resubmitted the approved revised graph.");
}
void successorRecoveryRetainsUnobservedRevisionAndRequiresFreshApproval(){
    const auto recover=[](Fixture& f,const Json& plan) {
        std::lock_guard lock{f.chatMutex};const auto predecessor=f.conversation;
        f.observationExtras={{"verified_predecessor",predecessor},{"verified_handoff_id","verified-packet"},
            {"predecessor_conversation",{{"conversation_id",predecessor},{"user_messages",f.messages},{"native_tool_results",f.results}}}};
        f.conversation="verified-successor-chat";f.messages=Json::array({{{"text","Resume this Forge project from its model-written continuity packet."},{"message_index",0U},{"selected_version",0U},{"forge_generated",true}}});
        const Json packet{{"ok",true},{"action","get"},{"found",true},{"handoff_id","verified-packet"},{"workspace_project_id",Fixture::project().value()},
            {"packet",{{"narrative","Pending render plan "+plan.at("plan_id").get<std::string>()+" awaits preview approval."}}}};
        f.results=Json::array({{{"name","context_get"},{"plugin_identifier","mcp/forge-conductor"},{"message_index",1U},{"selected_version",0U},{"text_bodies",Json::array({packet.dump()})}}});f.position=2U;
    };
    Fixture revised;const auto oldPlan=revised.execute("comfy_run",revised.args());revised.wait(oldPlan);revised.reply("make it blue");recover(revised,oldPlan);revised.reply("yes");
    requireError(revised.invoke("comfy_run",{{"stage","final"},{"plan_id",oldPlan.at("plan_id")}}),Domain::ErrorCodes::Unauthorized,"A predecessor revision followed by rollover and yes approved the original graph.");
    const auto invalidated=revised.wait(oldPlan);require(invalidated.at("requires_new_preview")==true && invalidated.at("requires_operator_approval")==false && invalidated.at("preview_revision_evidence")==Json{{"message_index",2U},{"selected_version",0U}} && revised.backend->posts==1U,"Rollover discarded the exact predecessor revision evidence or submitted final work.");
    revised.reopen();requireError(revised.invoke("comfy_run",{{"stage","final"},{"plan_id",oldPlan.at("plan_id")}}),Domain::ErrorCodes::Unauthorized,"Restart discarded a predecessor revision invalidation.");
    Fixture clean;const auto plan=clean.execute("comfy_run",clean.args());clean.wait(plan);recover(clean,plan);clean.reply("yes");
    const Json final{{"stage","final"},{"plan_id",plan.at("plan_id")}};requireError(clean.invoke("comfy_run",final),Domain::ErrorCodes::Conflict,"A recovered successor inherited a prior approval without its own preview boundary.");
    const auto rebound=clean.wait(plan);require(rebound.at("conversation_id")=="verified-successor-chat" && rebound.at("requires_operator_approval")==true && clean.backend->posts==1U,"Clean authoritative predecessor evidence could not rebind a pending preview.");
    requireError(clean.invoke("comfy_run",final),Domain::ErrorCodes::Unauthorized,"A pre-transfer successor yes approved a later delivered preview.");clean.reply("approved");
    require(clean.wait(clean.execute("comfy_run",final)).at("state")=="completed" && clean.backend->posts==2U,"A fresh real successor approval did not render the unchanged saved final exactly once.");
    for(const auto* fault:{"missing_predecessor","edited_prefix","foreign_context","wrong_handoff","uuid_suffix","failed_context"}) {
        Fixture f;const auto admitted=f.execute("comfy_run",f.args());f.wait(admitted);recover(f,admitted);
        {std::lock_guard lock{f.chatMutex};const std::string_view kind{fault};
            if(kind=="missing_predecessor") f.observationExtras.erase("predecessor_conversation");
            else if(kind=="edited_prefix") f.observationExtras["predecessor_conversation"]["user_messages"][0]["selected_version"]=1U;
            else if(kind=="foreign_context") f.results[0]["plugin_identifier"]="mcp/foreign";
            else {auto packet=Json::parse(f.results[0]["text_bodies"][0].get<std::string>());
                if(kind=="wrong_handoff")packet["handoff_id"]="other-packet";
                if(kind=="uuid_suffix")packet["packet"]["narrative"]=admitted.at("plan_id").get<std::string>()+"-other-plan";
                if(kind=="failed_context")packet["ok"]=false;f.results[0]["text_bodies"][0]=packet.dump();}
        }
        const Json blockedFinal{{"stage","final"},{"plan_id",admitted.at("plan_id")}};
        requireError(f.invoke("comfy_run",blockedFinal),Domain::ErrorCodes::Conflict,"Unverifiable predecessor or foreign/unmatched context recovery rebound a pending plan.");
        const auto blocked=f.wait(admitted);const bool needsNew=std::string_view{fault}=="missing_predecessor" || std::string_view{fault}=="edited_prefix";
        require(blocked.at("requires_new_preview")==needsNew && blocked.at("requires_operator_approval")==!needsNew && f.backend->posts==1U,"Failed recovery lost actionable preview renewal state or submitted provider work.");
        if(needsNew){f.reopen();f.reply("yes");requireError(f.invoke("comfy_run",blockedFinal),Domain::ErrorCodes::Unauthorized,"Missing/edited predecessor evidence was silently accepted after restart and yes.");}
    }
}
void rediscoveredPreviewApprovesExactlyOnceAfterRestart(){
    Fixture f;const auto plan=f.execute("comfy_run",f.args());const auto preview=f.wait(plan);f.reopen();
    rediscoverPreviewConversation(f);deliverPreview(f,preview);f.reply(" Verified! ");
    const Json final{{"stage","final"},{"plan_id",plan.at("plan_id")}};
    requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Unauthorized,"Verified acknowledgment was accepted as recovered preview approval.");
    const auto pending=f.execute("comfy_job_status",{{"job_id",plan.at("job_id")}});
    require(pending.at("requires_new_preview")==false && pending.at("requires_operator_approval")==true && f.backend->posts==1U,
        "Verified acknowledgment invalidated the existing preview or submitted final inference.");
    f.reply(" approved. ");std::barrier start{3};std::optional<Domain::Result<std::string>> first,second;
    std::thread one{[&]{start.arrive_and_wait();first.emplace(f.invoke("comfy_run",final));}};
    std::thread two{[&]{start.arrive_and_wait();second.emplace(f.invoke("comfy_run",final));}};
    start.arrive_and_wait();one.join();two.join();
    require(first.has_value() && second.has_value() && *first && *second,"Concurrent recovered approvals did not admit the same saved final.");
    const auto oneReceipt=Json::parse(take(std::move(*first))),twoReceipt=Json::parse(take(std::move(*second)));
    require(oneReceipt.at("job_id")==twoReceipt.at("job_id") && oneReceipt.at("stage")=="final" && twoReceipt.at("stage")=="final",
        "Concurrent recovery changed the exact admitted render plan.");
    const auto complete=f.wait(plan);require(complete.at("state")=="completed" && complete.at("conversation_id")=="native-preview-delivery-chat" && f.backend->posts==2U,
        "A real approval after exact preview rediscovery did not finish one final after restart.");
    const auto receipt=envelope(f.root/"private"/Fixture::project().value()/plan.at("job_id").get<std::string>()/"receipt.json").at("payload");
    require(receipt.at("approval_boundary")==1U && receipt.at("conversation_prefix").size()==1U && receipt.at("approval_evidence").at("message_index")==3U &&
        receipt.at("approved_revision")==receipt.at("revision") && receipt.at("preview_recovery_evidence").at("predecessor_conversation_id")=="native-owned-chat" &&
        receipt.at("preview_recovery_evidence").at("preview_message_index")==1U && receipt.at("preview_recovery_evidence").at("preview_selected_version")==0U &&
        receipt.at("preview_recovery_evidence").at("final_graph_sha256")==preview.at("final_graph_sha256"),
        "Rediscovery failed to retain the delivered preview boundary, original binding and exact approved final revision.");
    {std::lock_guard lock{f.chatMutex};require(std::find(f.requestedBoundConversations.begin(),f.requestedBoundConversations.end(),"native-owned-chat")!=f.requestedBoundConversations.end(),
        "Final recovery did not request the actual original native conversation evidence.");}
    f.reopen();f.execute("comfy_run",final);f.execute("comfy_run",final);require(f.backend->posts==2U,"Duplicate final calls after recovered completion and restart replayed generation.");

    Fixture fallback;const auto other=fallback.execute("comfy_run",fallback.args());fallback.wait(other);fallback.reopen();
    const auto resumed=fallback.execute("comfy_job_resume",{{"job_id",other.at("job_id")}});
    rediscoverPreviewConversation(fallback);deliverPreview(fallback,resumed,"mcp/forge-conductor-fallback","comfy_job_resume");fallback.reply("render final");
    require(fallback.wait(fallback.execute("comfy_run",{{"stage","final"},{"plan_id",other.at("plan_id")}})).at("state")=="completed" && fallback.backend->posts==2U,
        "Exact saved resume delivery from the canonical Fallback could not recover real operator approval.");
}
void rediscoveryNeedsApprovalAfterDeliveredNativePreview(){
    for(const auto* fault:{"no_reply","before_delivery","generated_reply","verified_only"}) {
        Fixture f;const auto plan=f.execute("comfy_run",f.args());const auto preview=f.wait(plan);rediscoverPreviewConversation(f);
        const std::string_view kind{fault};if(kind=="before_delivery")f.reply("approved");deliverPreview(f,preview);
        if(kind=="generated_reply"){f.reply("approved");std::lock_guard lock{f.chatMutex};f.messages.back()["forge_generated"]=true;}
        if(kind=="verified_only")f.reply("verified");
        const Json final{{"stage","final"},{"plan_id",plan.at("plan_id")}};
        requireError(f.invoke("comfy_run",final),kind=="verified_only"?Domain::ErrorCodes::Unauthorized:Domain::ErrorCodes::Conflict,
            "A missing, pre-delivery, generated or acknowledgment-only reply approved a recovered final.");
        const auto pending=f.execute("comfy_job_status",{{"job_id",plan.at("job_id")}});
        require(pending.at("state")=="awaiting_preview_approval" && pending.at("requires_new_preview")==false && f.backend->posts==1U,
            "An absent real subsequent approval invalidated the saved creative plan or submitted inference.");
    }
}
void rediscoveryRequiresExactLatestPlanAndArtifacts(){
    for(const auto* fault:{"wrong_plugin","job_id","plan_id","project_id","revision","final_graph_sha256","graph_sha256","dependency_identity",
        "stage","publication_suppressed","requires_new_preview","artifact_hash","artifact_path","artifact_bytes","artifact_node","artifact_media"}) {
        Fixture f;const auto plan=f.execute("comfy_run",f.args());auto delivered=f.wait(plan);rediscoverPreviewConversation(f);
        const std::string_view kind{fault};
        if(kind=="job_id" || kind=="plan_id" || kind=="project_id")delivered[std::string{kind}]="30000000-0000-4000-8000-000000000001";
        else if(kind=="revision")delivered["revision"]=delivered.at("revision").get<unsigned>()+1U;
        else if(kind=="final_graph_sha256" || kind=="graph_sha256")delivered[std::string{kind}]=std::string(64U,'0');
        else if(kind=="dependency_identity")delivered["dependency_identity"]["changed"]=true;
        else if(kind=="stage")delivered["stage"]="final";
        else if(kind=="publication_suppressed" || kind=="requires_new_preview")delivered[std::string{kind}]=true;
        else if(kind=="artifact_hash")delivered["artifacts"][0]["sha256"]=std::string(64U,'0');
        else if(kind=="artifact_path")delivered["artifacts"][0]["path"]=pathText(f.root/"workspace"/"different.png").value();
        else if(kind=="artifact_bytes")delivered["artifacts"][0]["bytes"]=delivered["artifacts"][0].at("bytes").get<std::uint64_t>()+1U;
        else if(kind=="artifact_node")delivered["artifacts"][0]["node_id"]="foreign-output";
        else if(kind=="artifact_media")delivered["artifacts"][0]["media_type"]="video/mp4";
        deliverPreview(f,delivered,kind=="wrong_plugin"?"mcp/foreign":"mcp/forge-conductor");f.reply("yes");
        requireError(f.invoke("comfy_run",{{"stage","final"},{"plan_id",plan.at("plan_id")}}),Domain::ErrorCodes::Conflict,
            "A foreign or stale delivered receipt changed the saved preview, output semantics or final revision during recovery.");
        require(f.backend->posts==1U,"Refused exact-plan rediscovery submitted final inference.");
    }
    Fixture latest;auto firstArgs=latest.args(),secondArgs=latest.args();
    const auto firstDirectory=latest.root/"workspace"/"first",secondDirectory=latest.root/"workspace"/"second";
    std::filesystem::create_directories(firstDirectory);std::filesystem::create_directories(secondDirectory);
    firstArgs["output_directory"]=pathText(firstDirectory).value();secondArgs["output_directory"]=pathText(secondDirectory).value();
    const auto first=latest.execute("comfy_run",firstArgs),firstPreview=latest.wait(first);
    const auto second=latest.execute("comfy_run",secondArgs),secondPreview=latest.wait(second);
    rediscoverPreviewConversation(latest);deliverPreview(latest,firstPreview);deliverPreview(latest,secondPreview);latest.reply("approved");
    requireError(latest.invoke("comfy_run",{{"stage","final"},{"plan_id",first.at("plan_id")}}),Domain::ErrorCodes::Conflict,
        "Recovery searched behind another plan's latest native preview to reuse one approval for an older final.");
    require(latest.backend->posts==2U,"Latest-other-plan rejection submitted an unapproved final.");
}
void rediscoveryPreservesOriginalConversationAndRevisionEvidence(){
    for(const auto* fault:{"missing_original","wrong_original","edited_prefix","missing_prefix","original_revision"}) {
        Fixture f;const auto plan=f.execute("comfy_run",f.args());const auto preview=f.wait(plan);
        const std::string_view kind{fault};if(kind=="original_revision")f.reply("make it blue");rediscoverPreviewConversation(f);deliverPreview(f,preview);f.reply("approved");
        {std::lock_guard lock{f.chatMutex};
            if(kind=="missing_original")f.observationExtras.erase("bound_conversation");
            else if(kind=="wrong_original")f.observationExtras["bound_conversation"]["conversation_id"]="another-native-chat";
            else if(kind=="edited_prefix")f.observationExtras["bound_conversation"]["user_messages"][0]["selected_version"]=1U;
            else if(kind=="missing_prefix")f.observationExtras["bound_conversation"]["user_messages"]=Json::array();
        }
        const Json final{{"stage","final"},{"plan_id",plan.at("plan_id")}};
        requireError(f.invoke("comfy_run",final),kind=="original_revision"?Domain::ErrorCodes::Unauthorized:Domain::ErrorCodes::Conflict,
            "Missing, edited or revised original native conversation evidence was bypassed by fresh-chat approval.");
        require(f.backend->posts==1U,"Invalid original preview history submitted final inference.");
        if(kind=="edited_prefix" || kind=="missing_prefix" || kind=="original_revision") {
            const auto pending=f.execute("comfy_job_status",{{"job_id",plan.at("job_id")}});
            require(pending.at("requires_new_preview")==true && pending.at("requires_operator_approval")==false,
                "An original conversation edit or unobserved revision was not durably invalidated.");
            if(kind=="original_revision")require(pending.at("preview_revision_evidence")==Json{{"message_index",2U},{"selected_version",0U}},
                "Unobserved original revision lost its actual selected-message position.");
            f.reopen();requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Unauthorized,
                "Restart cleared an original preview history invalidation and reused the fresh-chat approval.");
            require(f.backend->posts==1U,"Restarted invalidation submitted an old final.");
        }
    }
}
void rediscoveryCreativeRevisionStillRequiresNewPreview(){
    Fixture f;const auto plan=f.execute("comfy_run",f.args());const auto preview=f.wait(plan);rediscoverPreviewConversation(f);deliverPreview(f,preview);
    f.reply("make the fox blue");f.reply("approved");const Json final{{"stage","final"},{"plan_id",plan.at("plan_id")}};
    requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Unauthorized,"A later approval erased a creative revision after rediscovered preview delivery.");
    const auto pending=f.execute("comfy_job_status",{{"job_id",plan.at("job_id")}});
    require(pending.at("requires_new_preview")==true && pending.at("requires_operator_approval")==false &&
        pending.at("preview_revision_evidence")==Json{{"message_index",2U},{"selected_version",0U}} && f.backend->posts==1U,
        "Fresh-chat creative revision lost its selected-message evidence or submitted the unchanged final.");
    f.reopen();requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Unauthorized,"Restart allowed approval of a revised rediscovered preview.");
    require(f.backend->posts==1U,"Restart replayed final inference for a requested creative revision.");
}
void rediscoveryStillVerifiesInputArtifactAndDependencySeals(){
    for(const auto* fault:{"input","artifact","dependency"}) {
        Fixture f;const auto input=f.root/"workspace"/"input.png";const auto original=png();write(input,original);auto args=f.args();
        args["inputs"]=Json::array({{{"node_id","1"},{"input","value"},{"path",pathText(input).value()}}});
        const auto plan=f.execute("comfy_run",args),preview=f.wait(plan);rediscoverPreviewConversation(f);deliverPreview(f,preview);f.reply("yes");
        const std::string_view kind{fault};auto changed=original;changed.push_back(std::byte{1});
        if(kind=="input")write(input,changed);
        else if(kind=="artifact")write(std::filesystem::path(preview.at("artifacts")[0].at("path").get<std::string>()),changed);
        else f.backend->changedIdentity=true;
        requireError(f.invoke("comfy_run",{{"stage","final"},{"plan_id",plan.at("plan_id")}}),Domain::ErrorCodes::Conflict,
            "Exact native preview rediscovery bypassed changed input, artifact or actual runtime dependency seals.");
        require(f.backend->posts==1U,"Changed plan seals submitted final inference through recovery.");
    }
}
void rediscoveryMatchesBoundedIdentityAndVerifiesCompleteLiveEvidence(){
    for(const bool changed:{false,true}) {
        Fixture f;f.backend->largeIdentity=true;const auto plan=f.execute("comfy_run",f.args()),preview=f.wait(plan);
        const auto saved=envelope(f.root/"private"/Fixture::project().value()/plan.at("job_id").get<std::string>()/"receipt.json").at("payload").at("dependency_identity");
        const auto& projected=preview.at("dependency_identity");
        require(saved.dump().size()>16U*1024U && projected.at("truncated")==true && projected.at("full_evidence")=="saved_in_private_receipt" &&
            !projected.contains("z_inventory"),"Large dependency identity did not retain complete durable evidence and a bounded public preview projection.");
        f.reopen();rediscoverPreviewConversation(f);deliverPreview(f,preview);f.reply("approved");f.backend->changedIdentitySuffix=changed;
        const auto actual=Json::parse(take(f.backend->perform("identity",Json::object().dump(),f.configuration.config.comfyUi,*f.authority,context())));
        const auto prefix=projected.at("json_prefix").get<std::string>();
        require(actual.dump().substr(0U,prefix.size())==prefix && (actual==saved)==!changed,
            "Large identity fixture did not place the controlled publisher revision change outside the delivered public prefix.");
        const Json final{{"stage","final"},{"plan_id",plan.at("plan_id")}};
        if(changed) {
            requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Conflict,
                "An unchanged bounded identity prefix approved a different complete live dependency identity.");
            require(f.backend->posts==1U,"A publisher revision outside the public identity prefix submitted final inference.");
        } else {
            const auto complete=f.wait(f.execute("comfy_run",final));
            require(complete.at("state")=="completed" && complete.at("conversation_id")=="native-preview-delivery-chat" && f.backend->posts==2U,
                "Exact delivered bounded identity could not recover approval of the unchanged complete dependency inventory.");
            f.execute("comfy_run",final);require(f.backend->posts==2U,"Duplicate final with a recovered large identity resubmitted inference.");
        }
    }
}
void partialPublicationOutcomeSurvivesFailureCancellationAndRestart(){
    for(const bool cancel:{false,true}) {
        Fixture f;f.backend->partialCollection=true;f.backend->blockCollection=cancel;const auto admitted=f.execute("comfy_run",f.args());
        if(cancel){await([&]{return f.backend->collectionEntered.load();},"Partial publication did not reach its guarded cancellation boundary.");f.execute("comfy_job_cancel",{{"job_id",admitted.at("job_id")}});}
        const auto result=f.wait(admitted);const auto& outcome=result.at("collection_outcome");
        require(result.at("state")== (cancel?"cancelled":"failed") && result.at("artifacts").empty() && result.at("publication_suppressed")==true && outcome.at("ok")==false && outcome.at("partial")==true && outcome.at("publication_evidence").size()==1U,"Failed/cancelled collection discarded actual partial publication evidence or exposed successful delivery.");
        const auto& item=outcome.at("publication_evidence")[0];require(std::filesystem::exists(std::filesystem::path(item.at("destination").get<std::string>())) && std::filesystem::exists(std::filesystem::path(item.at("transfer_receipt").get<std::string>())) && item.at("sha256")==digest(read(std::filesystem::path(item.at("destination").get<std::string>()))) && item.at("state")=="published_verified","Partial publication diagnostic invented or lost its actual file/transfer identity.");
        requirePublicBudget(result);f.reopen();const auto recovered=f.wait(admitted);require(recovered.at("collection_outcome")==outcome && recovered.at("artifacts").empty() && f.backend->posts==1U,"Restart dropped partial evidence or replayed provider rendering.");
    }
}
void resumedAdmissionPublishesNonterminalStateBeforeProviderEntry(){
    using Lease=NativeTools::Windows::Detail::ProviderOperationLease;
    for(const bool large:{false,true}) {
        Fixture f;f.backend->partialCollection=true;f.backend->largeEvidence=large;const auto admitted=f.execute("comfy_run",f.args()),failed=f.wait(admitted);
        require(failed.at("state")=="failed" && failed.at("collection_outcome").at("ok")==false && f.backend->posts==1U,
            "Resume admission fixture did not retain an actual failed collection after one provider submission.");
        const auto receipt=std::filesystem::path(failed.at("receipt_path").get<std::string>());const auto sealed=envelope(receipt).at("payload");
        f.reopen();f.backend->partialCollection=false;
        auto held=take(Lease::acquire(f.configuration.config.comfyUi.endpoint,context()));
        const auto resumed=f.execute("comfy_job_resume",{{"job_id",admitted.at("job_id")}}),saved=envelope(receipt).at("payload");
        require(resumed.at("state")=="before_dispatch" && resumed.at("phase")=="waiting_provider_lease" && resumed.at("done")==false &&
            resumed.at("error").is_null() && !resumed.at("requires_operator_approval").get<bool>(),
            "Immediate exact-job resume retained the previous terminal failure while its worker waited for the provider lease.");
        require(saved.at("state")=="before_dispatch" && saved.at("phase")=="waiting_provider_lease" && saved.at("error").is_null() &&
            saved.at("prompt_id")==sealed.at("prompt_id") && saved.at("graph")==sealed.at("graph") && saved.at("graph_sha256")==sealed.at("graph_sha256"),
            "Resume admission did not persist its nonterminal state or changed the exact provider submission seal.");
        auto historical=saved.at("collection_outcome");require(resumed.at("collection_outcome").at("previous_attempt")==true && historical.at("previous_attempt")==true,
            "Retained collection error was presented as the new recovery attempt or lost its historical label during bounded projection.");
        historical.erase("previous_attempt");require(historical==sealed.at("collection_outcome") && saved.at("collection_outcome").at("error")==sealed.at("collection_outcome").at("error"),
            "Resume admission discarded the actual previous collection error or partial-publication evidence.");
        if(large)require(resumed.at("collection_outcome").at("truncated")==true && historical.at("retained_log").get_ref<const std::string&>().size()==128U*1024U,
            "The large historical collection fixture was not bounded or lost its complete durable evidence.");
        else{auto publicHistorical=resumed.at("collection_outcome");publicHistorical.erase("previous_attempt");require(publicHistorical==failed.at("collection_outcome"),
            "Small historical collection evidence changed during recovery admission.");}
        requirePublicBudget(resumed);
        require(f.backend->posts==1U && f.backend->queueWrites==0U && f.backend->cancellations==0U,
            "Waiting exact-job recovery resubmitted or modified provider work.");
        held.reset();const auto preview=f.wait(resumed);
        require(preview.at("state")=="awaiting_preview_approval" && preview.at("done")==true && preview.at("requires_operator_approval")==true &&
            preview.at("collection_outcome").value("ok",true) && !preview.at("collection_outcome").contains("previous_attempt") && f.backend->posts==1U,
            "Exact existing history did not recover its preview or replaced the submission with another POST.");
    }
    for(const auto* tool:{"comfy_prepare","comfy_control"}) {
        Fixture f;f.files.failSecond=true;const auto arguments=std::string_view{tool}=="comfy_control"?Json{{"action","start"}}:Json::object();
        requireError(f.invoke(tool,arguments),Domain::ErrorCodes::InternalFailure,"Saved-operation resume fixture did not fail before worker entry.");
        const auto jobs=f.execute("comfy_job_list",Json::object());require(jobs.at("jobs").size()==1U && jobs.at("jobs")[0].at("state")=="failed",
            "Failed saved operation was not retained for explicit recovery.");
        f.files.failSecond=false;auto held=take(Lease::acquire(f.configuration.config.comfyUi.endpoint,context()));
        const auto resumed=f.execute("comfy_job_resume",{{"job_id",jobs.at("jobs")[0].at("job_id")}});
        require(resumed.at("state")=="before_dispatch" && resumed.at("phase")=="waiting_provider_lease" && resumed.at("done")==false &&
            resumed.at("error").is_null() && f.backend->prepareCalls==0U && f.backend->controlCalls==0U && f.backend->posts==0U,
            "Resumed preparation/control returned its historical failure or entered the leased provider prematurely.");
        held.reset();const auto completed=f.wait(resumed);
        require(completed.at("state")=="completed" && (std::string_view{tool}=="comfy_prepare"?f.backend->prepareCalls.load():f.backend->controlCalls.load())==1U && f.backend->posts==0U,
            "Saved preparation/control did not resume its exact operation once after provider release.");
    }
}
void videoPlanRequiresMotionPreview(){
    Fixture f;auto args=f.args();args["media_kind"]="video";auto failed=f.wait(f.execute("comfy_run",args));
    require(failed.at("state")=="failed" && failed.at("error").at("code").get<std::string>()==Domain::ErrorCodes::IntegrityFailure && failed.at("artifacts").empty(),"An image artifact was admitted as a video motion preview.");
    require(failed.at("requires_operator_approval")==false && f.backend->posts==1U,"An invalid video preview requested approval or replayed generation.");
}
void animatedImageMotionRequiresMeasuredDecode(){
    for(const auto* type:{"image/apng","image/webp"}){
        Fixture animated;animated.backend->artifactMediaType=type;
        animated.backend->artifactMetadata={{"decoded",true},{"width",64U},{"height",64U},{"duration",1.25},{"frame_count",3U}};
        auto args=animated.args();args["media_kind"]="video";const auto admitted=animated.execute("comfy_run",args),preview=animated.wait(admitted);
        require(preview.at("state")=="awaiting_preview_approval" && preview.at("requires_operator_approval")==true && preview.at("artifacts").size()==1U &&
            preview.at("artifacts")[0].at("media_type")==type,"Decoded animated image motion did not reach its saved preview approval boundary.");
        animated.reply("approved");const auto final=animated.wait(animated.execute("comfy_run",{{"stage","final"},{"plan_id",admitted.at("plan_id")}}));
        require(final.at("state")=="completed" && final.at("artifacts")[0].at("media_type")==type && animated.backend->posts==2U,"Approved animated image motion was rejected or resubmitted.");
        const std::vector<Json> invalid{
            {{"decoded",true},{"duration",1.25},{"frame_count",1U}},
            {{"decoded",true},{"duration",0.0},{"frame_count",3U}},
            {{"decoded",false},{"duration",1.25},{"frame_count",3U}},
            {{"decoded",true},{"frame_count",3U}}
        };
        for(const auto& metadata:invalid){
            Fixture rejected;rejected.backend->artifactMediaType=type;rejected.backend->artifactMetadata=metadata;
            auto request=rejected.args();request["media_kind"]="video";const auto failed=rejected.wait(rejected.execute("comfy_run",request));
            require(failed.at("state")=="failed" && failed.at("artifacts").empty() && failed.at("requires_operator_approval")==false &&
                failed.at("error").at("code").get<std::string>()==Domain::ErrorCodes::IntegrityFailure && rejected.backend->posts==1U,
                "Static, undecoded or unmeasured animated-image output was admitted as a motion preview.");
        }
    }
}
void changedInputAndPreviewInvalidateApproval(){
    Fixture f;auto input=f.root/"workspace"/"input.png";const auto original=png();write(input,original);
    auto args=f.args();args["inputs"]=Json::array({{{"node_id","1"},{"input","value"},{"path",pathText(input).value()}}});
    auto job=f.execute("comfy_run",args);auto preview=f.wait(job);f.reply("render final");const Json final{{"stage","final"},{"plan_id",job.at("plan_id")}};
    auto changed=original;changed.push_back(std::byte{1});write(input,changed);
    requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Conflict,"Changed original input bypassed its saved seal.");
    write(input,original);const auto artifact=std::filesystem::path(preview.at("artifacts")[0].at("path").get<std::string>());write(artifact,changed);
    auto status=f.execute("comfy_job_status",{{"job_id",job.at("job_id")}});
    require(status.at("publication_suppressed")==true && status.at("state")=="failed" && status.at("artifacts").empty(),"Status published a changed preview artifact.");
    requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Conflict,"Changed preview artifact bypassed native operator approval.");
    require(f.backend->posts==1U,"Changed input or preview caused unapproved final inference.");
}
void unresolvedPreparationPersistsCounters(){
    Fixture f;f.backend->prepareFails=true;auto job=f.execute("comfy_prepare",Json::object());auto failed=f.wait(job);
    require(failed.at("state")=="failed" && failed.at("downloaded_bytes")==1234567U && failed.at("outcome").at("ok")==false,"Unresolved preparation was declared complete or lost transferred-byte accounting.");
    require(failed.at("error").at("code").get<std::string>()==Domain::ErrorCodes::HostCapabilityUnavailable && f.backend->posts==0U,"Unresolved preparation omitted its actual error or submitted rendering.");
    f.reopen();auto recovered=f.wait(job);require(recovered.at("state")=="failed" && recovered.at("downloaded_bytes")==1234567U,"Restart lost failed preparation counters.");
}
void malformedTimeoutHasNoDurableAdmission(){
    Fixture f;for(const Json timeout:{Json(0U),Json(-1),Json(7201U),Json(1.5),Json("30"),Json(nullptr)}){
        auto preview=f.args();preview["timeout_sec"]=timeout;
        requireError(f.invoke("comfy_run",preview),Domain::ErrorCodes::InvalidRequest,"Malformed render timeout created a durable job.");
        requireError(f.invoke("comfy_prepare",{{"timeout_sec",timeout}}),Domain::ErrorCodes::InvalidRequest,"Malformed preparation timeout created a durable job.");
        requireError(f.invoke("comfy_control",{{"action","start"},{"timeout_sec",timeout}}),Domain::ErrorCodes::InvalidRequest,"Malformed control timeout created a durable job.");
    }
    auto listing=f.execute("comfy_job_list",Json::object());require(listing.at("jobs").empty() && listing.at("total")==0U && f.backend->posts==0U,"Invalid timeout left an admitted receipt or submitted provider work.");
}
void receiptsRejectTamperAndLiveOwners(){
    Fixture f;auto job=f.execute("comfy_run",f.args());const auto pending=f.wait(job);const auto receipt=std::filesystem::path(pending.at("receipt_path").get<std::string>());
    auto competitor=std::make_unique<Service>(*f.revocable,f.files,f.configuration,*f.storageIssuer,*f.storage,pathText(f.root/"private"),f.uuids,f.clock,f.hasher,f.backend);
    requireError(competitor->execute("comfy_job_status",Json{{"job_id",job.at("job_id")}}.dump(),*f.authority,context()),Domain::ErrorCodes::Conflict,"Two live Managers acquired the same durable job lease.");competitor.reset();
    f.service.reset();auto row=envelope(receipt);row["payload"]["owner_released"]=false;row["payload"]["state"]="running";writeEnvelope(receipt,row,true);const auto before=read(receipt);f.reopen();
    requireError(f.invoke("comfy_job_status",{{"job_id",job.at("job_id")}}),Domain::ErrorCodes::Conflict,"A recovered receipt with an active process identity was trusted by a new owner.");
    require(read(receipt)==before && f.backend->posts==1U,"Fenced recovery rewrote a receipt or replayed inference.");
    f.service.reset();row["payload"]["owner_released"]=true;row["payload"]["final_graph"]["1"]["inputs"]["value"]="tampered";writeEnvelope(receipt,row,false);f.reopen();
    requireError(f.invoke("comfy_job_status",{{"job_id",job.at("job_id")}}),Domain::ErrorCodes::IntegrityFailure,"Unsealed receipt tampering was accepted.");
    f.service.reset();writeEnvelope(receipt,row,true);f.reopen();
    requireError(f.invoke("comfy_job_status",{{"job_id",job.at("job_id")}}),Domain::ErrorCodes::IntegrityFailure,"Rehashed receipt bypassed the independent final-workflow seal.");
    const auto invalid=f.invoke("comfy_job_status",{{"job_id","../receipt.json"}});require(!invalid && f.backend->posts==1U,"A non-UUID job path was admitted or triggered inference.");
}
void queueWaitRechecksAuthorityAndConfiguration(){
    for(const bool revoke:{true,false}){
        Fixture f;f.backend->foreignQueue=true;auto job=f.execute("comfy_run",f.args());
        await([&]{return f.backend->queueReads.load()>0U;},"Fixture did not reach provider queue wait.");
        if(revoke)f.revocable->revoked=true;
        else{std::lock_guard lock{f.configuration.mutex};f.configuration.config.comfyUi.endpoint="http://127.0.0.1:8189";}
        f.backend->foreignQueue=false;auto failed=f.wait(job);
        require(failed.at("state")=="failed" && failed.at("error").at("code").get<std::string>()== (revoke?Domain::ErrorCodes::Unauthorized:Domain::ErrorCodes::Conflict),"Queue wait admitted work after current authority or configuration changed.");
        require(f.backend->posts==0U && f.backend->queueWrites==0U && f.backend->cancellations==0U,"Waiting for unrelated work submitted inference or mutated unrelated provider queue state.");
    }
}
void queueWaitRechecksPreviewDependenciesBeforeOnePost(){
    for(const bool change:{true,false}) {
        Fixture f;f.backend->foreignQueue=true;const auto job=f.execute("comfy_run",f.args());
        await([&]{return f.backend->queueReads.load()>0U;},"Preview did not reach foreign provider queue after its initial dependency seal.");
        if(change)f.backend->changedIdentity=true;
        f.backend->foreignQueue=false;const auto result=f.wait(job);
        if(change){
            require(result.at("state")=="failed"&&result.at("error").at("code").get<std::string>()==Domain::ErrorCodes::Conflict&&result.at("artifacts").empty()&&f.backend->posts==0U,"A preview used dependencies changed during its queue wait under the original saved identity.");
            f.backend->changedIdentity=false;f.reopen();requireError(f.invoke("comfy_job_resume",{{"job_id",job.at("job_id")}}),Domain::ErrorCodes::Conflict,"Restored dependency bytes resumed a preview whose submission was correctly prevented.");
            require(f.backend->posts==0U,"Dependency restoration replayed an unsubmitted preview after restart.");
        }else{
            require(result.at("state")=="awaiting_preview_approval"&&f.backend->posts==1U,"Unchanged preview dependency identity could not submit exactly once after foreign queue completion.");
            f.reply("yes");const auto final=f.wait(f.execute("comfy_run",{{"stage","final"},{"plan_id",job.at("plan_id")}}));
            require(final.at("state")=="completed"&&f.backend->posts==2U,"Unchanged waited preview lost its native approval or submitted duplicate final work.");
        }
        require(f.backend->queueWrites==0U&&f.backend->cancellations==0U,"Preview dependency revalidation mutated or interrupted unrelated provider work.");
    }
}
void persistedPhasesDescribeObservedWorkAndLegacyRecovery(){
    {
        Fixture f;f.backend->blockIdentity=true;f.backend->foreignQueue=true;const auto job=f.execute("comfy_run",f.args());
        await([&]{return f.backend->identityEntered.load();},"Fixture did not reach its actual dependency-sealing backend boundary.");
        auto observed=f.execute("comfy_job_status",{{"job_id",job.at("job_id")}});const auto receipt=std::filesystem::path(observed.at("receipt_path").get<std::string>());
        require(observed.at("state")=="before_dispatch"&&observed.at("phase")=="sealing_dependencies"&&envelope(receipt).at("payload").at("phase")=="sealing_dependencies"&&f.backend->posts==0U,"Dependency sealing was not durably reported as an actual pre-submission phase.");
        f.backend->blockIdentity=false;await([&]{return f.backend->queueReads.load()>0U;},"Fixture did not reach actual unrelated provider queue waiting.");
        observed=f.execute("comfy_job_status",{{"job_id",job.at("job_id")}});
        require(observed.at("phase")=="waiting_provider_queue"&&envelope(receipt).at("payload").at("phase")=="waiting_provider_queue"&&f.backend->posts==0U,"Foreign queue waiting was reported as generation or concealed behind an old sealing phase.");
        f.execute("comfy_job_cancel",{{"job_id",job.at("job_id")}});const auto cancelled=f.wait(job);
        require(cancelled.at("state")=="cancelled"&&cancelled.at("phase")=="cancelled"&&f.backend->posts==0U&&f.backend->queueWrites==0U&&f.backend->cancellations==0U,"Cancellation retained a stale phase or affected unrelated provider work.");
        f.reopen();require(f.wait(job).at("phase")=="cancelled","Restart did not retain the actual terminal cancellation phase.");
    }
    {
        Fixture f;const auto job=f.execute("comfy_run",f.args());const auto preview=f.wait(job);const auto receipt=std::filesystem::path(preview.at("receipt_path").get<std::string>());f.service.reset();
        auto saved=envelope(receipt);saved["payload"]["phase"]="sealing_dependencies";writeEnvelope(receipt,saved,true);f.reopen();
        require(f.wait(job).at("phase")=="awaiting_preview_approval","A stale persisted phase overrode the recovered terminal preview state.");
        f.service.reset();saved["payload"].erase("phase");writeEnvelope(receipt,saved,true);f.reopen();
        require(f.wait(job).at("phase")=="awaiting_preview_approval"&&f.backend->posts==1U,"Legacy receipt without phase could not recover its exact pending preview without resubmission.");
        f.reply("yes");const auto final=f.wait(f.execute("comfy_run",{{"stage","final"},{"plan_id",job.at("plan_id")}}));
        require(final.at("state")=="completed"&&final.at("phase")=="completed"&&f.backend->posts==2U,"Approved final retained the preview's stale phase or duplicated provider submission.");
    }
}
void actualProviderRejectionAndPartialAdmission(){
    Fixture rejected;rejected.backend->rejectPrompt=true;auto job=rejected.execute("comfy_run",rejected.args());auto failed=rejected.wait(job);
    require(failed.at("state")=="failed" && failed.at("remote_state")=="rejected" && failed.at("node_errors").contains("2") && failed.at("error").at("code").get<std::string>()==Domain::ErrorCodes::InvalidRequest,"Known HTTP 400 rejection lost its actual node errors or became uncertain.");
    require(failed.at("artifacts").empty() && !failed.at("remote_execution_may_continue").get<bool>() && rejected.backend->posts==1U,"Rejected submission reported artifacts, possible execution or another POST.");
    rejected.reopen();requireError(rejected.invoke("comfy_job_resume",{{"job_id",job.at("job_id")}}),Domain::ErrorCodes::Conflict,"Known rejected prompt was resumed as uncertain provider work.");
    require(rejected.wait(job).at("state")=="failed" && rejected.backend->posts==1U,"Rejected prompt recovery changed its known failure or resubmitted inference.");
    Fixture partial;partial.backend->partialAdmission=true;partial.backend->missingOutput=true;auto omitted=partial.wait(partial.execute("comfy_run",partial.args()));
    require(omitted.at("state")=="failed" && omitted.at("node_errors").contains("2") && omitted.at("artifacts").empty() && omitted.at("error").at("code").get<std::string>()==Domain::ErrorCodes::IntegrityFailure,"Partially admitted output hid submission-time node errors or published incomplete coverage.");
    Fixture execution;execution.backend->executionError=true;auto executionJob=execution.execute("comfy_run",execution.args());auto providerFailed=execution.wait(executionJob);
    require(providerFailed.at("state")=="failed" && providerFailed.at("provider_error").at("messages")==Json::array({"Controlled custom-node exception"}) && providerFailed.at("artifacts").empty(),"Custom-node execution failure lost actual provider evidence or published output.");
    requireError(execution.invoke("comfy_job_resume",{{"job_id",executionJob.at("job_id")}}),Domain::ErrorCodes::Conflict,"Known failed execution was resumed as uncertain work.");
    require(execution.backend->posts==1U,"Known provider failure replayed rendering.");
}
void providerLeaseNestingCancellationAndRelease(){
    using Lease=NativeTools::Windows::Detail::ProviderOperationLease;
    const std::string endpoint="http://127.0.0.1:49173";
    auto outer=take(Lease::acquire(endpoint,context()));auto nested=take(Lease::acquire("http://[::1]:49173",context()));
    auto deadline=context();deadline.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds{150};
    std::optional<Domain::Result<std::unique_ptr<Lease>>> blocked;
    std::thread first{[&]{blocked.emplace(Lease::acquire(endpoint,deadline));}};first.join();
    requireError(*blocked,Domain::ErrorCodes::DeadlineExceeded,"Another thread entered a provider operation while nested leases were held.");
    nested.reset();std::stop_source cancellation;auto waiting=context();waiting.cancellation=cancellation.get_token();
    std::atomic<bool> attempting{};std::optional<Domain::Result<std::unique_ptr<Lease>>> cancelled;
    std::thread second{[&]{attempting=true;cancelled.emplace(Lease::acquire(endpoint,waiting));}};
    await([&]{return attempting.load();},"Fixture provider-lease waiter did not start.");cancellation.request_stop();second.join();
    requireError(*cancelled,Domain::ErrorCodes::Cancelled,"Cancellation did not stop a provider-lease waiter, or releasing a nested lease released the outer lease.");
    outer.reset();std::atomic<bool> acquired{};std::optional<Domain::Error> failure;
    std::thread third{[&]{auto obtained=Lease::acquire(endpoint,context());if(obtained)acquired=true;else failure=obtained.error();}};third.join();
    require(acquired.load() && !failure,"Provider lease remained locked after its owning thread released every nested acquisition.");
    for(unsigned attempt=0U;attempt<4U;++attempt){
        auto held=take(Lease::acquire(endpoint,context()));std::stop_source stop;auto race=context();race.cancellation=stop.get_token();
        std::atomic<bool> started{};std::optional<Domain::Result<std::unique_ptr<Lease>>> result;
        std::thread waiter{[&]{started=true;result.emplace(Lease::acquire(endpoint,race));}};
        await([&]{return started.load();},"Fixture cancellation-release waiter did not start.");std::this_thread::sleep_for(std::chrono::milliseconds{10});
        stop.request_stop();held.reset();waiter.join();
        requireError(*result,Domain::ErrorCodes::Cancelled,"Provider lease granted ownership even though cancellation preceded the owner's release.");
    }
    requireError(Lease::acquire("http://127.0.0.1",context()),Domain::ErrorCodes::InvalidRequest,"Provider lease admitted an endpoint without an explicit port.");
}
void sharedProviderSerializesIndependentServices(){
    Fixture first,second;first.configuration.config.comfyUi.endpoint="http://127.0.0.1:49174";second.configuration.config.comfyUi.endpoint="http://[::1]:49174";
    first.backend->running=true;auto active=first.execute("comfy_run",first.args());
    await([&]{return first.backend->posts.load()==1U;},"First independent service never submitted its fixture render.");
    auto queued=second.execute("comfy_run",second.args());std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto waiting=second.execute("comfy_job_status",{{"job_id",queued.at("job_id")}});
    require(waiting.at("state")=="before_dispatch" && second.backend->posts==0U && second.backend->controlCalls==0U,"A second service entered backend preparation or rendering while the first held the shared native provider lease.");
    first.execute("comfy_job_cancel",{{"job_id",active.at("job_id")}});first.wait(active);
    auto preview=second.wait(queued);require(preview.at("state")=="awaiting_preview_approval" && second.backend->posts==1U && first.backend->posts==1U,"Shared provider lease failed to release after cancellation or replayed a generation.");
}
void cancelledDurableWorkerDoesNotEnterLeasedProvider(){
    Fixture f;f.configuration.config.comfyUi.endpoint="http://127.0.0.1:49175";
    auto held=take(NativeTools::Windows::Detail::ProviderOperationLease::acquire(f.configuration.config.comfyUi.endpoint,context()));
    auto job=f.execute("comfy_prepare",Json::object());f.execute("comfy_job_cancel",{{"job_id",job.at("job_id")}});auto cancelled=f.wait(job);
    require(cancelled.at("state")=="cancelled" && f.backend->prepareCalls==0U && f.backend->controlCalls==0U && f.backend->posts==0U,"A worker cancelled while waiting for the shared provider lease still entered its backend operation.");
}
void boundedSnapshotsRetainDurableEvidence(){
    constexpr std::size_t maximum=128U*1024U;
    Fixture f;f.backend->largeEvidence=true;auto prepare=f.execute("comfy_prepare",Json::object());auto prepared=f.wait(prepare);
    require(prepared.at("state")=="completed" && prepared.dump().size()<=maximum && !prepared.at("outcome").contains("manifest"),"Public preparation status delivered a full retained installation manifest.");
    requirePublicBudget(prepared);
    auto receipt=std::filesystem::path(prepared.at("receipt_path").get<std::string>());require(envelope(receipt).at("payload").at("outcome").at("manifest").at("retained_log").get_ref<const std::string&>().size()==600U*1024U,"Status projection discarded durable installation evidence.");
    auto rendered=f.wait(f.execute("comfy_run",f.args()));require(rendered.at("state")=="awaiting_preview_approval" && rendered.dump().size()<=maximum && !rendered.at("artifacts")[0].at("metadata").contains("raw") && !rendered.at("artifacts")[0].contains("preview"),"Public render status exceeded its projection limit or retained oversized raw media evidence.");
    requirePublicBudget(rendered);const auto durable=envelope(std::filesystem::path(rendered.at("receipt_path").get<std::string>())).at("payload").at("artifacts")[0];
    for(const auto* key:{"node_id","path","sha256","bytes","media_type","provider_view_url"})require(rendered.at("artifacts")[0].at(key)==durable.at(key),"Artifact projection changed an approval hash or local result link.");
    Fixture pages;pages.backend->largeSummary=true;std::set<std::string> admitted;
    for(unsigned index=0;index<4U;++index){auto directory=pages.root/"workspace"/std::to_string(index);std::filesystem::create_directories(directory);auto args=pages.args();args["output_directory"]=pathText(directory).value();auto job=pages.execute("comfy_run",args);pages.wait(job);admitted.insert(job.at("job_id").get<std::string>());}
    std::set<std::string> seen;std::uint64_t offset{};unsigned pageCount{};
    for(;;){auto page=pages.execute("comfy_job_list",{{"offset",offset},{"limit",2U}});++pageCount;
        requirePublicBudget(page);
        require(page.dump().size()<=maximum && !page.at("jobs").empty(),"Job list emitted an oversized or nonadvancing page.");
        for(const auto& row:page.at("jobs")){require(row.dump().size()<=maximum,"One public job snapshot exceeded its byte limit.");require(!row.at("artifacts")[0].at("metadata").contains("publisher_notes"),"Public artifact metadata retained an unbounded custom field.");require(seen.insert(row.at("job_id").get<std::string>()).second,"Bounded job-list pages repeated a job.");}
        if(!page.at("has_more").get<bool>())break;auto next=page.at("next_offset").get<std::uint64_t>();require(next>offset,"Bounded job-list continuation did not advance.");offset=next;require(pageCount<5U,"Bounded job-list pagination did not finish.");}
    require(seen==admitted && pageCount>1U,"Byte-bounded job-list pagination dropped jobs or did not limit the aggregate response.");
}
void escapedCatalogPagesFitTransport(){
    Fixture f;f.backend->largeCatalog=true;std::set<std::string> seen;std::uint64_t offset{};unsigned pages{};
    for(;;){auto page=f.execute("comfy_catalog",{{"kind","nodes"},{"offset",offset},{"limit",4U}});++pages;requirePublicBudget(page);
        require(!page.at("entries").empty(),"Byte-limited node catalog did not advance.");for(const auto& row:page.at("entries"))require(seen.insert(row.at("name").get<std::string>()).second,"Byte-limited node catalog repeated an entry.");
        if(!page.at("has_more").get<bool>())break;auto next=page.at("next_offset").get<std::uint64_t>();require(next>offset,"Node catalog continuation did not advance.");offset=next;require(pages<5U,"Node catalog pagination did not finish.");}
    require(seen.size()==4U && pages>1U,"Byte-limited node catalog omitted a schema or exceeded its requested delivery budget.");
    for(unsigned index=0U;index<4U;++index)f.backend->localCatalog.push_back({{"name",std::string(30000U,'"')+std::to_string(index)},{"path","D:\\workflows\\"+std::to_string(index)+".json"}});
    offset=0U;pages=0U;seen.clear();for(;;){auto page=f.execute("comfy_catalog",{{"kind","workflows"},{"offset",offset},{"limit",4U}});++pages;requirePublicBudget(page);
        require(!page.at("items").empty(),"Byte-limited local workflow catalog did not advance.");for(const auto& row:page.at("items"))require(seen.insert(row.at("path").get<std::string>()).second,"Local workflow catalog repeated an entry.");
        if(!page.at("has_more").get<bool>())break;auto next=page.at("next_offset").get<std::uint64_t>();require(next>offset,"Local workflow continuation did not advance.");offset=next;require(pages<5U,"Local workflow pagination did not finish.");}
    require(seen.size()==4U && pages>1U,"Byte-limited local workflow catalog omitted an entry.");
    f.backend->oversizedCatalog=true;requireError(f.invoke("comfy_catalog",{{"kind","nodes"},{"limit",1U}}),Domain::ErrorCodes::PayloadTooLarge,"Oversized individual node schema was hidden or delivered beyond the transport budget.");
}
void modelCatalogCategorySelectsFolderWithCompatiblePaging(){
    Fixture f;const auto first=f.execute("comfy_catalog",{{"kind","models"},{"limit",1U}});
    require(first.at("entries")==Json::array({"checkpoints"}) && first.at("total")==3U && first.at("has_more")==true && first.at("next_offset")==1U,
        "Provider folder discovery did not preserve the actual model category page.");
    const auto rest=f.execute("comfy_catalog",{{"kind","models"},{"offset",first.at("next_offset")},{"limit",2U}});
    require(rest.at("entries")==Json::array({"text_encoders","diffusion_models"}) && rest.at("has_more")==false && rest.at("next_offset").is_null(),
        "Provider category continuation dropped or substituted an advertised folder alias.");
    const auto selected=f.execute("comfy_catalog",{{"kind","models"},{"category","checkpoints"},{"filter","Wan/"},{"limit",1U}});
    require(selected.at("entries")==Json::array({"Wan/draft.safetensors"}) && selected.at("total")==2U && selected.at("next_offset")==1U,
        "Model category was ignored or model filtering happened after the page boundary.");
    const auto next=f.execute("comfy_catalog",{{"kind","models"},{"category","checkpoints"},{"filter","Wan/"},{"offset",selected.at("next_offset")},{"limit",1U}});
    require(next.at("entries")==Json::array({"Wan/final.safetensors"}) && next.at("has_more")==false && next.at("next_offset").is_null(),
        "Filtered model continuation repeated, omitted or switched the selected provider folder.");
    for(const auto& args:{Json{{"kind","models"},{"name","checkpoints"},{"filter","Wan/"},{"limit",1U}},
        Json{{"kind","models"},{"name","checkpoints"},{"category","checkpoints"},{"filter","Wan/"},{"limit",1U}}})
        require(f.execute("comfy_catalog",args)==selected,"The existing model name selector no longer matches the explicit category selector.");
    std::size_t calls{};{std::lock_guard lock{f.backend->mutex};calls=f.backend->modelRequests.size();
        require(calls==6U && f.backend->modelRequests[0]=="/models" && f.backend->modelRequests[1]=="/models" &&
            std::all_of(f.backend->modelRequests.begin()+2,f.backend->modelRequests.end(),[](const auto& route){return route=="/models/checkpoints";}),
            "Catalog did not route the category/name selector to the exact provider folder endpoint.");}
    requireError(f.invoke("comfy_catalog",{{"kind","models"},{"name","checkpoints"},{"category","text_encoders"}}),Domain::ErrorCodes::InvalidRequest,
        "Conflicting model folder selectors silently selected one provider folder.");
    {std::lock_guard lock{f.backend->mutex};require(f.backend->modelRequests.size()==calls,"Conflicting model selectors reached the provider before rejection.");}
    const auto nodes=f.execute("comfy_catalog",{{"kind","nodes"},{"category","image"},{"limit",1U}});
    require(nodes.at("entries").size()==1U && nodes.at("entries")[0].at("name")=="CatalogNode0" && nodes.at("total")==2U && nodes.at("next_offset")==1U,
        "Model folder selection changed the existing node category substring filter.");
    const auto remainingNodes=f.execute("comfy_catalog",{{"kind","nodes"},{"category","image"},{"offset",nodes.at("next_offset")},{"limit",1U}});
    require(remainingNodes.at("entries")[0].at("name")=="CatalogNode2" && remainingNodes.at("has_more")==false,
        "Node category pagination omitted or repeated a schema after the model selector correction.");
    requirePublicBudget(first);requirePublicBudget(selected);requirePublicBudget(nodes);
}
void automaticPreparationCoversBothSavedGraphs(){
    Fixture f;auto args=f.args();args["preview_workflow"]["1"]["class_type"]="PreviewDependency";args["final_workflow"]["1"]["class_type"]="FinalDependency";
    auto preview=f.wait(f.execute("comfy_run",args));require(preview.at("state")=="awaiting_preview_approval" && f.backend->prepareCalls==1U && f.backend->posts==1U,"Saved graph requirements were not prepared as one operation before the preview submission.");
    Json preparation;{std::lock_guard lock{f.backend->mutex};preparation=f.backend->preparationArguments;}
    std::set<std::string> classes;for(const auto& [id,node]:preparation.at("workflow").items()){static_cast<void>(id);classes.insert(node.at("class_type").get<std::string>());}
    require(classes.contains("PreviewDependency") && classes.contains("FinalDependency"),"Automatic preparation omitted dependencies unique to one saved graph.");
}
void knownAnimationInspectionPreparesBeforeReadyImageSubmission(){
    for(const auto* type:{"SaveAnimatedPNG","SaveAnimatedWEBP"})for(const bool inPreview:{true,false}){
        Fixture f;auto args=f.args();auto graph=f.graph();graph["2"]["class_type"]=type;graph["2"]["inputs"]["fps"]=6.0;
        if(std::string_view{type}=="SaveAnimatedPNG")graph["2"]["inputs"]["compress_level"]=4U;
        else{graph["2"]["inputs"]["lossless"]=true;graph["2"]["inputs"]["quality"]=80U;graph["2"]["inputs"]["method"]="default";}
        args[inPreview?"preview_workflow":"final_workflow"]=graph;const auto preflight=f.execute("comfy_validate",{{"workflow",graph}});
        require(preflight.at("ready")==true && preflight.at("media_inspection_required")==true && preflight.at("motion_output_nodes").empty(),"Known core animation output did not request media inspection or incorrectly imposed a motion contract.");
        const auto admitted=f.execute("comfy_run",args),preview=f.wait(admitted);require(preview.at("state")=="awaiting_preview_approval" && f.backend->prepareCalls==1U && f.backend->posts==1U,"Ready image plan omitted media inspection preparation or prepared its two stages separately.");
        Json preparation;{std::lock_guard lock{f.backend->mutex};preparation=f.backend->preparationArguments;}
        require(preparation.at("media_kind")=="image" && preparation.at("media_inspection_required")==true && preparation.at("workflow").size()==4U,"Animation inspection was not carried to the single combined preparation operation.");
        f.reply("approved");require(f.wait(f.execute("comfy_run",{{"stage","final"},{"plan_id",admitted.at("plan_id")}})).at("state")=="completed" && f.backend->posts==2U && f.backend->prepareCalls==1U,"Approved image plan repeated media preparation during final execution.");
    }
    Fixture still;const auto preflight=still.execute("comfy_validate",{{"workflow",still.graph()}});require(preflight.at("media_inspection_required")==false,"A still-image schema was assigned an animation inspection requirement.");
    require(still.wait(still.execute("comfy_run",still.args())).at("state")=="awaiting_preview_approval" && still.backend->prepareCalls==0U,"Ready still-image plan unnecessarily prepared external media components.");
}
void oneReplyApprovesOnlyLatestPresentedPlan(){
    Fixture f;auto firstArgs=f.args(),secondArgs=f.args();
    auto firstDirectory=f.root/"workspace"/"first",secondDirectory=f.root/"workspace"/"second";std::filesystem::create_directories(firstDirectory);std::filesystem::create_directories(secondDirectory);
    firstArgs["output_directory"]=pathText(firstDirectory).value();secondArgs["output_directory"]=pathText(secondDirectory).value();
    auto first=f.execute("comfy_run",firstArgs);f.wait(first);auto second=f.execute("comfy_run",secondArgs);f.wait(second);f.reply("yes");
    requireError(f.invoke("comfy_run",{{"stage","final"},{"plan_id",first.at("plan_id")}}),Domain::ErrorCodes::Unauthorized,"A reply to the latest preview also approved an older pending final revision.");
    auto done=f.wait(f.execute("comfy_run",{{"stage","final"},{"plan_id",second.at("plan_id")}}));
    require(done.at("state")=="completed" && f.backend->posts==3U,"Latest preview approval failed or one reply submitted more than one final.");
}
void concurrentAdmissionReservesSixteenJobs(){
    Fixture f;f.files.blockSecond=true;{std::lock_guard lock{f.configuration.mutex};f.configuration.config.comfyUi.endpoint="http://127.0.0.1:49179";}
    std::barrier start{21};std::vector<std::optional<Domain::Result<std::string>>> outcomes(20U);std::atomic<unsigned> rejected{};std::vector<std::thread> calls;
    for(std::size_t index=0U;index<outcomes.size();++index)calls.emplace_back([&,index]{start.arrive_and_wait();outcomes[index]=f.invoke("comfy_prepare",{{"timeout_sec",30U}});if(!*outcomes[index])++rejected;});
    start.arrive_and_wait();const auto end=std::chrono::steady_clock::now()+std::chrono::seconds{5};
    while(f.files.blocked.load()+rejected.load()<20U && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds{5});
    const auto blocked=f.files.blocked.load(),refused=rejected.load();f.files.releaseSecond=true;for(auto& call:calls)call.join();
    require(blocked==16U && refused==4U,"Concurrent admissions did not reserve exactly sixteen slots before worker dispatch.");
    unsigned admitted{};for(const auto& outcome:outcomes){require(outcome.has_value(),"Concurrent admission lost its result.");if(!*outcome){require(outcome->error().code==Domain::ErrorCodes::LimitExceeded,"Admission rejected a capacity overflow for another reason.");continue;}
        const auto job=Json::parse(outcome->value());require(f.wait(job).at("state")=="completed","Reserved preparation did not finish after the receipt gate released.");++admitted;}
    require(admitted==16U && f.backend->prepareCalls==16U && f.backend->posts==0U,"Concurrent reservation duplicated preparation or submitted generation.");
    require(f.wait(f.execute("comfy_prepare",Json::object())).at("state")=="completed" && f.backend->prepareCalls==17U,"Finished workers did not release their admission slots.");
}
void earlyResumeAndAbandonedPreparationStayExact(){
    Fixture f;{std::lock_guard lock{f.configuration.mutex};f.configuration.config.comfyUi.endpoint="http://127.0.0.1:49180";}
    auto lease=take(NativeTools::Windows::Detail::ProviderOperationLease::acquire("http://127.0.0.1:49180",context()));
    const auto admitted=f.execute("comfy_prepare",{{"timeout_sec",30U}});
    requireError(f.invoke("comfy_job_resume",{{"job_id",admitted.at("job_id")}}),Domain::ErrorCodes::Conflict,"An admitted worker awaiting its provider lease was launched again by resume.");
    require(f.backend->prepareCalls==0U,"A leased provider was entered before release.");lease.reset();
    const auto completed=f.wait(admitted);require(completed.at("state")=="completed" && f.backend->prepareCalls==1U,"One early-resume refusal changed the original preparation count.");
    f.service.reset();const auto receipt=std::filesystem::path(completed.at("receipt_path").get<std::string>());auto saved=envelope(receipt);
    saved["payload"]["state"]="before_dispatch";saved["payload"]["owner_released"]=true;writeEnvelope(receipt,saved,true);f.reopen();
    require(f.wait(admitted).at("state")=="unknown","Abandoned before-dispatch receipt was asserted live or silently completed.");
    const auto recovered=f.wait(f.execute("comfy_job_resume",{{"job_id",admitted.at("job_id")}}));
    require(recovered.at("state")=="completed" && f.backend->prepareCalls==2U && f.backend->posts==0U,"Abandoned preparation did not reconcile its saved operation exactly once.");
}
void launchFailureReleasesReservation(){
    Fixture f;f.files.failSecond=true;requireError(f.invoke("comfy_prepare",Json::object()),Domain::ErrorCodes::InternalFailure,"Controlled worker-launch failure was swallowed.");
    const auto listed=f.execute("comfy_job_list",Json::object());require(listed.at("jobs").size()==1U && listed.at("jobs")[0].at("state")=="failed" && listed.at("jobs")[0].at("done")==true,"Unlaunched job retained a live admission reservation.");
    require(f.backend->prepareCalls==0U && f.backend->posts==0U,"Failed worker launch executed provider work.");
    f.service.reset();f.reopen();const auto job=f.execute("comfy_job_status",{{"job_id",listed.at("jobs")[0].at("job_id")}});require(job.at("recovered")==true && job.at("owner_released")==true,"Failed launch fenced its own receipt as a live Manager forever.");
    require(f.wait(f.execute("comfy_job_resume",{{"job_id",job.at("job_id")}})).at("state")=="completed" && f.backend->prepareCalls==1U,"Failed launch lost its saved preparation request.");
}
void mixedVideoContractsRequireMotionPreview(){
    for(const unsigned variant:{0U,1U,2U}){
        Fixture f;auto args=f.args();args["media_kind"]=variant==2U?"image":"mixed";auto final=f.graph();
        if(variant==0U){final["1"]["class_type"]="VideoSource";final["2"]={{"class_type","TypedSaver"},{"inputs",{{"video",Json::array({"1",0U})},{"filename_prefix","owner"}}}};}
        else{final["2"]["class_type"]="EnumSaver";final["2"]["inputs"]["format"]=variant==2U?Json{{"__value__","video/h264-mp4"}}:Json("video/h264-mp4");}
        args["final_workflow"]=final;const auto check=f.execute("comfy_validate",{{"workflow",final}});
        require(check.at("ready")==true && check.at("motion_output_nodes")==Json::array({"2"}) && check.at("media_inspection_required")==true,"Actual output contracts or accepted wrapped video format did not disclose the motion/inspection intent.");
        const auto failed=f.wait(f.execute("comfy_run",args));require(failed.at("state")=="failed" && failed.at("requires_operator_approval")==false && failed.at("artifacts").empty() && failed.at("error").at("code").get<std::string>()==Domain::ErrorCodes::IntegrityFailure,"A mixed video plan reached approval with only an image preview.");
        require(f.backend->posts==1U && f.backend->prepareCalls==1U,"Motion-contract rejection resubmitted the preview or omitted required media preparation.");
    }
    Fixture image;auto args=image.args();args["media_kind"]="mixed";require(image.wait(image.execute("comfy_run",args)).at("state")=="awaiting_preview_approval","An image-only mixed contract was incorrectly assigned video intent.");
}
void everyIntendedStageMotionOutputNeedsItsOwnPlayableArtifact(){
    const Json still{{"width",64U},{"height",64U},{"decoded",true}},motion{{"decoded",true},{"duration",1.25},{"frame_count",3U}};
    const auto artifacts=[&](bool intendedMotion) {return Json::array({
        Json{{"node_id","2"},{"media_type",intendedMotion?"video/mp4":"image/png"},{"metadata",intendedMotion?motion:still}},
        Json{{"node_id","3"},{"media_type",intendedMotion?"image/png":"video/mp4"},{"metadata",intendedMotion?still:motion}}});};
    const auto request=[](Fixture& fixture,bool previewMotion) {
        auto args=fixture.args();args["media_kind"]="mixed";
        auto preview=fixture.graph();preview["3"]={{"class_type","CustomSaver"},{"inputs",{{"images",Json::array({"1",0U})},{"filename_prefix","other"}}}};
        auto final=preview;final["2"]["class_type"]="EnumSaver";final["2"]["inputs"]["format"]="video/h264-mp4";
        if(previewMotion)preview=final;args["preview_workflow"]=preview;args["final_workflow"]=final;return args;
    };
    Fixture rejected;rejected.backend->collectionMedia=artifacts(false);const auto failed=rejected.wait(rejected.execute("comfy_run",request(rejected,true)));
    require(failed.at("state")=="failed" && failed.at("artifacts").empty() && failed.at("requires_operator_approval")==false &&
        failed.at("error").at("code").get<std::string>()==Domain::ErrorCodes::IntegrityFailure && rejected.backend->posts==1U,
        "An unrelated custom video satisfied another node's intended motion preview output.");
    require(failed.at("preview_motion_output_nodes")==Json::array({"2"}) && failed.at("final_motion_output_nodes")==Json::array({"2"}),
        "The exact per-stage motion output IDs were not retained in the saved plan.");

    Fixture finalOnly;finalOnly.backend->collectionMedia=artifacts(false);const auto plan=finalOnly.execute("comfy_run",request(finalOnly,false));const auto preview=finalOnly.wait(plan);
    require(preview.at("state")=="awaiting_preview_approval" && preview.at("preview_motion_output_nodes").empty() && preview.at("final_motion_output_nodes")==Json::array({"2"}),
        "A suitable motion draft with final-only video intent was rejected or assigned universal preview node IDs.");
    finalOnly.reopen();finalOnly.reply("yes");const auto final=finalOnly.wait(finalOnly.execute("comfy_run",{{"stage","final"},{"plan_id",plan.at("plan_id")}}));
    require(final.at("state")=="failed" && final.at("artifacts").empty() && final.at("error").at("message").get<std::string>().find("Intended motion output")!=std::string::npos && finalOnly.backend->posts==2U,
        "Reopened approved final accepted a still-only intended video node because another node produced motion.");
    finalOnly.reopen();const auto reattached=finalOnly.wait(finalOnly.execute("comfy_job_resume",{{"job_id",plan.at("job_id")}}));
    require(reattached.at("state")=="failed" && reattached.at("artifacts").empty() && finalOnly.backend->posts==2U,
        "Exact final reattachment lost stage motion coverage or resubmitted the rejected artifact set.");

    Fixture valid;valid.backend->collectionMedia=artifacts(true);const auto validPlan=valid.execute("comfy_run",request(valid,true));
    require(valid.wait(validPlan).at("state")=="awaiting_preview_approval","Actual per-node decoded motion coverage did not reach approval.");
    valid.reopen();valid.reply("approved");require(valid.wait(valid.execute("comfy_run",{{"stage","final"},{"plan_id",validPlan.at("plan_id")}})).at("state")=="completed" && valid.backend->posts==2U,
        "Verified per-node motion and still-image coverage failed after restart and approval.");

    Fixture legacy;legacy.backend->collectionMedia=artifacts(false);legacy.backend->loseAck=true;const auto legacyPlan=legacy.execute("comfy_run",request(legacy,true));const auto unknown=legacy.wait(legacyPlan);
    require(unknown.at("state")=="unknown" && legacy.backend->posts==1U,"Motion receipt legacy-recovery fixture did not retain a single uncertain submission.");
    const auto receipt=std::filesystem::path(unknown.at("receipt_path").get<std::string>());legacy.service.reset();auto saved=envelope(receipt);
    saved["payload"].erase("preview_motion_output_nodes");saved["payload"].erase("final_motion_output_nodes");writeEnvelope(receipt,saved,true);legacy.backend->loseAck=false;legacy.reopen();
    const auto recovered=legacy.wait(legacy.execute("comfy_job_resume",{{"job_id",legacyPlan.at("job_id")}}));
    require(recovered.at("state")=="failed" && recovered.at("artifacts").empty() && recovered.at("preview_motion_output_nodes")==Json::array({"2"}) &&
        recovered.at("final_motion_output_nodes")==Json::array({"2"}) && legacy.backend->posts==1U,
        "Earlier v1 exact-ID recovery omitted actual stage contracts or replayed generation while backfilling them.");
}
void cancellationDuringWorkerPublicationHasNoEffects(){
    Fixture f;f.files.blockSecond=true;{std::lock_guard lock{f.configuration.mutex};f.configuration.config.comfyUi.endpoint="http://127.0.0.1:49181";}
    auto lease=take(NativeTools::Windows::Detail::ProviderOperationLease::acquire("http://127.0.0.1:49181",context()));
    std::optional<Domain::Result<std::string>> admitted,cancelled;std::thread admission{[&]{admitted=f.invoke("comfy_prepare",{{"timeout_sec",30U}});}};
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds{3};while(f.files.blocked.load()!=1U && std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds{2});
    const bool entered=f.files.blocked.load()==1U;std::string id;
    if(entered){for(const auto& item:std::filesystem::directory_iterator(f.root/"private"/Fixture::project().value()))if(item.is_directory())id=item.path().filename().string();}
    std::atomic<bool> cancelStarted{};std::thread cancellation;
    if(!id.empty())cancellation=std::thread{[&]{cancelStarted=true;cancelled=f.invoke("comfy_job_cancel",{{"job_id",id}});}};
    while(cancellation.joinable() && !cancelStarted.load())std::this_thread::yield();f.files.releaseSecond=true;admission.join();if(cancellation.joinable())cancellation.join();lease.reset();
    require(entered && !id.empty() && admitted && *admitted && cancelled && *cancelled,"Worker-publication cancellation did not reach its exact retained receipt.");
    const auto stopped=f.wait(Json::parse(admitted->value()));require(stopped.at("state")=="cancelled" && stopped.at("done")==true && stopped.at("artifacts").empty() && f.backend->prepareCalls==0U && f.backend->controlCalls==0U && f.backend->posts==0U,"Cancellation pending during worker publication executed provider preparation or generation.");
}
void strictVersionOneReceiptCoreRejectsMalformedEvidence(){
    Fixture f;const auto admitted=f.execute("comfy_run",f.args());const auto preview=f.wait(admitted);const auto receipt=std::filesystem::path(preview.at("receipt_path").get<std::string>());f.service.reset();const auto original=envelope(receipt);
    const std::vector<std::function<void(Json&)>> changes{
        [](Json& r){r["schema_version"]=1.0;},[](Json& r){r["state"]="future_unknown_state";},
        [](Json& r){r["phase"]="future_unknown_phase";},[](Json& r){r["phase"]=false;},[](Json& r){r["phase"]=std::string(65U,'x');},
        [](Json& r){r["plan_id"]="90000000-0000-4000-8000-000000000009";},[](Json& r){r["prompt_id"]="foreign-provider-id";},
        [](Json& r){r["owner_released"]="true";},[](Json& r){r["downloaded_bytes"]=-1;},
        [](Json& r){r["preview_revision_requested"]="true";},[](Json& r){r["preview_revision_requested"]=true;},
        [](Json& r){r["preview_revision_evidence"]={{"message_index",2U},{"selected_version",0U}};},
        [](Json& r){r["preview_revision_requested"]=true;r["preview_revision_evidence"]={{"message_index",-1},{"selected_version",0U}};},
        [](Json& r){r["preview_revision_requested"]=true;r["preview_revision_evidence"]={{"message_index",2U},{"selected_version","0"}};},
        [](Json& r){r["preview_recovery_requires_new_preview"]="true";},[](Json& r){r["preview_recovery_requires_new_preview"]=true;},
        [](Json& r){r["preview_recovery_error"]={{"code","conflict"},{"message","missing predecessor"}};},
        [](Json& r){r["preview_recovery_requires_new_preview"]=true;r["preview_recovery_error"]={{"code","conflict"},{"message",false}};},
        [](Json& r){r["scope"]["grants"]=Json::array({99U});},[](Json& r){r["scope"]["roots"]=Json::array({"relative/path"});},
        [](Json& r){r["artifacts"][0]["sha256"]="not-a-file-hash";},[](Json& r){r["artifacts"][0]["bytes"]="42";},
        [](Json& r){r["artifacts"][0]["path"]="relative.png";},[](Json& r){r["artifacts"][0]["metadata"]=false;},
        [](Json& r){r["approval_boundary"]=999U;},[](Json& r){r["preview_artifacts"]=Json::array();},
        [](Json& r){r["graph_sha256"]=std::string(64U,'0');},[](Json& r){r.erase("final_graph_sha256");},
        [](Json& r){r["preview_motion_output_nodes"]=false;},[](Json& r){r.erase("final_motion_output_nodes");},
        [](Json& r){r["preview_motion_output_nodes"]=Json::array({"2","2"});},[](Json& r){r["final_motion_output_nodes"]=Json::array({"foreign"});},
        [](Json& r){r["preview_motion_output_nodes"]=Json::array({"1"});},
        [](Json& r){r["stage"]="final";r["approved_revision"]=2U;r["approval_evidence"]={{"text","yes"},{"message_index",10U},{"selected_version",0U}};}};
    for(const auto& change:changes){auto modified=original;change(modified["payload"]);writeEnvelope(receipt,modified,true);f.reopen();
        requireError(f.invoke("comfy_job_status",{{"job_id",admitted.at("job_id")}}),Domain::ErrorCodes::IntegrityFailure,"Malformed rehashed v1 receipt core was admitted.");
        require(f.backend->posts==1U && f.backend->prepareCalls==0U,"Malformed receipt recovery executed provider work.");f.service.reset();}
    auto interrupted=original;auto& row=interrupted["payload"];row["state"]="before_dispatch";row["prompt_id"]=nullptr;row["artifacts"]=Json::array();row["preview_artifacts"]=Json::array();
    for(const auto* key:{"graph","graph_sha256","final_graph_sha256","dependency_identity","input_seals"})row.erase(key);
    writeEnvelope(receipt,interrupted,true);f.reopen();const auto recovered=f.wait(admitted);
    require(recovered.at("state")=="unknown" && recovered.at("recovered")==true,"Interrupted pre-dispatch receipt without later seals could not be observed.");
    requireError(f.invoke("comfy_job_resume",{{"job_id",admitted.at("job_id")}}),Domain::ErrorCodes::Conflict,"An unsubmitted interrupted render was fabricated as an existing provider generation.");
    require(f.backend->posts==1U,"Unsubmitted receipt recovery replayed generation.");
}
void unreadableReceiptsDoNotBlockPagedRecovery(){
    Fixture f;f.files.failFirst=true;
    requireError(f.invoke("comfy_prepare",Json::object()),Domain::ErrorCodes::InternalFailure,"Controlled initial receipt failure was swallowed.");
    const auto projectRoot=f.root/"private"/Fixture::project().value();std::string missingId;
    for(const auto& item:std::filesystem::directory_iterator(projectRoot))if(item.is_directory())missingId=item.path().filename().string();
    require(!missingId.empty() && !std::filesystem::exists(projectRoot/missingId/"receipt.json"),"Initial persistence failure did not leave the real created job directory without a receipt.");
    const auto valid=f.wait(f.execute("comfy_prepare",Json::object()));require(valid.at("state")=="completed","A failed initial receipt retained an admission slot or blocked later work.");
    const auto validId=valid.at("job_id").get<std::string>(),corruptId=take(f.uuids.next()).value();
    auto corrupt=envelope(std::filesystem::path(valid.at("receipt_path").get<std::string>()));
    corrupt["payload"]["job_id"]=corruptId;corrupt["payload"]["plan_id"]=corruptId;corrupt["payload"]["state"]="unrecognized_receipt_state";
    std::filesystem::create_directories(projectRoot/corruptId);writeEnvelope(projectRoot/corruptId/"receipt.json",corrupt,true);
    f.reopen();std::set<std::string> observed;std::map<std::string,std::string> errors;std::uint64_t offset{};unsigned pages{};
    for(;;){const auto page=f.execute("comfy_job_list",{{"offset",offset},{"limit",1U}});requirePublicBudget(page);++pages;
        require(page.at("total")==3U && page.at("jobs").size()+page.at("unreadable_jobs").size()==1U,"Recovery pagination omitted a UUID row or exceeded its count bound.");
        for(const auto& job:page.at("jobs")){const auto id=job.at("job_id").get<std::string>();require(observed.insert(id).second && id==validId && job.at("recovered")==true,"Readable job recovery was duplicated or replaced by an unreadable row.");}
        for(const auto& row:page.at("unreadable_jobs")){const auto id=row.at("job_id").get<std::string>();require(observed.insert(id).second && row.at("receipt_path")==pathText(projectRoot/id/"receipt.json").value(),"Unreadable job diagnostic lost its exact receipt identity.");errors.emplace(id,row.at("error").at("code").get<std::string>());}
        require(page.at("partial")==!page.at("unreadable_jobs").empty(),"Recovery page did not disclose its unreadable rows.");
        if(!page.at("has_more").get<bool>())break;const auto next=page.at("next_offset").get<std::uint64_t>();require(next==offset+1U,"Unreadable rows did not advance the durable list cursor.");offset=next;
    }
    require(pages==3U && observed.size()==3U && errors.at(missingId)==Domain::ErrorCodes::RecordNotFound && errors.at(corruptId)==Domain::ErrorCodes::IntegrityFailure,"One unreadable receipt blocked another job or lost its actual error.");
    require(f.backend->prepareCalls==1U && f.backend->posts==0U,"Receipt discovery retried failed admission or generated provider work.");
}
void providerImageInputsRequireExplicitPrivateSeals(){
    for(const bool optional:{false,true}){
        Fixture f;auto args=f.args();auto graph=f.graph();
        graph["1"]=optional?Json{{"class_type","OptionalUploadImage"},{"inputs",{{"value","draft"},{"reference","root-image.png"}}}}:
            Json{{"class_type","UploadImage"},{"inputs",{{"image","root-image.png"}}}};
        args["preview_workflow"]=graph;args["final_workflow"]=graph;const auto preflight=f.execute("comfy_validate",{{"workflow",graph}});
        require(preflight.at("ready")==true && preflight.at("provider_file_inputs").size()==1U && preflight.at("provider_file_inputs")[0].at("input")== (optional?"reference":"image"),"A discovered image_upload contract lost its explicit file-input requirement.");
        const auto rejected=f.wait(f.execute("comfy_run",args));require(rejected.at("state")=="failed" && rejected.at("error").at("code").get<std::string>()==Domain::ErrorCodes::InvalidRequest && f.backend->posts==0U,"An unbound installed provider image bypassed the private input seal policy.");
    }
    Fixture f;const auto input=f.root/"workspace"/"bound.png";const auto original=png();write(input,original);auto args=f.args();auto graph=f.graph();
    graph["1"]={{"class_type","UploadImage"},{"inputs",{{"image","root-image.png"}}}};args["preview_workflow"]=graph;args["final_workflow"]=graph;
    args["inputs"]=Json::array({{{"node_id","1"},{"path",pathText(input).value()}}});const auto admitted=f.execute("comfy_run",args);const auto preview=f.wait(admitted);
    require(preview.at("state")=="awaiting_preview_approval" && f.backend->posts==1U,"Explicit local image binding did not produce its sealed preview.");
    std::filesystem::path providerFile;{std::lock_guard lock{f.backend->mutex};require(f.backend->providerInputs.size()==1U,"A single explicit input produced multiple provider copies.");providerFile=f.backend->providerInputs.begin()->second;}
    const auto saved=envelope(std::filesystem::path(preview.at("receipt_path").get<std::string>()));const auto& sealed=saved.at("payload").at("input_seals")[0];
    require(sealed.at("provider_sha256")==digest(original) && sealed.at("provider_bytes")==original.size() && sealed.at("node_id")=="1" && sealed.at("input")=="image","The receipt did not retain the exact private provider input bytes and binding.");
    f.reply("approved");auto changed=original;changed.push_back(std::byte{7});write(providerFile,changed);
    requireError(f.invoke("comfy_run",{{"stage","final"},{"plan_id",admitted.at("plan_id")}}),Domain::ErrorCodes::Conflict,"Changed private provider copy retained approval while its original source stayed unchanged.");
    require(read(input)==original && f.backend->posts==1U,"Private input invalidation modified the original or submitted final work.");
    write(providerFile,original);require(f.wait(f.execute("comfy_run",{{"stage","final"},{"plan_id",admitted.at("plan_id")}})).at("state")=="completed" && f.backend->posts==2U,"A verified unchanged private input could not render its approved final plan.");
    Fixture duplicate;const auto duplicateInput=duplicate.root/"workspace"/"input.png";write(duplicateInput,original);auto duplicateArgs=duplicate.args();
    const Json binding{{"node_id","1"},{"input","value"},{"path",pathText(duplicateInput).value()}};duplicateArgs["inputs"]=Json::array({binding,binding});
    requireError(duplicate.invoke("comfy_run",duplicateArgs),Domain::ErrorCodes::InvalidRequest,"Duplicate source bindings changed the same graph input twice.");
    require(duplicate.execute("comfy_job_list",Json::object()).at("total")==0U && duplicate.backend->posts==0U,"Invalid duplicate binding admitted durable work.");
}
void providerAudioAndVideoInputsRequireRestartSafePrivateSeals(){
    for(const bool audio:{true,false})for(const bool optional:{false,true})for(const bool wrapped:{false,true}) {
        Fixture f;const auto inputName=optional?"reference":audio?"audio":"file",classType=optional?(audio?"OptionalLoadAudio":"OptionalLoadVideo"):(audio?"LoadAudio":"LoadVideo");
        auto args=f.args(),graph=f.graph();const auto literal=wrapped?Json{{"__value__",audio?"unlisted-audio.wav":"unlisted-video.mp4"}}:Json(audio?"root-audio.wav":"root-video.mp4");
        graph["1"]={{"class_type",classType},{"inputs",optional?Json{{"value","draft"},{inputName,literal}}:Json{{inputName,literal}}}};
        graph["2"]["class_type"]="CustomSaver";args["preview_workflow"]=graph;args["final_workflow"]=graph;
        if(!audio){args["media_kind"]="video";f.backend->artifactMediaType="video/mp4";f.backend->artifactMetadata={{"decoded",true},{"duration",1.25},{"frame_count",3U}};}
        const auto preflight=f.execute("comfy_validate",{{"workflow",graph}});
        require(preflight.at("ready")==true && preflight.at("provider_file_inputs").size()==1U && preflight.at("provider_file_inputs")[0].at("input")==inputName &&
            preflight.at("provider_file_inputs")[0].at("value")== (wrapped?literal.at("__value__"):literal),
            "Actual required/optional audio or video upload contract did not disclose its exact unwrapped file input.");
        if(wrapped && !optional)require(preflight.at("warnings").size()==1U && preflight.at("warnings")[0].at("code")=="provider_file_validation_required",
            "A private audio/video upload was rejected against only the provider root's enumerated filenames.");
        const auto unbound=f.wait(f.execute("comfy_run",args));require(unbound.at("state")=="failed" && unbound.at("artifacts").empty() &&
            unbound.at("error").at("code").get<std::string>()==Domain::ErrorCodes::InvalidRequest && f.backend->posts==0U,
            "An unsealed existing audio/video provider filename executed managed rendering.");
        const auto source=f.root/"workspace"/(audio?"source.wav":"source.mp4");const auto original=png();write(source,original);
        args["inputs"]=Json::array({{{"node_id","1"},{"input",inputName},{"path",pathText(source).value()}}});const auto admitted=f.execute("comfy_run",args),preview=f.wait(admitted);
        require(preview.at("state")=="awaiting_preview_approval" && f.backend->posts==1U,"Explicit audio/video input binding did not reach its sealed preview boundary.");
        const auto saved=envelope(std::filesystem::path(preview.at("receipt_path").get<std::string>()));const auto& seal=saved.at("payload").at("input_seals")[0];
        require(seal.at("input")==inputName && seal.at("provider_sha256")==digest(original) && seal.at("provider_bytes")==original.size(),
            "The durable audio/video source seal omitted its input identity or private provider bytes.");
        std::filesystem::path provider;{std::lock_guard lock{f.backend->mutex};require(f.backend->providerInputs.size()==1U,"Audio/video binding did not retain exactly one private provider copy.");provider=f.backend->providerInputs.begin()->second;}
        f.reopen();f.reply("approved");const Json final{{"stage","final"},{"plan_id",admitted.at("plan_id")}};
        auto changed=original;changed.push_back(std::byte{9});write(source,changed);requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Conflict,"Restarted audio/video plan approved changed original bytes.");
        write(source,original);write(provider,changed);requireError(f.invoke("comfy_run",final),Domain::ErrorCodes::Conflict,"Restarted audio/video plan approved changed private provider bytes.");
        require(f.backend->posts==1U,"Changed audio/video input seals submitted final inference.");write(provider,original);
        require(f.wait(f.execute("comfy_run",final)).at("state")=="completed" && f.backend->posts==2U,"Restored exact audio/video bytes could not complete the originally approved final revision.");
    }
}
}
}
int main(){using namespace ForgeConductor::Tests;try{approvalAndExactlyOnce();pendingApprovalSurvivesReopen();changedConversationAndDependencies();lostAcknowledgementReattach();normalizedLiteralGraphMatcherIsNarrow();providerLiteralArrayPreflightMatchesInstalledContract();normalizedLiteralArraysSurviveHistoryRecoveryAndCancellation();wrongHistoryAndPartialOutput();disabledAndPreflightBoundaries();cancellationAndList();simultaneousFinalIsOneSubmission();approvalNeedsEarlierDeliveredPreview();resumedPreviewDeliveryPreservesApprovalBoundaries();revisionReplyRequiresFreshPreviewDespiteLaterApproval();successorRecoveryRetainsUnobservedRevisionAndRequiresFreshApproval();rediscoveredPreviewApprovesExactlyOnceAfterRestart();rediscoveryNeedsApprovalAfterDeliveredNativePreview();rediscoveryRequiresExactLatestPlanAndArtifacts();rediscoveryPreservesOriginalConversationAndRevisionEvidence();rediscoveryCreativeRevisionStillRequiresNewPreview();rediscoveryStillVerifiesInputArtifactAndDependencySeals();rediscoveryMatchesBoundedIdentityAndVerifiesCompleteLiveEvidence();partialPublicationOutcomeSurvivesFailureCancellationAndRestart();resumedAdmissionPublishesNonterminalStateBeforeProviderEntry();videoPlanRequiresMotionPreview();animatedImageMotionRequiresMeasuredDecode();changedInputAndPreviewInvalidateApproval();unresolvedPreparationPersistsCounters();malformedTimeoutHasNoDurableAdmission();receiptsRejectTamperAndLiveOwners();queueWaitRechecksAuthorityAndConfiguration();queueWaitRechecksPreviewDependenciesBeforeOnePost();persistedPhasesDescribeObservedWorkAndLegacyRecovery();actualProviderRejectionAndPartialAdmission();providerLeaseNestingCancellationAndRelease();sharedProviderSerializesIndependentServices();cancelledDurableWorkerDoesNotEnterLeasedProvider();boundedSnapshotsRetainDurableEvidence();escapedCatalogPagesFitTransport();modelCatalogCategorySelectsFolderWithCompatiblePaging();automaticPreparationCoversBothSavedGraphs();knownAnimationInspectionPreparesBeforeReadyImageSubmission();oneReplyApprovesOnlyLatestPresentedPlan();concurrentAdmissionReservesSixteenJobs();earlyResumeAndAbandonedPreparationStayExact();launchFailureReleasesReservation();mixedVideoContractsRequireMotionPreview();everyIntendedStageMotionOutputNeedsItsOwnPlayableArtifact();cancellationDuringWorkerPublicationHasNoEffects();strictVersionOneReceiptCoreRejectsMalformedEvidence();unreadableReceiptsDoNotBlockPagedRecovery();providerImageInputsRequireExplicitPrivateSeals();providerAudioAndVideoInputsRequireRestartSafePrivateSeals();std::cout<<"ComfyUI service: 53 groups passed.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
