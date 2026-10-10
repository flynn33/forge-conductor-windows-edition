#include "Infrastructure/TestSupport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/NativeTools/Windows/WindowsImageProviderService.h"
#include "NativeTools/Windows/ImageProviderCodec.h"
#include "Infrastructure/Windows/Detail/UtfConversion.h"

#include <Windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <span>
#include <thread>

namespace ForgeConductor::Tests {
namespace {
using Json = nlohmann::json;
using Service = NativeTools::Windows::WindowsImageProviderService;
using Pixels = NativeTools::Windows::Detail::ImageProviderPixels;
using Infrastructure::Windows::WindowsWorkspaceAuthority;
using Infrastructure::Windows::WindowsWorkspaceAuthorityPolicy;
using namespace std::chrono_literals;
Domain::OperationContext context() { return TestContext{}.active(); }
Domain::PathText pathText(const std::filesystem::path& p) {
    return take(Domain::PathText::create(take(Infrastructure::Windows::Detail::strictUtf16ToUtf8(p.native()))));
}
std::vector<std::byte> read(const std::filesystem::path& path) {
    std::ifstream stream{path,std::ios::binary}; require(static_cast<bool>(stream),"Fixture read failed.");
    std::vector<char> bytes{std::istreambuf_iterator<char>{stream},std::istreambuf_iterator<char>{}};
    return {reinterpret_cast<const std::byte*>(bytes.data()),reinterpret_cast<const std::byte*>(bytes.data()+bytes.size())};
}
void write(const std::filesystem::path& p,std::span<const std::byte> bytes) {
    std::ofstream stream{p,std::ios::binary|std::ios::trunc}; stream.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    require(static_cast<bool>(stream),"Fixture write failed.");
}
std::string digest(std::span<const std::byte> bytes) {
    Infrastructure::Windows::BCryptSha256Hasher hasher; return take(hasher.sha256(bytes)).value();
}
std::string upperId(const Json& job){auto id=job.at("job_id").get<std::string>();for(auto& c:id)if(c>='a'&&c<='f')c=static_cast<char>(c-32);return id;}
class Configuration final : public Contracts::IConfigurationStore {
public:
    std::mutex mutex;
    Domain::AppConfig config;
    Configuration() { config.imageProvider = {true,"http://127.0.0.1:49182","sd1","cyberrealistic_final2.safetensors"}; }
    Domain::Result<Domain::AppConfig> load(const Domain::OperationContext&) noexcept override {
        std::lock_guard lock{mutex}; return Domain::Result<Domain::AppConfig>::success(config);
    }
    Domain::Result<Domain::AppConfig> reload(const Domain::OperationContext& c) noexcept override { return load(c); }
    Domain::Result<Domain::AppConfig> update(const Domain::AppConfigPatch& p,const Domain::OperationContext&) noexcept override {
        std::lock_guard lock{mutex}; auto changed = Domain::applyConfigPatch(config,p);
        if (!changed) return changed; config = changed.value(); return changed;
    }
    void shutdown() noexcept override {}
};
class Provider final : public Contracts::IImageProviderHttpTransport {
public:
    enum class Mode { Complete, Queued, Missing, LostAck, Reject, WrongGraph, WrongDimensions, PauseView, BlockPreflight, PauseQueueDelete };
    struct Call { std::string method,route,contentType,body; };
    std::mutex mutex;
    std::vector<Call> calls;
    Json workflow;
    std::string promptId;
    std::string jobId;
    std::atomic<Mode> mode{Mode::Complete};
    std::atomic<bool> viewEntered{};
    std::atomic<bool> releaseView{};
    std::atomic<unsigned> preflightEntered{};
    std::atomic<bool> releasePreflight{};
    std::atomic<bool> queueDeleteEntered{};
    std::atomic<bool> releaseQueueDelete{};
    Pixels generated{64U,64U,std::vector<std::byte>(64U*64U*4U)};
    Provider() {
        for (std::size_t i{};i<generated.rgba.size();i+=4U) {
            generated.rgba[i]=std::byte{231};generated.rgba[i+1U]=std::byte{43};
            generated.rgba[i+2U]=std::byte{19};generated.rgba[i+3U]=std::byte{177};
        }
    }
    std::size_t count(std::string_view route) { std::lock_guard lock{mutex};
        return std::count_if(calls.begin(),calls.end(),[&](const auto& c){return c.route==route;}); }
    Contracts::ImageProviderHttpResponse json(Json value,std::uint32_t status=200U) {
        const auto encoded=value.dump();return {status,"application/json",{reinterpret_cast<const std::byte*>(encoded.data()),reinterpret_cast<const std::byte*>(encoded.data()+encoded.size())}};
    }
    Domain::Result<Contracts::ImageProviderHttpResponse> request(const Domain::ImageProviderConfig&,std::string_view method,
        std::string_view route,std::string_view type,std::span<const std::byte> body,std::size_t maximum,
        const Domain::OperationContext& c) noexcept override {
        try {
            const auto input = std::string{reinterpret_cast<const char*>(body.data()),body.size()};
            {std::lock_guard lock{mutex};calls.push_back({std::string{method},std::string{route},std::string{type},input});}
            Contracts::ImageProviderHttpResponse response;
            if (route.starts_with("/object_info/")) {
                if(mode.load()==Mode::BlockPreflight) {
                    ++preflightEntered;
                    while(!releasePreflight.load()) {
                        if(c.cancellation.stop_requested())return Domain::Result<Contracts::ImageProviderHttpResponse>::failure(Domain::makeError(Domain::ErrorCodes::Cancelled,"Fake preflight was cancelled."));
                        if(std::chrono::steady_clock::now()>=c.deadline)return Domain::Result<Contracts::ImageProviderHttpResponse>::failure(Domain::makeError(Domain::ErrorCodes::DeadlineExceeded,"Fake preflight expired."));
                        std::this_thread::sleep_for(2ms);
                    }
                }
                const auto name=std::string{route.substr(13U)};
                Json required=Json::object();
                for(const auto* field:{"ckpt_name","text","clip","model","seed","steps","cfg","sampler_name","scheduler","positive","negative","latent_image","denoise","samples","vae","images","filename_prefix","width","height","batch_size","image","pixels","mask","grow_mask_by","channel"})required[field]=Json::array({"TYPE"});
                required["ckpt_name"]=Json::array({Json::array({"cyberrealistic_final2.safetensors"})});
                required["sampler_name"]=Json::array({Json::array({"dpmpp_2m"})});required["scheduler"]=Json::array({Json::array({"karras"})});
                required["channel"]=Json::array({Json::array({"red"})});response=json({{name,{{"input",{{"required",required}}}}}});
            } else if (route=="/upload/image") {
                const auto begin=input.find("filename=\"");require(begin!=std::string::npos,"Multipart filename missing.");
                const auto start=begin+10U; const auto end=input.find('"',start);
                require(type.starts_with("multipart/form-data;")&&input.find("\x89PNG\r\n\x1a\n")!=std::string::npos,"Actual binary PNG multipart was lost.");
                response=json({{"name",input.substr(start,end-start)},{"type","input"},{"subfolder",""}});
            } else if (route=="/prompt") {
                const auto outgoing=Json::parse(input);
                {std::lock_guard lock{mutex};workflow=outgoing.at("prompt");promptId=outgoing.at("prompt_id");jobId=outgoing.at("client_id");}
                if(mode.load()==Mode::LostAck)return Domain::Result<Contracts::ImageProviderHttpResponse>::failure(Domain::makeError(Domain::ErrorCodes::TransportClosed,"Actual fake transport lost the acknowledgement after accepting the ID."));
                response=mode.load()==Mode::Reject?json({{"error",{{"type","prompt_outputs_failed_validation"}}},{"node_errors",Json::object()}},400U):json({{"prompt_id",promptId},{"number",1},{"node_errors",Json::object()}});
            } else if(route.starts_with("/history/")) {
                std::lock_guard lock{mutex};
                if(mode.load()==Mode::Missing||mode.load()==Mode::Queued||mode.load()==Mode::PauseQueueDelete||promptId.empty())response=json(Json::object());
                else {
                    auto actual=workflow;if(mode.load()==Mode::WrongGraph)actual["5"]["inputs"]["seed"]=999;
                    const auto prefix=workflow.at("7").at("inputs").at("filename_prefix").get<std::string>();
                    response=json({{promptId,{{"prompt",Json::array({1,promptId,actual})},
                        {"status",{{"status_str","success"},{"completed",true}}},
                        {"outputs",{{"7",{{"images",Json::array({{{"filename","image_00001_.png"},{"subfolder",prefix.substr(0,prefix.size()-6U)},{"type","output"}}})}}}}}}}});
                }
            } else if(route=="/queue"&&method=="GET") {
                std::lock_guard lock{mutex}; response=json({{"queue_running",Json::array()},
                    {"queue_pending",(mode.load()==Mode::Queued||mode.load()==Mode::PauseQueueDelete)?Json::array({Json::array({1,promptId,workflow})}):Json::array()}});
            } else if(route=="/queue"&&method=="POST") {
                auto deletion=Json::parse(input);require(deletion.size()==1U&&deletion.at("delete")==Json::array({promptId}),"Cancellation addressed more than the exact owned ID.");
                if(mode.load()==Mode::PauseQueueDelete) {
                    queueDeleteEntered=true;
                    while(!releaseQueueDelete.load()) {
                        if(c.cancellation.stop_requested())return Domain::Result<Contracts::ImageProviderHttpResponse>::failure(Domain::makeError(Domain::ErrorCodes::Cancelled,"Fake queue delete was cancelled."));
                        if(std::chrono::steady_clock::now()>=c.deadline)return Domain::Result<Contracts::ImageProviderHttpResponse>::failure(Domain::makeError(Domain::ErrorCodes::DeadlineExceeded,"Fake queue delete expired."));
                        std::this_thread::sleep_for(2ms);
                    }
                }
                response=json(Json::object());
            } else if(route.starts_with("/view?")) {
                viewEntered.store(true);
                if(mode.load()==Mode::PauseView)while(!releaseView.load()) {
                    if(c.cancellation.stop_requested())return Domain::Result<Contracts::ImageProviderHttpResponse>::failure(Domain::makeError(Domain::ErrorCodes::Cancelled,"Fake view was cancelled."));
                    if(std::chrono::steady_clock::now()>=c.deadline)return Domain::Result<Contracts::ImageProviderHttpResponse>::failure(Domain::makeError(Domain::ErrorCodes::DeadlineExceeded,"Fake view expired."));
                    std::this_thread::sleep_for(2ms);
                }
                auto pixels=generated;if(mode.load()==Mode::WrongDimensions){pixels.width=32U;pixels.rgba.resize(32U*64U*4U);}
                response={200U,"image/png",take(NativeTools::Windows::Detail::encodeProviderImage(pixels,c))};
            } else throw TestFailure{"Unexpected provider route (possibly a shared global interrupt)."};
            require(response.body.size()<=maximum,"Fake response exceeds actual requested transport bound.");
            return Domain::Result<Contracts::ImageProviderHttpResponse>::success(std::move(response));
        }catch(const std::exception& e){return Domain::Result<Contracts::ImageProviderHttpResponse>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,e.what()));}
    }
};
class Fixture final {
public:
    std::filesystem::path root;
    Infrastructure::Windows::WindowsAtomicFileStore files;
    Infrastructure::Windows::WindowsUuidGenerator uuids;
    Infrastructure::Windows::SystemClock clock;
    Infrastructure::Windows::BCryptSha256Hasher hasher;
    Configuration configuration;
    std::shared_ptr<Provider> provider=std::make_shared<Provider>();
    std::unique_ptr<WindowsWorkspaceAuthority> issuer,storageIssuer;
    std::unique_ptr<Contracts::WorkspaceAuthority> authority,storage;
    std::unique_ptr<Service> service;
    Fixture() {
        root=std::filesystem::temp_directory_path()/("ForgeConductor.ImageProvider."+take(uuids.next()).value());
        std::filesystem::create_directories(root/"workspace");std::filesystem::create_directories(root/"private");root=std::filesystem::canonical(root);
        storageIssuer=std::make_unique<WindowsWorkspaceAuthority>(std::vector<WindowsWorkspaceAuthorityPolicy>{{
            parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000002"),project(),parse<Domain::ClientId>("image-provider-storage"),
            {pathText(root/"private")},Domain::FileAccess::Write,{Domain::FileAccess::Read,Domain::FileAccess::Write,Domain::FileAccess::Create,Domain::FileAccess::Delete},{},false,1U}});
        storage=std::make_unique<Contracts::WorkspaceAuthority>(take(storageIssuer->authorityFor(project(),context())));reopen();
    }
    ~Fixture(){service.reset();std::error_code ignored;std::filesystem::remove_all(root,ignored);}
    static Domain::ProjectId project(){return parse<Domain::ProjectId>("10000000-0000-4000-8000-000000000001");}
    void reopen(std::vector<Domain::FileAccess> grants={Domain::FileAccess::Read,Domain::FileAccess::Write,Domain::FileAccess::Create},
        std::string_view caller="forge-conductor-manager",bool restricted=false) {
        service.reset();
        if(restricted)std::filesystem::create_directories(root/"workspace"/"restricted");
        issuer=std::make_unique<WindowsWorkspaceAuthority>(std::vector<WindowsWorkspaceAuthorityPolicy>{{
            parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000001"),project(),parse<Domain::ClientId>(caller),
            {pathText(restricted?root/"workspace"/"restricted":root/"workspace")},
            std::find(grants.begin(),grants.end(),Domain::FileAccess::Write)!=grants.end()?Domain::FileAccess::Write:Domain::FileAccess::Read,
            std::move(grants),{},false,1U}});
        authority=std::make_unique<Contracts::WorkspaceAuthority>(take(issuer->authorityFor(project(),context())));
        service=std::make_unique<Service>(*issuer,files,configuration,*storageIssuer,*storage,pathText(root/"private"),uuids,clock,hasher,provider);
    }
    Json args(std::string_view name="output.png") {return {{"prompt","A privately owned test image"},{"path",pathText(root/"workspace"/name).value()},{"seed",42U},{"width",64U},{"height",64U},{"timeout_sec",5U}};}
    Domain::Result<std::string> invoke(std::string_view tool,const Json& a){return service->execute(tool,a.dump(),*authority,context());}
    Json execute(std::string_view tool,const Json& a){return Json::parse(take(invoke(tool,a)));}
    Json wait(const Json& job){return execute("image_job_status",{{"job_id",job.at("job_id")},{"wait_sec",10U}});}
    void awaitView(){const auto end=std::chrono::steady_clock::now()+3s;while(!provider->viewEntered.load()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(2ms);require(provider->viewEntered.load(),"Worker did not reach real fake download boundary.");}
};
void disabledAndInvalidBeforeEffects(){
    Fixture f;{std::lock_guard lock{f.configuration.mutex};f.configuration.config.imageProvider={};}
    auto status=f.execute("image_provider_status",Json::object());require(!status.at("configured")&&!status.at("available")&&f.provider->calls.empty(),"Disabled provider must perform no network activity.");
    requireError(f.invoke("image_generate",f.args()),Domain::ErrorCodes::HostCapabilityUnavailable,"Disabled generation was admitted.");
    {std::lock_guard lock{f.configuration.mutex};f.configuration.config.imageProvider={true,"http://127.0.0.1:49182","sd1","cyberrealistic_final2.safetensors"};}
    for(const auto& change:std::vector<Json>{{{"width",65U}},{{"seed",-1}},{{"denoise",0.0}},{{"preview_max_dimension",127U}},{{"extra",true}}}){
        auto args=f.args();args.update(change);requireError(f.invoke("image_generate",args),Domain::ErrorCodes::InvalidRequest,"Invalid boundary argument was admitted.");
    }
    require(f.provider->calls.empty()&&std::filesystem::is_empty(f.root/"private")&&std::filesystem::is_empty(f.root/"workspace"),"Invalid input produced private, destination, or network effects.");
}
void generateAndReopenActualArtifact(){
    Fixture f;auto admitted=f.execute("image_generate",f.args());auto result=f.wait(admitted);
    require(result.at("ok")==true&&result.at("state")=="completed"&&result.at("done")==true&&result.at("artifact_published")==true,"Actual provider artifact was not completed.");
    auto output=read(f.root/"workspace"/"output.png");require(result.at("artifact_sha256").get<std::string>()==digest(output),"Artifact hash does not match actual output bytes.");
    auto pixels=take(NativeTools::Windows::Detail::decodeProviderImage(output,context()));require(pixels.rgba==f.provider->generated.rgba,"Provider output channels/alpha changed.");
    require(result.at("decoded_rgba8_sha256").get<std::string>()==digest(pixels.rgba)&&result.at("preview_width")==64U&&result.at("preview_height")==64U,"Canonical pixel or preview facts are wrong.");
    require(f.provider->count("/prompt")==1U,"Generation was submitted more than once.");
    f.reopen();auto restored=f.wait(admitted);require(restored.at("state")=="completed"&&restored.at("artifact_sha256")==result.at("artifact_sha256")&&f.provider->count("/prompt")==1U,"Completed reconnect changed facts or replayed generation.");
    auto bytes=read(std::filesystem::path{take(Infrastructure::Windows::Detail::strictUtf8ToUtf16(restored.at("receipt_path").get<std::string>()))});
    auto receipt=Json::parse(reinterpret_cast<const char*>(bytes.data()),reinterpret_cast<const char*>(bytes.data()+bytes.size()));
    const auto p=receipt.at("payload").dump();require(receipt.at("sha256").get<std::string>()==digest(std::as_bytes(std::span{p.data(),p.size()})),"Durable receipt is not independently sealed.");
}
void maskedEditPreservesOriginalExact(){
    Fixture f;Pixels original{64U,64U,std::vector<std::byte>(64U*64U*4U)},mask=original;
    for(std::size_t i{};i<original.rgba.size();i+=4U){original.rgba[i]=std::byte{9};original.rgba[i+1]=std::byte{61};original.rgba[i+2]=std::byte{202};original.rgba[i+3]=std::byte{31};mask.rgba[i]=std::byte{i<original.rgba.size()/2U?0U:255U};mask.rgba[i+3]=std::byte{255};}
    write(f.root/"workspace"/"source.png",take(NativeTools::Windows::Detail::encodeProviderImage(original,context())));
    write(f.root/"workspace"/"mask.png",take(NativeTools::Windows::Detail::encodeProviderImage(mask,context())));
    auto args=f.args();args["source_path"]=pathText(f.root/"workspace"/"source.png").value();args["mask_path"]=pathText(f.root/"workspace"/"mask.png").value();
    auto result=f.wait(f.execute("image_edit",args));require(result.at("state")=="completed"&&f.provider->count("/upload/image")==2U,"Masked edit did not upload exact owned source/mask.");
    const auto pixels=take(NativeTools::Windows::Detail::decodeProviderImage(read(f.root/"workspace"/"output.png"),context()));
    require(std::equal(pixels.rgba.begin(),pixels.rgba.begin()+pixels.rgba.size()/2U,original.rgba.begin()),"Pixels outside mask lost original RGBA values.");
    require(std::equal(pixels.rgba.begin()+pixels.rgba.size()/2U,pixels.rgba.end(),f.provider->generated.rgba.begin()+pixels.rgba.size()/2U),"Fully selected mask pixels differ from actual generated RGBA.");
    std::lock_guard lock{f.provider->mutex};require(f.provider->workflow.at("4").at("class_type")=="VAEEncodeForInpaint"&&f.provider->workflow.at("9").at("inputs").at("channel")=="red","Masked graph uses the wrong actual core node/channel.");
}
void ambiguousRecoveryStatusReadOnlyResumeNoReplay(){
    Fixture f;f.provider->mode=Provider::Mode::LostAck;auto admitted=f.execute("image_generate",f.args());auto failed=f.wait(admitted);
    require(failed.at("state")=="unknown"&&failed.at("artifact_published")==false&&failed.at("submission_acknowledged")==false,"Lost acknowledgement was fabricated as success or safe rejection.");
    f.reopen();const auto receipt=std::filesystem::path{take(Infrastructure::Windows::Detail::strictUtf8ToUtf16(failed.at("receipt_path").get<std::string>()))};const auto before=read(receipt);
    auto observed=f.execute("image_job_status",{{"job_id",upperId(admitted)}});require(observed.at("job_id")==admitted.at("job_id")&&observed.at("observed_remote_state")=="completed"&&read(receipt)==before&&!std::filesystem::exists(f.root/"workspace"/"output.png"),"Uppercase/read-only recovered status changed identity or wrote receipt/artifact.");
    auto resume=f.execute("image_job_resume",{{"job_id",upperId(admitted)}});auto completed=f.wait(resume);
    require(completed.at("state")=="completed"&&completed.at("recovered")==true&&f.provider->count("/prompt")==1U,"Explicit reattachment did not complete exact accepted ID without replay.");
}
void cancellationSuppressesPublicationAndNeverInterrupts(){
    Fixture f;f.provider->mode=Provider::Mode::PauseView;auto admitted=f.execute("image_generate",f.args());f.awaitView();
    auto cancelled=f.execute("image_job_cancel",{{"job_id",admitted.at("job_id")}});auto stopped=f.wait(cancelled);
    require(stopped.at("state")=="cancelled"&&stopped.at("publication_suppressed")==true&&!std::filesystem::exists(f.root/"workspace"/"output.png"),"Cancellation published an output artifact.");
    {std::lock_guard lock{f.provider->mutex};require(std::none_of(f.provider->calls.begin(),f.provider->calls.end(),[](const auto& c){return c.route=="/interrupt"||c.route=="/free";}),"Cancellation interrupted a shared provider.");}
    f.provider->mode=Provider::Mode::Complete;auto resumed=f.execute("image_job_resume",{{"job_id",admitted.at("job_id")}});
    require(f.wait(resumed).at("state")=="completed"&&f.provider->count("/prompt")==1U,"Explicit resume after local cancellation replayed generation or failed completion.");
}
void exactQueuedDeletionOnly(){
    Fixture f;f.provider->mode=Provider::Mode::Queued;auto admitted=f.execute("image_generate",f.args());
    const auto end=std::chrono::steady_clock::now()+3s;while(f.provider->count("/queue")==0U&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(2ms);
    auto cancelled=f.execute("image_job_cancel",{{"job_id",upperId(admitted)}});auto result=f.wait(cancelled);
    require(result.at("queue_delete_accepted")==true&&result.at("remote_cancel_confirmed")==false&&result.at("remote_execution_may_continue")==true,"Exact pending deletion claimed unobserved active cancellation.");
    require(f.provider->count("/prompt")==1U,"Uppercase equivalent cancellation submitted another generation.");
    std::lock_guard lock{f.provider->mutex};for(const auto& call:f.provider->calls)if(call.method=="POST"&&call.route=="/queue")require(Json::parse(call.body)==Json{{"delete",Json::array({f.provider->promptId})}},"Queue cancel widened to unrelated work.");
}
void destinationAndProviderPreconditions(){
    Fixture f;f.provider->mode=Provider::Mode::PauseView;auto admitted=f.execute("image_generate",f.args());f.awaitView();
    const std::string changed="External destination content";write(f.root/"workspace"/"output.png",std::as_bytes(std::span{changed.data(),changed.size()}));f.provider->releaseView=true;
    auto result=f.wait(admitted);require(result.at("state")=="unknown"&&result.at("error").at("code").get<std::string>()==Domain::ErrorCodes::Conflict&&read(f.root/"workspace"/"output.png")==std::vector<std::byte>(reinterpret_cast<const std::byte*>(changed.data()),reinterpret_cast<const std::byte*>(changed.data()+changed.size())),"Changed destination was overwritten.");
    Fixture p;p.provider->mode=Provider::Mode::PauseView;auto a=p.execute("image_generate",p.args());p.awaitView();
    {std::lock_guard lock{p.configuration.mutex};p.configuration.config.imageProvider.endpoint="http://127.0.0.1:8189";}p.provider->releaseView=true;
    auto r=p.wait(a);require(r.at("state")=="unknown"&&r.at("error").at("code").get<std::string>()==Domain::ErrorCodes::Conflict&&!std::filesystem::exists(p.root/"workspace"/"output.png"),"Changed endpoint was accepted for publication.");
}
void deadlineUnknownMissingAndRevokedScope(){
    Fixture f;f.provider->mode=Provider::Mode::Missing;auto args=f.args();args["timeout_sec"]=1U;auto admitted=f.execute("image_generate",args);auto result=f.wait(admitted);
    require(result.at("state")=="unknown"&&result.at("error").at("code").get<std::string>()==Domain::ErrorCodes::DeadlineExceeded,"Unobserved remote outcome was not deadline-bounded unknown.");
    f.reopen();const auto before=f.provider->count("/prompt");auto resume=f.execute("image_job_resume",{{"job_id",admitted.at("job_id")}});require(resume.at("state")=="unknown"&&f.provider->count("/prompt")==before,"Absent exact ID was replayed.");
    f.reopen({Domain::FileAccess::Read});require(f.wait(admitted).at("state")=="unknown","Read-only narrowed status could not observe its retained job.");
    requireError(f.invoke("image_job_resume",{{"job_id",admitted.at("job_id")}}),Domain::ErrorCodes::Unauthorized,"Revoked write/create grant authorized resume.");
    require(f.execute("image_job_cancel",{{"job_id",admitted.at("job_id")}}).at("state")=="cancelled","Revoked destination grant blocked owned local publication suppression.");
    f.reopen({Domain::FileAccess::Read,Domain::FileAccess::Write,Domain::FileAccess::Create},"other-principal");
    requireError(f.invoke("image_job_status",{{"job_id",admitted.at("job_id")}}),Domain::ErrorCodes::ProjectScopeMismatch,"Different authenticated principal accepted a foreign durable receipt.");
    f.reopen({Domain::FileAccess::Read,Domain::FileAccess::Write,Domain::FileAccess::Create},"forge-conductor-manager",true);
    requireError(f.invoke("image_job_status",{{"job_id",admitted.at("job_id")}}),Domain::ErrorCodes::PathOutsideAuthority,"Narrowed root restored a broader saved scope.");
}
void providerErrorsAreActualNotArtifacts(){
    for(const auto mode:{Provider::Mode::Reject,Provider::Mode::WrongGraph,Provider::Mode::WrongDimensions}){
        Fixture f;f.provider->mode=mode;auto result=f.wait(f.execute("image_generate",f.args()));
        require(result.at("state")== (mode==Provider::Mode::Reject?"failed":"unknown")&&result.at("artifact_published")==false&&result.at("artifact_sha256").is_null()&&!std::filesystem::exists(f.root/"workspace"/"output.png"),"Rejected, wrong-ID workflow or wrong-size PNG produced fabricated artifact success.");
        require(f.provider->count("/prompt")==1U,"Error retried generation.");
    }
}
void sealedReceiptsRejectLiveOwnerAndWrongProjectWithoutReplay(){
    Fixture f;f.provider->mode=Provider::Mode::LostAck;auto admitted=f.execute("image_generate",f.args());auto failed=f.wait(admitted);
    const auto receipt=std::filesystem::path{take(Infrastructure::Windows::Detail::strictUtf8ToUtf16(failed.at("receipt_path").get<std::string>()))};
    f.service.reset();auto raw=read(receipt);auto envelope=Json::parse(reinterpret_cast<const char*>(raw.data()),reinterpret_cast<const char*>(raw.data()+raw.size()));
    envelope["payload"]["state"]=static_cast<unsigned>(Domain::ImageJobState::Running);
    envelope["payload"]["owner_released"]=false;
    const auto body=envelope.at("payload").dump();envelope["sha256"]=digest(std::as_bytes(std::span{body.data(),body.size()}));const auto encoded=envelope.dump();
    write(receipt,std::as_bytes(std::span{encoded.data(),encoded.size()}));f.reopen();const auto before=read(receipt);
    auto status=f.wait(admitted);require(status.at("state")=="unknown"&&status.at("observed_remote_state")=="completed"&&read(receipt)==before,"Restored interrupted receipt was rewritten or auto-published by read-only status.");
    requireError(f.invoke("image_job_resume",{{"job_id",admitted.at("job_id")}}),Domain::ErrorCodes::Conflict,"A still-live observed owner permitted a second publication worker.");
    require(!std::filesystem::exists(f.root/"workspace"/"output.png")&&f.provider->count("/prompt")==1U,"Live-owner recovery replayed or published generation.");
    const auto otherProject=parse<Domain::ProjectId>("10000000-0000-4000-8000-000000000003");
    WindowsWorkspaceAuthority other{{{parse<Domain::AuthorityId>("20000000-0000-4000-8000-000000000003"),otherProject,
        parse<Domain::ClientId>("forge-conductor-manager"),{pathText(f.root/"workspace")},Domain::FileAccess::Write,
        {Domain::FileAccess::Read,Domain::FileAccess::Write,Domain::FileAccess::Create},{},false,1U}}};
    auto otherAuthority=take(other.authorityFor(otherProject,context()));
    const auto foreign=f.root/"private"/otherProject.value()/admitted.at("job_id").get<std::string>();std::filesystem::create_directories(foreign);write(foreign/"receipt.json",before);
    const auto count=f.provider->calls.size();
    requireError(f.service->execute("image_job_status",Json{{"job_id",admitted.at("job_id")}}.dump(),otherAuthority,context()),Domain::ErrorCodes::IntegrityFailure,"A copied foreign-project receipt was trusted by storage location.");
    require(f.provider->calls.size()==count,"Foreign-project receipt triggered provider access.");
    f.service.reset();raw=read(receipt);envelope=Json::parse(reinterpret_cast<const char*>(raw.data()),reinterpret_cast<const char*>(raw.data()+raw.size()));envelope["payload"]["request"]["seed"]=100U;
    const auto corrupted=envelope.dump();write(receipt,std::as_bytes(std::span{corrupted.data(),corrupted.size()}));f.reopen();
    requireError(f.invoke("image_job_status",{{"job_id",admitted.at("job_id")}}),Domain::ErrorCodes::IntegrityFailure,"Unsealed modified receipt was accepted.");
}
void localCancellationSurvivesDisabledProviderAndNarrowedGrant(){
    for(const bool disable:{true,false}){
        Fixture f;f.provider->mode=Provider::Mode::Queued;auto admitted=f.execute("image_generate",f.args());
        const auto end=std::chrono::steady_clock::now()+3s;while(f.provider->count("/queue")==0U&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(2ms);
        if(disable){std::lock_guard lock{f.configuration.mutex};f.configuration.config.imageProvider.enabled=false;}
        else f.authority=std::make_unique<Contracts::WorkspaceAuthority>(take(f.issuer->narrow(*f.authority,f.authority->trustedRoots(),{Domain::FileAccess::Read},false,2U,context())));
        auto cancelled=f.execute("image_job_cancel",{{"job_id",admitted.at("job_id")}});auto result=f.wait(cancelled);
        require(result.at("state")=="cancelled"&&result.at("publication_suppressed")==true&&!std::filesystem::exists(f.root/"workspace"/"output.png"),"Changed config or narrowed native scope blocked owned local cancellation.");
        {std::lock_guard lock{f.provider->mutex};require(std::none_of(f.provider->calls.begin(),f.provider->calls.end(),[](const auto& c){return c.method=="POST"&&c.route=="/queue";}),"Changed provider or revoked effect scope mutated the remote queue.");}
        if(disable)require(result.at("observed_remote_state")=="unavailable"&&result.at("observed_remote_error").at("code").get<std::string>()==Domain::ErrorCodes::Conflict,"Unavailable provider observation hid actual local receipt or fabricated remote completion.");
    }
}
void boundedRetentionEvictsOnlyTerminalEvidencePreservesArtifacts(){
    Fixture f;Json first;std::filesystem::path firstReceipt;
    for(unsigned i{};i<35U;++i){
        auto job=f.execute("image_generate",f.args("artifact-"+std::to_string(i)+".png"));auto completed=f.wait(job);require(completed.at("state")=="completed","Ordinary generation stopped at retained receipt capacity.");
        if(i==0U){first=job;firstReceipt=std::filesystem::path{take(Infrastructure::Windows::Detail::strictUtf8ToUtf16(completed.at("receipt_path").get<std::string>()))};}
    }
    const auto projectDirectory=f.root/"private"/Fixture::project().value();
    require(std::distance(std::filesystem::directory_iterator{projectDirectory},std::filesystem::directory_iterator{})==32,"Terminal retention did not keep the bounded32 evidence slots.");
    require(!std::filesystem::exists(firstReceipt)&&std::distance(std::filesystem::directory_iterator{f.root/"workspace"},std::filesystem::directory_iterator{})==35,"Evidence retirement deleted a published destination or retained oldest receipt.");
    requireError(f.invoke("image_job_status",{{"job_id",first.at("job_id")}}),Domain::ErrorCodes::RecordNotFound,"Retired receipt remained in the in-memory cache as durable evidence.");
    Fixture blocked;const auto directory=blocked.root/"private"/Fixture::project().value();std::filesystem::create_directories(directory);
    const std::string invalid="Unverified evidence must survive";
    for(unsigned i{};i<32U;++i){const auto dir=directory/take(blocked.uuids.next()).value();std::filesystem::create_directories(dir);write(dir/"receipt.json",std::as_bytes(std::span{invalid.data(),invalid.size()}));}
    requireError(blocked.invoke("image_generate",blocked.args()),Domain::ErrorCodes::LimitExceeded,"Unverified evidence was evicted to admit new generation.");
    require(blocked.provider->calls.empty()&&std::filesystem::is_empty(blocked.root/"workspace")&&std::distance(std::filesystem::directory_iterator{directory},std::filesystem::directory_iterator{})==32,"Capacity refusal changed unverified evidence or started provider effects.");
}
void finishedCachePressureKeepsDurableReadsAndAdmissions(){
    Fixture f;auto original=f.wait(f.execute("image_generate",f.args()));
    const auto receiptPath=std::filesystem::path{take(Infrastructure::Windows::Detail::strictUtf8ToUtf16(original.at("receipt_path").get<std::string>()))};
    const auto raw=read(receiptPath);auto base=Json::parse(reinterpret_cast<const char*>(raw.data()),reinterpret_cast<const char*>(raw.data()+raw.size())).at("payload");
    const auto artifact=read(f.root/"workspace"/"output.png");f.service.reset();
    std::vector<WindowsWorkspaceAuthorityPolicy> policies;
    std::vector<Domain::ProjectId> projects;std::vector<std::filesystem::path> roots;
    for(unsigned p{};p<8U;++p){
        auto project=p==0U?Fixture::project():parse<Domain::ProjectId>(take(f.uuids.next()).value());
        auto root=p==0U?f.root/"workspace":f.root/("workspace-"+std::to_string(p));std::filesystem::create_directories(root);
        projects.push_back(project);roots.push_back(root);write(root/"output.png",artifact);
        policies.push_back({parse<Domain::AuthorityId>(take(f.uuids.next()).value()),project,parse<Domain::ClientId>("forge-conductor-manager"),
            {pathText(root)},Domain::FileAccess::Write,{Domain::FileAccess::Read,Domain::FileAccess::Write,Domain::FileAccess::Create},{},false,1U});
    }
    f.issuer=std::make_unique<WindowsWorkspaceAuthority>(std::move(policies));
    f.authority=std::make_unique<Contracts::WorkspaceAuthority>(take(f.issuer->authorityFor(projects.front(),context())));
    f.service=std::make_unique<Service>(*f.issuer,f.files,f.configuration,*f.storageIssuer,*f.storage,pathText(f.root/"private"),f.uuids,f.clock,f.hasher,f.provider);
    f.provider->mode=Provider::Mode::BlockPreflight;auto liveArgs=f.args("live-through-cache-pressure.png");liveArgs["timeout_sec"]=60U;
    const auto live=f.execute("image_generate",liveArgs);
    const auto liveEnd=std::chrono::steady_clock::now()+3s;
    while(f.provider->preflightEntered.load()==0U&&std::chrono::steady_clock::now()<liveEnd)std::this_thread::sleep_for(2ms);
    require(f.provider->preflightEntered.load()==1U,"Live cache fixture never reached its owned blocked native preflight.");
    const auto liveReceipt=std::filesystem::path{take(Infrastructure::Windows::Detail::strictUtf8ToUtf16(live.at("receipt_path").get<std::string>()))};
    const auto liveBefore=read(liveReceipt);
    std::string first;
    // Independently staged, sealed completed receipt fixtures exercise cache pressure; they are not new provider generations.
    for(unsigned p{};p<projects.size();++p){
        auto authority=take(f.issuer->authorityFor(projects[p],context()));
        for(unsigned i{};i<32U;++i){
            auto payload=base;const auto id=take(f.uuids.next()).value();if(p==0U&&i==0U)first=id;
            payload["job_id"]=id;payload["prompt_id"]=take(f.uuids.next()).value();payload["scope"]["project_id"]=projects[p].value();
            payload["scope"]["roots"]=Json::array({pathText(roots[p]).value()});payload["request"]["path"]=pathText(roots[p]/"output.png").value();
            payload["created_utc_ms"]=base.at("created_utc_ms").get<std::int64_t>()+1U+p*32U+i;
            auto workflow=Json::parse(payload.at("graph_json").get<std::string>());workflow["7"]["inputs"]["filename_prefix"]="ForgeConductor/"+projects[p].value()+"/"+id+"/image";
            const auto graph=workflow.dump();payload["graph_json"]=graph;payload["graph_sha256"]=digest(std::as_bytes(std::span{graph.data(),graph.size()}));
            const auto encodedPayload=payload.dump();const auto encoded=Json{{"payload",payload},{"sha256",digest(std::as_bytes(std::span{encodedPayload.data(),encodedPayload.size()}))}}.dump();
            const auto directory=f.root/"private"/projects[p].value()/id;std::filesystem::create_directories(directory);write(directory/"receipt.json",std::as_bytes(std::span{encoded.data(),encoded.size()}));
            const auto result=Json::parse(take(f.service->execute("image_job_status",Json{{"job_id",id}}.dump(),authority,context())));
            require(result.at("state")=="completed"&&result.at("artifact_published")==true,"Staged completed cache fixture lost actual artifact facts.");
        }
    }
    auto old=f.execute("image_job_status",{{"job_id",original.at("job_id")}});require(old.at("state")=="completed","256finished cache entries blocked an uncached retained receipt read.");
    auto reloaded=f.execute("image_job_status",{{"job_id",first}});require(reloaded.at("state")=="completed","Memory eviction deleted durable receipt evidence.");
    const auto liveAfter=f.execute("image_job_status",{{"job_id",live.at("job_id")}});
    require(liveAfter.at("done")==false&&liveAfter.at("state")=="before_dispatch"&&read(liveReceipt)==liveBefore&&f.provider->count("/prompt")==1U,
        "Cache pressure evicted, stopped, replayed or altered a live worker.");
    f.provider->mode=Provider::Mode::Complete;f.provider->releasePreflight=true;
    require(f.wait(live).at("state")=="completed","Preserved live worker failed to finish its one exact generation after release.");
    auto next=f.wait(f.execute("image_generate",f.args("after-cache-pressure.png")));
    require(next.at("state")=="completed"&&f.provider->count("/prompt")==3U,"Finished cache pressure blocked ordinary admission or fabricated/replayed remote generations.");
}
void activeWorkerLimitPreservesAllOwnedWorkers(){
    Fixture f;{std::lock_guard lock{f.configuration.mutex};f.configuration.config.imageProvider.endpoint="http://127.0.0.1:49178";}
    f.provider->mode=Provider::Mode::BlockPreflight;std::vector<Json> live;
    for(unsigned i{};i<8U;++i){auto args=f.args("live-"+std::to_string(i)+".png");args["timeout_sec"]=30U;live.push_back(f.execute("image_generate",args));}
    const auto end=std::chrono::steady_clock::now()+3s;
    while(f.provider->preflightEntered.load()!=1U&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(2ms);
    require(f.provider->preflightEntered.load()==1U&&f.provider->count("/object_info/CheckpointLoaderSimple")==1U,
        "The shared provider lease did not admit exactly one worker to the blocked preflight boundary.");
    requireError(f.invoke("image_generate",f.args("ninth.png")),Domain::ErrorCodes::LimitExceeded,"A ninth active worker was admitted.");
    require(f.provider->count("/prompt")==0U&&std::filesystem::is_empty(f.root/"workspace"),"Admission limit started a generation or published a destination.");
    for(const auto& job:live){const auto observed=f.execute("image_job_status",{{"job_id",job.at("job_id")}});require(observed.at("done")==false&&observed.at("state")=="before_dispatch","Admission refusal evicted or stopped an owned active worker.");}
    for(const auto& job:live){const auto stopped=f.wait(f.execute("image_job_cancel",{{"job_id",job.at("job_id")}}));require(stopped.at("state")=="cancelled"&&stopped.at("artifact_published")==false,"Explicit owned cancellation did not stop its blocked worker.");}
    require(f.provider->count("/prompt")==0U,"Cancelling pre-dispatch workers submitted provider generation.");
}
void cachePressurePreservesBorrowedCancelResumeControl(){
    Fixture donor;const auto completed=donor.wait(donor.execute("image_generate",donor.args()));
    const auto donorReceipt=std::filesystem::path{take(Infrastructure::Windows::Detail::strictUtf8ToUtf16(completed.at("receipt_path").get<std::string>()))};
    const auto donorRaw=read(donorReceipt);const auto base=Json::parse(reinterpret_cast<const char*>(donorRaw.data()),reinterpret_cast<const char*>(donorRaw.data()+donorRaw.size())).at("payload");
    const auto artifact=read(donor.root/"workspace"/"output.png");
    Fixture f;f.service.reset();std::vector<WindowsWorkspaceAuthorityPolicy> policies;std::vector<Domain::ProjectId> projects;std::vector<std::filesystem::path> roots;
    for(unsigned p{};p<8U;++p){
        const auto project=p==0U?Fixture::project():parse<Domain::ProjectId>(take(f.uuids.next()).value());
        const auto root=p==0U?f.root/"workspace":f.root/("workspace-"+std::to_string(p));std::filesystem::create_directories(root);write(root/"output.png",artifact);
        projects.push_back(project);roots.push_back(root);policies.push_back({parse<Domain::AuthorityId>(take(f.uuids.next()).value()),project,parse<Domain::ClientId>("forge-conductor-manager"),
            {pathText(root)},Domain::FileAccess::Write,{Domain::FileAccess::Read,Domain::FileAccess::Write,Domain::FileAccess::Create},{},false,1U});
    }
    f.issuer=std::make_unique<WindowsWorkspaceAuthority>(std::move(policies));f.authority=std::make_unique<Contracts::WorkspaceAuthority>(take(f.issuer->authorityFor(projects.front(),context())));
    f.service=std::make_unique<Service>(*f.issuer,f.files,f.configuration,*f.storageIssuer,*f.storage,pathText(f.root/"private"),f.uuids,f.clock,f.hasher,f.provider);
    f.provider->mode=Provider::Mode::PauseQueueDelete;auto requested=f.args("owned-resume.png");requested["timeout_sec"]=60U;const auto owned=f.execute("image_generate",requested);
    const auto queueEnd=std::chrono::steady_clock::now()+3s;while(f.provider->count("/queue")==0U&&std::chrono::steady_clock::now()<queueEnd)std::this_thread::sleep_for(2ms);
    require(f.provider->count("/queue")!=0U,"Owned generation never reached its actual queued fixture state.");
    const auto ownedReceipt=std::filesystem::path{take(Infrastructure::Windows::Detail::strictUtf8ToUtf16(owned.at("receipt_path").get<std::string>()))};
    const auto ownedRaw=read(ownedReceipt);const auto created=Json::parse(reinterpret_cast<const char*>(ownedRaw.data()),reinterpret_cast<const char*>(ownedRaw.data()+ownedRaw.size())).at("payload").at("created_utc_ms").get<std::int64_t>();
    const auto stage=[&](unsigned ordinal){
        const auto projectIndex=ordinal/32U;auto payload=base;const auto id=take(f.uuids.next()).value();
        payload["job_id"]=id;payload["prompt_id"]=take(f.uuids.next()).value();payload["scope"]["project_id"]=projects[projectIndex].value();payload["scope"]["roots"]=Json::array({pathText(roots[projectIndex]).value()});
        payload["request"]["path"]=pathText(roots[projectIndex]/"output.png").value();payload["created_utc_ms"]=created+1000+ordinal;
        auto workflow=Json::parse(payload.at("graph_json").get<std::string>());workflow["7"]["inputs"]["filename_prefix"]="ForgeConductor/"+projects[projectIndex].value()+"/"+id+"/image";
        const auto graph=workflow.dump();payload["graph_json"]=graph;payload["graph_sha256"]=digest(std::as_bytes(std::span{graph.data(),graph.size()}));
        const auto body=payload.dump();const auto encoded=Json{{"payload",payload},{"sha256",digest(std::as_bytes(std::span{body.data(),body.size()}))}}.dump();
        const auto directory=f.root/"private"/projects[projectIndex].value()/id;std::filesystem::create_directories(directory);write(directory/"receipt.json",std::as_bytes(std::span{encoded.data(),encoded.size()}));
        const auto authority=take(f.issuer->authorityFor(projects[projectIndex],context()));
        const auto result=Json::parse(take(f.service->execute("image_job_status",Json{{"job_id",id}}.dump(),authority,context())));
        require(result.at("state")=="completed","Independent completed cache fixture lost its actual retained artifact facts.");
    };
    // These sealed completed fixtures are cache entries, not fabricated provider generations.
    for(unsigned i{};i<255U;++i)stage(i);
    std::optional<Domain::Result<std::string>> cancellation,resume;std::atomic<bool> resumeEntered{},resumeReturned{};
    std::jthread cancelThread,resumeThread;
    struct Release final{Provider& provider;~Release(){provider.mode=Provider::Mode::Complete;provider.releaseQueueDelete=true;}} release{*f.provider};
    cancelThread=std::jthread{[&]{cancellation=f.invoke("image_job_cancel",{{"job_id",owned.at("job_id")}});}};
    const auto cancelEnd=std::chrono::steady_clock::now()+3s;while(!f.provider->queueDeleteEntered.load()&&std::chrono::steady_clock::now()<cancelEnd)std::this_thread::sleep_for(2ms);
    require(f.provider->queueDeleteEntered.load(),"Cancellation did not retain its control lease during the exact queued DELETE reply.");
    require(f.wait(owned).at("done")==true,"Local cancellation worker did not finish before cache pressure.");
    stage(255U);
    resumeThread=std::jthread{[&]{resumeEntered=true;resume=f.invoke("image_job_resume",{{"job_id",owned.at("job_id")}});resumeReturned=true;}};
    const auto enteredEnd=std::chrono::steady_clock::now()+3s;while(!resumeEntered.load()&&std::chrono::steady_clock::now()<enteredEnd)std::this_thread::sleep_for(2ms);
    require(resumeEntered.load(),"Concurrent explicit resume did not begin.");
    const auto blockedEnd=std::chrono::steady_clock::now()+2s;while(!resumeReturned.load()&&std::chrono::steady_clock::now()<blockedEnd)std::this_thread::sleep_for(2ms);
    const bool returnedBeforeControlRelease=resumeReturned.load();
    f.provider->mode=Provider::Mode::Complete;f.provider->releaseQueueDelete=true;cancelThread.join();resumeThread.join();
    require(cancellation.has_value()&&resume.has_value(),"Concurrent owned control calls lost their actual results.");static_cast<void>(take(std::move(*cancellation)));static_cast<void>(take(std::move(*resume)));
    const auto final=f.wait(owned);f.service->shutdown();
    require(!returnedBeforeControlRelease,"Cache pressure replaced a borrowed Entry, allowing explicit resume to bypass its pending cancellation control lease.");
    require(final.at("state")=="completed"&&final.at("artifact_published")==true&&f.provider->count("/prompt")==1U,"Tracked exact-ID resume failed completion or replayed generation after cache pressure.");
    {std::lock_guard lock{f.provider->mutex};require(std::none_of(f.provider->calls.begin(),f.provider->calls.end(),[](const auto& call){return call.route=="/interrupt"||call.route=="/free";}),"Borrowed-control recovery used a global provider control.");}
}
}
}
int main(){
    using namespace ForgeConductor::Tests;
    TestRegistry tests{{"disabled-invalid-before-effects",disabledAndInvalidBeforeEffects},{"generate-reopen-actual-artifact",generateAndReopenActualArtifact},
        {"masked-edit-exact-original-rgba",maskedEditPreservesOriginalExact},{"ambiguous-status-read-only-resume-no-replay",ambiguousRecoveryStatusReadOnlyResumeNoReplay},
        {"cancel-publication-no-interrupt",cancellationSuppressesPublicationAndNeverInterrupts},{"exact-queued-id-delete",exactQueuedDeletionOnly},
        {"destination-provider-preconditions",destinationAndProviderPreconditions},{"deadline-missing-revoked-scope",deadlineUnknownMissingAndRevokedScope},
        {"actual-provider-errors-no-artifact",providerErrorsAreActualNotArtifacts},
        {"sealed-live-owner-wrong-project-no-replay",sealedReceiptsRejectLiveOwnerAndWrongProjectWithoutReplay},
        {"local-cancel-disabled-provider-narrowed-grants",localCancellationSurvivesDisabledProviderAndNarrowedGrant},
        {"bounded-terminal-retention-artifacts-preserved",boundedRetentionEvictsOnlyTerminalEvidencePreservesArtifacts},
        {"finished-cache-pressure-durable-read-admission",finishedCachePressureKeepsDurableReadsAndAdmissions},
        {"eight-active-workers-preserved-ninth-refused",activeWorkerLimitPreservesAllOwnedWorkers},
        {"cache-pressure-borrowed-cancel-resume-control",cachePressurePreservesBorrowedCancelResumeControl}};
    try{for(const auto& [name,run]:tests){run();std::cout<<"PASS "<<name<<'\n';}}catch(const std::exception& error){std::cerr<<"FAIL image-provider service: "<<error.what()<<'\n';return 1;}return 0;
}
