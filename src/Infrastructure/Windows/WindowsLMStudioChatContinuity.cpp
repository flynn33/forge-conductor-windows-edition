#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioChatContinuity.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioConversationReader.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioChatControl.h"
#include "LMStudioChatContinuityControl.h"
#include "LMStudioChatCheckpoint.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <chrono>
#include <utility>
#include <algorithm>
#include <set>
#include <optional>
#include <cctype>
#include <Windows.h>
namespace ForgeConductor::Infrastructure::Windows {
namespace {
using Json = nlohmann::json;
std::shared_ptr<Detail::LMStudioChatControlActions> defaultChatControls() {
    auto actions=std::make_shared<Detail::LMStudioChatControlActions>();
    actions->activate=WindowsLMStudioChatControl::activate;
    actions->idle=WindowsLMStudioChatControl::idle;
    actions->pause=WindowsLMStudioChatControl::pauseAtToolBoundary;
    actions->send=WindowsLMStudioChatControl::send;
    return actions;
}
std::filesystem::path filePath(std::string_view value) {
    std::u8string encoded;
    for (const auto character : value) encoded.push_back(static_cast<char8_t>(character));
    return std::filesystem::path{encoded};
}
std::string checkpointDigest(std::string_view value) {
    BCryptSha256Hasher hasher;
    auto result=hasher.sha256(std::as_bytes(std::span{value.data(),value.size()}));
    if(!result) throw std::runtime_error{result.error().message};
    return result.value().value();
}
Json packetBody(const Domain::LegacyHandoffPacket& p) {
    Json value{{"id",p.id.value()},{"goal",p.goal},{"status",p.status},{"narrative",p.narrative},
        {"next_actions",p.nextActions},{"blockers",p.blockers},{"key_files",p.keyFiles},
        {"decisions",p.decisions},{"resume_seed",p.resumeSeed},{"agents",Json::array()}};
    if(p.workingDirectory) value["cwd"]=*p.workingDirectory;
    for(const auto& agent:p.agents) {
        Json snapshot{{"session_id",agent.sessionId.value()},{"agent_id",agent.agentId.value()},
            {"goal",agent.goal},{"status",agent.status},{"resume_hint",agent.resumeHint}};
        if(agent.workingDirectory) snapshot["cwd"]=*agent.workingDirectory;
        value["agents"].push_back(std::move(snapshot));
    }
    return value;
}
bool containsFiller(std::string text) {
    std::transform(text.begin(),text.end(),text.begin(),[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::size_t start{};
    for(std::size_t end=0U;end<=text.size();++end) {
        const bool boundary=end==text.size() || text[end]=='\n' || text[end]=='\r' ||
            ((text[end]=='.' || text[end]=='!' || text[end]=='?') &&
                (end+1U==text.size() || std::isspace(static_cast<unsigned char>(text[end+1U]))!=0));
        if(!boundary) continue;
        auto sentence=std::string_view{text}.substr(start,end-start);
        const auto first=sentence.find_first_not_of(" \t\r\n\f\v");
        if(first!=std::string_view::npos) {
            sentence.remove_prefix(first);
            for(const auto marker:{std::string_view{"lorem ipsum"},std::string_view{"placeholder"}}) {
                if(sentence.starts_with(marker) && (sentence.size()==marker.size() ||
                    std::isalnum(static_cast<unsigned char>(sentence[marker.size()]))==0)) return true;
            }
        }
        start=end+1U;
    }
    return false;
}
bool singleCharacterPadding(std::string_view text) {
    std::optional<unsigned char> first;
    for (const unsigned char c : text) {
        if (std::isspace(c)) continue;
        const auto lower=static_cast<unsigned char>(std::tolower(c));
        if (!first) first=lower;
        else if (*first!=lower) return false;
    }
    return true;
}
bool opaqueAction(std::string text) {
    const auto start=text.find_first_not_of(" \t\r\n"),end=text.find_last_not_of(" \t\r\n");
    if(start==std::string::npos) return true;
    text=text.substr(start,end-start+1);
    std::transform(text.begin(),text.end(),text.begin(),[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text=="continue" || text=="resume" || text=="next";
}
std::string taskInstructions(const LMStudioConversationObservation& chat) {
    Json messages=Json::array();std::size_t remaining=12U*1024U;
    for(auto it=chat.userMessages.rbegin();it!=chat.userMessages.rend();++it) {
        if(it->starts_with("Auto Continuity:") || it->starts_with("Auto Continuity cannot") ||
            it->starts_with("Resume this Forge project from its model-written continuity packet.")) continue;
        if(it->size()>remaining) break;
        messages.insert(messages.begin(),*it);remaining-=it->size();
    }
    while(!messages.empty() && messages.dump().size()>12U*1024U) messages.erase(messages.begin());
    return messages.empty()?std::string{}:"\nRecent original user task instructions (verbatim source; preserve their concrete pending actions and constraints):\n"+messages.dump();
}
std::string nativeCallHistory(const LMStudioConversationObservation& chat) {
    Json counts=Json::object(),calls=Json::array();
    for(const auto& call:chat.nativeToolResults) {
        if(call.pluginIdentifier!="mcp/forge-conductor" && call.pluginIdentifier!="mcp/forge-conductor-fallback" && call.pluginIdentifier!="mcp/forge-conductor-clu") continue;
        auto& count=counts[call.name];
        if(count.is_null()) count={{"pairs",0U},{"success",0U},{"failure",0U},{"unknown",0U}};
        count["pairs"]=count.at("pairs").get<unsigned>()+1U;
        Json entry{{"tool",call.name},{"plugin",call.pluginIdentifier},{"request_id",call.requestId}};
        std::string outcome="unknown";
        for(const auto& body:call.textBodies) {
            const auto result=Json::parse(body,nullptr,false);
            if(!result.is_object()) continue;
            if(result.contains("ok") && result.at("ok").is_boolean()) {
                entry["ok"]=result.at("ok");outcome=result.at("ok").get<bool>()?"success":"failure";
            }
            if(result.contains("code") && result.at("code").is_string()) entry["code"]=result.at("code");
            if(call.name=="agent_get" || call.name=="agent_context") {
                for(const char* field:{"id","display_name"})
                    if(result.contains(field) && result.at(field).is_string()) entry[field]=result.at(field);
            }
            if(call.name=="get_forge_status" || call.name=="forge_status") {
                if(result.contains("workspace") && result.at("workspace").is_object()) {
                    Json workspace=Json::object();
                    for(const char* field:{"project_id","project_root","binding_source"})
                        if(result.at("workspace").contains(field) && result.at("workspace").at(field).is_string()) workspace[field]=result.at("workspace").at(field);
                    entry["workspace"]=std::move(workspace);
                }
                for(const char* field:{"tool_count","agent_count"})
                    if(result.contains(field) && result.at(field).is_number_unsigned()) entry[field]=result.at(field);
            }
        }
        count[outcome]=count.at(outcome).get<unsigned>()+1U;
        calls.push_back(std::move(entry));if(calls.size()>40U) calls.erase(calls.begin());
    }
    std::size_t pairedCalls{};for(const auto& item:counts.items()) pairedCalls+=item.value().at("pairs").get<unsigned>();
    Json history{{"per_tool_call_counts",counts},{"recent_calls",calls},{"entries_truncated",pairedCalls>calls.size()},{"counts_truncated",false}};
    while(history.dump().size()>4096U && !history["recent_calls"].empty()) {
        history["recent_calls"].erase(history["recent_calls"].begin());history["entries_truncated"]=true;
    }
    while(history.dump().size()>4096U && !history["per_tool_call_counts"].empty()) {
        history["per_tool_call_counts"].erase(history["per_tool_call_counts"].begin());history["counts_truncated"]=true;
    }
    return "\nNative renderer Forge request/result history (counts are observed pairs, with success/failure/unknown separated; entries are recent and may be truncated. Do not convert intended actions into executed calls):\n"+history.dump();
}
bool samePacketBody(const Json& value,const Domain::LegacyHandoffPacket& packet) {
    if(!value.is_object()) return false;
    const auto meta=value.find("meta"),task=value.find("task"),working=value.find("working_set"),resume=value.find("resume");
    if(meta==value.end() || task==value.end() || working==value.end() || resume==value.end() ||
        !meta->is_object() || !task->is_object() || !working->is_object() || !resume->is_object()) return false;
    if(meta->value("id",std::string{})!=packet.id.value() || meta->value("source",std::string{})!="model" ||
        !meta->value("resume_ready",false) || resume->value("seed",std::string{})!=packet.resumeSeed) return false;
    Json body{{"id",packet.id.value()},{"goal",task->value("goal",std::string{})},
        {"status",task->value("status",std::string{})},{"narrative",value.value("narrative",std::string{})},
        {"next_actions",task->value("next_actions",Json::array())},{"blockers",task->value("blockers",Json::array())},
        {"key_files",working->value("key_files",Json::array())},{"decisions",working->value("decisions",Json::array())},
        {"resume_seed",resume->value("seed",std::string{})},{"agents",value.value("agents",Json::array())}};
    if(task->contains("cwd")) body["cwd"]=task->at("cwd");
    if(!body["agents"].is_array()) return false;
    for(auto& agent:body["agents"]) { if(!agent.is_object()) return false;agent.erase("updated_at"); }
    return body==packetBody(packet);
}
}
class WindowsLMStudioChatContinuity::Impl final {
public:
    Impl(Domain::ProjectId project,Domain::PathText root,Domain::PathText home,Domain::PathText studio,
        Domain::PathText exe,Domain::LocalModelConfig config,Contracts::ILegacyMemoryService& memory,
        Contracts::ILegacyContextContinuityService& continuity,Contracts::IProjectMemoryService& projects,
        Contracts::IClock& clock,Contracts::IUuidGenerator& uuid,Contracts::IConfigurationStore& configurationStore,
        bool initialWorkspaceConfirmed,std::shared_ptr<Detail::LMStudioChatControlActions> controls)
        :project_{std::move(project)},root_{std::move(root)},home_{std::move(home)},studio_{std::move(studio)},
         exe_{std::move(exe)},config_{std::move(config)},memory_{memory},continuity_{continuity},
         projects_{projects},clock_{clock},uuid_{uuid},configurationStore_{configurationStore},
         controls_{controls?std::move(controls):defaultChatControls()},workspaceConfirmed_{initialWorkspaceConfirmed} {
        status_["state"]=initialWorkspaceConfirmed?"observing":"awaiting_bound_workspace";
    }
    void start() { worker_=std::jthread([this](std::stop_token stop) {
        while(!stop.stop_requested()) {
            try { tick(stop); } catch(const std::exception& e) { failure(e.what()); }
            catch(...) { failure("LM Studio continuity observation failed."); }
            try {
                std::lock_guard lock{mutex_};
                if(checkpoint_ && !checkpointLoadFailed_ && phase_!=Phase::Observe) saveCheckpointUnlocked(context(stop));
            } catch(const std::exception& e) { checkpointFailure(e.what()); }
            for(int i=0;i<5 && !stop.stop_requested();++i) std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }); }
    void shutdown() noexcept {
        worker_.request_stop(); if(worker_.joinable()) worker_.join();
        checkpoint_.reset();checkpointLoaded_=false;checkpointLoadFailed_=false;
    }
    std::string status() const { std::lock_guard lock{mutex_}; return status_.dump(); }
    void recordTool(std::string_view name,bool succeeded,std::string_view payload) noexcept {
        try {
            if (!bound()) return;
            std::lock_guard lock{mutex_};
            traceUnlocked({{"event","connector_tool_result"},{"tool",name},{"ok",succeeded},
                {"result",Json::parse(payload)}});
        } catch (...) {
            // Optional observation cannot change a completed Forge result.
        }
    }

    void bindWorkspace(const Domain::ProjectId& project,const Domain::PathText& root) noexcept {
        try {
            std::lock_guard lock{mutex_};
            if(workspaceConfirmed_ && project_==project && root_==root) {
                pendingWorkspace_.reset();return;
            }
            pendingWorkspace_=WorkspaceBinding{project,root};
        } catch (...) {
            // Optional native observation cannot change an authorized MCP result.
        }
    }
private:
    enum class Phase { Observe,WaitingPacket,Creating,Resuming,Complete };
    std::string phaseName() const {
        switch(phase_) {
        case Phase::Observe:return "observing";
        case Phase::WaitingPacket:return repairRequest_.empty() || repairAcknowledged_?"waiting_for_model_packet":"repairing_model_packet";
        case Phase::Creating:return "successor_creating";
        case Phase::Resuming:return "resuming";
        case Phase::Complete:return "resumed";
        }
        throw std::runtime_error{"Visible chat checkpoint phase is invalid."};
    }
    Json checkpointScope() {
        const auto canonical=[](const Domain::PathText& path) {
            const auto value=std::filesystem::weakly_canonical(filePath(path.value())).generic_u8string();
            return std::string{reinterpret_cast<const char*>(value.data()),value.size()};
        };
        std::ifstream input{filePath(studio_.value())/"mcp.json"};
        if(!input) throw std::runtime_error{"Visible chat checkpoint route cannot be verified."};
        const auto routes=Json::parse(input).at("mcpServers");
        Json active=Json::object();
        for(const char* key:{"forge-conductor","forge-conductor-fallback","forge-conductor-clu"}) active[key]=routes.at(key);
        return Json{{"project_id",project_.value()},{"project_root",canonical(root_)},{"home",canonical(home_)},
            {"lmstudio_root",canonical(studio_)},{"executable",canonical(exe_)},{"routing_sha256",checkpointDigest(active.dump())},
            {"provider",{{"host",config_.host},{"port",config_.port},{"secure",config_.secure},{"model",config_.model?Json(*config_.model):Json(nullptr)}}}};
    }
    Json checkpointStateUnlocked() const {
        return Json{{"phase",static_cast<unsigned>(phase_)},{"predecessor",predecessor_},{"successor",successor_},
            {"created_successor",createdSuccessor_},{"packet_id",packetId_},{"previous_packet",previousPacket_},
            {"packet_request",packetRequest_},{"packet_request_generation",packetRequestGeneration_},
            {"handed_message",handedMessage_},{"completed_generation",completedGeneration_},{"handed",handed_},
            {"last_rejected_packet",lastRejectedPacket_},{"repair_request",repairRequest_},{"repair_generation",repairGeneration_},
            {"last_rejected_sequence",lastRejectedSequence_},{"repair_attempts",packetRepairAttempts_},
            {"repair_acknowledged",repairAcknowledged_},{"previous_repair_user_messages",previousRepairUserMessages_},
            {"context_recovered",contextRecovered_},{"packet_request_acknowledged",packetRequestAcknowledged_},
            {"previous_user_messages",previousUserMessages_},{"previous_native_handoffs",previousNativeHandoffs_},
            {"processed_invalid_requests",processedInvalidRequests_},{"previous_sequence",previousSequence_},
            {"delivery_acknowledged",deliveryAcknowledged_},{"packet_write_sequence",packetWriteSequence_},
            {"effect",effect_},{"operational_error",continuityError_},{"dispatch_error",dispatchError_}};
    }
    void restoreCheckpointUnlocked(const Json& state) {
        const auto expected=checkpointStateUnlocked();
        if(!state.is_object() || state.size()!=expected.size()) throw std::runtime_error{"Visible chat checkpoint fields differ from this source contract."};
        for(const auto& member:expected.items()) if(!state.contains(member.key())) throw std::runtime_error{"Visible chat checkpoint field is missing."};
        const auto text=[&](const char* key,std::string& target) {
            const auto& value=state.at(key);
            if(!value.is_string() || value.get_ref<const std::string&>().size()>2U*1024U*1024U)
                throw std::runtime_error{"Visible chat checkpoint text is invalid or oversized."};
            target=value.get<std::string>();
        };
        if(!state.at("phase").is_number_unsigned() || state.at("phase").get<std::uint64_t>()>static_cast<std::uint64_t>(Phase::Complete))
            throw std::runtime_error{"Visible chat checkpoint phase is invalid."};
        phase_=static_cast<Phase>(state.at("phase").get<unsigned>());
        text("predecessor",predecessor_);text("successor",successor_);text("created_successor",createdSuccessor_);
        text("packet_id",packetId_);text("previous_packet",previousPacket_);text("packet_request",packetRequest_);
        text("packet_request_generation",packetRequestGeneration_);text("handed_message",handedMessage_);
        text("completed_generation",completedGeneration_);text("last_rejected_packet",lastRejectedPacket_);
        text("repair_request",repairRequest_);text("repair_generation",repairGeneration_);text("operational_error",continuityError_);text("dispatch_error",dispatchError_);
        for(const char* key:{"last_rejected_sequence","repair_attempts","previous_repair_user_messages","previous_user_messages","previous_sequence","packet_write_sequence"})
            if(!state.at(key).is_number_unsigned()) throw std::runtime_error{"Visible chat checkpoint numeric boundary is invalid."};
        lastRejectedSequence_=state.at("last_rejected_sequence").get<std::uint64_t>();
        packetRepairAttempts_=state.at("repair_attempts").get<std::uint32_t>();
        if(state.at("repair_attempts").get<std::uint64_t>()>3U) throw std::runtime_error{"Visible chat checkpoint repair limit is invalid."};
        previousRepairUserMessages_=state.at("previous_repair_user_messages").get<std::size_t>();
        previousUserMessages_=state.at("previous_user_messages").get<std::size_t>();
        previousSequence_=state.at("previous_sequence").get<std::uint64_t>();packetWriteSequence_=state.at("packet_write_sequence").get<std::uint64_t>();
        for(const char* key:{"repair_acknowledged","context_recovered","packet_request_acknowledged","delivery_acknowledged"})
            if(!state.at(key).is_boolean()) throw std::runtime_error{"Visible chat checkpoint acknowledgement is invalid."};
        repairAcknowledged_=state.at("repair_acknowledged").get<bool>();contextRecovered_=state.at("context_recovered").get<bool>();
        packetRequestAcknowledged_=state.at("packet_request_acknowledged").get<bool>();deliveryAcknowledged_=state.at("delivery_acknowledged").get<bool>();
        const auto set=[&](const char* key,std::set<std::string>& target) {
            const auto& values=state.at(key);
            if(!values.is_array() || values.size()>16U*1024U) throw std::runtime_error{"Visible chat checkpoint native baseline is invalid."};
            target.clear();for(const auto& value:values) {
                if(!value.is_string() || value.get_ref<const std::string&>().size()>4096U || !target.insert(value.get<std::string>()).second)
                    throw std::runtime_error{"Visible chat checkpoint native baseline entry is invalid."};
            }
        };
        set("previous_native_handoffs",previousNativeHandoffs_);set("processed_invalid_requests",processedInvalidRequests_);
        handed_=state.at("handed");effect_=state.at("effect");
        if(!handed_.is_null() && (!handed_.is_object() || handed_.dump().size()>Domain::LegacyContinuityLimits::MaximumPacketBytes))
            throw std::runtime_error{"Visible chat checkpoint packet body is invalid."};
        if(!effect_.is_null()) {
            if(!effect_.is_object() || effect_.size()!=5U ||
                (effect_.at("kind")!="send" && effect_.at("kind")!="new_chat") ||
                (effect_.at("stage")!="uncertain" && effect_.at("stage")!="confirmed") ||
                (effect_.at("purpose")!="request" && effect_.at("purpose")!="repair" && effect_.at("purpose")!="delivery") ||
                !effect_.at("conversation_id").is_string() || effect_.at("conversation_id").get<std::string>().empty() ||
                !effect_.at("previous_user_messages").is_number_unsigned()) throw std::runtime_error{"Visible chat checkpoint effect receipt is invalid."};
        }
        if(phase_!=Phase::Observe && (predecessor_.empty() || packetRequest_.empty()))
            throw std::runtime_error{"Visible chat checkpoint lost its predecessor request."};
        if((phase_==Phase::Creating || phase_==Phase::Resuming || phase_==Phase::Complete) &&
            (!Domain::LegacyHandoffId::parse(packetId_) || !handed_.is_object() || handedMessage_.empty() || packetWriteSequence_==0U))
            throw std::runtime_error{"Visible chat checkpoint lost its saved packet revision."};
        if((phase_==Phase::Resuming || phase_==Phase::Complete) && (successor_.empty() || successor_!=createdSuccessor_))
            throw std::runtime_error{"Visible chat checkpoint lost its verified successor."};
        retryAfter_={};repairRetryAfter_={};restored_=phase_!=Phase::Observe;
        if(!continuityError_.empty()) {status_["error"]=continuityError_;lastError_=continuityError_;lastErrorFromObservation_=false;}
    }
    void recoveryUnlocked(std::string_view reason) {
        status_["state"]="recovery_pending";
        status_["handoff_recovery"]={{"pending",true},{"phase",phaseName()},{"reason",reason},
            {"effect",effect_},{"packet_id",packetId_.empty()?Json(nullptr):Json(packetId_)},
            {"predecessor_lmstudio_session_id",predecessor_.empty()?Json(nullptr):Json(predecessor_)},
            {"successor_lmstudio_session_id",createdSuccessor_.empty()?Json(nullptr):Json(createdSuccessor_)},
            {"automatic_replay",false},{"reconciliation","Select the recorded chat and restore its exact native message evidence, or recover the retained packet with context_get followed by a successful Forge tool in its successor."}};
    }
    void checkpointFailure(std::string_view message) {
        failure(message);
        std::lock_guard lock{mutex_};checkpointError_=message;recoveryUnlocked(message);
    }
    void checkpointReadRecoveredUnlocked() {
        if(checkpointError_.empty()) return;
        if(continuityError_==checkpointError_) continuityError_.clear();
        if(status_.contains("error") && status_.at("error")==checkpointError_) {
            if(continuityError_.empty()) status_.erase("error");else status_["error"]=continuityError_;
            lastError_=continuityError_;lastErrorFromObservation_=false;
        }
        checkpointError_.clear();
        if(phase_==Phase::Observe && !restored_) {status_.erase("handoff_recovery");status_["state"]="observing";}
    }
    void saveCheckpointUnlocked(const Domain::OperationContext& operation) {
        if(!checkpoint_ || checkpointLoadFailed_) throw std::runtime_error{"Visible chat checkpoint is unavailable; control is deferred."};
        const auto state=checkpointStateUnlocked();
        if(state==lastSavedState_) return;
        for(const auto& field:state.items()) if(field.value().is_string() && field.value().get_ref<const std::string&>().size()>2U*1024U*1024U)
            throw std::runtime_error{"Visible chat checkpoint text exceeds its restorable bound; no control is dispatched."};
        for(const auto* values:{&previousNativeHandoffs_,&processedInvalidRequests_}) {
            if(values->size()>16U*1024U || std::any_of(values->begin(),values->end(),[](const std::string& value) {return value.size()>4096U;}))
                throw std::runtime_error{"Visible chat checkpoint native baseline exceeds its restorable bound; no control is dispatched."};
        }
        if(!handed_.is_null() && (!handed_.is_object() || handed_.dump().size()>Domain::LegacyContinuityLimits::MaximumPacketBytes))
            throw std::runtime_error{"Visible chat checkpoint packet exceeds its restorable bound; no control is dispatched."};
        auto result=checkpoint_->save(state,operation);
        if(!result) throw std::runtime_error{result.error().message};
        lastSavedState_=state;
        if(!checkpointError_.empty()) {
            if(continuityError_==checkpointError_) {continuityError_.clear();lastError_.clear();status_.erase("error");lastErrorFromObservation_=false;}
            checkpointError_.clear();
        }
    }
    bool ensureCheckpoint(const Domain::OperationContext& operation) {
        try {
            std::lock_guard lock{mutex_};
            if(!workspaceConfirmed_) return false;
            if(checkpointLoadFailed_) return false;
            const auto scope=checkpointScope();
            if(checkpoint_ && checkpoint_->scope()!=scope) {
                recoveryUnlocked("The current project, provider or LM Studio route differs from the saved handoff scope.");return false;
            }
            if(!checkpoint_) checkpoint_=std::make_unique<Detail::LMStudioChatCheckpoint>(home_,project_,scope,operation);
            if(!checkpointLoaded_) {
                auto loaded=checkpoint_->load(operation);
                if(!loaded) {checkpointLoadFailed_=true;throw std::runtime_error{loaded.error().message};}
                if(loaded.value()) {
                    const auto previous=checkpointStateUnlocked();
                    try {restoreCheckpointUnlocked(*loaded.value());}
                    catch(...) {restoreCheckpointUnlocked(previous);checkpointLoadFailed_=true;throw;}
                    lastSavedState_=*loaded.value();
                    if(restored_) recoveryUnlocked("The saved handoff awaits fresh selected native chat evidence; no uncertain effect will be replayed.");
                }
                checkpointLoaded_=true;
            }
            checkpointReadRecoveredUnlocked();
            return true;
        } catch(const std::exception& error) {checkpointFailure(error.what());return false;}
    }
    Domain::Result<void> sendControl(std::string_view text,bool newChat,const Domain::OperationContext& operation,
        std::string_view expected,std::string_view purpose,std::function<void(std::string_view)> successorCreated={}) {
        try {
            {std::lock_guard lock{mutex_};saveCheckpointUnlocked(operation);}
            auto result=controls_->sendMessage(exe_,text,newChat,operation,expected,std::move(successorCreated),
                [this,&operation,purpose](const LMStudioChatEffectReceipt& receipt) {
                    try {
                        if(receipt.stage==LMStudioChatEffectStage::BeforeDispatch) {
                            auto configuration=configurationStore_.reload(operation);
                            if(!configuration) return Domain::Result<void>::failure(configuration.error());
                            if(configuration.value().localModel!=config_ || !bound()) return Domain::Result<void>::failure(Domain::makeError(
                                Domain::ErrorCodes::Conflict,"Visible chat authority or provider configuration changed before dispatch."));
                        }
                        std::lock_guard lock{mutex_};
                        if(!workspaceConfirmed_ || pendingWorkspace_ || !checkpoint_ || checkpointLoadFailed_ || checkpoint_->scope()!=checkpointScope())
                            return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::Unauthorized,"Fresh visible chat workspace and route authority cannot be confirmed."));
                        if(receipt.effect==LMStudioChatEffect::NewChat && receipt.stage==LMStudioChatEffectStage::Confirmed) createdSuccessor_=receipt.conversationId;
                        effect_=Json{{"kind",receipt.effect==LMStudioChatEffect::NewChat?"new_chat":"send"},
                            {"stage",receipt.stage==LMStudioChatEffectStage::BeforeDispatch?"uncertain":"confirmed"},
                            {"purpose",purpose},{"conversation_id",receipt.conversationId},{"previous_user_messages",receipt.previousUserMessages}};
                        if(receipt.effect==LMStudioChatEffect::Send) {
                            const bool confirmed=receipt.stage==LMStudioChatEffectStage::Confirmed;
                            if(purpose=="request") {previousUserMessages_=receipt.previousUserMessages;packetRequestAcknowledged_=confirmed;}
                            else if(purpose=="repair") {previousRepairUserMessages_=receipt.previousUserMessages;repairAcknowledged_=confirmed;}
                            else deliveryAcknowledged_=confirmed;
                        }
                        saveCheckpointUnlocked(operation);
                        if(receipt.stage==LMStudioChatEffectStage::BeforeDispatch) recoveryUnlocked("A native control dispatch is pending confirmation; an uncertain effect will not be repeated.");
                        return Domain::Result<void>::success();
                    } catch(const std::exception& error) {
                        checkpointFailure(error.what());
                        return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::IntegrityFailure,error.what()));
                    }
                });
            if(!result) {std::lock_guard lock{mutex_};dispatchError_=result.error().message;}
            return result;
        } catch(const std::exception& error) {
            checkpointFailure(error.what());return Domain::Result<void>::failure(Domain::makeError(Domain::ErrorCodes::IntegrityFailure,error.what()));
        }
    }
    bool reconcileCheckpoint(const LMStudioConversationObservation& chat,const Domain::OperationContext& operation) {
        std::lock_guard lock{mutex_};
        const auto uncertain=!effect_.is_null() && effect_.at("stage")=="uncertain";
        if(!restored_ && !uncertain && !status_.contains("handoff_recovery")) return true;
        const auto present=[&](const std::string& text,std::size_t boundary) {
            return boundary<chat.userMessages.size() && std::find(chat.userMessages.begin()+boundary,chat.userMessages.end(),text)!=chat.userMessages.end();
        };
        if(phase_==Phase::WaitingPacket) {
            if(chat.conversationId!=predecessor_ || !hasForgeIntegrations(chat)) {recoveryUnlocked("Select the recorded predecessor with its Forge integrations to reconcile the saved packet request.");return false;}
            if((packetRequestAcknowledged_ || (!effect_.is_null() && effect_.at("purpose")=="request")) && !present(packetRequest_,previousUserMessages_)) {
                recoveryUnlocked("The saved packet request has no exact new native user-message evidence; Send will not be repeated.");return false;
            }
            if(!repairRequest_.empty() && (repairAcknowledged_ || (!effect_.is_null() && effect_.at("purpose")=="repair")) && !present(repairRequest_,previousRepairUserMessages_)) {
                recoveryUnlocked("The saved repair request has no exact new native user-message evidence; Send will not be repeated.");return false;
            }
            packetRequestAcknowledged_=present(packetRequest_,previousUserMessages_);
            if(!repairRequest_.empty()) repairAcknowledged_=present(repairRequest_,previousRepairUserMessages_);
            if(uncertain) effect_["stage"]="confirmed";
        } else if(phase_==Phase::Creating || phase_==Phase::Resuming || phase_==Phase::Complete) {
            if(phase_==Phase::Complete && chat.conversationId!=successor_) {
                restored_=false;status_.erase("handoff_recovery");status_["state"]="resumed";return true;
            }
            auto packet=continuity_.get({Domain::LegacyHandoffId::parse(packetId_).value(),true},operation);
            if(!packet) throw std::runtime_error{packet.error().message};
            if(!packet.value().record || packet.value().record->writeSequence!=packetWriteSequence_ || !samePacketBody(handed_,packet.value().record->packet)) {
                recoveryUnlocked("The retained handoff packet revision differs from the durable/native packet evidence; automatic control is deferred.");return false;
            }
            bool nativeRecovered=false,following=false;
            for(const auto& result:chat.nativeToolResults) {
                if(!forgeConnector(result.pluginIdentifier)) continue;
                for(const auto& body:result.textBodies) {
                    const auto value=Json::parse(body,nullptr,false);
                    if(!value.is_object() || !value.value("ok",false)) continue;
                    if(result.name=="context_get" && value.value("found",false) && value.value("handoff_id",std::string{})==packetId_) nativeRecovered=true;
                    else if(nativeRecovered && result.name!="session_handoff" && result.name!="session_checkpoint" && result.name!="context_get") following=true;
                }
            }
            std::size_t boundary{};
            if(!effect_.is_null() && effect_.at("kind")=="send") boundary=effect_.at("previous_user_messages").get<std::size_t>();
            const bool message=present(handedMessage_,boundary);
            if(createdSuccessor_.empty()) {
                if(chat.conversationId!=predecessor_ && hasForgeIntegrations(chat) && (message || (nativeRecovered && following))) {
                    createdSuccessor_=chat.conversationId;
                } else if(uncertain) {recoveryUnlocked("New chat was dispatched without a confirmed successor. A different empty or third chat is insufficient evidence; New chat will not be repeated.");return false;}
                else if(chat.conversationId!=predecessor_) {recoveryUnlocked("Select the recorded predecessor before the planned successor creation can proceed.");return false;}
            }
            if(!createdSuccessor_.empty()) {
                if(chat.conversationId!=createdSuccessor_ || !hasForgeIntegrations(chat)) {recoveryUnlocked("Select the exact recorded successor with its Forge integrations; another chat will not be adopted.");return false;}
                if(message || (nativeRecovered && following)) {
                    deliveryAcknowledged_=true;
                    if(!effect_.is_null()) effect_["stage"]="confirmed";
                    if(message && (phase_==Phase::Resuming || phase_==Phase::Complete)) {
                        status_["available"]=true;status_["successor_lmstudio_session_id"]=successor_;status_["packet_id"]=packetId_;
                    }
                    if(nativeRecovered && following && !message) {
                        successor_=createdSuccessor_;phase_=Phase::Resuming;status_["available"]=true;
                        status_["successor_lmstudio_session_id"]=successor_;status_["packet_id"]=packetId_;
                        traceUnlocked({{"event","successor_reconciled_by_native_packet_resume"},{"packet_id",packetId_},{"successor_lmstudio_session_id",successor_}});
                    }
                } else if(uncertain || deliveryAcknowledged_) {recoveryUnlocked("The dispatched handoff has no exact new native user-message or completed packet-recovery evidence; Send will not be repeated.");return false;}
            }
        }
        if(!effect_.is_null() && effect_.at("stage")=="confirmed" && !dispatchError_.empty() &&
            ((phase_==Phase::WaitingPacket && packetRequestAcknowledged_ && (repairRequest_.empty() || repairAcknowledged_)) || deliveryAcknowledged_)) {
            if(continuityError_==dispatchError_) {continuityError_.clear();lastError_.clear();status_.erase("error");lastErrorFromObservation_=false;}
            dispatchError_.clear();
        }
        restored_=false;saveCheckpointUnlocked(operation);status_.erase("handoff_recovery");status_["state"]=phaseName();
        return true;
    }
    struct WorkspaceBinding final { Domain::ProjectId project;Domain::PathText root; };
    bool applyPendingWorkspace(const LMStudioConversationObservation& chat,const Domain::OperationContext& operation) {
        std::optional<WorkspaceBinding> pending;Phase phase;
        {
            std::lock_guard lock{mutex_};pending=pendingWorkspace_;phase=phase_;
            if(!pending) return workspaceConfirmed_;
        }
        if(phase!=Phase::Observe && phase!=Phase::Complete) {
            if(chat.toolsActive) return false;
            auto idle=controls_->idle(exe_,operation);
            if(!idle) throw std::runtime_error{idle.error().message};if(!idle.value()) return false;
        }
        std::lock_guard lock{mutex_};
        if(!pendingWorkspace_ || pendingWorkspace_->project!=pending->project || pendingWorkspace_->root!=pending->root) return false;
        const auto previousProject=project_.value();
        if(checkpoint_ && !checkpointLoadFailed_ && phase_!=Phase::Observe) saveCheckpointUnlocked(operation);
        checkpoint_.reset();checkpointLoaded_=false;checkpointLoadFailed_=false;restored_=false;effect_=Json{};lastSavedState_=Json{};checkpointError_.clear();dispatchError_.clear();packetWriteSequence_=0U;
        project_=pending->project;root_=pending->root;pendingWorkspace_.reset();workspaceConfirmed_=true;
        phase_=Phase::Observe;predecessor_.clear();successor_.clear();createdSuccessor_.clear();packetId_.clear();
        previousPacket_.clear();packetRequest_.clear();packetRequestGeneration_.clear();handedMessage_.clear();completedGeneration_.clear();handed_=Json{};
        lastPressureConversation_.clear();lastPressureGeneration_.clear();lastRejectedPacket_.clear();repairRequest_.clear();repairGeneration_.clear();
        previousNativeHandoffs_.clear();processedInvalidRequests_.clear();previousSequence_=0U;lastRejectedSequence_=0U;packetRepairAttempts_=0U;
        contextRecovered_=false;packetRequestAcknowledged_=false;repairAcknowledged_=false;deliveryAcknowledged_=false;lastError_.clear();continuityError_.clear();lastErrorFromObservation_=false;
        status_=Json{{"available",false},{"enabled",true},{"state","observing"},
            {"project_id",project_.value()},{"project_folder",root_.value()},{"binding_source","authorized_mcp_workspace"}};
        traceUnlocked({{"event","workspace_binding_confirmed"},{"previous_project_id",previousProject},
            {"binding_source","authorized_mcp_workspace"},{"previous_handoff_abandoned",phase!=Phase::Observe && phase!=Phase::Complete}});
        return true;
    }
    Domain::OperationContext context(std::stop_token stop) {
        auto id=uuid_.next(); if(!id) throw std::runtime_error{id.error().message};
        auto correlation=Domain::CorrelationId::parse("lmstudio-visible-continuity");
        return {Domain::OperationId{std::move(id).value()},clock_.monotonicNow()+std::chrono::seconds{20},stop,std::move(correlation).value()};
    }
    bool bound() {
        std::ifstream input{filePath(studio_.value())/"mcp.json"}; if(!input) return false;
        const auto config=Json::parse(input);const auto& servers=config.at("mcpServers");
        for(const char* name:{"forge-conductor","forge-conductor-fallback","forge-conductor-clu"}) {
            if(!servers.contains(name)) return false;
        }
        const auto& server=servers.at("forge-conductor");std::string boundHome;
        if(server.contains("env")) boundHome=server.at("env").value("FORGE_CONDUCTOR_HOME",std::string{});
        if(server.contains("args")) { const auto args=server.at("args").get<std::vector<std::string>>();
            for(std::size_t i=0;i+1<args.size();++i) if(args[i]=="--home") boundHome=args[i+1]; }
        if(boundHome.empty()) return false;
        return std::filesystem::weakly_canonical(filePath(boundHome))==std::filesystem::weakly_canonical(filePath(home_.value()));
    }
    bool enabled(const Domain::OperationContext& operation) {
        const auto provider=std::string{"lmstudio://"}+(config_.secure?"https/":"http/")+config_.host+":"+
            std::to_string(config_.port)+"/"+config_.model.value_or("<automatic>");
        std::optional<std::string> cursor;
        do { auto page=projects_.listRecent({project_,{"automatic_continuity_preference"},std::nullopt,100U,cursor,true,256U*1024U},operation);
            if(!page) throw std::runtime_error{page.error().message};
            for(const auto& hit:page.value().records) if(hit.record.body) {
                const auto value=Json::parse(*hit.record.body);
                if(value.value("provider_id",std::string{})==provider) return value.at("enabled").get<bool>();
            }
            cursor=page.value().nextCursor;
        } while(cursor);
        return true;
    }
    void failure(std::string_view message,const bool fromObservation=false) noexcept {
        try {
        std::lock_guard lock{mutex_};status_["error"]=message;
        lastErrorFromObservation_=fromObservation;
        if(!fromObservation) continuityError_=message;
        if(lastError_!=message) {traceUnlocked({{"event","error"},{"message",message}});lastError_=message;}
        } catch (...) {}
    }
    void observationRecovered(const std::string& conversationId) {
        std::lock_guard lock{mutex_};
        if(!lastErrorFromObservation_) return;
        const auto previousError=lastError_;
        if(continuityError_.empty()) status_.erase("error");
        else status_["error"]=continuityError_;
        lastError_=continuityError_;lastErrorFromObservation_=false;
        traceUnlocked({{"event","conversation_observation_recovered"},
            {"conversation_id",conversationId.empty()?Json(nullptr):Json(conversationId)},{"previous_error",previousError},
            {"other_error_retained",!continuityError_.empty()}});
    }
    void traceUnlocked(Json event) {
        event["project_id"]=project_.value();event["project_folder"]=root_.value();event["serving_pid"]=::GetCurrentProcessId();
        event["time_unix_ms"]=std::chrono::duration_cast<std::chrono::milliseconds>(clock_.utcNow().time_since_epoch()).count();
        const auto dir=filePath(home_.value())/"continuity";std::filesystem::create_directories(dir);
        std::ofstream output{dir/("lmstudio-chat-trace-"+std::to_string(::GetCurrentProcessId())+".jsonl"),std::ios::app};
        if(!output) throw std::runtime_error{"The native continuity trace could not be opened."};
        output<<event.dump()<<'\n';
        if(!output) throw std::runtime_error{"The native continuity trace could not be written."};
    }
    void requestMissingPacket(const LMStudioConversationObservation& chat,const Domain::OperationContext& operation) {
        if(chat.toolsActive || chat.generationEvidence.empty() ||
            chat.generationEvidence==packetRequestGeneration_ || chat.generationEvidence==repairGeneration_) return;
        auto idle=controls_->idle(exe_,operation);
        if(!idle) throw std::runtime_error{idle.error().message};if(!idle.value()) return;
        if(packetRepairAttempts_>=3U) {
            failure("No complete native handoff packet was saved after three corrective requests; rollover is deferred and Forge tools remain callable.");return;
        }
        const auto repair="Auto Continuity: your completed response did not save a new continuity packet. Invoke the actual callable MCP tool session_handoff from forge-conductor NOW. "
            "Use its packet_json STRING argument containing a complete JSON object with goal, detailed narrative and resume_seed (each at least 256 characters), "
            "decisions as a nonempty array of EVERY explicit user constraint and exact release/version/protected-process values, key_files as actual path strings, "
            "and ordered next_actions as a nonempty array. Distinguish source reads from executed commands. Preserve project "+root_.value()+
            ". The tool is present in your MCP tool list. Do not use shell_exec, console commands, printed acknowledgments or placeholders as a substitute. "
            "Do not ask the user to send it. After the actual successful session_handoff call, acknowledge briefly and stop for automatic rollover.";
        repairRequest_=repair;repairAcknowledged_=false;repairGeneration_=chat.generationEvidence;
        previousRepairUserMessages_=chat.userMessages.size();++packetRepairAttempts_;
        repairRetryAfter_=clock_.monotonicNow()+std::chrono::seconds{5};
        {
            std::lock_guard lock{mutex_};status_["state"]="repairing_model_packet";
            traceUnlocked({{"event","model_packet_missing"},{"predecessor_lmstudio_session_id",predecessor_},
                {"attempt",packetRepairAttempts_},{"request_to_model",repair}});
        }
        auto sent=sendControl(repair,false,operation,predecessor_,"repair");
        if(!sent) throw std::runtime_error{sent.error().message};repairAcknowledged_=true;
    }
    void tick(std::stop_token stop) {
        bool isBound{};
        try {isBound=bound();}
        catch(const std::exception& error) {failure(error.what(),true);return;}
        catch(...) {failure("The LM Studio MCP route configuration could not be read.",true);return;}
        if(!isBound) return;
        auto operation=context(stop);
        auto configuration=configurationStore_.reload(operation);
        if(!configuration) {failure(configuration.error().message,true);return;}
        config_=configuration.value().localModel;
        auto observed=WindowsLMStudioConversationReader::read(studio_,operation);
        if(!observed) {failure(observed.error().message,true);return;}
        if(!observed.value()) {
            {
                std::lock_guard lock{mutex_};
                const auto reserved=static_cast<std::uint64_t>(config_.nextResponseReserve)+config_.handoffReserve+config_.estimationSafetyMargin;
                status_["context_telemetry"]=Json{{"available",false},
                    {"source","selected native LM Studio generation provider statistics"},
                    {"measurement_scope","latest_provider_generation"},{"conversation_id",nullptr},{"generation_reference",nullptr},
                    {"tokens_used",nullptr},{"context_capacity",nullptr},{"reason","No native LM Studio chat is selected."},
                    {"sampling_note","Tokens added after the latest observed provider generation are not measured."},
                    {"reserved_tokens",reserved},{"headroom_tokens",nullptr},{"overflow",false},{"tools_active",false},
                    {"observed_at_unix_ms",std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()}};
            }
            bool isEnabled{};
            try {isEnabled=enabled(operation);}
            catch(const std::exception& error) {failure(error.what(),true);return;}
            catch(...) {failure("The automatic continuity preference could not be read.",true);return;}
            observationRecovered({});
            {std::lock_guard lock{mutex_};status_["enabled"]=isEnabled;}
            if(workspaceConfirmed_ && ensureCheckpoint(operation)) {
                std::lock_guard lock{mutex_};
                if(phase_!=Phase::Observe && phase_!=Phase::Complete) recoveryUnlocked("No native chat is selected; saved handoff state is retained and no control is dispatched.");
            }
            return;
        }
        auto chat=*observed.value();
        if(!applyPendingWorkspace(chat,operation)) return;
        {
            std::lock_guard lock{mutex_};
            const bool measured = hasForgeIntegrations(chat) && chat.contextCapacity > 0U && !chat.generationEvidence.empty();
            const auto reserved = static_cast<std::uint64_t>(config_.nextResponseReserve) + config_.handoffReserve + config_.estimationSafetyMargin;
            const auto remaining = chat.usedTokens >= chat.contextCapacity ? 0U : chat.contextCapacity - chat.usedTokens;
            Json reference = nullptr;
            if (measured) {
                const auto evidence = Json::parse(chat.generationEvidence, nullptr, false);
                if (!evidence.is_discarded() && evidence.is_object()) {
                    reference = Json::object();
                    for (const char* field : {"message_index", "selected_version", "step_index"})
                        if (evidence.contains(field)) reference[field] = evidence.at(field);
                }
            }
            status_["context_telemetry"] = Json{{"available", measured},
                {"source", "selected native LM Studio generation provider statistics"},
                {"measurement_scope", "latest_provider_generation"}, {"conversation_id", chat.conversationId}, {"generation_reference", std::move(reference)},
                {"tokens_used", measured ? Json(chat.usedTokens) : Json(nullptr)},
                {"context_capacity", measured ? Json(chat.contextCapacity) : Json(nullptr)},
                {"reason", measured ? Json(nullptr) : Json("No completed provider usage/context observation is available for this selected Forge chat.")},
                {"sampling_note", "Tokens added after the latest observed provider generation are not measured."},
                {"reserved_tokens", reserved}, {"headroom_tokens", measured ? Json(remaining > reserved ? remaining - reserved : 0U) : Json(nullptr)},
                {"overflow", chat.overflow}, {"tools_active", chat.toolsActive},
                {"observed_at_unix_ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()}};
        }
        bool isEnabled{};
        try {isEnabled=enabled(operation);}
        catch(const std::exception& error) {failure(error.what(),true);return;}
        catch(...) {failure("The automatic continuity preference could not be read.",true);return;}
        observationRecovered(chat.conversationId);
        { std::lock_guard lock{mutex_};status_["enabled"]=isEnabled; }
        if(!ensureCheckpoint(operation)) return;
        if(!isEnabled) return;
        if(!reconcileCheckpoint(chat,operation)) return;
        Phase phase;bool delivered; {std::lock_guard lock{mutex_};phase=phase_;delivered=deliveryAcknowledged_;}
        if(phase==Phase::Complete) {
            if(chat.conversationId!=successor_ || chat.generationEvidence!=completedGeneration_) {
                std::lock_guard lock{mutex_};phase_=Phase::Observe;phase=Phase::Observe;
            } else return;
        }
        if(phase==Phase::Observe) {
            if(!hasForgeIntegrations(chat) || chat.contextCapacity==0 || chat.generationEvidence.empty()) return;
            auto budget=Domain::resolveContextBudget({chat.contextCapacity,
                static_cast<std::uint64_t>(config_.nextResponseReserve)+config_.handoffReserve+config_.estimationSafetyMargin,
                std::nullopt,chat.usedTokens,std::nullopt,std::nullopt,chat.overflow});
            if(!budget) throw std::runtime_error{budget.error().message};
            if(budget.value().action!=Domain::ContextBudgetAction::Rollover && budget.value().action!=Domain::ContextBudgetAction::Emergency) return;
            if(lastPressureConversation_!=chat.conversationId || lastPressureGeneration_!=chat.generationEvidence) {
                std::lock_guard lock{mutex_};lastPressureConversation_=chat.conversationId;lastPressureGeneration_=chat.generationEvidence;
                traceUnlocked({{"event","context_pressure_detected"},{"predecessor_lmstudio_session_id",chat.conversationId},
                    {"provider_used",chat.usedTokens},{"capacity",chat.contextCapacity},{"reserved",budget.value().reserved},
                    {"remaining",budget.value().remaining},{"overflow",chat.overflow},{"stop_reason",chat.stopReason},
                    {"tools_active",chat.toolsActive}});
            }
            auto activated=controls_->activate(exe_,operation);
            if(!activated) throw std::runtime_error{activated.error().message};
            if(chat.toolsActive) return;
            auto paused=controls_->pause(exe_,chat.conversationId,operation);
            if(!paused) throw std::runtime_error{paused.error().message};
            if(!paused.value()) return;
            auto atPause=WindowsLMStudioConversationReader::read(studio_,operation);
            if(!atPause) throw std::runtime_error{atPause.error().message};
            if(!atPause.value() || atPause.value()->conversationId!=chat.conversationId ||
                atPause.value()->toolsActive || !hasForgeIntegrations(*atPause.value())) return;
            chat=*atPause.value();
            budget=Domain::resolveContextBudget({chat.contextCapacity,
                static_cast<std::uint64_t>(config_.nextResponseReserve)+config_.handoffReserve+config_.estimationSafetyMargin,
                std::nullopt,chat.usedTokens,std::nullopt,std::nullopt,chat.overflow});
            if(!budget) throw std::runtime_error{budget.error().message};
            if(budget.value().action!=Domain::ContextBudgetAction::Rollover && budget.value().action!=Domain::ContextBudgetAction::Emergency) return;
            auto pointer=memory_.get({"continuity/project/"+project_.value()},operation);
            if(!pointer) throw std::runtime_error{pointer.error().message};
            previousNativeHandoffs_.clear();processedInvalidRequests_.clear();
            for(const auto& result:chat.nativeToolResults)
                if(result.name=="session_handoff") previousNativeHandoffs_.insert(checkpointDigest(result.content));
            previousPacket_=pointer.value().note?pointer.value().note->body:std::string{};
            previousSequence_=0;
            if(!previousPacket_.empty()) {
                auto previousId=Domain::LegacyHandoffId::parse(previousPacket_);
                if(!previousId) throw std::runtime_error{previousId.error().message};
                auto previous=continuity_.get({std::move(previousId).value(),true},operation);
                if(!previous) throw std::runtime_error{previous.error().message};
                if(previous.value().record) previousSequence_=previous.value().record->writeSequence;
            }
            const auto prompt="Auto Continuity: context pressure reached at this completed pause. Provider measured "+
                std::to_string(chat.usedTokens)+" tokens of "+std::to_string(chat.contextCapacity)+
                ", with "+std::to_string(budget.value().reserved)+" tokens reserved. Stop additional task work at this pause. "
                "Invoke the actual MCP tool session_handoff through forge-conductor with packet_json: a JSON STRING containing the complete packet object. This callable tool is in your tool list. A shell command or a printed claim does not save a packet. "
                "Include the current task goal, all constraints, completed changes with exact paths, commands and actual results, "
                "decisions, blockers, key_files, agent session IDs, and ordered next_actions. In decisions, record EVERY explicit user constraint as separate strings: prohibitions, release/version instructions, protected installed processes, approval boundaries and project/package/policy bindings. Preserve exact values supplied by the user. Do not replace them with a generic claim that constraints were preserved. Distinguish code you read from commands actually executed; do not claim reading a function proves it ran. Put detailed state in resume_seed "
                "as well as narrative and collections. Omit handoff_id to create a new packet; never invent an existing packet ID. Inside the packet_json string, include JSON object fields goal, narrative, resume_seed, decisions as a nonempty array of explicit user constraints, key_files as a nonempty array of actual path strings (the real project root is valid when no individual files were read), and next_actions as a nonempty array of concrete ordered action strings. The resume_seed must be detailed task prose, not an opaque identifier. The arrays must be actual JSON fields in that object, not descriptions inside narrative or resume_seed. Do not use summary alone. Serialize the full object into packet_json in the actual tool call. Include the exact user constraints, measured tool counts, actual file reads and findings. Use task-specific prose; never use draft markers, filler, padded strings or a summary-only patch. Write task-specific facts from this chat; state unverified claims as unverified. The narrative and resume_seed each need at least 256 characters. Preserve project folder "+root_.value()+
                ". Preserve all existing user constraints and follow the bound instruction packages and development policies. "
                "Do not use repeated-character padding. Keep the concrete pending next_actions; a generic continue/resume/next entry cannot resume the work. After saving the packet, acknowledge briefly and stop; Forge will open the successor LM Studio chat."+taskInstructions(chat)+nativeCallHistory(chat);
            { std::lock_guard lock{mutex_};predecessor_=chat.conversationId;phase_=Phase::WaitingPacket;
                effect_=Json{};packetId_.clear();createdSuccessor_.clear();successor_.clear();handed_=Json{};handedMessage_.clear();packetWriteSequence_=0U;
                packetRequest_=prompt;packetRequestGeneration_=chat.generationEvidence;previousUserMessages_=chat.userMessages.size();packetRequestAcknowledged_=false;packetRepairAttempts_=0U;repairRequest_.clear();repairAcknowledged_=false;
                lastRejectedPacket_.clear();lastRejectedSequence_=0U;
                retryAfter_=clock_.monotonicNow()+std::chrono::seconds{5};status_["state"]="requesting_model_packet";
                traceUnlocked({{"event","context_pressure_pause"},{"predecessor_lmstudio_session_id",predecessor_},
                    {"provider_used",chat.usedTokens},{"capacity",chat.contextCapacity},{"reserved",budget.value().reserved},
                    {"remaining",budget.value().remaining},{"overflow",chat.overflow},{"stop_reason",chat.stopReason},
                    {"tools_active",false},{"request_to_model",prompt}}); }
            auto sent=sendControl(prompt,false,operation,chat.conversationId,"request");
            if(!sent) throw std::runtime_error{sent.error().message};
            std::lock_guard lock{mutex_};packetRequestAcknowledged_=true;status_["state"]="waiting_for_model_packet";
            traceUnlocked({{"event","model_packet_request_sent"},{"predecessor_lmstudio_session_id",predecessor_}});
            return;
        }
        if(phase==Phase::WaitingPacket) {
            if(chat.conversationId!=predecessor_) return;
            if(!packetRequestAcknowledged_) {
                if(chat.userMessages.size()>previousUserMessages_ &&
                    std::find(chat.userMessages.begin()+previousUserMessages_,chat.userMessages.end(),packetRequest_)!=chat.userMessages.end()) {
                    std::lock_guard lock{mutex_};packetRequestAcknowledged_=true;status_["state"]="waiting_for_model_packet";
                    traceUnlocked({{"event","model_packet_request_confirmed"},{"predecessor_lmstudio_session_id",predecessor_}});
                } else {
                    if(chat.toolsActive || clock_.monotonicNow()<retryAfter_) return;
                    auto idle=controls_->idle(exe_,operation);
                    if(!idle) throw std::runtime_error{idle.error().message};if(!idle.value()) return;
                    retryAfter_=clock_.monotonicNow()+std::chrono::seconds{5};
                    auto sent=sendControl(packetRequest_,false,operation,predecessor_,"request");
                    if(!sent) throw std::runtime_error{sent.error().message};
                    std::lock_guard lock{mutex_};packetRequestAcknowledged_=true;status_["state"]="waiting_for_model_packet";
                    traceUnlocked({{"event","model_packet_request_retry"},{"predecessor_lmstudio_session_id",predecessor_}});
                    return;
                }
            }
            if(!repairRequest_.empty() && !repairAcknowledged_) {
                if(chat.userMessages.size()>previousRepairUserMessages_ &&
                    std::find(chat.userMessages.begin()+previousRepairUserMessages_,chat.userMessages.end(),repairRequest_)!=chat.userMessages.end()) {
                    repairAcknowledged_=true;
                    std::lock_guard lock{mutex_};
                    traceUnlocked({{"event","model_packet_repair_confirmed"},{"packet_id",lastRejectedPacket_},
                        {"predecessor_lmstudio_session_id",predecessor_},{"attempt",packetRepairAttempts_}});
                } else {
                    if(chat.toolsActive || clock_.monotonicNow()<repairRetryAfter_) return;
                    auto idle=controls_->idle(exe_,operation);
                    if(!idle) throw std::runtime_error{idle.error().message};if(!idle.value()) return;
                    repairRetryAfter_=clock_.monotonicNow()+std::chrono::seconds{5};
                    auto sent=sendControl(repairRequest_,false,operation,predecessor_,"repair");
                    if(!sent) throw std::runtime_error{sent.error().message};
                    repairAcknowledged_=true;
                    std::lock_guard lock{mutex_};
                    traceUnlocked({{"event","model_packet_repair_retry"},{"packet_id",lastRejectedPacket_},
                        {"predecessor_lmstudio_session_id",predecessor_},{"attempt",packetRepairAttempts_}});
                    return;
                }
            }
            if(chat.generationEvidence!=packetRequestGeneration_ &&
                chat.generationEvidence!=repairGeneration_ && !chat.toolsActive) {
                std::string rejection,rejectedRequest;
                for(const auto& result:chat.nativeToolResults) {
                    if(result.name!="session_handoff" || !forgeConnector(result.pluginIdentifier) ||
                        previousNativeHandoffs_.contains(checkpointDigest(result.content)) || processedInvalidRequests_.contains(result.requestId)) continue;
                    rejection.clear();
                    for(const auto& body:result.textBodies) {
                        const auto value=Json::parse(body,nullptr,false);
                        if(value.is_object() && !value.value("ok",false) &&
                            value.value("code",std::string{})==Domain::ErrorCodes::InvalidRequest)
                            {rejection=value.value("message",std::string{});rejectedRequest=result.requestId;}
                    }
                }
                if(!rejection.empty()) {
                    auto paused=controls_->pause(exe_,predecessor_,operation);
                    if(!paused) throw std::runtime_error{paused.error().message};if(!paused.value()) return;
                    processedInvalidRequests_.insert(rejectedRequest);
                    if(packetRepairAttempts_>=3U) {
                        failure("The model still has not supplied a complete packet after three corrective requests; automatic rollover is deferred and Forge tools remain callable.");return;
                    }
                    const auto repair="Auto Continuity: YOUR session_handoff call was rejected. You must correct YOUR tool call now; do not ask the user to resend it. "
                        "Use the packet_json argument: serialize a complete JSON object into this STRING argument on session_handoff. Omit handoff_id for a new packet; do not invent an existing ID. Use detailed task prose for resume_seed, never an opaque ID or padded token. The object must keep your full detailed goal, narrative and resume_seed, with decisions as a nonempty array listing EVERY explicit user constraint, exact release/version values, protected installed processes and project/package/policy bindings. Distinguish source reads from commands actually executed. Then include these actual JSON array fields: "
                        "\"key_files\":["+Json(root_.value()).dump()+"] (include the actual task files as additional array items); "
                        "\"next_actions\":[\"Call context_get for this packet\",\"Call agent_list to verify agents remain available\",\"Call get_forge_status, then resume the user's pending task\"]. "
                        "Do not merely describe arrays in narrative, or pass summary alone. No packet has been saved by the rejected call. Make the corrected tool call, acknowledge briefly, then stop for automatic rollover. Exact tool error: "+rejection;
                    repairRequest_=repair;repairAcknowledged_=false;repairGeneration_=chat.generationEvidence;
                    previousRepairUserMessages_=chat.userMessages.size();++packetRepairAttempts_;
                    repairRetryAfter_=clock_.monotonicNow()+std::chrono::seconds{5};
                    {
                        std::lock_guard lock{mutex_};status_["state"]="repairing_model_packet";
                        traceUnlocked({{"event","model_packet_call_rejected"},{"predecessor_lmstudio_session_id",predecessor_},
                            {"attempt",packetRepairAttempts_},{"error",rejection},{"request_to_model",repair}});
                    }
                    auto sent=sendControl(repair,false,operation,predecessor_,"repair");
                    if(!sent) throw std::runtime_error{sent.error().message};
                    repairAcknowledged_=true;return;
                }
            }
            auto pointer=memory_.get({"continuity/project/"+project_.value()},operation);
            if(!pointer) throw std::runtime_error{pointer.error().message};
            if(!pointer.value().note) {requestMissingPacket(chat,operation);return;}
            auto id=Domain::LegacyHandoffId::parse(pointer.value().note->body);if(!id) throw std::runtime_error{id.error().message};
            auto fetched=continuity_.get({std::move(id).value(),true},operation);
            if(!fetched) throw std::runtime_error{fetched.error().message};if(!fetched.value().record) return;
            if(pointer.value().note->body==previousPacket_ && fetched.value().record->writeSequence<=previousSequence_) {
                requestMissingPacket(chat,operation);return;
            }
            const auto& packet=fetched.value().record->packet;
            bool nativePacketCall=false;Json nativePacketBody,nativePacketResult;std::string nativeRequestId,nativePlugin;
            for(const auto& result:chat.nativeToolResults) {
                if(result.name!="session_handoff" || !forgeConnector(result.pluginIdentifier) ||
                    previousNativeHandoffs_.contains(checkpointDigest(result.content))) continue;
                for(const auto& body:result.textBodies) {
                    const auto value=Json::parse(body,nullptr,false);
                    if(value.is_object() && value.value("ok",false) && value.value("handoff_id",std::string{})==packet.id.value() &&
                        value.value("resume_seed",std::string{})==packet.resumeSeed && value.contains("packet") && samePacketBody(value.at("packet"),packet))
                        { nativePacketCall=true;nativePacketBody=value.at("packet");nativePacketResult=value;nativeRequestId=result.requestId;nativePlugin=result.pluginIdentifier; }
                }
            }
            if(!nativePacketCall) return;
            if(!packet.resumeReady || packet.source!=Domain::LegacyHandoffSource::Model || packet.goal.empty() ||
                packet.narrative.size()<256U || packet.resumeSeed.size()<256U || packet.nextActions.empty() || packet.keyFiles.empty() || packet.decisions.empty() ||
                containsFiller(packet.narrative) || containsFiller(packet.resumeSeed) ||
                singleCharacterPadding(packet.narrative) || singleCharacterPadding(packet.resumeSeed) ||
                std::any_of(packet.nextActions.begin(),packet.nextActions.end(),opaqueAction)) {
                const bool sameRevision=lastRejectedPacket_==packet.id.value() &&
                    lastRejectedSequence_==fetched.value().record->writeSequence;
                if(sameRevision && (chat.generationEvidence.empty() || chat.generationEvidence==repairGeneration_)) return;
                if(packetRepairAttempts_>=3U) {
                    failure("The model packet remains incomplete after three repair requests; automatic rollover is deferred and Forge tools remain callable.");return;
                }
                auto idle=controls_->idle(exe_,operation);
                if(!idle) throw std::runtime_error{idle.error().message};if(chat.toolsActive || !idle.value()) return;
                const auto repair="Auto Continuity cannot use this incomplete packet. Call session_handoff with handoff_id=\""+
                    packet.id.value()+"\" and packet_json: a STRING containing the complete JSON object with ALL these fields: goal (non-empty task goal), narrative (full detailed string, at least 256 characters), "
                    "resume_seed (full detailed string, at least 256 characters), key_files (non-empty array of actual path strings), "
                    "next_actions (non-empty array of ordered action strings), decisions (non-empty array listing EVERY explicit user constraint with exact release/version values and protected processes). Do not send a summary-only or status-only patch. "
                    "Put real arrays in the JSON object serialized into packet_json; do not put them only in narrative or resume_seed. Preserve every user constraint, exact tool results/counts, "
                    "actual source-read paths and findings, decisions and blockers. No filler or placeholders; use task-specific facts from this chat and mark unverified claims. Current sizes: narrative="+
                    std::to_string(packet.narrative.size())+", resume_seed="+std::to_string(packet.resumeSeed.size())+
                    ", decisions="+std::to_string(packet.decisions.size())+", key_files="+std::to_string(packet.keyFiles.size())+", next_actions="+std::to_string(packet.nextActions.size())+
                    ". Expand the detailed state below into the actual packet_json object, including goal, narrative, resume_seed, decisions, key_files and next_actions fields; after saving, acknowledge briefly and stop.\n"+
                    nativePacketBody.dump(2);
                lastRejectedPacket_=packet.id.value();lastRejectedSequence_=fetched.value().record->writeSequence;
                repairRequest_=repair;repairAcknowledged_=false;repairGeneration_=chat.generationEvidence;
                previousRepairUserMessages_=chat.userMessages.size();++packetRepairAttempts_;
                repairRetryAfter_=clock_.monotonicNow()+std::chrono::seconds{5};
                {
                    std::lock_guard lock{mutex_};status_["state"]="repairing_model_packet";
                    traceUnlocked({{"event","model_packet_repair_requested"},{"packet_id",packet.id.value()},
                        {"predecessor_lmstudio_session_id",predecessor_},{"attempt",packetRepairAttempts_},{"request_to_model",repair}});
                }
                auto sent=sendControl(repair,false,operation,predecessor_,"repair");
                if(!sent) throw std::runtime_error{sent.error().message};
                repairAcknowledged_=true;return;
            }
            auto idle=controls_->idle(exe_,operation);
            if(!idle) throw std::runtime_error{idle.error().message};if(chat.toolsActive || !idle.value()) return;
            { std::lock_guard lock{mutex_}; packetId_=packet.id.value();handed_=nativePacketBody;
                packetWriteSequence_=fetched.value().record->writeSequence;
                traceUnlocked({{"event","model_packet_saved"},{"packet_id",packetId_},{"predecessor_lmstudio_session_id",predecessor_},
                    {"native_request_id",nativeRequestId},{"plugin",nativePlugin},{"result",nativePacketResult}}); }
            const auto prompt="Resume this Forge project from its model-written continuity packet. Call context_get through "
                "forge-conductor with handoff_id=\""+packetId_+"\", reattach any open agent sessions with agent_run_status, "
                "call get_forge_status to confirm the bound project, tools and agents are callable, then resume the packet's concrete next_actions with Forge tools. All three existing integrations remain enabled. "
                "Preserve all existing user constraints and the package/policy order.\nContinuity packet:\n"+handed_.dump(2);
            { std::lock_guard lock{mutex_};phase_=Phase::Creating;deliveryAcknowledged_=false;createdSuccessor_.clear();handedMessage_=prompt;effect_=Json{};status_["state"]="successor_creating"; }
            auto sent=sendControl(prompt,true,operation,predecessor_,"delivery",
                [this](std::string_view id) {
                    std::lock_guard lock{mutex_};createdSuccessor_=id;
                    traceUnlocked({{"event","successor_created"},{"packet_id",packetId_},
                        {"predecessor_lmstudio_session_id",predecessor_},{"successor_lmstudio_session_id",createdSuccessor_}});
                });
            { std::lock_guard lock{mutex_};retryAfter_=clock_.monotonicNow()+std::chrono::seconds{5}; }
            if(!sent) throw std::runtime_error{sent.error().message};
            std::lock_guard lock{mutex_};deliveryAcknowledged_=true;
            traceUnlocked({{"event","packet_handed_to_new_chat"},{"packet_id",packetId_},
                {"predecessor_lmstudio_session_id",predecessor_},{"successor_lmstudio_session_id",createdSuccessor_},
                {"packet",handed_},{"handed_message",prompt}});
            return;
        }
        if(phase==Phase::Creating) {
            if(!createdSuccessor_.empty() && chat.conversationId==createdSuccessor_ &&
                std::find(chat.userMessages.begin(),chat.userMessages.end(),handedMessage_)!=chat.userMessages.end() &&
                hasForgeIntegrations(chat)) {
                std::lock_guard lock{mutex_};successorUnlocked(chat,operation);phase=Phase::Resuming;
            } else if(!delivered && clock_.monotonicNow()>=retryAfter_) {
                if(createdSuccessor_.empty()) {
                    if(chat.conversationId==predecessor_) {
                        std::lock_guard lock{mutex_};phase_=Phase::WaitingPacket;status_["state"]="retrying_successor_creation";
                    }
                    return;
                }
                if(chat.conversationId!=createdSuccessor_ || chat.toolsActive) return;
                auto idle=controls_->idle(exe_,operation);
                if(!idle) throw std::runtime_error{idle.error().message};if(!idle.value()) return;
                retryAfter_=clock_.monotonicNow()+std::chrono::seconds{5};
                auto sent=sendControl(handedMessage_,false,operation,createdSuccessor_,"delivery");
                if(!sent) throw std::runtime_error{sent.error().message};
                std::lock_guard lock{mutex_};deliveryAcknowledged_=true;
                traceUnlocked({{"event","packet_delivery_retry"},{"packet_id",packetId_},
                    {"successor_lmstudio_session_id",createdSuccessor_},{"packet",handed_},{"handed_message",handedMessage_}});
                return;
            }
        }
        if(phase==Phase::Resuming && chat.conversationId==successor_) {
            bool recovered=false;
            for(const auto& result:chat.nativeToolResults) {
                if(!forgeConnector(result.pluginIdentifier) ||
                    result.name=="session_handoff" || result.name=="session_checkpoint") continue;
                for(const auto& body:result.textBodies) {
                    const auto value=Json::parse(body,nullptr,false);
                    if(!value.is_object() || !value.value("ok",false)) continue;
                    if(result.name=="context_get") {
                        if(value.value("found",false) && value.value("handoff_id",std::string{})==packetId_) {
                            recovered=true;
                            if(!contextRecovered_) {
                                std::lock_guard lock{mutex_};contextRecovered_=true;
                                traceUnlocked({{"event","successor_packet_read"},{"packet_id",packetId_},
                                    {"successor_lmstudio_session_id",successor_},{"native_request_id",result.requestId},
                                    {"plugin",result.pluginIdentifier},{"result",value}});
                            }
                        }
                        continue;
                    }
                    if(!recovered) continue;
                    const auto budget=Domain::resolveContextBudget({chat.contextCapacity,
                        static_cast<std::uint64_t>(config_.nextResponseReserve)+config_.handoffReserve+config_.estimationSafetyMargin,
                        std::nullopt,chat.usedTokens,std::nullopt,std::nullopt,chat.overflow});
                    std::lock_guard lock{mutex_};
                    traceUnlocked({{"event","following_forge_tool"},{"tool",result.name},{"ok",true},
                        {"successor_lmstudio_session_id",successor_},{"packet_id",packetId_},
                        {"native_request_id",result.requestId},{"plugin",result.pluginIdentifier},{"result",value},
                        {"provider_used",chat.usedTokens},{"capacity",chat.contextCapacity},
                        {"context_budget_cleared",!chat.generationEvidence.empty() && budget && budget.value().action!=Domain::ContextBudgetAction::Rollover &&
                            budget.value().action!=Domain::ContextBudgetAction::Emergency}});
                    completedGeneration_=chat.generationEvidence;phase_=Phase::Complete;saveCheckpointUnlocked(operation);status_["state"]="resumed";return;
                }
            }
        }
    }
    void successorUnlocked(const LMStudioConversationObservation& chat,const Domain::OperationContext& operation) {
        successor_=chat.conversationId;contextRecovered_=false;phase_=Phase::Resuming;status_.erase("error");lastError_.clear();continuityError_.clear();lastErrorFromObservation_=false;
        saveCheckpointUnlocked(operation);status_["state"]="resuming";
        status_["available"]=true;status_["successor_lmstudio_session_id"]=successor_;status_["packet_id"]=packetId_;
        traceUnlocked({{"event","successor_lmstudio_session"},{"successor_lmstudio_session_id",successor_},
            {"predecessor_lmstudio_session_id",predecessor_},{"packet_id",packetId_},{"plugins",chat.plugins},
            {"visible_chat_handoff_available",true},{"handed_message_verified",true}});
    }
    static bool forgeConnector(std::string_view plugin) {
        return plugin=="mcp/forge-conductor" || plugin=="mcp/forge-conductor-fallback" || plugin=="mcp/forge-conductor-clu";
    }
    static bool hasForgeIntegrations(const LMStudioConversationObservation& chat) {
        for(const char* plugin:{"mcp/forge-conductor","mcp/forge-conductor-fallback","mcp/forge-conductor-clu"})
            if(std::find(chat.plugins.begin(),chat.plugins.end(),plugin)==chat.plugins.end()) return false;
        return true;
    }

    Domain::ProjectId project_;Domain::PathText root_,home_,studio_,exe_;Domain::LocalModelConfig config_;
    Contracts::ILegacyMemoryService& memory_;Contracts::ILegacyContextContinuityService& continuity_;
    Contracts::IProjectMemoryService& projects_;Contracts::IClock& clock_;Contracts::IUuidGenerator& uuid_;
    Contracts::IConfigurationStore& configurationStore_;
    std::shared_ptr<Detail::LMStudioChatControlActions> controls_;
    bool workspaceConfirmed_{};std::optional<WorkspaceBinding> pendingWorkspace_;
    mutable std::mutex mutex_;std::jthread worker_;Phase phase_{Phase::Observe};
    Json status_{{"available",false},{"enabled",true},{"state","observing"}};
    std::string predecessor_,successor_,createdSuccessor_,packetId_,previousPacket_,lastError_,continuityError_,packetRequest_,packetRequestGeneration_,handedMessage_,completedGeneration_;Json handed_;
    bool lastErrorFromObservation_{};
    std::string lastPressureConversation_,lastPressureGeneration_;
    std::string lastRejectedPacket_,repairRequest_,repairGeneration_;
    std::uint64_t lastRejectedSequence_{};std::uint32_t packetRepairAttempts_{};
    Domain::MonotonicTimePoint repairRetryAfter_{};bool repairAcknowledged_{};std::size_t previousRepairUserMessages_{};
    Domain::MonotonicTimePoint retryAfter_{};bool contextRecovered_{},packetRequestAcknowledged_{};std::size_t previousUserMessages_{};
    std::set<std::string> previousNativeHandoffs_,processedInvalidRequests_;
    std::uint64_t previousSequence_{};bool deliveryAcknowledged_{};
    std::unique_ptr<Detail::LMStudioChatCheckpoint> checkpoint_;
    bool checkpointLoaded_{},checkpointLoadFailed_{},restored_{};
    Json effect_,lastSavedState_;
    std::string checkpointError_,dispatchError_;
    std::uint64_t packetWriteSequence_{};
};
WindowsLMStudioChatContinuity::WindowsLMStudioChatContinuity(Domain::ProjectId project,Domain::PathText root,
    Domain::PathText home,Domain::PathText studio,Domain::PathText exe,Domain::LocalModelConfig config,
    Contracts::ILegacyMemoryService& memory,Contracts::ILegacyContextContinuityService& continuity,
    Contracts::IProjectMemoryService& projects,Contracts::IClock& clock,Contracts::IUuidGenerator& uuid,Contracts::IConfigurationStore& configurationStore,
    bool initialWorkspaceConfirmed)
    :WindowsLMStudioChatContinuity{std::move(project),std::move(root),std::move(home),std::move(studio),std::move(exe),
        std::move(config),memory,continuity,projects,clock,uuid,configurationStore,initialWorkspaceConfirmed,{}} {}
WindowsLMStudioChatContinuity::WindowsLMStudioChatContinuity(Domain::ProjectId project,Domain::PathText root,
    Domain::PathText home,Domain::PathText studio,Domain::PathText exe,Domain::LocalModelConfig config,
    Contracts::ILegacyMemoryService& memory,Contracts::ILegacyContextContinuityService& continuity,
    Contracts::IProjectMemoryService& projects,Contracts::IClock& clock,Contracts::IUuidGenerator& uuid,Contracts::IConfigurationStore& configurationStore,
    bool initialWorkspaceConfirmed,std::shared_ptr<Detail::LMStudioChatControlActions> controls)
    :impl_{std::make_unique<Impl>(std::move(project),std::move(root),std::move(home),std::move(studio),std::move(exe),
        std::move(config),memory,continuity,projects,clock,uuid,configurationStore,initialWorkspaceConfirmed,std::move(controls))} {}
WindowsLMStudioChatContinuity::~WindowsLMStudioChatContinuity(){shutdown();}
void WindowsLMStudioChatContinuity::start(){impl_->start();}
void WindowsLMStudioChatContinuity::shutdown() noexcept {impl_->shutdown();}
std::string WindowsLMStudioChatContinuity::status() const{return impl_->status();}
void WindowsLMStudioChatContinuity::recordTool(std::string_view name,bool succeeded,std::string_view payload){impl_->recordTool(name,succeeded,payload);}
void WindowsLMStudioChatContinuity::bindWorkspace(const Domain::ProjectId& project,const Domain::PathText& root) noexcept {impl_->bindWorkspace(project,root);}
}
