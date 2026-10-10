#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "ForgeConductor/NativeTools/Windows/WindowsComfyUiBackend.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ComfyUiNativeSupport.h"
#include "ComfyUiPackageContracts.h"
#include "ProviderOperationLease.h"
#include "NativeFileOperations.h"
#include "ImageProviderCodec.h"
#include <Windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

namespace ForgeConductor::NativeTools::Windows {
namespace {
using namespace ComfyDetail;
namespace Fs = std::filesystem;
namespace Native = Infrastructure::Windows::Detail;
constexpr std::uint64_t MaximumArtifactBytes = 64ULL * 1024ULL * 1024ULL * 1024ULL;
std::string digest(std::string_view value) {
    Infrastructure::Windows::BCryptSha256Hasher hasher;
    return take(hasher.sha256(std::as_bytes(std::span{value.data(),value.size()}))).value();
}
bool safeName(std::string_view value) {
    return !value.empty() && value.size()<=240U && value!="." && value!=".." &&
        value.find_first_of("/\\:\r\n\t\"<>|?*")==std::string_view::npos && value.back()!='.' && value.back()!=' ';
}
bool listenerOwned(const Domain::ComfyUiConfig& config,DWORD pid){
    const auto port=static_cast<unsigned long>(std::stoul(config.endpoint.substr(config.endpoint.rfind(':')+1U)));
    const bool ipv6=config.endpoint.find("[::1]")!=std::string::npos;DWORD bytes{};
    const auto family=ipv6?AF_INET6:AF_INET;const auto first=GetExtendedTcpTable(nullptr,&bytes,FALSE,family,TCP_TABLE_OWNER_PID_LISTENER,0);
    if(first!=ERROR_INSUFFICIENT_BUFFER)return false;std::vector<std::byte> table(bytes);if(GetExtendedTcpTable(table.data(),&bytes,FALSE,family,TCP_TABLE_OWNER_PID_LISTENER,0)!=NO_ERROR)return false;
    if(ipv6){const auto rows=reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(table.data());for(DWORD index=0;index<rows->dwNumEntries;++index){const auto& row=rows->table[index];if(row.dwOwningPid==pid&&ntohs(static_cast<u_short>(row.dwLocalPort))==port)return true;}}
    else{const auto rows=reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(table.data());for(DWORD index=0;index<rows->dwNumEntries;++index){const auto& row=rows->table[index];if(row.dwOwningPid==pid&&ntohs(static_cast<u_short>(row.dwLocalPort))==port)return true;}}
    return false;
}
Fs::path installedHome(const Domain::ComfyUiConfig& config) {
    std::vector<Fs::path> candidates;
    if(!config.installationPath.empty())candidates.emplace_back(wide(config.installationPath));
    else {
        const DWORD drives=GetLogicalDrives();
        for(unsigned index=0;index<26U;++index)if(drives&(1U<<index)){
            const std::wstring volume{static_cast<wchar_t>(L'A'+index),L':',L'\\'};
            if(GetDriveTypeW(volume.c_str())!=DRIVE_FIXED&&GetDriveTypeW(volume.c_str())!=DRIVE_REMOVABLE)continue;
            candidates.emplace_back(Fs::path{volume}/L"ComfyUI"/L"ComfyUI_windows_portable");
            candidates.emplace_back(Fs::path{volume}/L"ComfyUI_windows_portable");candidates.emplace_back(Fs::path{volume}/L"ComfyUI");
        }
        std::array<wchar_t,32768> user{};const auto count=GetEnvironmentVariableW(L"USERPROFILE",user.data(),static_cast<DWORD>(user.size()));
        if(count&&count<user.size())for(const auto folder:{L"",L"Desktop",L"Documents"})candidates.emplace_back(Fs::path{user.data()}/folder/L"ComfyUI");
    }
    for(const auto& root:candidates){
        if(Fs::is_regular_file(root/L"ComfyUI"/L"main.py")){regularParents(root);return Fs::absolute(root/L"ComfyUI");}
        if(Fs::is_regular_file(root/L"main.py")){regularParents(root);return Fs::absolute(root);}
    }
    fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Configured ComfyUI installation has no main.py.");
}
Fs::path executableOnPath(std::wstring_view name) {
    std::array<wchar_t,32768> path{}; const auto count=SearchPathW(nullptr,std::wstring{name}.c_str(),nullptr,static_cast<DWORD>(path.size()),path.data(),nullptr);
    return count&&count<path.size()?Fs::path{path.data()}:Fs::path{};
}
Fs::path edgeExecutable() {
    for(const auto variable:{L"ProgramFiles(x86)",L"ProgramFiles",L"LOCALAPPDATA"}){
        std::array<wchar_t,32768> root{};const auto count=GetEnvironmentVariableW(variable,root.data(),static_cast<DWORD>(root.size()));
        if(count&&count<root.size()){const auto candidate=Fs::path{root.data()}/L"Microsoft"/L"Edge"/L"Application"/L"msedge.exe";if(Fs::is_regular_file(candidate))return candidate;}}
    fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Microsoft Edge is unavailable for the private ComfyUI workflow session.");
}
std::string endpointPort(std::string_view endpoint) {
    const auto colon=endpoint.rfind(':');if(colon==std::string_view::npos)fail(Domain::ErrorCodes::InvalidRequest,"ComfyUI endpoint needs a port.");return std::string{endpoint.substr(colon+1)};
}
Fs::path shippedResources(){std::array<wchar_t,32768> value{};const auto count=GetModuleFileNameW(nullptr,value.data(),static_cast<DWORD>(value.size()));return count&&count<value.size()?Fs::path{value.data()}.parent_path()/L"Resources"/L"ComfyUI":Fs::path{};}
std::string pathEnvironment(std::string_view name){
    const auto key=wide(name);SetLastError(ERROR_SUCCESS);const auto count=GetEnvironmentVariableW(key.c_str(),nullptr,0U);
    if(!count){if(GetLastError()==ERROR_ENVVAR_NOT_FOUND)return {};return {};}
    if(count>32768U)fail(Domain::ErrorCodes::PayloadTooLarge,"Provider path environment value exceeds the supported path bound.");
    std::vector<wchar_t> value(count);const auto written=GetEnvironmentVariableW(key.c_str(),value.data(),count);
    if(written>=count)fail(Domain::ErrorCodes::Conflict,"Provider path environment changed during configuration projection.");return utf8(std::wstring_view{value.data(),written});
}
std::string expandProviderBasePath(std::string value){
    // ntpath.expanduser/expandvars are applied only to base_path by the installed loader.
    if(!value.empty()&&value.front()=='~'){
        auto end=value.find_first_of("/\\");if(end==std::string::npos)end=value.size();auto userHome=pathEnvironment("USERPROFILE");
        if(userHome.empty()){const auto homePath=pathEnvironment("HOMEPATH");if(!homePath.empty())userHome=pathEnvironment("HOMEDRIVE")+homePath;}
        if(!userHome.empty()){
            if(end>1U){const auto user=value.substr(1U,end-1U),current=pathEnvironment("USERNAME");const Fs::path home{wide(userHome)};
                if(user!=current&&pathText(home.filename())==current)userHome=pathText(home.parent_path()/wide(user));else if(user!=current)userHome.clear();}
            if(!userHome.empty())value=userHome+value.substr(end);
        }
    }
    std::string expanded;for(std::size_t index=0U;index<value.size();){const auto character=value[index];
        if(character=='\''){auto end=value.find('\'',index+1U);if(end==std::string::npos)end=value.size()-1U;expanded+=value.substr(index,end-index+1U);index=end+1U;continue;}
        if(character!='%'&&character!='$'){expanded+=character;++index;continue;}
        if(index+1U<value.size()&&value[index+1U]==character){expanded+=character;index+=2U;continue;}
        std::size_t start=index+1U,end=start;bool closed=false;
        if(character=='%'||(start<value.size()&&value[start]=='{')){const auto terminator=character=='%'?'%':'}';if(character=='$')++start;end=value.find(terminator,start);closed=end!=std::string::npos;if(!closed){expanded+=value.substr(index);break;}}
        else {while(end<value.size()&&((value[end]>='a'&&value[end]<='z')||(value[end]>='A'&&value[end]<='Z')||(value[end]>='0'&&value[end]<='9')||value[end]=='_'||value[end]=='-'))++end;if(end==start){expanded+=character;++index;continue;}}
        const auto name=value.substr(start,end-start),substitution=pathEnvironment(name);const auto consumed=end+(closed?1U:0U);
        if(substitution.empty()){const auto key=wide(name);SetLastError(ERROR_SUCCESS);if(GetEnvironmentVariableW(key.c_str(),nullptr,0U)==0U&&GetLastError()==ERROR_ENVVAR_NOT_FOUND)expanded+=value.substr(index,consumed-index);}
        else expanded+=substitution;index=consumed;
    }return expanded;
}
Json modelCategoryPaths(const Domain::ComfyUiConfig& config,const Fs::path& home){
    Json categories=Json::object(),aliases=Json::object();std::vector<std::pair<std::string,std::string>> stock;
    const auto normalize=[](Fs::path path){path=path.lexically_normal();path.make_preferred();while(path!=path.root_path()&&path.filename().empty())path=path.parent_path();if(!path.is_absolute()||path.has_root_name()&&!path.has_root_directory())fail(Domain::ErrorCodes::InvalidRequest,"Provider model category path is not absolute.");return path;};
    const auto add=[&](std::string category,Fs::path path,bool isDefault=false){if(aliases.contains(category))category=aliases.at(category).get<std::string>();path=normalize(std::move(path));if(!categories.contains(category))categories[category]=Json::array();auto& paths=categories[category];const auto found=std::find_if(paths.begin(),paths.end(),[&](const Json& item){return !_wcsicmp(wide(item.get<std::string>()).c_str(),path.c_str());});
        if(found!=paths.end()){if(!isDefault)return;paths.erase(found);}if(isDefault)paths.insert(paths.begin(),pathText(path));else paths.push_back(pathText(path));};
    const auto folders=home/L"folder_paths.py";if(Fs::is_regular_file(folders)){regularParents(folders);std::ifstream source{folders,std::ios::binary};std::string value(65536U,'\0');source.read(value.data(),static_cast<std::streamsize>(value.size()));value.resize(static_cast<std::size_t>(source.gcount()));
        const std::regex declarations{R"FORGEMODEL(folder_names_and_paths\["([^"]+)"\]\s*=\s*\(\[([^\]]*)\])FORGEMODEL"},paths{R"FORGEMODEL(os\.path\.join\(models_dir,\s*"([^"]+)"\))FORGEMODEL"};
        const auto legacy=value.find("legacy = {");if(legacy!=std::string::npos){const auto first=value.find('{',legacy),last=value.find('}',first);if(last==std::string::npos)fail(Domain::ErrorCodes::MalformedMessage,"Installed model-category aliases are incomplete.");aliases=parse(value.substr(first,last-first+1U));if(!aliases.is_object())fail(Domain::ErrorCodes::MalformedMessage,"Installed model-category aliases must be a literal object.");for(const auto& alias:aliases)if(!alias.is_string())fail(Domain::ErrorCodes::MalformedMessage,"Installed model-category alias is not a literal string.");}
        for(auto declaration=std::sregex_iterator(value.begin(),value.end(),declarations);declaration!=std::sregex_iterator{};++declaration){const auto category=(*declaration)[1].str(),body=(*declaration)[2].str();for(auto entry=std::sregex_iterator(body.begin(),body.end(),paths);entry!=std::sregex_iterator{};++entry){const auto folder=(*entry)[1].str();add(category,home/L"models"/wide(folder));stock.emplace_back(category,folder);}}
    }
    add("custom_nodes",home/L"custom_nodes");
    const auto extra=home/L"extra_model_paths.yaml";if(Fs::is_regular_file(extra)){
        regularParents(extra);std::ifstream input{extra,std::ios::binary};std::string line,block;std::size_t bytes{},childIndent{},blockIndent{},mappingIndent{};std::vector<std::pair<std::string,std::string>> group;std::set<std::string> groups;
        const std::regex nonStringScalar{R"FORGEYAML((true|false|yes|no|on|off|null|~|[-+]?[0-9][0-9_]*|[-+]?0[xX][0-9a-fA-F_]+|[-+]?[0-9][0-9_]*\.[0-9_eE+.-]*|[0-9]{4}-[0-9]{2}-[0-9]{2}))FORGEYAML",std::regex::icase};
        const auto trim=[](std::string value){const auto first=value.find_first_not_of(" \t\r"),last=value.find_last_not_of(" \t\r");return first==std::string::npos?std::string{}:value.substr(first,last-first+1U);};
        const auto literal=[&](std::string value){value=trim(std::move(value));if(value.empty())return value;
            if(value.front()=='"'){auto end=1U;for(;end<value.size();++end)if(value[end]=='"'){std::size_t slashes{};for(auto index=end;index>0U&&value[index-1U]=='\\';--index)++slashes;if(slashes%2U==0U)break;}if(end>=value.size())fail(Domain::ErrorCodes::InvalidRequest,"Provider double-quoted path is incomplete.");const auto suffix=trim(value.substr(end+1U));if(!suffix.empty()&&suffix.front()!='#')fail(Domain::ErrorCodes::InvalidRequest,"Provider quoted path has unsupported trailing content.");return parse(value.substr(0U,end+1U)).get<std::string>();}
            if(value.front()=='\''){std::string result;std::size_t index=1U;for(;index<value.size();++index){if(value[index]!='\''){result+=value[index];continue;}if(index+1U<value.size()&&value[index+1U]=='\''){result+='\'';++index;continue;}break;}if(index>=value.size())fail(Domain::ErrorCodes::InvalidRequest,"Provider single-quoted path is incomplete.");const auto suffix=trim(value.substr(index+1U));if(!suffix.empty()&&suffix.front()!='#')fail(Domain::ErrorCodes::InvalidRequest,"Provider quoted path has unsupported trailing content.");return result;}
            if(value.front()=='#')return std::string{};const auto comment=value.find(" #");if(comment!=std::string::npos)value=trim(value.substr(0U,comment));if(value.front()=='['||value.front()=='{'||value.front()=='&'||value.front()=='*'||value.front()=='!'||value.front()=='>'||value.find(": ")!=std::string::npos)fail(Domain::ErrorCodes::InvalidRequest,"Provider path configuration requires supported scalar block mappings.");return value;};
        const auto publish=[&]{if(group.empty())return;auto base=home;bool isDefault=false;for(const auto& [key,value]:group){if(key=="base_path"){base=Fs::path{wide(expandProviderBasePath(value))};if(!base.is_absolute())base=home/base;}if(key=="is_default"){auto flag=value;std::transform(flag.begin(),flag.end(),flag.begin(),[](unsigned char character){return static_cast<char>(std::tolower(character));});if(flag=="true"||flag=="yes"||flag=="on")isDefault=true;else if(flag!="false"&&flag!="no"&&flag!="off")fail(Domain::ErrorCodes::InvalidRequest,"Provider is_default must be a supported boolean scalar.");}}
            for(const auto& [category,value]:group)if(category!="base_path"&&category!="is_default"){std::istringstream paths{value};std::string path;while(std::getline(paths,path)){if(path.empty())continue;auto location=Fs::path{wide(path)};add(category,location.is_absolute()?location:base/location,isDefault);}}group.clear();mappingIndent=0U;};
        while(std::getline(input,line)){bytes+=line.size()+1U;if(bytes>65536U)fail(Domain::ErrorCodes::PayloadTooLarge,"Provider path configuration exceeds its bounded inventory.");if(!line.empty()&&line.back()=='\r')line.pop_back();const auto first=line.find_first_not_of(' ');if(first==std::string::npos){if(!block.empty())group.back().second+='\n';continue;}if(line.substr(0U,first).find('\t')!=std::string::npos||line[first]=='\t')fail(Domain::ErrorCodes::InvalidRequest,"Provider path mapping uses unsupported tab indentation.");
            if(!block.empty()&&first>childIndent){if(!blockIndent)blockIndent=first;if(first<blockIndent)fail(Domain::ErrorCodes::InvalidRequest,"Provider literal block indentation changed.");group.back().second+=line.substr(blockIndent)+"\n";continue;}block.clear();blockIndent=0U;if(line[first]=='#')continue;
            const auto colon=line.find(':',first);if(colon==std::string::npos)fail(Domain::ErrorCodes::InvalidRequest,"Provider path configuration contains an unsupported line.");const auto key=literal(line.substr(first,colon-first)),value=literal(line.substr(colon+1U));if(first==0U){publish();if(!groups.insert(key).second)fail(Domain::ErrorCodes::InvalidRequest,"Provider path configuration has duplicate groups.");if(!value.empty()&&value!="null"&&value!="~")fail(Domain::ErrorCodes::InvalidRequest,"Provider path groups require literal block mappings.");continue;}if(!mappingIndent)mappingIndent=first;if(first!=mappingIndent)fail(Domain::ErrorCodes::InvalidRequest,"Provider path group contains an unsupported nested mapping.");if(key.empty()||std::any_of(group.begin(),group.end(),[&](const auto& entry){return entry.first==key;}))fail(Domain::ErrorCodes::InvalidRequest,"Provider path group has an empty or duplicate category.");
            const auto raw=trim(line.substr(colon+1U));const bool quoted=!raw.empty()&&(raw.front()=='\''||raw.front()=='"');if(key=="is_default"&&quoted)fail(Domain::ErrorCodes::InvalidRequest,"Provider is_default must be an unquoted boolean scalar.");
            if(value=="|"||value=="|-"||value=="|+"){block=key;childIndent=first;group.emplace_back(key,"");}else {if(value.empty()||(key!="is_default"&&!quoted&&std::regex_match(value,nonStringScalar)))fail(Domain::ErrorCodes::InvalidRequest,"Provider category path must be a scalar string.");group.emplace_back(key,value);}
        }publish();
    }
    // main.py loads the operator YAML before the explicit Forge model configuration.
    if(!config.modelStoragePath.empty())for(const auto& [category,folder]:stock)add(category,Fs::path{wide(config.modelStoragePath)}/wide(folder));
    add("custom_nodes",home.parent_path()/L"forge-managed"/L"custom_nodes");return categories;
}
std::vector<Fs::path> modelRoots(const Domain::ComfyUiConfig& config,const Fs::path& home){
    std::vector<Fs::path> roots{home/L"models",home.parent_path().parent_path()/L"models"};if(!config.modelStoragePath.empty())roots.emplace_back(wide(config.modelStoragePath));
    const auto categories=modelCategoryPaths(config,home);for(const auto& [category,paths]:categories.items())if(category!="custom_nodes")for(const auto& path:paths)roots.emplace_back(wide(path.get<std::string>()));
    for(auto& root:roots){root=root.lexically_normal();root.make_preferred();while(root!=root.root_path()&&root.filename().empty())root=root.parent_path();}
    std::sort(roots.begin(),roots.end());roots.erase(std::unique(roots.begin(),roots.end()),roots.end());return roots;
}
Json modelPublisherCatalog(const Fs::path& home,const Domain::OperationContext& context){
    const auto manager=home/L"custom_nodes"/L"ComfyUI-Manager",path=manager/L"model-list.json";
    Json models=Fs::is_regular_file(path)?readJson(path).value("models",Json::array()):Json::array(),mapping=Json::object(),aliases=Json::object(),legacyAliases=Json::object();
    if(!models.is_array()||models.size()>100000U)fail(Domain::ErrorCodes::LimitExceeded,"Publisher model catalog exceeds its bounded inventory.");
    const auto sourceText=[&](const Fs::path& source){check(context);regularParents(source);std::ifstream input{source,std::ios::binary};std::string value(65536U,'\0');input.read(value.data(),static_cast<std::streamsize>(value.size()));value.resize(static_cast<std::size_t>(input.gcount()));return value;};
    const auto source=manager/L"glob"/L"manager_server.py";
    if(Fs::is_regular_file(source)){const auto value=sourceText(source);const auto start=value.find("model_dir_name_map = {");if(start!=std::string::npos){const auto begin=value.find('{',start),end=value.find('}',begin);
        if(end==std::string::npos)fail(Domain::ErrorCodes::MalformedMessage,"Installed Manager model-directory literal is incomplete.");auto literal=value.substr(begin,end-begin+1U);const auto last=literal.find_last_not_of(" \t\r\n",literal.size()-2U);if(last!=std::string::npos&&literal[last]==',')literal.erase(last,1U);mapping=parse(literal);
        if(!mapping.is_object())fail(Domain::ErrorCodes::MalformedMessage,"Installed Manager model-directory mapping is not a literal object.");for(const auto& item:mapping)if(!item.is_string())fail(Domain::ErrorCodes::MalformedMessage,"Installed Manager model-directory mapping contains a nonliteral path.");}}
    const auto folders=home/L"folder_paths.py";if(Fs::is_regular_file(folders)){
        const auto value=sourceText(folders);const std::regex categories{R"FORGEMODEL(folder_names_and_paths\["([^"]+)"\]\s*=\s*\(\[([^\]]*)\])FORGEMODEL"},paths{R"FORGEMODEL(os\.path\.join\(models_dir,\s*"([^"]+)"\))FORGEMODEL"};
        const auto legacy=value.find("legacy = {");if(legacy!=std::string::npos){const auto first=value.find('{',legacy),last=value.find('}',first);if(last==std::string::npos)fail(Domain::ErrorCodes::MalformedMessage,"Installed model-category aliases are incomplete.");legacyAliases=parse(value.substr(first,last-first+1U));if(!legacyAliases.is_object())fail(Domain::ErrorCodes::MalformedMessage,"Installed model-category aliases must be a literal object.");}
        for(auto category=std::sregex_iterator(value.begin(),value.end(),categories);category!=std::sregex_iterator{};++category){const auto name=(*category)[1].str(),body=(*category)[2].str();aliases[name]=Json::array();for(auto entry=std::sregex_iterator(body.begin(),body.end(),paths);entry!=std::sregex_iterator{};++entry)aliases[name].push_back((*entry)[1].str());}
    }
    for(auto& model:models){check(context);if(!model.is_object())fail(Domain::ErrorCodes::MalformedMessage,"Publisher model catalog contains a non-object entry.");auto category=model.value("save_path",std::string{});
        if(category=="default"){auto type=model.value("type",std::string{});std::transform(type.begin(),type.end(),type.begin(),[](unsigned char byte){return static_cast<char>(std::tolower(byte));});category=mapping.contains(type)?mapping.at(type).get<std::string>():std::string{};}
        auto first=category.substr(0,category.find('/'));if(legacyAliases.contains(first)){const auto canonical=legacyAliases.at(first).get<std::string>();category=canonical+category.substr(first.size());first=canonical;}
        model["storage_category"]=category;model["storage_aliases"]=aliases.value(first,Json::array({first}));
    }
    return models;
}
Json referencedModelChoices(const Json& graph,const Json& schemas,const Json& catalog,bool allowEmptyClass=false){
    Json choices=Json::array();if(!graph.is_object())return choices;
    for(const auto& [id,node]:graph.items()){if(!node.is_object()||!node.contains("inputs")||!node.at("inputs").is_object())continue;const auto name=node.value("class_type",std::string{});
        for(const auto& [input,wrapped]:node.at("inputs").items()){const auto& literal=wrapped.is_object()&&wrapped.size()==1U&&wrapped.contains("__value__")?wrapped.at("__value__"):wrapped;if(!literal.is_string())continue;
            const auto selected=literal.get<std::string>();if(selected.empty()||selected.size()>4096U)continue;const auto filename=Fs::path{wide(selected)}.filename();Json matches=Json::array();
            for(const auto& model:catalog)if(model.contains("filename")&&model.at("filename").is_string()&&!_wcsicmp(Fs::path{wide(model.at("filename").get<std::string>())}.filename().c_str(),filename.c_str()))matches.push_back(model);
            bool selector=!schemas.contains(name)&&(!matches.empty()||(allowEmptyClass&&name.empty())),present=false,upload=false;
            if(schemas.contains(name)){const auto contracts=schemas.at(name).value("input",Json::object());for(const auto* section:{"required","optional"})if(contracts.contains(section)&&contracts.at(section).contains(input)){
                const auto& contract=contracts.at(section).at(input);if(!contract.is_array()||contract.empty())continue;
                if(contract.size()>1U&&contract[1].is_object())upload=upload||contract[1].value("image_upload",false)||contract[1].value("audio_upload",false)||contract[1].value("video_upload",false);
                if(contract[0].is_array()){selector=true;for(const auto& choice:contract[0])present=present||choice==literal;}}}
            if(selector&&!upload){if(choices.size()>=65536U)fail(Domain::ErrorCodes::LimitExceeded,"Referenced model choices exceed their bounded inventory.");choices.push_back({{"node_id",id},{"class_type",name},{"input",input},{"selected",selected},{"enum_present",present},{"catalog_matches",matches}});}
        }
    }
    return choices;
}
std::vector<Fs::path> selectedModelFiles(const Json& choice,const std::vector<Fs::path>& roots,const Domain::OperationContext& context,const Json& categoryPaths=Json::object()){
    const auto selected=text(choice,"selected",4096U);const Fs::path wanted{wide(selected)};
    if(wanted.is_absolute()||wanted.has_root_name()||selected.find_first_of(":\r\n")!=std::string::npos)fail(Domain::ErrorCodes::InvalidRequest,"Selected model must name a relative provider inventory path.");for(const auto& part:wanted)if(part==L"..")fail(Domain::ErrorCodes::InvalidRequest,"Selected model leaves its provider inventory.");
    std::vector<Fs::path> found;const auto matches=choice.value("catalog_matches",Json::array());std::size_t visited{};
    const auto inventory=[&](const Fs::path& path){if(!Fs::exists(path))return;bool scoped=false;for(const auto& root:roots)scoped=scoped||contained(root,path);if(!scoped)fail(Domain::ErrorCodes::PathOutsideAuthority,"Selected model category is outside the authorized inventory roots.");regularParents(path);if(Fs::is_regular_file(path))found.push_back(path);else if(Fs::is_directory(path))for(const auto& item:Fs::recursive_directory_iterator(path,Fs::directory_options::skip_permission_denied)){check(context);if(++visited>100000U)fail(Domain::ErrorCodes::LimitExceeded,"Selected model directory exceeds its bounded inventory.");regularParents(item.path());if(item.is_regular_file())found.push_back(item.path());}};
    if(matches.empty()&&categoryPaths.contains(selected))for(const auto& path:categoryPaths.at(selected))inventory(Fs::path{wide(path.get<std::string>())});
    bool exactCategory=matches.empty()&&categoryPaths.contains(selected);for(const auto& model:matches){const Fs::path category{wide(model.value("storage_category",std::string{}))};if(category.empty())continue;const auto key=pathText(*category.begin());if(!categoryPaths.contains(key))continue;exactCategory=true;
        // A live enum names the actual category-relative selection. The catalog's
        // preferred installation subfolder does not relocate an installed model.
        const auto relative=wanted.has_parent_path()||choice.value("enum_present",false)?wanted:category.lexically_relative(*category.begin())/wanted;for(const auto& path:categoryPaths.at(key))inventory(Fs::path{wide(path.get<std::string>())}/relative);}
    const auto same=[](const Fs::path& left,const Fs::path& right){return !_wcsicmp(left.lexically_normal().c_str(),right.lexically_normal().c_str());};
    for(const auto& root:roots){if(exactCategory||!Fs::is_directory(root))continue;regularParents(root);
        for(auto item=Fs::recursive_directory_iterator(root,Fs::directory_options::skip_permission_denied);item!=Fs::recursive_directory_iterator{};++item){check(context);if(++visited>100000U)fail(Domain::ErrorCodes::LimitExceeded,"Selected model inventory exceeds 100,000 entries.");regularParents(item->path());
            const auto relative=Fs::relative(item->path(),root);bool match=false,narrowed=false;
            for(const auto& model:matches){const auto category=model.value("storage_category",std::string{});if(category.empty())continue;narrowed=true;const Fs::path categoryPath{wide(category)};
                for(const auto& alias:model.value("storage_aliases",Json::array({pathText(*categoryPath.begin())}))){const Fs::path suffix{wide(alias.get<std::string>())};auto expected=suffix/(wanted.has_parent_path()?wanted:categoryPath.lexically_relative(*categoryPath.begin())/wanted);
                    if(same(relative,expected)||(same(root.filename(),suffix)&&same(relative,wanted)))match=true;}}
            if(!narrowed){const auto relativeText=relative.lexically_normal().native(),wantedText=wanted.lexically_normal().native();const auto tail=L"\\"+wantedText;match=same(relative,wanted)||(!wanted.has_parent_path()&&same(item->path().filename(),wanted.filename()))||(wanted.has_parent_path()&&relativeText.size()>tail.size()&&!_wcsicmp(relativeText.c_str()+relativeText.size()-tail.size(),tail.c_str()));}
            if(!match)continue;if(item->is_regular_file())found.push_back(item->path());
            else if(item->is_directory()){for(const auto& nested:Fs::recursive_directory_iterator(item->path(),Fs::directory_options::skip_permission_denied)){check(context);if(++visited>100000U)fail(Domain::ErrorCodes::LimitExceeded,"Selected model directory exceeds 100,000 entries.");regularParents(nested.path());if(nested.is_regular_file())found.push_back(nested.path());}item.disable_recursion_pending();}
        }
    }
    std::sort(found.begin(),found.end());found.erase(std::unique(found.begin(),found.end()),found.end());return found;
}
std::vector<Fs::path> customNodeRoots(const Domain::ComfyUiConfig& config,const Fs::path& home){
    const auto categories=modelCategoryPaths(config,home);std::vector<Fs::path> roots;
    for(const auto& value:categories.at("custom_nodes")){const Fs::path path{wide(value.get<std::string>())};if(std::none_of(roots.begin(),roots.end(),[&](const Fs::path& existing){return !_wcsicmp(existing.c_str(),path.c_str());}))roots.push_back(path);}return roots;
}
Json sealCustomNodeSource(const Fs::path& path,const std::vector<Fs::path>& roots,const Domain::OperationContext& context,Json publisherIdentity=Json::object(),Json identityFiles=Json::array()){
    check(context);bool scoped=false;for(const auto& root:roots)scoped=scoped||(contained(root,path)&&path!=root);if(!scoped)fail(Domain::ErrorCodes::IntegrityFailure,"Referenced custom-node source leaves the loaded node roots.");regularParents(path);
    Json result{{"path",pathText(path)},{"publisher_identity",std::move(publisherIdentity)},{"identity_files",Json::array()}};
    if(Fs::is_directory(path)){result["kind"]="directory";result["tree"]=directoryFacts(path,MaximumArtifactBytes,context);}
    else if(Fs::is_regular_file(path)&&!_wcsicmp(path.extension().c_str(),L".py")){const auto facts=fileFacts(path,context);result["kind"]="file";result["sha256"]=facts.at("sha256");result["bytes"]=facts.at("bytes");}
    else fail(Domain::ErrorCodes::IntegrityFailure,"Referenced custom-node source is not an enabled directory or Python file.");
    if(!identityFiles.is_array()||identityFiles.size()>64U)fail(Domain::ErrorCodes::LimitExceeded,"Custom-node publisher metadata exceeds its bounded inventory.");
    for(const auto& identity:identityFiles){const Fs::path file{wide(text(identity,"path"))};if(!contained(path,file))fail(Domain::ErrorCodes::IntegrityFailure,"Custom-node publisher metadata leaves its selected source.");const auto facts=fileFacts(file,context);
        if(!identity.contains("sha256")||!identity.contains("bytes")||facts.at("sha256")!=identity.at("sha256")||facts.at("bytes")!=identity.at("bytes"))fail(Domain::ErrorCodes::IntegrityFailure,"Custom-node publisher identity changed during source binding.");
        result["identity_files"].push_back({{"path",pathText(file)},{"sha256",facts.at("sha256")},{"bytes",facts.at("bytes")}});
    }return result;
}
Json originalExtraPaths(const Fs::path& home,const Domain::OperationContext& context){
    const auto path=home/L"extra_model_paths.yaml";Json seal{{"path",pathText(path)},{"present",Fs::exists(path)}};
    if(seal.at("present")==true){regularParents(path);if(!Fs::is_regular_file(path))fail(Domain::ErrorCodes::InvalidRequest,"Original ComfyUI path configuration is not a regular file.");const auto facts=fileFacts(path,context);seal["sha256"]=facts.at("sha256");seal["bytes"]=facts.at("bytes");}return seal;
}
std::vector<std::string> runtimeCustomNodes(const Domain::ComfyUiConfig& config,const Fs::path& home){
    std::set<std::string> nodes;
    for(const auto& root:customNodeRoots(config,home)){
        if(!Fs::is_directory(root))continue;regularParents(root);
        for(const auto& item:Fs::directory_iterator{root}){
            const auto name=pathText(item.path().filename());
            if(_wcsicmp(item.path().filename().c_str(),L"ComfyUI-Manager")==0||name=="__pycache__"||name.ends_with(".disabled"))continue;
            if(!item.is_directory()&&(!item.is_regular_file()||item.path().extension()!=L".py"))continue;
            if(!safeName(name))fail(Domain::ErrorCodes::InvalidRequest,"Installed custom node has an unsupported runtime name.");
            regularParents(item.path());nodes.insert(name);
            if(nodes.size()>512U)fail(Domain::ErrorCodes::LimitExceeded,"Installed custom-node runtime whitelist exceeds 512 entries.");
        }
    }
    return {nodes.begin(),nodes.end()};
}
Json errorJson(const Domain::Error& error){return {{"code",error.code},{"message",error.message},{"retryable",error.retryable}};}
std::string publisherRepository(std::string value){
    const auto first=value.find_first_not_of(" \t\r\n"),last=value.find_last_not_of(" \t\r\n");if(first==std::string::npos)return {};value=value.substr(first,last-first+1U);
    if(value.starts_with("git@github.com:"))value="https://github.com/"+value.substr(15U);
    else if(value.starts_with("ssh://git@github.com/"))value="https://github.com/"+value.substr(21U);
    if(!value.starts_with("https://")||value.find_first_of("?#\\\r\n\t ")!=std::string::npos)return {};
    while(value.ends_with('/'))value.pop_back();if(value.ends_with(".git"))value.resize(value.size()-4U);
    const auto slash=value.find('/',8U);if(slash==std::string::npos||slash==8U||slash+1U==value.size()||value.substr(8U,slash-8U).find('@')!=std::string::npos)return {};
    std::transform(value.begin()+8,value.begin()+static_cast<std::ptrdiff_t>(slash),value.begin()+8,[](unsigned char byte){return static_cast<char>(std::tolower(byte));});
    if(value.starts_with("https://github.com/")){const auto name=value.substr(19U);if(std::count(name.begin(),name.end(),'/')!=1||name.front()=='/'||name.back()=='/')return {};
        std::transform(value.begin()+19,value.end(),value.begin()+19,[](unsigned char byte){return static_cast<char>(std::tolower(byte));});}
    return value;
}
Json installedPublisherSource(const std::string& repository,const std::string& packageId,const Json& registryNode,
    const std::vector<Fs::path>& roots,const Domain::OperationContext& context,
    const std::function<void(const Fs::path&,const Fs::path&)>& authorize,
    const std::function<Json(const Fs::path&)>& projectIdentity){
    const auto expectedRepository=publisherRepository(repository);Json candidates=Json::array();std::set<std::wstring> visited;std::size_t count{};
    const auto trimmed=[](std::string value){const auto first=value.find_first_not_of(" \t\r\n"),last=value.find_last_not_of(" \t\r\n");return first==std::string::npos?std::string{}:value.substr(first,last-first+1U);};
    const auto revision=[](const std::string& value){return value.size()==40U&&value.find_first_not_of("0123456789abcdefABCDEF")==std::string::npos;};
    for(const auto& root:roots){if(!Fs::is_directory(root))continue;authorize(root,root);regularParents(root);
        for(const auto& entry:Fs::directory_iterator(root)){check(context);const auto path=entry.path();const auto name=pathText(path.filename());
            if(name=="__pycache__"||name.ends_with(".disabled")||!_wcsicmp(path.filename().c_str(),L"ComfyUI-Manager")||!entry.is_directory())continue;
            if(++count>512U)fail(Domain::ErrorCodes::LimitExceeded,"Installed custom-node publisher inventory exceeds 512 sources.");auto key=path.lexically_normal().native();std::transform(key.begin(),key.end(),key.begin(),[](wchar_t value){return static_cast<wchar_t>(std::towlower(value));});if(!visited.insert(key).second)continue;
            authorize(path,root);regularParents(path);Json files=Json::array(),identity=Json::object(),expectedTree;bool differentGitPublisher=false;
            const auto metadata=[&](const Fs::path& source,std::size_t maximum){authorize(source,path);regularParents(source);const auto before=fileFacts(source,context);if(before.at("bytes").get<std::uint64_t>()>maximum)fail(Domain::ErrorCodes::PayloadTooLarge,"Installed custom-node publisher metadata exceeds its bounded inventory.");
                std::ifstream input{source,std::ios::binary};if(!input)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot read installed custom-node publisher metadata.");std::string contents(maximum+1U,'\0');input.read(contents.data(),static_cast<std::streamsize>(contents.size()));contents.resize(static_cast<std::size_t>(input.gcount()));if(contents.size()>maximum||fileFacts(source,context)!=before)fail(Domain::ErrorCodes::IntegrityFailure,"Installed custom-node publisher metadata changed during discovery.");files.push_back(before);return contents;};
            const auto marker=path/L".forge-dependency.json";if(Fs::is_regular_file(marker)){const auto retained=parse(metadata(marker,1024U*1024U));const auto provenance=retained.value("provenance",Json::object());
                if(provenance.is_object()){const auto kind=provenance.value("kind",std::string{}),identified=publisherRepository(provenance.value("repository",std::string{}));
                    const bool git=kind=="identified_publisher_commit"&&!expectedRepository.empty()&&identified==expectedRepository;
                    const bool cnr=kind=="comfy_registry_immutable_version"&&!packageId.empty()&&provenance.value("package_id",std::string{})==packageId&&(expectedRepository.empty()||identified==expectedRepository);
                    if(git||cnr){if(!retained.contains("tree")||!retained.at("tree").is_object()||!Domain::Sha256Digest::parse(text(retained,"sha256",64U)))fail(Domain::ErrorCodes::IntegrityFailure,"Identified installed custom-node marker lacks its source content seal.");
                        if(git&&!revision(text(provenance,"revision",40U)))fail(Domain::ErrorCodes::IntegrityFailure,"Identified installed custom-node marker lacks its pinned commit.");
                        identity={{"kind","forge_manifest"},{"provenance",provenance},{"sha256",retained.at("sha256")},{"revision",text(retained,"revision",128U)}};if(cnr)identity["registry_version"]=text(retained,"registry_version",128U);expectedTree=retained.at("tree");}
                }}
            if(identity.empty()){files=Json::array();const auto git=path/L".git",config=git/L"config";
                if(Fs::is_directory(git)&&Fs::is_regular_file(config)&&!expectedRepository.empty()){
                    const auto contents=metadata(config,65536U);std::istringstream lines{contents};std::string line,origin;bool selected=false;
                    const std::regex section{R"FORGENODE(^\[remote\s+"origin"\]$)FORGENODE"};
                    while(std::getline(lines,line)){line=trimmed(line);if(line.empty()||line.front()=='#'||line.front()==';')continue;if(line.front()=='['){selected=std::regex_match(line,section);continue;}if(!selected)continue;
                        const auto equals=line.find('=');if(equals==std::string::npos)continue;auto field=trimmed(line.substr(0,equals));std::transform(field.begin(),field.end(),field.begin(),[](unsigned char byte){return static_cast<char>(std::tolower(byte));});if(field!="url")continue;
                        const auto value=publisherRepository(trimmed(line.substr(equals+1U)));if(!origin.empty()&&origin!=value)fail(Domain::ErrorCodes::Conflict,"Installed Git origin has multiple publisher repositories.");origin=value;
                    }
                    differentGitPublisher=!origin.empty()&&origin!=expectedRepository;if(origin==expectedRepository){const auto head=git/L"HEAD";if(!Fs::is_regular_file(head))fail(Domain::ErrorCodes::Conflict,"Identified installed custom-node Git source has no HEAD revision.");auto commit=trimmed(metadata(head,65536U));
                        if(commit.starts_with("ref:")){const auto ref=trimmed(commit.substr(4U));const Fs::path relative{wide(ref)};if(!ref.starts_with("refs/")||ref.find_first_of("\\:\r\n\t ")!=std::string::npos||relative.is_absolute()||relative.has_root_name())fail(Domain::ErrorCodes::Conflict,"Identified installed custom-node Git HEAD has an unsupported ref.");for(const auto& part:relative)if(part==L".."||part==L".")fail(Domain::ErrorCodes::Conflict,"Identified installed custom-node Git HEAD leaves its metadata directory.");
                            const auto loose=git/relative;commit.clear();if(Fs::is_regular_file(loose))commit=trimmed(metadata(loose,65536U));else{const auto packed=git/L"packed-refs";if(Fs::is_regular_file(packed)){std::istringstream refs{metadata(packed,4U*1024U*1024U)};while(std::getline(refs,line)){line=trimmed(line);if(line.empty()||line.front()=='#'||line.front()=='^')continue;const auto space=line.find(' ');if(space==std::string::npos||trimmed(line.substr(space+1U))!=ref)continue;const auto value=line.substr(0,space);if(!commit.empty()&&commit!=value)fail(Domain::ErrorCodes::Conflict,"Installed Git packed ref has multiple revisions.");commit=value;}}}}
                        if(!revision(commit))fail(Domain::ErrorCodes::Conflict,"Identified installed custom-node Git revision cannot be resolved exactly.");std::transform(commit.begin(),commit.end(),commit.begin(),[](unsigned char byte){return static_cast<char>(std::tolower(byte));});
                        identity={{"kind","installed_git_revision"},{"repository",expectedRepository},{"revision",commit}};
                    }
                }}
            if(identity.empty()&&!packageId.empty()){files=Json::array();const auto project=path/L"pyproject.toml",tracking=path/L".tracking";
                if(Fs::is_regular_file(project)&&Fs::is_regular_file(tracking)){static_cast<void>(metadata(project,1024U*1024U));const auto declared=projectIdentity(project);if(declared.value("id",std::string{})==packageId){
                    if(differentGitPublisher)fail(Domain::ErrorCodes::Conflict,"Installed Registry metadata conflicts with its Git publisher origin.");
                    const auto declaredRepository=publisherRepository(declared.value("repository",std::string{})),publisher=declared.value("publisher_id",std::string{}),identifiedPublisher=registryNode.value("publisher",Json::object()).value("id",std::string{});
                    if((!declaredRepository.empty()&&!expectedRepository.empty()&&declaredRepository!=expectedRepository)||(!publisher.empty()&&!identifiedPublisher.empty()&&publisher!=identifiedPublisher))fail(Domain::ErrorCodes::Conflict,"Installed Registry source declares a different publisher identity.");
                    const auto tracked=metadata(tracking,4U*1024U*1024U);std::istringstream names{tracked};std::string trackedName;std::size_t trackedCount{};while(std::getline(names,trackedName)){trackedName=trimmed(trackedName);if(trackedName.empty())continue;if(++trackedCount>8192U)fail(Domain::ErrorCodes::LimitExceeded,"Installed Registry tracking exceeds its bounded inventory.");const Fs::path relative{wide(trackedName)};if(relative.is_absolute()||relative.has_root_name()||trackedName.find_first_of("\\:\r\n")!=std::string::npos)fail(Domain::ErrorCodes::IntegrityFailure,"Installed Registry tracking leaves its source directory.");for(const auto& part:relative)if(part==L"..")fail(Domain::ErrorCodes::IntegrityFailure,"Installed Registry tracking leaves its source directory.");const auto file=path/relative;authorize(file,path);regularParents(file);if(!Fs::exists(file))fail(Domain::ErrorCodes::IntegrityFailure,"Installed Registry tracking names missing publisher content.");}
                    if(!trackedCount)fail(Domain::ErrorCodes::IntegrityFailure,"Identified installed Registry source has empty tracking.");
                    const auto version=text(declared,"version",128U);if(version.empty())fail(Domain::ErrorCodes::IntegrityFailure,"Identified installed Registry source has no pinned version.");
                    identity={{"kind","installed_registry_version"},{"package_id",packageId},{"version",version},{"repository",expectedRepository},{"publisher",registryNode.value("publisher",Json::object())}};
                }}}
            if(!identity.empty()){Json candidate{{"path",pathText(path)},{"publisher_identity",identity},{"identity_files",files}};if(!expectedTree.is_null())candidate["expected_tree"]=expectedTree;candidates.push_back(std::move(candidate));}
        }
    }
    if(candidates.size()>1U)fail(Domain::ErrorCodes::Conflict,"Identified custom-node publisher has multiple installed sources; select and reconcile the existing installation before preparation.");return candidates.empty()?Json{}:candidates.front();
}
Json imagePreviewFacts(HANDLE file,const Domain::OperationContext& context){
    using Microsoft::WRL::ComPtr;
    const auto checked=[](HRESULT value,const char* action){if(FAILED(value))fail(Domain::ErrorCodes::IntegrityFailure,action);};
    const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(initialized!=RPC_E_CHANGED_MODE)checked(initialized,"Initialize ComfyUI image decoder.");
    struct Apartment {bool initialized;~Apartment(){if(initialized)CoUninitialize();}} apartment{SUCCEEDED(initialized)};
    ComPtr<IWICImagingFactory> factory;checked(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"Create ComfyUI image decoder.");
    ComPtr<IWICBitmapDecoder> decoder;checked(factory->CreateDecoderFromFileHandle(reinterpret_cast<ULONG_PTR>(file),nullptr,WICDecodeMetadataCacheOnDemand,&decoder),"Decode ComfyUI image file.");
    UINT frames{};checked(decoder->GetFrameCount(&frames),"Read ComfyUI image frame count.");if(frames!=1U)fail(Domain::ErrorCodes::IntegrityFailure,"Still image has multiple frames; animation requires complete media inspection.");
    ComPtr<IWICBitmapFrameDecode> frame;checked(decoder->GetFrame(0U,&frame),"Read ComfyUI image frame.");UINT width{},height{};checked(frame->GetSize(&width,&height),"Read ComfyUI image dimensions.");
    if(!width||!height||width>32768U||height>32768U||static_cast<std::uint64_t>(width)*height>268435456ULL)fail(Domain::ErrorCodes::PayloadTooLarge,"ComfyUI image exceeds 32768 pixels per dimension or 268 million decoded pixels.");
    ComPtr<IWICFormatConverter> converter;checked(factory->CreateFormatConverter(&converter),"Create ComfyUI RGBA converter.");
    checked(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0.0,WICBitmapPaletteTypeCustom),"Convert ComfyUI image pixels.");
    // Read every source row to reject truncated/corrupt pixels, retaining only
    // a bounded strip and the reduced preview rather than the full RGBA image.
    const auto stride=width*4U;std::vector<BYTE> strip(static_cast<std::size_t>(stride)*(std::min)(64U,height));
    for(UINT row=0U;row<height;row+=64U){check(context);const auto rows=(std::min)(64U,height-row);const WICRect region{0,static_cast<INT>(row),static_cast<INT>(width),static_cast<INT>(rows)};
        checked(converter->CopyPixels(&region,stride,rows*stride,strip.data()),"Decode complete ComfyUI image pixels.");}
    auto scale=(std::min)(1.0,768.0/(std::max)(width,height));
    for(;;){check(context);const auto previewWidth=(std::max)(1U,static_cast<UINT>(width*scale)),previewHeight=(std::max)(1U,static_cast<UINT>(height*scale));
        ComPtr<IWICBitmapScaler> scaler;checked(factory->CreateBitmapScaler(&scaler),"Create ComfyUI preview scaler.");checked(scaler->Initialize(converter.Get(),previewWidth,previewHeight,WICBitmapInterpolationModeFant),"Scale ComfyUI image preview.");
        Detail::ImageProviderPixels pixels{previewWidth,previewHeight,std::vector<std::byte>(static_cast<std::size_t>(previewWidth)*previewHeight*4U)};
        checked(scaler->CopyPixels(nullptr,previewWidth*4U,static_cast<UINT>(pixels.rgba.size()),reinterpret_cast<BYTE*>(pixels.rgba.data())),"Read ComfyUI preview pixels.");
        const auto preview=take(Detail::previewProviderImage(pixels,768U,context));
        if(preview.base64.size()<=24U*1024U)return {{"preview",{{"mime_type","image/png"},{"base64",preview.base64},{"sha256",preview.sha256},{"width",preview.width},{"height",preview.height}}},
            {"metadata",{{"decoded",true},{"width",width},{"height",height},{"frame_count",1U},{"duration",0.0}}},{"metadata_method","native_WIC_full_image_decode_and_bounded_preview"}};
        if(previewWidth==1U&&previewHeight==1U)fail(Domain::ErrorCodes::PayloadTooLarge,"ComfyUI preview cannot fit the native delivery budget.");
        scale*=0.75;
    }
}
bool animatedPng(const Fs::path& path){
    std::ifstream stream{path,std::ios::binary};stream.seekg(8);std::array<unsigned char,8> header{};
    for(unsigned count=0U;count<8192U;++count){if(!stream.read(reinterpret_cast<char*>(header.data()),8))return false;
        const auto length=(static_cast<std::uint64_t>(header[0])<<24U)|(static_cast<std::uint64_t>(header[1])<<16U)|(static_cast<std::uint64_t>(header[2])<<8U)|header[3];
        const std::string_view kind{reinterpret_cast<const char*>(header.data()+4U),4U};if(kind=="acTL")return true;if(kind=="IDAT"||kind=="IEND")return false;
        stream.seekg(static_cast<std::streamoff>(length+4U),std::ios::cur);
    }
    fail(Domain::ErrorCodes::LimitExceeded,"PNG metadata chunks exceed the animation-discovery bound.");
}
}

class WindowsComfyUiBackend::Impl final {
public:
    Impl(Contracts::IWorkspaceAuthority& resolver,Domain::PathText root):resolver_{resolver},root_{wide(root.value())}{}
    ~Impl(){shutdown();}
    Domain::Result<std::string> perform(std::string_view operation,std::string_view encoded,const Domain::ComfyUiConfig& config,
        const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& context)noexcept {
        try{
            check(context);if(stopped_)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"ComfyUI backend has shut down.");
            const auto valid=Domain::validateComfyUiConfig(config);if(!valid)throw Failure{valid.error()};
            const auto arguments=parse(encoded);if(!arguments.is_object())fail(Domain::ErrorCodes::InvalidRequest,"ComfyUI arguments must be an object.");
            if(!config.enabled&&operation!="seal")fail(Domain::ErrorCodes::HostCapabilityUnavailable,"ComfyUI automation is disabled.");
            Json result;
            if(operation=="request")result=request(arguments,config,context);
            else if(operation=="status")result=status(config,context);
            else if(operation=="control")result=control(arguments,config,authority,context);
            else if(operation=="prepare")result=prepare(arguments,config,authority,context);
            else if(operation=="workflow")result=workflow(arguments,config,authority,context);
            else if(operation=="upload")result=upload(arguments,config,authority,context);
            else if(operation=="seal_input")result=sealInput(arguments,config,authority,context);
            else if(operation=="collect")result=collect(arguments,config,authority,context);
            else if(operation=="inspect"||operation=="seal")result=inspect(text(arguments,"path"),authority,context);
            else if(operation=="identity")result=identity(arguments,config,authority,context);
            else if(operation=="catalog")result=catalog(arguments,config,authority,context);
            else if(operation=="progress")result=progress(arguments,config,context);
            else if(operation=="cancel")result=cancel(arguments,config,context);
            else fail(Domain::ErrorCodes::InvalidRequest,"Unknown native ComfyUI backend operation.");
            return Domain::Result<std::string>::success(result.dump());
        }catch(const Failure& error){return Domain::Result<std::string>::failure(error.error);}
        catch(const Json::exception& error){return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::MalformedMessage,"Invalid ComfyUI JSON: "+std::string{error.what()}));}
        catch(const Fs::filesystem_error& error){return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,"ComfyUI filesystem operation failed: "+std::string{error.what()}));}
        catch(const std::exception& error){return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,"ComfyUI operation failed: "+std::string{error.what()}));}
        catch(...){return Domain::Result<std::string>::failure(Domain::makeError(Domain::ErrorCodes::InternalFailure,"ComfyUI native backend failed."));}
    }
    void shutdown()noexcept{
        stopped_=true;std::lock_guard lock{browserMutex_};
        if(browser_.job)TerminateJobObject(browser_.job.get(),0);
        browser_={};
        // An owned ComfyUI process can finish a sealed job after Manager exit.
        // Its PID/creation identity remains in the durable owner ledger.
        std::lock_guard ownerLock{ownerMutex_};owner_={};
        std::lock_guard progressLock{progressMutex_};progress_.clear();
    }
private:
    Contracts::AuthorizedPath authorized(const Fs::path& path,Domain::FileAccess access,const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext& context,bool missing=false){
        if(!path.is_absolute())fail(Domain::ErrorCodes::InvalidRequest,"ComfyUI file paths must be absolute.");
        auto result=take(resolver_.authorize(authority,{take(Domain::PathText::create(pathText(path))),std::nullopt,access,missing},context));
        regularParents(Fs::path{wide(result.canonicalPath().value())});return result;
    }
    void execution(const Fs::path& path,const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& context){
        if(!authority.shellEnabled()||std::find(authority.grants().begin(),authority.grants().end(),Domain::FileAccess::Execute)==authority.grants().end()||
            std::find(authority.denials().begin(),authority.denials().end(),Domain::FileAccess::Execute)!=authority.denials().end())
            fail(Domain::ErrorCodes::Unauthorized,"ComfyUI setup and runtime control require owner-enabled Execute permission.");
        static_cast<void>(internalAuthorized(path,path.parent_path(),Domain::FileAccess::Execute,authority,context));
    }
    Contracts::AuthorizedPath internalAuthorized(const Fs::path& path,const Fs::path& scope,Domain::FileAccess access,
        const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& context,bool missing=false){
        if(!scope.is_absolute()||!contained(scope,path))fail(Domain::ErrorCodes::PathOutsideAuthority,"Internal ComfyUI path escaped its configured scope.");
        regularParents(scope);auto existing=scope.parent_path();while(!Fs::exists(existing)&&existing!=existing.parent_path())existing=existing.parent_path();
        Infrastructure::Windows::WindowsWorkspaceAuthority issuer{std::vector<Infrastructure::Windows::WindowsWorkspaceAuthorityPolicy>{{
            authority.authorityId(),authority.projectId(),authority.callerId(),{take(Domain::PathText::create(pathText(existing)))},Domain::FileAccess::Read,
            {Domain::FileAccess::Read,Domain::FileAccess::Write,Domain::FileAccess::Create,Domain::FileAccess::Execute},{},true,1U}}};
        const auto capability=take(issuer.authorityFor(authority.projectId(),context));
        auto result=take(issuer.authorize(capability,{take(Domain::PathText::create(pathText(path))),std::nullopt,access,missing},context));
        regularParents(Fs::path{wide(result.canonicalPath().value())});return result;
    }
    Fs::path internalDirectory(const Fs::path& path,const Fs::path& scope,const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& context){
        static_cast<void>(internalAuthorized(path,scope,Domain::FileAccess::Write,authority,context,true));
        if(!Fs::exists(path))static_cast<void>(internalAuthorized(path,scope,Domain::FileAccess::Create,authority,context,true));
        Fs::create_directories(path);regularParents(path);return path;
    }
    Fs::path writableDirectory(const Fs::path& path,const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& context){
        if(Fs::exists(path)){static_cast<void>(authorized(path,Domain::FileAccess::Read,authority,context));if(!Fs::is_directory(path))fail(Domain::ErrorCodes::InvalidRequest,"Artifact destination is not a directory.");}
        else static_cast<void>(authorized(path,Domain::FileAccess::Create,authority,context,true));
        regularParents(path);Fs::create_directories(path);return path;
    }
    Json request(const Json& arguments,const Domain::ComfyUiConfig& config,const Domain::OperationContext& context){
        const auto method=text(arguments,"method",8U),route=text(arguments,"route",8192U);
        const bool read=method=="GET"&&(route=="/object_info"||route.starts_with("/object_info/")||route=="/system_stats"||route=="/features"||route=="/extensions"||route=="/models"||route.starts_with("/models/")||route=="/queue"||route=="/history"||route.starts_with("/history/"));
        const bool write=method=="POST"&&(route=="/prompt"||route=="/queue");
        std::unique_ptr<Detail::ProviderOperationLease> providerLease;
        if(method=="POST"&&route=="/prompt")providerLease=take(Detail::ProviderOperationLease::acquire(config.endpoint,context));
        if((!read&&!write)||route.find_first_of("\r\n\\#")!=std::string::npos||route.find("..")!=std::string::npos)
            fail(Domain::ErrorCodes::InvalidRequest,"Unsupported managed ComfyUI API route.");
        const auto body=method=="GET"?std::string{}:arguments.contains("body")?arguments.at("body").dump():std::string{};
        if(method=="GET"&&!body.empty())fail(Domain::ErrorCodes::InvalidRequest,"GET provider requests cannot carry a body.");
        if(write&&(!arguments.contains("body")||!arguments.at("body").is_object()))fail(Domain::ErrorCodes::InvalidRequest,"Provider POST requires an object body.");
        if(route=="/queue"&&write&&(arguments.at("body").contains("clear")||!arguments.at("body").contains("delete")))fail(Domain::ErrorCodes::InvalidRequest,"Only exact pending prompt deletion is supported.");
        if(write)requireOwner(config,context);
        if(route=="/prompt"){const auto queue=request({{"method","GET"},{"route","/queue"}},config,context);if(!queue.value("queue_running",Json::array()).empty()||!queue.value("queue_pending",Json::array()).empty())fail(Domain::ErrorCodes::Conflict,"ComfyUI queue became busy before sealed prompt dispatch.");requireOwner(config,context);}
        const auto response=http(config.endpoint+route,method,body.empty()?"":"application/json",body,16U*1024U*1024U,context);
        if(response.status<200||response.status>=300){
            if(route=="/prompt"&&response.status==400U){auto rejected=parse(response.body);rejected["http_status"]=response.status;return rejected;}
            fail(Domain::ErrorCodes::HostCapabilityUnavailable,"ComfyUI returned HTTP "+std::to_string(response.status)+": "+response.body.substr(0,32768U));
        }
        if(response.body.empty()){if(method=="GET"&&route=="/queue")fail(Domain::ErrorCodes::MalformedMessage,"Provider queue response is empty.");return {{"http_status",response.status}};}
        auto value=parse(response.body);if(method=="GET"&&route=="/queue"&&(!value.is_object()||!value.contains("queue_running")||!value.contains("queue_pending")||!value.at("queue_running").is_array()||!value.at("queue_pending").is_array()))
            fail(Domain::ErrorCodes::MalformedMessage,"Provider queue response must include running and pending arrays; received "+response.body.substr(0,8192U));
        return value;
    }
    Fs::path runtimeInterpreter(const Fs::path& home){
        const auto active=root_/L"active-environment.json";
        return Fs::is_regular_file(active)?Fs::path{wide(text(readJson(active),"python"))}:home.parent_path()/L"python_embeded"/L"python.exe";
    }
    Json interpreterPathConfiguration(const Fs::path& python,const Domain::OperationContext& context){
        Json seals=Json::array();if(!Fs::is_directory(python.parent_path()))return seals;
        std::vector<Fs::path> paths;for(const auto& entry:Fs::directory_iterator{python.parent_path()})if(entry.is_regular_file()&&entry.path().extension()==L"._pth")paths.push_back(entry.path());
        std::sort(paths.begin(),paths.end());for(const auto& path:paths)seals.push_back({{"path",pathText(path)},{"sha256",fileFacts(path,context).at("sha256")}});return seals;
    }
    Json requireOwner(const Domain::ComfyUiConfig& config,const Domain::OperationContext& context,bool verifyConfiguration=true){
        check(context);const auto ledgerPath=root_/L"runtime-owner.json";if(!Fs::is_regular_file(ledgerPath))fail(Domain::ErrorCodes::Unauthorized,"ComfyUI endpoint has no retained Forge process ownership.");
        const auto ledger=readJson(ledgerPath);if(ledger.value("endpoint",std::string{})!=config.endpoint)fail(Domain::ErrorCodes::Conflict,"ComfyUI owner endpoint changed.");
        const auto home=installedHome(config),python=verifyConfiguration?runtimeInterpreter(home):Fs::path{wide(text(ledger,"python"))};
        const auto samePath=[](const std::string& sealed,const Fs::path& expected){auto actual=Fs::path{wide(sealed)}.lexically_normal();actual.make_preferred();auto target=expected.lexically_normal();target.make_preferred();return _wcsicmp(actual.c_str(),target.c_str())==0;};
        if(!samePath(text(ledger,"installation"),home))fail(Domain::ErrorCodes::Conflict,"Live ComfyUI runtime belongs to a different selected installation.");
        if(verifyConfiguration&&(!samePath(text(ledger,"python"),python)||
            !samePath(text(ledger,"model_storage_path"),config.modelStoragePath.empty()?home/L"models":Fs::path{wide(config.modelStoragePath)})||
            !samePath(text(ledger,"input_directory"),root_/L"inputs")||!samePath(text(ledger,"output_directory"),root_/L"outputs")))
            fail(Domain::ErrorCodes::Conflict,"Live ComfyUI runtime does not match the selected installation, interpreter, model storage, or private media directories.");
        if(verifyConfiguration&&(!ledger.contains("extra_paths_sha256")||!Fs::is_regular_file(root_/L"extra-paths.yaml")||fileFacts(root_/L"extra-paths.yaml",context).at("sha256")!=ledger.at("extra_paths_sha256")||
            !ledger.contains("interpreter_path_configuration")||interpreterPathConfiguration(python,context)!=ledger.at("interpreter_path_configuration")||
            !ledger.contains("custom_node_whitelist")||Json(runtimeCustomNodes(config,home))!=ledger.at("custom_node_whitelist")||
            !ledger.contains("original_extra_paths")||originalExtraPaths(home,context)!=ledger.at("original_extra_paths")||
            !ledger.contains("model_category_paths")||modelCategoryPaths(config,home)!=ledger.at("model_category_paths")))
            fail(Domain::ErrorCodes::Conflict,"ComfyUI launch configuration changed; drain and reconcile the managed runtime before rendering.");
        const auto pid=ledger.at("pid").get<DWORD>();Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid)};FILETIME created{},exit{},kernel{},user{};
        if(!process||WaitForSingleObject(process.get(),0)!=WAIT_TIMEOUT||!GetProcessTimes(process.get(),&created,&exit,&kernel,&user)||
            ((static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime)!=ledger.at("creation_time").get<std::uint64_t>())fail(Domain::ErrorCodes::Conflict,"ComfyUI owned process identity is no longer live.");
        std::array<wchar_t,32768> image{};DWORD count=static_cast<DWORD>(image.size());if(!QueryFullProcessImageNameW(process.get(),0,image.data(),&count)||_wcsicmp(image.data(),wide(text(ledger,"python")).c_str())!=0)fail(Domain::ErrorCodes::IntegrityFailure,"ComfyUI owned process image changed.");
        if(!ledger.contains("image_identity")||fileFacts(Fs::path{image.data()},context).at("sha256")!=ledger.at("image_identity").at("sha256"))fail(Domain::ErrorCodes::IntegrityFailure,"ComfyUI interpreter content differs from its retained launch seal.");
        if(!listenerOwned(config,pid))fail(Domain::ErrorCodes::Conflict,"ComfyUI endpoint TCP listener is not owned by the sealed process.");return ledger;
    }
    Json cancel(const Json& arguments,const Domain::ComfyUiConfig& config,const Domain::OperationContext& context){
        static_cast<void>(requireOwner(config,context));const auto id=text(arguments,"prompt_id",128U);const auto& graph=arguments.at("graph");
        const auto queue=request({{"method","GET"},{"route","/queue"}},config,context);bool running=false,pending=false;
        for(const auto* kind:{"queue_running","queue_pending"})for(const auto& item:queue.value(kind,Json::array()))if(item.is_array()&&item.size()>2U&&item[1]==id){if(!promptGraphMatches(graph,item[2]))fail(Domain::ErrorCodes::IntegrityFailure,"Cancellation queue graph differs from the exact sealed prompt.");if(std::string_view{kind}=="queue_running")running=true;else pending=true;}
        if(!running&&!pending)return {{"ok",true},{"remote_cancel_confirmed",false},{"reason","Exact owned prompt is not queued or running; local publication is suppressed."}};
        static_cast<void>(requireOwner(config,context));
        if(running){if(queue.value("queue_running",Json::array()).size()!=1U)fail(Domain::ErrorCodes::Conflict,"Running cancellation is not exclusive to the exact sealed prompt.");
            const auto response=http(config.endpoint+"/interrupt","POST","application/json",Json{{"prompt_id",id}}.dump(),1024U*1024U,context);if(response.status!=200U)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Exact owned interruption was rejected.");}
        else static_cast<void>(request({{"method","POST"},{"route","/queue"},{"body",{{"delete",Json::array({id})}}}},config,context));
        for(;;){check(context);static_cast<void>(requireOwner(config,context));const auto observed=request({{"method","GET"},{"route","/queue"}},config,context);bool remains=false;
            for(const auto* kind:{"queue_running","queue_pending"})for(const auto& item:observed.value(kind,Json::array()))if(item.is_array()&&item.size()>2U&&item[1]==id){if(!promptGraphMatches(graph,item[2]))fail(Domain::ErrorCodes::IntegrityFailure,"Prompt changed while confirming cancellation.");remains=true;}
            if(!remains)return {{"ok",true},{"remote_cancel_confirmed",true},{"interruption_requested",running},{"pending_delete_requested",pending}};std::this_thread::sleep_for(std::chrono::milliseconds{100});}
    }
    Json progress(const Json& arguments,const Domain::ComfyUiConfig& config,const Domain::OperationContext& context){
        const auto id=text(arguments,"client_id",128U),action=text(arguments,"action",16U,false,"poll");if(!safeName(id))fail(Domain::ErrorCodes::InvalidRequest,"Invalid ComfyUI progress client identity.");
        std::lock_guard lock{progressMutex_};const auto key=config.endpoint+"/"+id;
        if(action=="close"){progress_.erase(key);return {{"ok",true},{"closed",true}};}
        if(action!="open"&&action!="poll")fail(Domain::ErrorCodes::InvalidRequest,"Progress action must be open, poll, or close.");
        if(!progress_.contains(key)){if(progress_.size()>=8U)fail(Domain::ErrorCodes::LimitExceeded,"ComfyUI progress connection limit reached.");auto url=config.endpoint;url.replace(0,4,"ws");progress_[key]=std::make_unique<Cdp>(url+"/ws?clientId="+encode(id),context);}
        if(action=="open")return {{"ok",true},{"connected",true},{"events",Json::array()}};
        Json events=Json::array();const auto prompt=text(arguments,"prompt_id",128U,false,"");
        for(unsigned count=0;count<32U;++count){const auto event=progress_.at(key)->receive(context);if(event.is_null())break;
            const auto data=event.value("data",Json::object());if(!prompt.empty()&&data.is_object()&&data.contains("prompt_id")&&data.at("prompt_id")!=prompt)continue;events.push_back(event);}
        return {{"ok",true},{"connected",true},{"events",events}};
    }
    Json status(const Domain::ComfyUiConfig& config,const Domain::OperationContext& context){
        Json result{{"ok",true},{"endpoint",config.endpoint},{"available",false},{"managed",false}};
        try{const auto home=installedHome(config);result["installation_path"]=pathText(home.parent_path());result["comfy_home"]=pathText(home);
            result["model_storage_path"]=config.modelStoragePath.empty()?pathText(home/L"models"):config.modelStoragePath;
            const auto categories=modelCategoryPaths(config,home),nodes=Json(runtimeCustomNodes(config,home));result["launch_configuration"]={{"original_extra_paths",originalExtraPaths(home,context)},{"model_category_paths",categories},{"custom_node_whitelist",nodes}};
            if(result.at("launch_configuration").dump().size()>48U*1024U){result["launch_configuration"]={{"inline_omitted",true},{"sha256",digest(result.at("launch_configuration").dump())},{"category_count",categories.size()},{"custom_node_count",nodes.size()}};}
        }catch(const Failure& error){result["installation_error"]=errorJson(error.error);}
        try{result["system_stats"]=request({{"method","GET"},{"route","/system_stats"}},config,context);
            if(result.at("system_stats").dump().size()>64U*1024U)result["system_stats"]={{"omitted",true},{"reason","Provider system statistics exceed the inline bound."},{"sha256",digest(result.at("system_stats").dump())}};
            const auto queue=request({{"method","GET"},{"route","/queue"}},config,context);Json summarized=Json::object();
            for(const auto* kind:{"queue_running","queue_pending"}){
                const auto entries=queue.value(kind,Json::array());if(!entries.is_array())fail(Domain::ErrorCodes::MalformedMessage,"Provider queue entries must be arrays.");
                summarized[kind]=Json::array();summarized[std::string{kind}+"_count"]=entries.size();
                for(const auto& entry:entries){if(summarized.at(kind).size()>=64U)break;
                    if(!entry.is_array()||entry.size()<3U||!entry[1].is_string()||entry[1].get_ref<const std::string&>().size()>128U)
                        summarized[kind].push_back({{"malformed",true}});
                    else summarized[kind].push_back({{"prompt_id",entry[1]},{"node_count",entry[2].is_object()?entry[2].size():0U},{"graph_sha256",digest(entry[2].dump())}});
                }
                summarized[std::string{kind}+"_omitted"]=entries.size()>64U?entries.size()-64U:0U;
            }
            result["queue"]=std::move(summarized);result["available"]=true;}
        catch(const Failure& error){result["error"]=errorJson(error.error);}
        const auto ledger=root_/L"runtime-owner.json";
        if(Fs::is_regular_file(ledger)){
            auto value=readJson(ledger);const auto pid=value.value("pid",0U);Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid)};
            FILETIME creation{},exit{},kernel{},user{};
            const auto time=process&&GetProcessTimes(process.get(),&creation,&exit,&kernel,&user)?(static_cast<std::uint64_t>(creation.dwHighDateTime)<<32)|creation.dwLowDateTime:0;
            const bool live=process&&WaitForSingleObject(process.get(),0)==WAIT_TIMEOUT&&time==value.value("creation_time",0ULL);
            Json publicOwner=Json::object();for(const auto* key:{"pid","creation_time","endpoint","installation","python","image_identity","model_storage_path","extra_paths_sha256","original_extra_paths","manager_disabled","output_directory","input_directory","log"})
                if(value.contains(key))publicOwner[key]=value.at(key);
            for(const auto* key:{"custom_node_whitelist","interpreter_path_configuration","model_category_paths"})if(value.contains(key)){
                publicOwner[std::string{key}+"_count"]=value.at(key).size();publicOwner[std::string{key}+"_sha256"]=digest(value.at(key).dump());
            }
            result["owner"]=std::move(publicOwner);result["owner_alive"]=live;
            if(live)try{static_cast<void>(requireOwner(config,context));result["managed"]=true;}catch(const Failure& error){result["ownership_error"]=errorJson(error.error);}
        }
        if(result.dump().size()>128U*1024U)fail(Domain::ErrorCodes::PayloadTooLarge,"Provider status exceeds the bounded inline delivery size.");
        return result;
    }
    Json control(const Json& arguments,const Domain::ComfyUiConfig& config,const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& context){
        const auto action=text(arguments,"action",16U);if(action=="status")return status(config,context);
        const auto providerLease=take(Detail::ProviderOperationLease::acquire(config.endpoint,context));
        const auto home=installedHome(config);execution(home/L"main.py",authority,context);
        std::unique_lock lock{ownerMutex_};auto current=status(config,context);
        if(action=="start"){
            if(current.value("owner_alive",false)&&!current.value("managed",false))fail(Domain::ErrorCodes::Conflict,"A live owned ComfyUI runtime differs from the selected launch configuration; inspect ownership_error before starting another runtime.");
            if(current.value("available",false)){current["started"]=false;current["borrowed"]=!current.value("managed",false);return current;}
            if(current.value("owner_alive",false))fail(Domain::ErrorCodes::Conflict,"Owned ComfyUI process is starting or unavailable; do not start a duplicate.");
            const auto python=runtimeInterpreter(home);
            if(!Fs::is_regular_file(python))fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Portable ComfyUI interpreter is unavailable.");
            execution(python,authority,context);internalDirectory(root_,root_,authority,context);internalDirectory(root_/L"outputs",root_,authority,context);internalDirectory(root_/L"inputs",root_,authority,context);
            internalDirectory(home.parent_path()/L"forge-managed"/L"custom_nodes",home.parent_path(),authority,context);
            Json extra{{"forge",{{"base_path",pathText(root_)},{"custom_nodes",pathText(home.parent_path()/L"forge-managed"/L"custom_nodes")}}}};
            if(!config.modelStoragePath.empty()){
                const Fs::path models{wide(config.modelStoragePath)};static_cast<void>(internalAuthorized(models,models,Domain::FileAccess::Read,authority,context));
                Json categories{{"base_path",pathText(models)}};const auto categoryPaths=modelCategoryPaths(config,home);for(const auto& [category,paths]:categoryPaths.items()){
                    std::vector<std::string> relativePaths;for(const auto& path:paths){const Fs::path location{wide(path.get<std::string>())};if(contained(models,location)&&location!=models){const auto relative=pathText(location.lexically_relative(models));if(std::find(relativePaths.begin(),relativePaths.end(),relative)==relativePaths.end())relativePaths.push_back(relative);}}
                    if(relativePaths.empty())continue;std::string value;for(const auto& path:relativePaths){if(!value.empty())value+='\n';value+=path;}categories[category]=std::move(value);
                }extra["forge_models"]=std::move(categories);
            }
            const auto extraPath=root_/L"extra-paths.yaml";writeJson(extraPath,extra);
            const auto originalPaths=originalExtraPaths(home,context),categoryPaths=modelCategoryPaths(config,home);const auto customNodes=runtimeCustomNodes(config,home);
            Handle originalConfiguration;if(originalPaths.at("present")==true){const auto path=home/L"extra_model_paths.yaml";static_cast<void>(internalAuthorized(path,home,Domain::FileAccess::Read,authority,context));originalConfiguration=Handle{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
                if(!originalConfiguration||fileFacts(originalConfiguration.get(),context).at("sha256")!=originalPaths.at("sha256"))fail(Domain::ErrorCodes::Conflict,"Original ComfyUI path configuration changed before launch.");}
            std::vector<std::string> launch{"-s",pathText(home/L"main.py"),"--windows-standalone-build","--disable-auto-launch","--disable-all-custom-nodes","--listen",config.endpoint.find("[::1]")!=std::string::npos?"::1":"127.0.0.1","--port",endpointPort(config.endpoint),"--input-directory",pathText(root_/L"inputs"),"--output-directory",pathText(root_/L"outputs"),"--extra-model-paths-config",pathText(extraPath)};
            if(!customNodes.empty()){launch.emplace_back("--whitelist-custom-nodes");launch.insert(launch.end(),customNodes.begin(),customNodes.end());}
            owner_=startProcess(python,launch,home.parent_path(),root_/L"runtime.log");
            writeJson(root_/L"runtime-owner.json",{{"pid",owner_.pid},{"creation_time",owner_.creationTime},{"endpoint",config.endpoint},{"installation",pathText(home)},
                {"python",pathText(python)},{"image_identity",fileFacts(python,context)},{"model_storage_path",config.modelStoragePath.empty()?pathText(home/L"models"):config.modelStoragePath},
                {"extra_paths_sha256",fileFacts(extraPath,context).at("sha256")},{"interpreter_path_configuration",interpreterPathConfiguration(python,context)},
                {"original_extra_paths",originalPaths},{"model_category_paths",categoryPaths},
                {"custom_node_whitelist",customNodes},{"manager_disabled",true},{"output_directory",pathText(root_/L"outputs")},{"input_directory",pathText(root_/L"inputs")},{"log",pathText(root_/L"runtime.log")}});
            while(WaitForSingleObject(owner_.process.get(),0)==WAIT_TIMEOUT){
                check(context);auto probe=context;probe.deadline=(std::min)(context.deadline,std::chrono::steady_clock::now()+std::chrono::seconds{3});
                auto observed=status(config,probe);if(observed.value("available",false)){static_cast<void>(requireOwner(config,context));observed["started"]=true;return observed;}std::this_thread::sleep_for(std::chrono::milliseconds{200});}
            fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Owned ComfyUI exited before readiness; inspect runtime.log.");
        }
        if(action!="stop"&&action!="restart")fail(Domain::ErrorCodes::InvalidRequest,"Runtime action must be start, stop, restart, or status.");
        if(!current.value("available",false))fail(Domain::ErrorCodes::Conflict,"Forge cannot stop ComfyUI without verified availability and a complete observed queue.");
        const auto ledger=requireOwner(config,context,false);const auto queues=request({{"method","GET"},{"route","/queue"}},config,context);if(!queues.at("queue_running").empty()||!queues.at("queue_pending").empty())fail(Domain::ErrorCodes::Conflict,"ComfyUI queue is not drained; owned restart/stop would disrupt jobs.");
        static_cast<void>(requireOwner(config,context,false));Handle process{OpenProcess(PROCESS_TERMINATE|SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,ledger.at("pid").get<DWORD>())};
        FILETIME created{},exit{},kernel{},user{};if(!process||!GetProcessTimes(process.get(),&created,&exit,&kernel,&user)||
            ((static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime)!=ledger.at("creation_time").get<std::uint64_t>())fail(Domain::ErrorCodes::Conflict,"ComfyUI process ownership changed before stop.");
        if(!TerminateProcess(process.get(),0)||WaitForSingleObject(process.get(),5000U)!=WAIT_OBJECT_0)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Owned ComfyUI shutdown was not confirmed.");
        owner_={};if(action=="restart"){lock.unlock();return control({{"action","start"}},config,authority,context);}
        return {{"ok",true},{"stopped",true}};
    }
    Json inspect(std::string_view value,const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& context){
        const auto path=authorized(Fs::path{wide(value)},Domain::FileAccess::Read,authority,context);
        auto object=take(Detail::openAuthorizedObject(path,Domain::FileAccess::Read,Native::MissingPathPolicy::Reject,context,GENERIC_READ,FILE_SHARE_READ));
        auto facts=fileFacts(object.handle.get(),context);facts["path"]=path.canonicalPath().value();facts["media_type"]="application/octet-stream";
        const auto extension=Fs::path{wide(value)}.extension().native();
        if(_wcsicmp(extension.c_str(),L".png")==0)facts["media_type"]="image/png";
        else if(_wcsicmp(extension.c_str(),L".jpg")==0||_wcsicmp(extension.c_str(),L".jpeg")==0)facts["media_type"]="image/jpeg";
        else if(_wcsicmp(extension.c_str(),L".mp4")==0)facts["media_type"]="video/mp4";
        else if(_wcsicmp(extension.c_str(),L".webm")==0)facts["media_type"]="video/webm";
        else if(_wcsicmp(extension.c_str(),L".wav")==0)facts["media_type"]="audio/wav";
        else if(_wcsicmp(extension.c_str(),L".mp3")==0)facts["media_type"]="audio/mpeg";
        else if(_wcsicmp(extension.c_str(),L".flac")==0)facts["media_type"]="audio/flac";
        else if(_wcsicmp(extension.c_str(),L".ogg")==0||_wcsicmp(extension.c_str(),L".opus")==0)facts["media_type"]="audio/ogg";
        return facts;
    }
    Json mediaFacts(const Fs::path& staged,std::string_view filename,const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& context){
        auto facts=inspect(pathText(staged),authority,context);const auto extension=Fs::path{wide(filename)}.extension().native();
        std::array<unsigned char,1024> signature{};std::size_t prefixBytes{};{std::ifstream stream{staged,std::ios::binary};
            if(!stream)fail(Domain::ErrorCodes::IntegrityFailure,"Generated media prefix cannot be read.");
            stream.read(reinterpret_cast<char*>(signature.data()),static_cast<std::streamsize>(signature.size()));prefixBytes=static_cast<std::size_t>(stream.gcount());
            if(stream.bad())fail(Domain::ErrorCodes::IntegrityFailure,"Generated media prefix read failed.");}
        const bool png=signature[0]==0x89U&&signature[1]=='P'&&signature[2]=='N'&&signature[3]=='G'&&signature[4]==0x0dU&&signature[5]==0x0aU&&signature[6]==0x1aU&&signature[7]==0x0aU;
        const bool jpeg=signature[0]==0xffU&&signature[1]==0xd8U&&signature[2]==0xffU;
        const bool webp=signature[0]=='R'&&signature[1]=='I'&&signature[2]=='F'&&signature[3]=='F'&&signature[8]=='W'&&signature[9]=='E'&&signature[10]=='B'&&signature[11]=='P';
        const bool animated=(png&&animatedPng(staged))||(webp&&signature[12]=='V'&&signature[13]=='P'&&signature[14]=='8'&&signature[15]=='X'&&(signature[20]&0x02U)!=0U);
        const bool image=(png||jpeg||webp)&&!animated;
        const bool mediaExtension=_wcsicmp(extension.c_str(),L".mp4")==0||_wcsicmp(extension.c_str(),L".webm")==0||_wcsicmp(extension.c_str(),L".mov")==0||_wcsicmp(extension.c_str(),L".mkv")==0||_wcsicmp(extension.c_str(),L".gif")==0||
            _wcsicmp(extension.c_str(),L".wav")==0||_wcsicmp(extension.c_str(),L".mp3")==0||_wcsicmp(extension.c_str(),L".flac")==0||_wcsicmp(extension.c_str(),L".ogg")==0||_wcsicmp(extension.c_str(),L".opus")==0;
        const bool mediaSignature=(signature[4]=='f'&&signature[5]=='t'&&signature[6]=='y'&&signature[7]=='p')||
            (signature[0]==0x1aU&&signature[1]==0x45U&&signature[2]==0xdfU&&signature[3]==0xa3U)||
            (signature[0]=='G'&&signature[1]=='I'&&signature[2]=='F'&&signature[3]=='8')||
            (signature[0]=='R'&&signature[1]=='I'&&signature[2]=='F'&&signature[3]=='F')||
            (signature[0]=='O'&&signature[1]=='g'&&signature[2]=='g'&&signature[3]=='S')||
            (signature[0]=='f'&&signature[1]=='L'&&signature[2]=='a'&&signature[3]=='C')||
            (signature[0]=='I'&&signature[1]=='D'&&signature[2]=='3')||(signature[0]==0xffU&&(signature[1]&0xe0U)==0xe0U);
        const auto preview=[&](const Fs::path& source){
            const auto path=authorized(source,Domain::FileAccess::Read,authority,context);auto opened=take(Detail::openAuthorizedObject(path,Domain::FileAccess::Read,Native::MissingPathPolicy::Reject,context,GENERIC_READ,FILE_SHARE_READ));
            facts.update(imagePreviewFacts(opened.handle.get(),context));
        };
        if(image){preview(staged);facts["media_type"]=png?"image/png":webp?"image/webp":"image/jpeg";}
        else if(mediaExtension||mediaSignature||animated){
            const auto probe=component(L"ffprobe.exe",context),ffmpeg=component(L"ffmpeg.exe",context);if(probe.empty()||ffmpeg.empty())fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Verified video/audio delivery needs ffmpeg and ffprobe; run automatic preparation before rendering.");
            execution(probe,authority,context);execution(ffmpeg,authority,context);Infrastructure::Windows::WindowsUuidGenerator ids;const auto suffix=wide(take(ids.next()).value());
            const auto logs=internalDirectory(root_/L"inspection",root_,authority,context);
            const auto observed=runProcess(probe,{"-v","error","-count_frames","-show_format","-show_streams","-of","json",pathText(staged)},probe.parent_path(),logs/(suffix+L".probe.log"),context);
            if(observed.at("exit_code")!=0)fail(Domain::ErrorCodes::IntegrityFailure,"Generated media failed ffprobe inspection; retained staging and probe log.");
            const auto metadata=parse(observed.at("output").get<std::string>());bool hasVideo=false,hasAudio=false;Json normalized{{"decoded",true},{"width",0U},{"height",0U},{"frame_count",0ULL},{"duration",0.0},{"raw",metadata}};
            for(const auto& stream:metadata.value("streams",Json::array())){hasVideo=hasVideo||stream.value("codec_type",std::string{})=="video";hasAudio=hasAudio||stream.value("codec_type",std::string{})=="audio";
                if(stream.value("codec_type",std::string{})=="video"){normalized["width"]=stream.value("width",0U);normalized["height"]=stream.value("height",0U);normalized["codec"]=stream.value("codec_name",std::string{});
                    const auto frames=stream.value("nb_read_frames",std::string{});if(!frames.empty()&&frames.find_first_not_of("0123456789")==std::string::npos)normalized["frame_count"]=std::stoull(frames);}}
            const auto format=metadata.value("format",Json::object());const auto duration=format.value("duration",std::string{});if(!duration.empty()&&duration!="N/A")normalized["duration"]=std::stod(duration);
            if(!hasVideo&&!hasAudio)fail(Domain::ErrorCodes::IntegrityFailure,"Generated media has no decodable video or audio stream.");
            const auto validation=runProcess(ffmpeg,{"-nostdin","-v","error","-xerror","-i",pathText(staged),"-f","null","NUL"},ffmpeg.parent_path(),logs/(suffix+L".full-decode.log"),context);
            if(validation.at("exit_code")!=0)fail(Domain::ErrorCodes::IntegrityFailure,"Generated media failed complete stream decoding; retained staging and decode log.");
            const auto contentType=animated?(png?"image/apng":"image/webp"):mediaContentType(std::span{signature.data(),prefixBytes},format,hasVideo);
            if(hasVideo){const auto poster=staged.parent_path()/(staged.filename().native()+L".preview.png");static_cast<void>(authorized(poster,Domain::FileAccess::Create,authority,context,true));
                const auto decoded=runProcess(ffmpeg,{"-nostdin","-v","error","-i",pathText(staged),"-frames:v","1","-vf","scale=768:768:force_original_aspect_ratio=decrease","-c:v","png","-f","image2",pathText(poster)},ffmpeg.parent_path(),logs/(suffix+L".decode.log"),context);
                if(decoded.at("exit_code")!=0)fail(Domain::ErrorCodes::IntegrityFailure,"Generated video frame failed decoding; retained staging and decode log.");preview(poster);
                facts["media_type"]=contentType;
                const auto seconds=normalized.at("duration").get<double>();
                if(std::isfinite(seconds)&&seconds>0.0&&normalized.at("frame_count").get<std::uint64_t>()>=4U){
                    const auto sheet=staged.parent_path()/(staged.filename().native()+L".contact.png");static_cast<void>(authorized(sheet,Domain::FileAccess::Create,authority,context,true));
                    const auto sampled=runProcess(ffmpeg,{"-nostdin","-v","error","-i",pathText(staged),"-vf","fps="+std::to_string(4.0/seconds)+",scale=384:384:force_original_aspect_ratio=decrease,tile=2x2","-frames:v","1","-c:v","png","-f","image2",pathText(sheet)},ffmpeg.parent_path(),logs/(suffix+L".contact.log"),context);
                    if(sampled.at("exit_code")!=0||!Fs::is_regular_file(sheet))fail(Domain::ErrorCodes::IntegrityFailure,"Sampled video contact sheet failed; retained staging and decode log.");
                    facts["contact_sheet_staging"]=pathText(sheet);
                }
            }else{const auto decoded=runProcess(ffmpeg,{"-nostdin","-v","error","-i",pathText(staged),"-t","1","-f","null","NUL"},ffmpeg.parent_path(),logs/(suffix+L".decode.log"),context);
                if(decoded.at("exit_code")!=0)fail(Domain::ErrorCodes::IntegrityFailure,"Generated audio failed decoding; retained staging and decode log.");
                facts["media_type"]=contentType;}
            facts["metadata"]=normalized;facts["metadata_method"]="external_ffprobe_and_ffmpeg_full_decode";
        }else if(_wcsicmp(extension.c_str(),L".png")==0||_wcsicmp(extension.c_str(),L".jpg")==0||_wcsicmp(extension.c_str(),L".jpeg")==0||_wcsicmp(extension.c_str(),L".webp")==0)fail(Domain::ErrorCodes::IntegrityFailure,"Generated image does not match a supported image signature.");
        return facts;
    }
    Fs::path component(std::wstring_view name,const Domain::OperationContext& context){
        const auto environment=root_/L"active-environment.json";
        if(Fs::is_regular_file(environment)){
            const auto dependencies=readJson(environment).value("dependencies",Json::array());
            if(!dependencies.is_array()||dependencies.size()>4096U)fail(Domain::ErrorCodes::IntegrityFailure,"Active component dependencies must be a bounded array.");
            for(auto item=dependencies.rbegin();item!=dependencies.rend();++item){
                if(!item->is_object()||!item->contains("kind")||!item->at("kind").is_string())fail(Domain::ErrorCodes::IntegrityFailure,"Active component dependency entries require an identified kind.");
                if(item->value("kind",std::string{})!="component")continue;const Fs::path directory{wide(text(*item,"target"))};
                if(!directory.is_absolute()||!contained(root_/L"components",directory)||directory.filename().native().ends_with(L".disabled")||!item->contains("tree")||!item->at("tree").is_object())fail(Domain::ErrorCodes::IntegrityFailure,"Active media component has no sealed owned installation.");
                const auto marker=readJson(directory/L".forge-dependency.json");
                if(marker.value("sha256",std::string{})!=text(*item,"sha256",64U)||marker.value("tree",Json::object())!=item->at("tree"))fail(Domain::ErrorCodes::IntegrityFailure,"Active media component installation marker changed.");
                const auto observed=directoryFacts(directory,MaximumArtifactBytes,context);if(observed!=item->at("tree"))fail(Domain::ErrorCodes::IntegrityFailure,"Active media component files changed; renew its installation and preview.");
                for(const auto& [relative,facts]:observed.items()){static_cast<void>(facts);auto path=directory/wide(relative);path.make_preferred();
                    if(_wcsicmp(path.filename().c_str(),std::wstring{name}.c_str())==0)return path;}
            }
        }
        const auto installed=executableOnPath(name);
        if(!installed.empty()&&contained(root_/L"components",installed))fail(Domain::ErrorCodes::IntegrityFailure,"An inactive managed media component appeared on PATH.");
        return installed;
    }
    Json sealInput(const Json& arguments,const Domain::ComfyUiConfig& config,const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& context){
        static_cast<void>(requireOwner(config,context));
        const auto& descriptor=arguments.at("descriptor");const auto name=text(descriptor,"name",240U),folder=text(descriptor,"subfolder",4096U);
        const auto owned=text(arguments,"namespace",4096U);const auto prefix="ForgeConductor/"+authority.projectId().value()+"/";
        if(!owned.starts_with(prefix)||!Domain::OperationId::parse(owned.substr(prefix.size()))||folder!=owned||!safeName(name)||descriptor.value("type",std::string{})!="input")
            fail(Domain::ErrorCodes::IntegrityFailure,"Input descriptor differs from the exact owned project/job namespace.");
        auto inputPath=root_/L"inputs"/wide(folder)/wide(name);inputPath.make_preferred();
        const auto path=internalAuthorized(inputPath,root_/L"inputs",Domain::FileAccess::Read,authority,context);
        auto file=take(Detail::openAuthorizedObject(path,Domain::FileAccess::Read,Native::MissingPathPolicy::Reject,context,GENERIC_READ,FILE_SHARE_READ));
        return fileFacts(file.handle.get(),context);
    }
    Json upload(const Json& arguments,const Domain::ComfyUiConfig& config,const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& context){
        static_cast<void>(requireOwner(config,context));
        const auto& inputs=arguments.at("inputs");if(!inputs.is_array()||inputs.size()>64U)fail(Domain::ErrorCodes::InvalidRequest,"ComfyUI uploads require at most 64 input bindings.");
        Json result{{"bindings",Json::array()},{"inputs",Json::array()}};
        // Validate every source before the first provider effect.
        std::vector<Contracts::AuthorizedPath> paths;for(const auto& input:inputs){text(input,"node_id",128U);text(input,"input",128U,false,"image");paths.push_back(authorized(Fs::path{wide(text(input,"path"))},Domain::FileAccess::Read,authority,context));}
        for(std::size_t i=0;i<inputs.size();++i){
            const auto& input=inputs[i];auto file=take(Detail::openAuthorizedObject(paths[i],Domain::FileAccess::Read,Native::MissingPathPolicy::Reject,context,GENERIC_READ,FILE_SHARE_READ));
            auto facts=fileFacts(file.handle.get(),context);const auto bytes=facts.at("bytes").get<std::uint64_t>();if(bytes>MAXDWORD-4096U)fail(Domain::ErrorCodes::PayloadTooLarge,"ComfyUI upload exceeds 4 GiB.");
            const auto basename=Fs::path{wide(paths[i].canonicalPath().value())}.filename();
            if(!safeName(pathText(basename)))fail(Domain::ErrorCodes::InvalidRequest,"ComfyUI upload filename is unsafe.");
            const auto subfolder="ForgeConductor/"+authority.projectId().value()+"/"+context.operationId.value();
            const auto boundary="ForgeComfy"+context.operationId.value();
            const auto prefix="--"+boundary+"\r\nContent-Disposition: form-data; name=\"type\"\r\n\r\ninput\r\n--"+boundary+"\r\nContent-Disposition: form-data; name=\"subfolder\"\r\n\r\n"+subfolder+"\r\n--"+boundary+"\r\nContent-Disposition: form-data; name=\"image\"; filename=\""+std::to_string(i)+"_"+pathText(basename)+"\"\r\nContent-Type: application/octet-stream\r\n\r\n";
            const auto suffix="\r\n--"+boundary+"--\r\n";
            const auto response=http(config.endpoint+"/upload/image","POST","multipart/form-data; boundary="+boundary,"",1024U*1024U,context,nullptr,0U,{},file.handle.get(),bytes,prefix,suffix);
            if(response.status!=200U)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"ComfyUI input upload returned HTTP "+std::to_string(response.status));
            const auto descriptor=parse(response.body);const auto name=text(descriptor,"name",240U),folder=text(descriptor,"subfolder",4096U);
            if(!safeName(name)||folder!=subfolder||descriptor.value("type",std::string{})!="input")fail(Domain::ErrorCodes::IntegrityFailure,"ComfyUI upload descriptor disagrees with the owned namespace.");
            const auto after=fileFacts(file.handle.get(),context);if(after.at("sha256")!=facts.at("sha256"))fail(Domain::ErrorCodes::Conflict,"Upload source changed during provider transfer.");
            const auto provider=sealInput({{"descriptor",descriptor},{"namespace",subfolder}},config,authority,context);
            if(provider.at("sha256")!=facts.at("sha256")||provider.at("bytes")!=facts.at("bytes"))fail(Domain::ErrorCodes::IntegrityFailure,"Private provider upload differs from the sealed input source.");
            const auto nodeId=text(input,"node_id",128U),inputName=text(input,"input",128U,false,"image"),value=folder+"/"+name;
            facts["path"]=paths[i].canonicalPath().value();facts["descriptor"]=descriptor;facts["node_id"]=nodeId;facts["input"]=inputName;facts["value"]=value;
            facts["provider_sha256"]=provider.at("sha256");facts["provider_bytes"]=provider.at("bytes");result["inputs"].push_back(std::move(facts));
            result["bindings"].push_back({{"node_id",nodeId},{"input",inputName},{"value",value}});
        }
        return result;
    }
    Json collect(const Json& arguments,const Domain::ComfyUiConfig& config,const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& context){
        static_cast<void>(requireOwner(config,context));
        const auto directory=writableDirectory(Fs::path{wide(text(arguments,"output_directory"))},authority,context);
        const auto reserve=[&](std::uint64_t incoming=0ULL){ULARGE_INTEGER free{},total{},available{};
            if(!GetDiskFreeSpaceExW(directory.c_str(),&free,&total,&available))fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot measure artifact staging disk space.");
            if(incoming>UINT64_MAX-config.freeSpaceReserveBytes||free.QuadPart<config.freeSpaceReserveBytes+incoming)fail(Domain::ErrorCodes::LimitExceeded,"ComfyUI artifact staging reached the configured free-space reserve.");};
        reserve();
        const auto owned=text(arguments,"namespace",4096U);if(owned.find("..")!=std::string::npos||owned.find_first_of("\\:\r\n")!=std::string::npos)fail(Domain::ErrorCodes::InvalidRequest,"Invalid ComfyUI artifact namespace.");
        const auto& outputs=arguments.at("outputs");if(!outputs.is_object())fail(Domain::ErrorCodes::InvalidRequest,"ComfyUI history outputs must be an object.");
        Json artifacts=Json::array(),unresolved=Json::array();std::set<std::string> outputNodes;
        struct Publication {Fs::path temporary,destination,receipt;Json transfer,facts;bool attempted{},moved{};};std::vector<Publication> publications;
        const auto compareDestination=[](const std::wstring& left,const std::wstring& right){
            const auto compared=::CompareStringOrdinal(left.c_str(),-1,right.c_str(),-1,TRUE);
            if(!compared)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot compare canonical artifact destination identities.");
            return compared==CSTR_LESS_THAN;
        };
        std::set<std::wstring,decltype(compareDestination)> destinations{compareDestination};
        const auto claimDestination=[&](const Contracts::AuthorizedPath& destination){
            if(!destinations.insert(wide(destination.canonicalPath().value())).second)
                fail(Domain::ErrorCodes::Conflict,"Multiple provider artifacts map to the same canonical output path; no batch files were published: "+destination.canonicalPath().value());
        };
        for(const auto& [node,value]:outputs.items()){
            if(!safeName(node))fail(Domain::ErrorCodes::MalformedMessage,"Provider output node identity is unsafe.");
            if(!value.is_object()){unresolved.push_back({{"node_id",node},{"reason","Output has no file descriptor object."}});continue;}
            for(const auto& [kind,items]:value.items()){
                if(!items.is_array())continue;
                for(const auto& descriptor:items){
                    if(!descriptor.is_object()||!descriptor.contains("filename"))continue;
                    const auto filename=text(descriptor,"filename",240U),folder=text(descriptor,"subfolder",4096U,false,"");
                    auto comparableFolder=folder;std::replace(comparableFolder.begin(),comparableFolder.end(),'\\','/');
                    if(!safeName(filename)||descriptor.value("type",std::string{})!="output"||(comparableFolder!=owned&&!comparableFolder.starts_with(owned+"/"))||comparableFolder.find("..")!=std::string::npos){
                        unresolved.push_back({{"node_id",node},{"descriptor",descriptor},{"reason","File descriptor is outside the sealed owned output namespace."}});continue;}
                    if(publications.size()>=256U)fail(Domain::ErrorCodes::LimitExceeded,"ComfyUI artifact count exceeds 256.");
                    const auto outputName=node+"_"+filename;
                    const auto destination=directory/wide(outputName.size()<=180U?outputName:node.substr(0U,48U)+"_"+digest(outputName).substr(0U,24U)+pathText(Fs::path{wide(filename)}.extension()));claimDestination(authorized(destination,Domain::FileAccess::Write,authority,context,true));
                    if(!Fs::exists(destination))static_cast<void>(authorized(destination,Domain::FileAccess::Create,authority,context,true));
                    const auto transfers=internalDirectory(root_/L"transfers",root_,authority,context);
                    const auto transferKey=digest(Json{{"prompt_id",text(arguments,"prompt_id",128U)},{"node_id",node},{"descriptor",descriptor},{"destination",pathText(destination)}}.dump());
                    const auto transferPath=transfers/(wide(transferKey)+L".json");
                    Json transfer{{"prompt_id",text(arguments,"prompt_id",128U)},{"node_id",node},{"descriptor",descriptor},{"destination",pathText(destination)},
                        {"downloaded_bytes",0ULL},{"attempts",Json::array()}};
                    if(Fs::is_regular_file(transferPath)){const auto previous=readJson(transferPath);if(previous.at("prompt_id")!=transfer.at("prompt_id")||previous.at("descriptor")!=descriptor||previous.at("destination")!=transfer.at("destination"))fail(Domain::ErrorCodes::IntegrityFailure,"Artifact transfer receipt identity changed.");transfer=previous;}
                    Infrastructure::Windows::WindowsUuidGenerator ids;const auto attempt=take(ids.next()).value();
                    const auto temporary=directory/(L".forge-comfy-"+wide(transferKey)+L"."+wide(attempt)+L".part");regularParents(temporary);
                    static_cast<void>(authorized(temporary,Domain::FileAccess::Create,authority,context,true));
                    transfer["state"]="transferring";transfer["attempts"].push_back({{"id",attempt},{"path",pathText(temporary)},{"bytes",0ULL},{"state","transferring"}});writeJson(transferPath,transfer);
                    Handle file{CreateFileW(temporary.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
                    if(!file)fail(Domain::ErrorCodes::Conflict,"Artifact transfer path already exists; inspect retained transfer evidence.");
                    const auto route="/view?filename="+encode(filename)+"&subfolder="+encode(folder)+"&type=output";
                    HttpResult response;
                    try{response=http(config.endpoint+route,"GET","","",1024U*1024U,context,file.get(),MaximumArtifactBytes,[&](std::uint64_t count){
                        transfer["downloaded_bytes"]=transfer.at("downloaded_bytes").get<std::uint64_t>()+count;
                        transfer["attempts"].back()["bytes"]=transfer["attempts"].back().at("bytes").get<std::uint64_t>()+count;
                        writeJson(transferPath,transfer);reserve(count);
                    });}catch(...){transfer["state"]="interrupted";transfer["attempts"].back()["state"]="interrupted";writeJson(transferPath,transfer);throw;}
                    if(response.status!=200U)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"ComfyUI artifact download returned HTTP "+std::to_string(response.status));
                    const auto transferred=fileFacts(file.get(),context);file.reset();auto facts=mediaFacts(temporary,filename,authority,context);
                    if(facts.at("sha256")!=transferred.at("sha256"))fail(Domain::ErrorCodes::IntegrityFailure,"Staged artifact changed during decoding.");
                    if(Fs::exists(destination)){
                        const auto prior=inspect(pathText(destination),authority,context);if(prior.at("sha256")!=facts.at("sha256"))fail(Domain::ErrorCodes::Conflict,"Existing output differs from the exact provider artifact.");
                    }
                    facts["node_id"]=node;facts["descriptor"]=descriptor;facts["history_key"]=kind;facts["provider_view_url"]=config.endpoint+route;
                    transfer["state"]="verified";transfer["attempts"].back()["state"]="verified";transfer["artifact"]=facts;writeJson(transferPath,transfer);
                    if(!response.contentType.empty())facts["provider_media_type"]=response.contentType;
                    publications.push_back({temporary,destination,transferPath,transfer,facts});outputNodes.insert(node);
                }
            }
        }
        for(const auto& expected:arguments.value("expected_outputs",Json::array()))if(expected.is_string()&&!outputNodes.contains(expected.get<std::string>()))unresolved.push_back({{"node_id",expected},{"reason","Expected output has no published owned file."}});
        if(!unresolved.empty())return {{"ok",false},{"artifacts",artifacts},{"unresolved",unresolved},{"prompt_id",text(arguments,"prompt_id",128U)},{"verified_staging_count",publications.size()}};
        std::vector<Publication> sampledFrames;
        for(auto& item:publications) if(item.facts.contains("contact_sheet_staging")) {
            const auto staged=Fs::path{wide(item.facts.at("contact_sheet_staging").get<std::string>())};
            const auto destination=directory/(item.destination.filename().native()+L".contact.png");
            claimDestination(authorized(destination,Domain::FileAccess::Write,authority,context,true));
            if(!Fs::exists(destination))static_cast<void>(authorized(destination,Domain::FileAccess::Create,authority,context,true));
            auto facts=mediaFacts(staged,"contact.png",authority,context);
            facts["node_id"]=item.facts.at("node_id");facts["descriptor"]=item.facts.at("descriptor");facts["role"]="sampled_video_contact_sheet";
            facts["source_sha256"]=item.facts.at("sha256");facts["sampled_frames"]=4U;
            const auto receipt=item.receipt.parent_path()/(wide(digest(pathText(item.receipt)+"/contact-sheet"))+L".json");
            Json transfer{{"state","verified"},{"source_sha256",item.facts.at("sha256")},{"artifact",facts},{"destination",pathText(destination)}};
            writeJson(receipt,transfer);sampledFrames.push_back({staged,destination,receipt,transfer,facts});item.facts.erase("contact_sheet_staging");
        }
        for(auto& sample:sampledFrames) publications.push_back(std::move(sample));
        // Every intended descriptor and media decoder has passed before any
        // final filename becomes visible. Recheck each pinned transfer now.
        for(const auto& item:publications){if(inspect(pathText(item.temporary),authority,context).at("sha256")!=item.facts.at("sha256"))fail(Domain::ErrorCodes::IntegrityFailure,"Verified staged output changed before publication.");
            if(Fs::exists(item.destination)&&inspect(pathText(item.destination),authority,context).at("sha256")!=item.facts.at("sha256"))fail(Domain::ErrorCodes::Conflict,"Output destination changed before publication.");}
        if(publications.empty())return {{"ok",false},{"artifacts",artifacts},{"unresolved",unresolved},{"prompt_id",text(arguments,"prompt_id",128U)}};
        Infrastructure::Windows::WindowsUuidGenerator batchIds;const auto batchId=take(batchIds.next()).value();
        const auto batchReceipt=publications.front().receipt.parent_path()/(L"batch-"+wide(digest(Json{{"prompt_id",arguments.at("prompt_id")},{"output_directory",pathText(directory)},{"outputs",outputs}}.dump()))+L"."+wide(batchId)+L".json");
        const auto entryEvidence=[&](const Publication& item){Json value{{"node_id",item.facts.at("node_id")},{"descriptor",item.facts.at("descriptor")},
            {"destination",pathText(item.destination)},{"staging_path",pathText(item.temporary)},{"transfer_receipt",pathText(item.receipt)},
            {"sha256",item.facts.at("sha256")},{"bytes",item.facts.at("bytes")},{"media_type",item.facts.at("media_type")},
            {"publication_attempted",item.attempted},{"moved_by_this_attempt",item.moved}};
            if(item.facts.contains("role"))value["role"]=item.facts.at("role");return value;};
        Json batch{{"batch_id",batchId},{"prompt_id",arguments.at("prompt_id")},{"state","publishing"},{"publication_evidence",Json::array()}};
        for(const auto& item:publications){auto evidence=entryEvidence(item);evidence["state"]="verified_staging";batch["publication_evidence"].push_back(std::move(evidence));}
        try {
            writeJson(batchReceipt,batch);
            std::size_t index{};
            for(auto& item:publications){check(context);regularParents(item.destination);item.attempted=true;
                if(Fs::exists(item.destination)){
                    if(inspect(pathText(item.destination),authority,context).at("sha256")!=item.facts.at("sha256"))fail(Domain::ErrorCodes::Conflict,"Output destination changed during publication; verified staging retained: "+pathText(item.temporary));
                    if(!DeleteFileW(item.temporary.c_str()))fail(Domain::ErrorCodes::InternalFailure,"Cannot retire verified duplicate transfer.");}
                else {if(!MoveFileExW(item.temporary.c_str(),item.destination.c_str(),MOVEFILE_WRITE_THROUGH))fail(Domain::ErrorCodes::Conflict,"Artifact publication failed or the destination appeared during transfer.");item.moved=true;}
                auto published=inspect(pathText(item.destination),authority,context);
                if(published.at("sha256")!=item.facts.at("sha256") || published.at("bytes")!=item.facts.at("bytes"))fail(Domain::ErrorCodes::IntegrityFailure,"Published artifact differs from its verified content; inspect transfer evidence: "+pathText(item.receipt));
                published.erase("media_type");item.facts.update(published);item.transfer["state"]="published";item.transfer["artifact"]=item.facts;writeJson(item.receipt,item.transfer);
                auto evidence=entryEvidence(item);evidence["state"]="published_verified";batch["publication_evidence"][index++]=std::move(evidence);writeJson(batchReceipt,batch);artifacts.push_back(item.facts);
            }
            batch["state"]="completed";writeJson(batchReceipt,batch);
            const auto manifest=fileFacts(batchReceipt,context);
            return {{"ok",true},{"artifacts",artifacts},{"unresolved",unresolved},{"prompt_id",arguments.at("prompt_id")},{"published_count",artifacts.size()},
                {"publication_manifest",{{"path",pathText(batchReceipt)},{"sha256",manifest.at("sha256")},{"bytes",manifest.at("bytes")}}}};
        } catch(...) {
            Domain::Error cause;try{throw;}catch(const Failure& failure){cause=failure.error;}catch(const std::exception& failure){cause=Domain::makeError(Domain::ErrorCodes::InternalFailure,failure.what());}catch(...){cause=Domain::makeError(Domain::ErrorCodes::InternalFailure,"Artifact publication failed.");}
            auto cleanup=context;cleanup.cancellation={};cleanup.deadline=std::chrono::steady_clock::now()+std::chrono::seconds{10};
            Json evidence=Json::array();std::size_t published{},uncertain{},staged{};bool partial{};
            for(const auto& item:publications){auto value=entryEvidence(item);partial=partial||item.moved;
                try {check(cleanup);std::error_code pathError;const auto exists=Fs::exists(item.destination,pathError);if(pathError)fail(Domain::ErrorCodes::IntegrityFailure,"Cannot observe publication destination: "+pathError.message());
                    if(exists){const auto actual=inspect(pathText(item.destination),authority,cleanup);value["observed_sha256"]=actual.at("sha256");value["observed_bytes"]=actual.at("bytes");
                        if(actual.at("sha256")==item.facts.at("sha256")&&actual.at("bytes")==item.facts.at("bytes")){value["state"]="published_verified";++published;partial=true;}
                        else {value["state"]="destination_changed";++uncertain;}}
                    else if(Fs::exists(item.temporary)){const auto actual=inspect(pathText(item.temporary),authority,cleanup);
                        if(actual.at("sha256")==item.facts.at("sha256")&&actual.at("bytes")==item.facts.at("bytes")){value["state"]="verified_staging";++staged;}else{value["state"]="staging_changed";++uncertain;}}
                    else value["state"]="destination_and_staging_absent";
                }catch(const Failure& failure){value["state"]="publication_unverifiable";value["inspection_error"]=errorJson(failure.error);++uncertain;}
                catch(const std::exception& failure){value["state"]="publication_unverifiable";value["inspection_error"]={{"code",Domain::ErrorCodes::InternalFailure},{"message",failure.what()}};++uncertain;}
                evidence.push_back(std::move(value));
            }
            Json result{{"ok",false},{"partial",partial},{"batch_id",batchId},{"artifacts",Json::array()},{"prompt_id",arguments.at("prompt_id")},{"error",errorJson(cause)},
                {"publication_evidence",evidence},{"published_count",published},{"publication_uncertain_count",uncertain},{"verified_staging_count",staged}};
            batch=result;batch["state"]="failed";
            try {check(cleanup);writeJson(batchReceipt,batch);const auto manifest=fileFacts(batchReceipt,cleanup);result["publication_manifest"]={{"path",pathText(batchReceipt)},{"sha256",manifest.at("sha256")},{"bytes",manifest.at("bytes")}};}
            catch(const Failure& failure){result["publication_manifest"]={{"path",pathText(batchReceipt)},{"verified",false},{"error",errorJson(failure.error)}};}
            catch(const std::exception& failure){result["publication_manifest"]={{"path",pathText(batchReceipt)},{"verified",false},{"error",{{"code",Domain::ErrorCodes::InternalFailure},{"message",failure.what()}}}};}
            return result;
        }
    }
    Json identity(const Json& arguments,const Domain::ComfyUiConfig& config,const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& context){
        const auto home=installedHome(config);static_cast<void>(internalAuthorized(home,home.parent_path(),Domain::FileAccess::Read,authority,context));
        const auto ownerPath=root_/L"runtime-owner.json";if(Fs::is_regular_file(ownerPath)){const auto owner=readJson(ownerPath);Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,owner.value("pid",0U))};
            if(process&&WaitForSingleObject(process.get(),0)==WAIT_TIMEOUT)static_cast<void>(requireOwner(config,context));}
        Json identity{{"installation",pathText(home)},{"endpoint",config.endpoint},{"configuration",{{"model_storage_path",config.modelStoragePath},{"quality_preference",config.qualityPreference}}},
            {"method","schema_sha256_source_content_sha256_package_RECORD_sha256_model_content_sha256_file_identity_size_mtime_media_component_content_sha256"},{"nodes",Json::object()},{"models",Json::array()},{"inputs",Json::array()},{"source_files",Json::array()},{"packages",Json::array()}};
        std::set<std::string> classes;
        for(const auto& graph:arguments.value("workflows",Json::array()))if(graph.is_object())for(const auto& [id,node]:graph.items()){
            static_cast<void>(id);if(!node.is_object())continue;const auto name=node.value("class_type",std::string{});if(!name.empty())classes.insert(name);
        }
        Json schemas=Json::object();for(const auto& name:classes){const auto schema=request({{"method","GET"},{"route","/object_info/"+encode(name)}},config,context);identity["nodes"][name]=digest(schema.dump());
            if(schema.is_object()&&schema.contains(name))schemas[name]=schema.at(name);}
        const auto manager=home/L"custom_nodes"/L"ComfyUI-Manager";
        for(const auto& source:{manager/L"model-list.json",manager/L"glob"/L"manager_server.py",home/L"folder_paths.py",home/L"extra_model_paths.yaml"})
            if(Fs::is_regular_file(source))static_cast<void>(internalAuthorized(source,home,Domain::FileAccess::Read,authority,context));
        const auto catalog=modelPublisherCatalog(home,context),categoryPaths=modelCategoryPaths(config,home);const auto roots=modelRoots(config,home);std::vector<Fs::path> authorizedRoots;
        identity["configuration"]["model_category_paths_sha256"]=digest(categoryPaths.dump());identity["configuration"]["original_extra_paths"]=originalExtraPaths(home,context);
        for(const auto& root:roots)if(Fs::is_directory(root)){static_cast<void>(internalAuthorized(root,root,Domain::FileAccess::Read,authority,context));authorizedRoots.push_back(root);}
        const auto less=[](const Fs::path& left,const Fs::path& right){const auto compared=CompareStringOrdinal(left.c_str(),-1,right.c_str(),-1,TRUE);
            if(!compared)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot compare selected model file identities.");return compared==CSTR_LESS_THAN;};
        std::set<Fs::path,decltype(less)> files{less};
        for(const auto& graph:arguments.value("workflows",Json::array()))if(graph.is_object())for(const auto& choice:referencedModelChoices(graph,schemas,catalog,true)){
            const auto selected=selectedModelFiles(choice,authorizedRoots,context,categoryPaths);
            if(selected.empty()&&!choice.at("catalog_matches").empty())fail(Domain::ErrorCodes::IntegrityFailure,"Requested model has no sealed local file identity in its declared storage category: "+text(choice,"selected"));
            for(auto path:selected){path=path.lexically_normal();path.make_preferred();files.insert(std::move(path));}
        }
        bool embeddingReference=false;
        for(const auto& graph:arguments.value("workflows",Json::array()))if(graph.is_object())for(const auto& [id,node]:graph.items()){
            static_cast<void>(id);if(!node.is_object()||!node.contains("inputs")||!node.at("inputs").is_object())continue;const auto name=node.value("class_type",std::string{});if(!schemas.contains(name))continue;
            const auto& schema=schemas.at(name);bool conditioning=false,clip=false,reference=false;
            for(const auto& output:schema.value("output",Json::array()))conditioning=conditioning||output=="CONDITIONING";
            const auto contracts=schema.value("input",Json::object());for(const auto* section:{"required","optional"})if(contracts.contains(section)&&contracts.at(section).is_object())for(const auto& [input,contract]:contracts.at(section).items()){
                if(!contract.is_array()||contract.empty()||!contract[0].is_string()||!node.at("inputs").contains(input))continue;
                if(contract[0]=="CLIP")clip=true;
                if(contract[0]=="STRING"){const auto& value=node.at("inputs").at(input);const auto& literal=value.is_object()&&value.size()==1U&&value.contains("__value__")?value.at("__value__"):value;
                    reference=reference||(literal.is_string()&&literal.get_ref<const std::string&>().find("embedding:")!=std::string::npos);}
            }
            embeddingReference=embeddingReference||(conditioning&&clip&&reference);
        }
        if(embeddingReference){
            // The installed CLIP tokenizer searches the complete embedding tree,
            // including weighted/escaped names and extension/comma fallbacks.
            const Json choice{{"selected","embeddings"},{"catalog_matches",Json::array()}};
            for(auto path:selectedModelFiles(choice,authorizedRoots,context,categoryPaths)){path=path.lexically_normal();path.make_preferred();files.insert(std::move(path));}
        }
        for(const auto& file:files){check(context);regularParents(file);Handle handle{CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};BY_HANDLE_FILE_INFORMATION information{};
            if(!handle||!GetFileInformationByHandle(handle.get(),&information)||(information.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))fail(Domain::ErrorCodes::IntegrityFailure,"Cannot seal selected regular model file identity.");
            identity["models"].push_back({{"path",pathText(file)},{"bytes",(static_cast<std::uint64_t>(information.nFileSizeHigh)<<32)|information.nFileSizeLow},
                {"modified_time",(static_cast<std::uint64_t>(information.ftLastWriteTime.dwHighDateTime)<<32)|information.ftLastWriteTime.dwLowDateTime},
                {"file_id",(static_cast<std::uint64_t>(information.nFileIndexHigh)<<32)|information.nFileIndexLow},{"volume_serial",information.dwVolumeSerialNumber},{"sha256",fileFacts(handle.get(),context).at("sha256")}});
        }
        const auto python=home.parent_path()/L"python_embeded"/L"python.exe";if(Fs::is_regular_file(python))identity["python"]=fileFacts(python,context);
        const auto extra=home/L"extra_model_paths.yaml";if(Fs::is_regular_file(extra))identity["extra_model_paths"]=fileFacts(extra,context);
        const auto inventory=[&](const Fs::path& base,Json& destination,bool packages){
            if(!Fs::is_directory(base))return;static_cast<void>(internalAuthorized(base,base,Domain::FileAccess::Read,authority,context));std::size_t count{};std::uint64_t hashed{};
            for(auto iterator=Fs::recursive_directory_iterator(base,Fs::directory_options::skip_permission_denied);iterator!=Fs::recursive_directory_iterator{};++iterator){check(context);if(++count>200000U)fail(Domain::ErrorCodes::LimitExceeded,"Dependency source inventory exceeds 200,000 entries.");
                regularParents(iterator->path());if(iterator->is_directory()){const auto name=iterator->path().filename().native();if(name==L".git"||name==L"__pycache__"||name==L"models"||name==L"output"||name==L"input"||name==L"user"||name==L"cache"||name==L"node_modules")iterator.disable_recursion_pending();continue;}
                if(!iterator->is_regular_file())continue;const auto extension=iterator->path().extension().native(),name=iterator->path().filename().native();
                const bool code=extension==L".py"||extension==L".js"||extension==L".mjs"||extension==L".toml"||extension==L".txt"||extension==L".in"||extension==L".pyd"||extension==L".dll"||extension==L".pth"||extension==L"._pth"||name==L"setup.cfg"||name==L"RECORD"||name==L"METADATA"||name==L".tracking"||name==L".forge-dependency.json";
                if(!code)continue;hashed+=iterator->file_size();if(hashed>8ULL*1024ULL*1024ULL*1024ULL)fail(Domain::ErrorCodes::LimitExceeded,"Dependency content inventory exceeds 8 GiB; approval cannot omit source files.");
                auto facts=fileFacts(iterator->path(),context);facts["relative_path"]=pathText(Fs::relative(iterator->path(),base));destination.push_back(std::move(facts));
            }
            static_cast<void>(packages);std::sort(destination.begin(),destination.end(),[](const Json& left,const Json& right){return left.at("path").get<std::string>()<right.at("path").get<std::string>();});
        };
        inventory(home,identity["source_files"],false);inventory(home.parent_path()/L"forge-managed",identity["source_files"],false);
        for(const auto& nodeRoot:customNodeRoots(config,home))if(!contained(home,nodeRoot)&&!contained(home.parent_path()/L"forge-managed",nodeRoot))inventory(nodeRoot,identity["source_files"],false);
        inventory(python.parent_path()/L"Lib"/L"site-packages",identity["packages"],true);
        identity["media_components"]=Json::array();for(const auto name:{L"ffmpeg.exe",L"ffprobe.exe"}){
            const auto executable=component(name,context);if(!executable.empty()){auto facts=fileFacts(executable,context);facts["name"]=utf8(name);identity["media_components"].push_back(std::move(facts));}}
        for(const auto& input:arguments.value("inputs",Json::array()))if(input.is_object()&&input.contains("path"))identity["inputs"].push_back(inspect(text(input,"path"),authority,context));
        const auto active=root_/L"active-environment.json";if(Fs::is_regular_file(active)){
            const auto environment=readJson(active);identity["environment"]=environment;const auto manifests=environment.value("manifests",Json::array());if(!manifests.is_array()||manifests.size()>1024U)fail(Domain::ErrorCodes::LimitExceeded,"Active preparation manifest inventory exceeds its bound.");
            std::map<Fs::path,Json,decltype(less)> bindings{less};for(const auto& value:manifests){const Fs::path path{wide(value.get<std::string>())};if(!contained(root_/L"preparations",path))fail(Domain::ErrorCodes::IntegrityFailure,"Active preparation manifest leaves its retained preparation scope.");static_cast<void>(internalAuthorized(path,root_,Domain::FileAccess::Read,authority,context));
                const auto manifest=readJson(path);for(const auto& binding:manifest.value("resolution",Json::object()).value("installed_node_sources",Json::array())){const Fs::path source{wide(text(binding,"path"))};bool scoped=false;for(const auto& nodeRoot:customNodeRoots(config,home))scoped=scoped||(contained(nodeRoot,source)&&source!=nodeRoot);if(!scoped)fail(Domain::ErrorCodes::IntegrityFailure,"Active custom-node source leaves its loaded roots.");bindings[source]=binding;}}
            identity["custom_node_publisher_metadata"]=Json::array();for(const auto& [source,binding]:bindings){Json metadata{{"path",pathText(source)},{"publisher_identity",binding.value("publisher_identity",Json::object())},{"identity_files",Json::array()}};const auto identityFiles=binding.value("identity_files",Json::array());if(!identityFiles.is_array()||identityFiles.size()>64U)fail(Domain::ErrorCodes::LimitExceeded,"Active custom-node publisher metadata exceeds its bound.");
                regularParents(source);Json content{{"present",Fs::exists(source)},{"kind",text(binding,"kind",16U)}};if(content.at("present")==true){static_cast<void>(internalAuthorized(source,source,Domain::FileAccess::Read,authority,context));
                    if(content.at("kind")=="directory"&&Fs::is_directory(source))content["tree_sha256"]=digest(directoryFacts(source,MaximumArtifactBytes,context).dump());
                    else if(content.at("kind")=="file"&&Fs::is_regular_file(source)&&!_wcsicmp(source.extension().c_str(),L".py")){const auto facts=fileFacts(source,context);content["sha256"]=facts.at("sha256");content["bytes"]=facts.at("bytes");}
                    else fail(Domain::ErrorCodes::IntegrityFailure,"Active custom-node source no longer matches its saved directory/Python-file kind.");}metadata["source_content"]=std::move(content);
                for(const auto& file:identityFiles){const Fs::path path{wide(text(file,"path"))};if(!contained(source,path))fail(Domain::ErrorCodes::IntegrityFailure,"Active publisher metadata leaves its selected node source.");Json current{{"path",pathText(path)},{"present",Fs::exists(path)}};if(current.at("present")==true){static_cast<void>(internalAuthorized(path,source,Domain::FileAccess::Read,authority,context));const auto facts=fileFacts(path,context);current["sha256"]=facts.at("sha256");current["bytes"]=facts.at("bytes");}metadata["identity_files"].push_back(std::move(current));}identity["custom_node_publisher_metadata"].push_back(std::move(metadata));}
        }
        const auto contentDigest=digest(identity.dump());const auto directory=internalDirectory(root_/L"identities",root_,authority,context);const auto manifest=directory/(wide(contentDigest)+L".json");
        const auto encoded=identity.dump(2),manifestDigest=digest(encoded);
        if(Fs::is_regular_file(manifest)){if(fileFacts(manifest,context).at("sha256").get<std::string>()!=manifestDigest)fail(Domain::ErrorCodes::IntegrityFailure,"Retained dependency identity manifest changed.");}
        else writeJson(manifest,identity);
        return {{"installation",pathText(home)},{"endpoint",config.endpoint},{"configuration",identity.at("configuration")},{"method",identity.at("method")},{"sha256",contentDigest},
            {"nodes",identity.at("nodes")},{"models",identity.at("models")},{"source_file_count",identity.at("source_files").size()},{"package_file_count",identity.at("packages").size()},
            {"manifest",{{"path",pathText(manifest)},{"sha256",manifestDigest},{"bytes",encoded.size()}}}};
    }
    Json prepare(const Json&,const Domain::ComfyUiConfig&,const Contracts::WorkspaceAuthority&,const Domain::OperationContext&);
    Json workflow(const Json&,const Domain::ComfyUiConfig&,const Contracts::WorkspaceAuthority&,const Domain::OperationContext&);
    Json catalog(const Json&,const Domain::ComfyUiConfig&,const Contracts::WorkspaceAuthority&,const Domain::OperationContext&);
    Contracts::IWorkspaceAuthority& resolver_;Fs::path root_;std::atomic<bool> stopped_{};
    std::mutex ownerMutex_,browserMutex_,progressMutex_;std::timed_mutex workflowMutex_;Process owner_,browser_;std::string browserEndpoint_;
    std::map<std::string,std::unique_ptr<Cdp>> progress_;
};

Json WindowsComfyUiBackend::Impl::catalog(const Json& arguments,const Domain::ComfyUiConfig& config,
    const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& context){
    const auto home=installedHome(config),portable=home.parent_path();const auto kind=text(arguments,"kind",32U);
    std::vector<Fs::path> locations;
    if(kind=="workflows")locations={root_/L"workflows",home/L"user"/L"default"/L"workflows",portable.parent_path()/L"automation"/L"api_workflows",portable.parent_path()/L"MythicStudio"/L"workflows"};
    else if(kind=="templates"){
        locations={home/L"web"/L"templates",portable/L"python_embeded"/L"Lib"/L"site-packages"/L"comfyui_workflow_templates",shippedResources()};
        const auto packages=portable/L"python_embeded"/L"Lib"/L"site-packages";if(Fs::is_directory(packages))for(const auto& package:Fs::directory_iterator(packages))if(package.is_directory()&&package.path().filename().native().starts_with(L"comfyui_workflow_templates_media_"))locations.push_back(package.path()/L"templates");
        for(const auto& node:Fs::directory_iterator(home/L"custom_nodes"))if(node.is_directory())locations.push_back(node.path()/L"example_workflows");
    }else fail(Domain::ErrorCodes::InvalidRequest,"Local catalog kind must be workflows or templates.");
    Json items=Json::array();const auto filter=text(arguments,"filter",4096U,false,"");std::size_t scanned{};
    for(const auto& directory:locations){if(!Fs::is_directory(directory))continue;static_cast<void>(internalAuthorized(directory,directory,Domain::FileAccess::Read,authority,context));
        for(const auto& file:Fs::recursive_directory_iterator(directory,Fs::directory_options::skip_permission_denied)){
            check(context);if(++scanned>100000U)fail(Domain::ErrorCodes::LimitExceeded,"Workflow catalog scan exceeds its bound.");
            if(!file.is_regular_file()||file.path().extension()!=L".json"||(!filter.empty()&&pathText(file.path()).find(filter)==std::string::npos))continue;
            regularParents(file.path());items.push_back({{"name",pathText(file.path().stem())},{"path",pathText(file.path())},{"bytes",file.file_size()},{"kind",kind}});
        }}
    std::sort(items.begin(),items.end(),[](const Json& left,const Json& right){return left.at("path").get<std::string>()<right.at("path").get<std::string>();});
    const auto offset=arguments.value("offset",0U),limit=arguments.value("limit",100U);if(limit==0U||limit>1000U||offset>items.size())fail(Domain::ErrorCodes::InvalidRequest,"Invalid workflow catalog page.");
    Json page=Json::array();for(std::size_t index=offset;index<items.size()&&page.size()<limit;++index)page.push_back(items[index]);
    Json result{{"ok",true},{"kind",kind},{"items",page},{"offset",offset},{"total",items.size()},{"has_more",offset+page.size()<items.size()},{"next_offset",offset+page.size()<items.size()?Json(offset+page.size()):Json(nullptr)}};
    const auto resources=shippedResources(),manifest=resources/L"starter_manifest.json";if(kind=="templates"&&Fs::is_regular_file(manifest)){static_cast<void>(internalAuthorized(manifest,resources,Domain::FileAccess::Read,authority,context));auto starters=readJson(manifest);
        for(auto& starter:starters.at("workflows"))for(const auto* field:{"preview_file","final_file"}){const auto filename=text(starter,field,256U);if(!safeName(filename)||Fs::path{wide(filename)}.extension()!=L".json")fail(Domain::ErrorCodes::IntegrityFailure,"Shipped starter workflow file is not a JSON filename.");const auto path=internalAuthorized(resources/wide(filename),resources,Domain::FileAccess::Read,authority,context);starter[field]=path.canonicalPath().value();}result["starters"]=std::move(starters);}return result;
}

Json WindowsComfyUiBackend::Impl::workflow(const Json& arguments,const Domain::ComfyUiConfig& config,
    const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& context){
    std::unique_lock workflowLock{workflowMutex_,std::defer_lock};while(!workflowLock.try_lock_for(std::chrono::milliseconds{50}))check(context);check(context);
    const auto action=text(arguments,"action",16U);if(action!="import"&&action!="inspect"&&action!="modify"&&action!="export")fail(Domain::ErrorCodes::InvalidRequest,"Unsupported typed workflow action.");
    Json graph;
    if(arguments.contains("workflow"))graph=arguments.at("workflow");
    else if(arguments.contains("path")&&action=="import"){
        const Fs::path source{wide(text(arguments,"path"))};const auto shipped=shippedResources();
        const auto home=installedHome(config),portable=home.parent_path();std::vector<Fs::path> assets{shipped,root_/L"workflows",home/L"user"/L"default"/L"workflows",home/L"web"/L"templates",
            portable.parent_path()/L"automation"/L"api_workflows",portable.parent_path()/L"MythicStudio"/L"workflows"};
        const auto packages=portable/L"python_embeded"/L"Lib"/L"site-packages";if(Fs::is_directory(packages))for(const auto& package:Fs::directory_iterator(packages))if(package.is_directory()&&package.path().filename().native().starts_with(L"comfyui_workflow_templates"))assets.push_back(package.path());
        if(Fs::is_directory(home/L"custom_nodes"))for(const auto& node:Fs::directory_iterator(home/L"custom_nodes"))if(node.is_directory())assets.push_back(node.path()/L"example_workflows");
        auto scope=assets.end();if(source.extension()==L".json")scope=std::find_if(assets.begin(),assets.end(),[&](const Fs::path& root){return !root.empty()&&contained(root,source);});
        const auto path=scope!=assets.end()?internalAuthorized(source,*scope,Domain::FileAccess::Read,authority,context):authorized(source,Domain::FileAccess::Read,authority,context);
        auto file=take(Detail::openAuthorizedObject(path,Domain::FileAccess::Read,Native::MissingPathPolicy::Reject,context,GENERIC_READ,FILE_SHARE_READ));
        const auto bytes=take(Detail::readOpenedFile(file.handle.get(),16U*1024U*1024U,context));graph=parse({reinterpret_cast<const char*>(bytes.data()),bytes.size()});
    }else if(Fs::is_regular_file(root_/L"workflow-session.json"))graph=readJson(root_/L"workflow-session.json").at("source");
    else fail(Domain::ErrorCodes::InvalidRequest,"Workflow action requires a workflow or an imported session.");
    if(graph.is_object()&&graph.contains("prompt")&&graph.at("prompt").is_object())graph=graph.at("prompt");
    const auto patches=arguments.value("patches",Json::array());if(!patches.is_array()||patches.size()>256U)fail(Domain::ErrorCodes::InvalidRequest,"Workflow patches must be an array of at most 256 node/input values.");
    Json result{{"ok",true},{"queued",false}};
    if(graph.is_object()&&!graph.contains("nodes")){
        for(const auto& [id,node]:graph.items()){static_cast<void>(id);if(!node.is_object()||!node.contains("class_type")||!node.contains("inputs"))fail(Domain::ErrorCodes::InvalidRequest,"API workflow nodes require class_type and inputs.");}
        for(const auto& patch:patches){const auto id=text(patch,"node_id",128U),input=text(patch,"input",128U);if(!graph.contains(id)||!graph.at(id).at("inputs").contains(input))fail(Domain::ErrorCodes::InvalidRequest,"Workflow patch names an absent node/input.");graph[id]["inputs"][input]=patch.at("value");}
        result["workflow"]=graph;result["format"]="api";result["workflow_sha256"]=digest(graph.dump());
    }else{
        if(!graph.is_object()||!graph.value("nodes",Json::array()).is_array())fail(Domain::ErrorCodes::InvalidRequest,"UI workflow requires a nodes array.");
        const auto edge=edgeExecutable();execution(edge,authority,context);internalDirectory(root_,root_,authority,context);
        std::lock_guard lock{browserMutex_};const auto profile=root_/L"edge-workflow-profile";
        if(!browser_.process||WaitForSingleObject(browser_.process.get(),0)!=WAIT_TIMEOUT||browserEndpoint_!=config.endpoint){
            if(browser_.job)TerminateJobObject(browser_.job.get(),0);
            internalDirectory(profile,root_,authority,context);const auto active=profile/L"DevToolsActivePort";
            if(Fs::exists(active)){regularParents(active);if(!DeleteFileW(active.c_str()))fail(Domain::ErrorCodes::Conflict,"Cannot reset private browser discovery file.");}
            browser_=startProcess(edge,{"--headless=new","--edge-skip-compat-layer-relaunch","--no-first-run","--no-default-browser-check","--disable-background-networking","--remote-debugging-address=127.0.0.1","--remote-debugging-port=0","--user-data-dir="+pathText(profile),config.endpoint},profile,root_/L"edge-workflow.log");browserEndpoint_=config.endpoint;
        }
        const auto active=profile/L"DevToolsActivePort";std::string port;
        while(port.empty()){check(context);if(WaitForSingleObject(browser_.process.get(),0)!=WAIT_TIMEOUT)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Private Edge workflow session exited.");
            if(Fs::is_regular_file(active)){std::ifstream stream{active};std::getline(stream,port);if(port.empty()||port.size()>5U||port.find_first_not_of("0123456789")!=std::string::npos)port.clear();}if(port.empty())std::this_thread::sleep_for(std::chrono::milliseconds{50});}
        std::string socket;
        while(socket.empty()){
            check(context);
            Domain::ComfyUiConfig debugger;debugger.endpoint="http://127.0.0.1:"+port;
            if(WaitForSingleObject(browser_.process.get(),0)!=WAIT_TIMEOUT || !listenerOwned(debugger,browser_.pid))
                fail(Domain::ErrorCodes::IntegrityFailure,"Private Edge debugger is not owned by the verified browser process.");
            const auto response=http(debugger.endpoint+"/json/list","GET","","",1024U*1024U,context);
            if(response.status!=200U)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Private Edge discovery failed.");
            for(const auto& target:parse(response.body))if(target.value("type",std::string{})=="page"&&(target.value("url",std::string{})==config.endpoint||target.value("url",std::string{})==config.endpoint+"/")){socket=target.value("webSocketDebuggerUrl",std::string{});break;}
            if(socket.empty())std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        if(!socket.starts_with("ws://127.0.0.1:"+port+"/devtools/page/"))
            fail(Domain::ErrorCodes::IntegrityFailure,"Private Edge returned a debugger WebSocket outside its owned loopback origin.");
        Cdp cdp{socket,context};static_cast<void>(cdp.call("Page.enable",Json::object(),context));Json app;
        // Capture the installed GraphCanvas ready event, which follows setup AND
        // workflow restoration. A merely present app/rootGraph is still too early.
        constexpr auto readinessHook=R"js((() => {
            if (window.__forgeComfyFrontendReady) return;
            const state = window.__forgeComfyFrontendReady = {ready:false, attached:false, error:'', root_publication:0, app_publication:0, graph_hooks:0};
            let timer, observer;
            const mark = () => { state.ready = true; clearInterval(timer); observer?.disconnect(); };
            const attach = (vnode, visited = new WeakSet()) => {
                if (!vnode || typeof vnode !== 'object' || visited.has(vnode)) return;
                visited.add(vnode);
                if (vnode.type?.__name === 'GraphCanvas') {
                    const actual = vnode.component?.vnode || vnode;
                    const props = actual.props || (actual.props = {});
                    const previous = props.onReady;
                    if (previous !== mark && !(Array.isArray(previous) && previous.includes(mark))) {
                        props.onReady = previous ? [...(Array.isArray(previous) ? previous : [previous]), mark] : mark;
                        ++state.graph_hooks;
                    }
                    state.attached = true;
                }
                attach(vnode.component?.subTree,visited);
                attach(vnode.suspense?.activeBranch,visited); attach(vnode.suspense?.pendingBranch,visited);
                if (Array.isArray(vnode.children)) for (const child of vnode.children) attach(child,visited);
            };
            const roots = new WeakSet();
            const inspect = () => { try {
                const root = document.getElementById('vue-app');
                if (!root) return;
                if (roots.has(root)) { attach(root._vnode); return; }
                roots.add(root);
                for (const key of ['_vnode','__vue_app__']) {
                    const descriptor = Object.getOwnPropertyDescriptor(root, key);
                    if (descriptor && !descriptor.configurable) throw new Error('Vue root readiness property is not configurable');
                    let value = root[key];
                    const assigned = next => {
                        value = next;
                        if (key === '_vnode') { ++state.root_publication; attach(next); }
                        else if (next?.mixin) { ++state.app_publication; next.mixin({beforeMount() { attach(this.$?.vnode); },beforeUpdate() { attach(this.$?.vnode); }}); }
                    };
                    Object.defineProperty(root,key,{configurable:true,enumerable:descriptor?.enumerable ?? true,get:()=>value,set:assigned});
                    if (value) assigned(value);
                }
            } catch (error) { state.error = String(error); clearInterval(timer); observer?.disconnect(); } };
            observer = new MutationObserver(inspect);
            observer.observe(document,{childList:true,subtree:true});
            timer = setInterval(inspect,10); inspect();
        })())js";
        const auto previous=cdp.call("Runtime.evaluate",{{"expression","window.__forgeComfyFrontendReady?.ready === true"},{"returnByValue",true}},context);
        if(!previous.value("result",Json::object()).value("value",false)){
            static_cast<void>(cdp.call("Page.addScriptToEvaluateOnNewDocument",{{"source",readinessHook}},context));
            static_cast<void>(cdp.call("Page.reload",{{"ignoreCache",true}},context));
        }
        for(;;){check(context);
            const auto diagnostic=cdp.call("Runtime.evaluate",{{"expression","({hook:window.__forgeComfyFrontendReady ?? null, root:!!document.getElementById('vue-app'), root_vnode:!!document.getElementById('vue-app')?._vnode, vue_app:!!document.getElementById('vue-app')?.__vue_app__, graph:!!window.comfyAPI?.app?.app?.rootGraph, canvas:!!window.comfyAPI?.app?.app?.canvas})"},{"returnByValue",true}},context);
            if(diagnostic.value("result",Json::object()).contains("value"))writeJson(root_/L"workflow-readiness.json",diagnostic.at("result").at("value"));
            const auto state=cdp.call("Runtime.evaluate",{{"expression","window.__forgeComfyFrontendReady?.error || ''"},{"returnByValue",true}},context);
            const auto error=state.value("result",Json::object()).value("value",std::string{});if(!error.empty())fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Private frontend readiness hook failed: "+error);
            const auto evaluated=cdp.call("Runtime.evaluate",{{"expression","(() => { const app = window.comfyAPI?.app?.app; return window.__forgeComfyFrontendReady?.ready === true && app && (app.rootGraph || app.graph) && app.canvas && typeof app.loadGraphData === 'function' && typeof app.graphToPrompt === 'function' ? app : undefined; })()"},{"returnByValue",false}},context);
            if(evaluated.contains("result")&&evaluated.at("result").contains("objectId")){app=evaluated.at("result");break;}std::this_thread::sleep_for(std::chrono::milliseconds{50});}
        // The function is fixed product code. Workflow strings and values are
        // CDP call arguments, never interpolated into JavaScript source.
        constexpr auto function=R"js(async function(data) {
            if (location.origin !== data.origin) throw new Error('ComfyUI origin changed');
            const loadingGraph = this.rootGraph || this.graph;
            if (!loadingGraph || typeof loadingGraph.configure !== 'function') throw new Error('ComfyUI graph is not initialized');
            const configure = loadingGraph.configure;
            let configureFailure;
            loadingGraph.configure = function(...args) { try { return configure.apply(this,args); }
                catch (error) { configureFailure = error; throw error; } };
            try { await this.loadGraphData(data.workflow, true, true); }
            finally { loadingGraph.configure = configure; }
            if (configureFailure) throw new Error('ComfyUI graph configuration failed: ' + String(configureFailure));
            const graph = this.rootGraph || this.graph;
            if (!graph || typeof graph.getNodeById !== 'function') throw new Error('ComfyUI graph is not initialized');
            for (const patch of data.patches) {
                const node = graph.getNodeById(patch.node_id);
                if (!node) throw new Error('Patch node is absent: ' + patch.node_id);
                const widget = node.widgets?.find(w => w.name === patch.input);
                if (!widget) throw new Error('Patch input is not a serializable widget: ' + patch.input);
                widget.value = patch.value;
            }
            const value = await this.graphToPrompt(graph);
            const retained = value.workflow?.nodes;
            if (!Array.isArray(retained) || retained.length !== data.workflow.nodes.length) throw new Error('ComfyUI did not retain the requested editor nodes');
            for (const requested of data.workflow.nodes) {
                const actual = retained.find(node => String(node.id) === String(requested.id));
                if (!actual || actual.type !== requested.type || (actual.mode ?? 0) !== (requested.mode ?? 0))
                    throw new Error('ComfyUI changed a requested node identity or mode: ' + requested.id);
            }
            return {workflow:value.output, ui_workflow:value.workflow};
        })js";
        const auto evaluated=cdp.call("Runtime.callFunctionOn",{{"objectId",app.at("objectId")},{"functionDeclaration",function},
            {"arguments",Json::array({{{"value",{{"origin",config.endpoint},{"workflow",graph},{"patches",patches}}}}})},{"awaitPromise",true},{"returnByValue",true}},context);
        if(evaluated.contains("exceptionDetails"))fail(Domain::ErrorCodes::InvalidRequest,"ComfyUI frontend workflow conversion failed: "+evaluated.at("exceptionDetails").dump());
        if(!evaluated.at("result").contains("value"))fail(Domain::ErrorCodes::MalformedMessage,"ComfyUI frontend did not return a serialized graph.");
        result.update(evaluated.at("result").at("value"));result["format"]="ui_and_api";result["workflow_sha256"]=digest(result.at("workflow").dump());graph=result.at("ui_workflow");
    }
    if(action!="inspect"){internalDirectory(root_,root_,authority,context);writeJson(root_/L"workflow-session.json",{{"source",graph},{"result",result},{"endpoint",config.endpoint}});}
    if(action=="export"){
        const auto destination=Fs::path{wide(text(arguments,"path"))};const auto path=authorized(destination,Domain::FileAccess::Write,authority,context,true);
        if(!Fs::exists(destination))static_cast<void>(authorized(destination,Domain::FileAccess::Create,authority,context,true));
        if(Fs::exists(destination))fail(Domain::ErrorCodes::Conflict,"Workflow export destination already exists; choose a new path.");
        auto parent=destination.parent_path();writableDirectory(parent,authority,context);writeJson(Fs::path{wide(path.canonicalPath().value())},result.contains("ui_workflow")?result.at("ui_workflow"):result.at("workflow"));result["path"]=path.canonicalPath().value();
        if(result.dump().size()>128U*1024U){result.erase("workflow");result.erase("ui_workflow");result["inline_graphs_omitted"]=true;}
    }
    if(result.dump().size()>128U*1024U)fail(Domain::ErrorCodes::PayloadTooLarge,"Serialized workflow exceeds inline delivery bounds; export the retained workflow session to an authorized file.");
    return result;
}

Json WindowsComfyUiBackend::Impl::prepare(const Json& arguments,const Domain::ComfyUiConfig& config,
    const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& context){
    const auto providerLease=take(Detail::ProviderOperationLease::acquire(config.endpoint,context));
    const auto home=installedHome(config);const auto python=home.parent_path()/L"python_embeded"/L"python.exe";
    execution(python,authority,context);internalDirectory(root_,root_,authority,context);
    const auto mutexName=L"Local\\ForgeComfyPrepare-"+wide(digest(pathText(home)));
    Handle mutex{CreateMutexW(nullptr,FALSE,mutexName.c_str())};if(!mutex)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot create shared ComfyUI preparation lock.");
    for(;;){check(context);const auto wait=WaitForSingleObject(mutex.get(),100U);if(wait==WAIT_OBJECT_0||wait==WAIT_ABANDONED)break;if(wait!=WAIT_TIMEOUT)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot acquire shared ComfyUI preparation lock.");}
    struct Unlock {HANDLE mutex;~Unlock(){ReleaseMutex(mutex);}} unlock{mutex.get()};
    const auto transaction=internalDirectory(root_/L"preparations"/wide(context.operationId.value()),root_,authority,context);
    const auto buildWorkspace=home.parent_path()/L"forge-builds.disabled"/wide(context.operationId.value());
    const auto inBuildScope=[&](const Fs::path& path){return contained(transaction,path)||contained(buildWorkspace,path);};
    const auto authorizedBuildSource=[&](const Fs::path& path){if(!inBuildScope(path))fail(Domain::ErrorCodes::IntegrityFailure,"Source-build content leaves its exact preparation or inactive build workspace.");return internalAuthorized(path,contained(transaction,path)?transaction:buildWorkspace,Domain::FileAccess::Read,authority,context);};
    const auto manifestPath=transaction/L"manifest.json";
    Json manifest{{"schema_version",1},{"operation_id",context.operationId.value()},{"installation",pathText(home)},{"state","preparing"},{"download_budget_bytes",config.downloadBudgetBytes},
        {"downloaded_bytes",0ULL},{"receive_accounting_version",1U},{"uncertain_download_bytes",0ULL},{"downloads",Json::array()},{"installed",Json::array()},{"unresolved",Json::array()},{"rollback",Json::array()}};
    std::uint64_t downloaded{};std::vector<Fs::path> committed;Json activationSeals=Json::object();
    Fs::path pth;std::string originalPth,changedPth;bool pthChanged=false;
    const auto ledger=[&]{manifest["downloaded_bytes"]=downloaded;manifest["budget_charged_bytes"]=preparationDownloadCharge(manifest);writeJson(manifestPath,manifest);};
    const auto reserve=[&](const Fs::path& directory,std::uint64_t incoming=0ULL){ULARGE_INTEGER free{},total{},available{};
        if(!GetDiskFreeSpaceExW(directory.c_str(),&free,&total,&available))fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot measure preparation disk space.");
        if(incoming>UINT64_MAX-config.freeSpaceReserveBytes||free.QuadPart<config.freeSpaceReserveBytes+incoming)fail(Domain::ErrorCodes::LimitExceeded,"ComfyUI preparation reached the configured free-space reserve.");};
    const auto requireSuccess=[&](const Json& result){if(result.at("exit_code")!=0)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"External dependency component failed; inspect "+result.at("log").get<std::string>()+": "+result.at("output").get<std::string>().substr(0,8192U));};
    const auto requireIdle=[&](const Domain::OperationContext& observation){
        const auto absent=[&]{
            check(observation);const auto port=static_cast<unsigned long>(std::stoul(endpointPort(config.endpoint)));
            for(const auto family:{AF_INET,AF_INET6}){
                DWORD size{};const auto measured=GetExtendedTcpTable(nullptr,&size,FALSE,family,TCP_TABLE_OWNER_PID_LISTENER,0);
                if(measured!=ERROR_INSUFFICIENT_BUFFER&&measured!=NO_ERROR)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot verify ComfyUI listener absence before changing its installation.");
                if(!size)continue;std::vector<std::byte> rows(size);if(GetExtendedTcpTable(rows.data(),&size,FALSE,family,TCP_TABLE_OWNER_PID_LISTENER,0)!=NO_ERROR)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot verify ComfyUI listener absence before changing its installation.");
                if(family==AF_INET){const auto table=reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(rows.data());for(DWORD row=0;row<table->dwNumEntries;++row)if(ntohs(static_cast<u_short>(table->table[row].dwLocalPort))==port)return false;}
                else{const auto table=reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(rows.data());for(DWORD row=0;row<table->dwNumEntries;++row)if(ntohs(static_cast<u_short>(table->table[row].dwLocalPort))==port)return false;}
            }
            const auto ownerPath=root_/L"runtime-owner.json";if(!Fs::exists(ownerPath))return true;
            const auto owner=readJson(ownerPath);if(!owner.contains("pid")||!owner.at("pid").is_number_unsigned()||owner.at("pid").get<std::uint64_t>()==0ULL||owner.at("pid").get<std::uint64_t>()>MAXDWORD)fail(Domain::ErrorCodes::IntegrityFailure,"Cannot verify retained ComfyUI process absence from its owner ledger.");
            Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,owner.at("pid").get<DWORD>())};
            if(!process){if(GetLastError()==ERROR_INVALID_PARAMETER)return true;fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot verify retained ComfyUI process exit before changing its installation.");}
            const auto waited=WaitForSingleObject(process.get(),0);if(waited==WAIT_OBJECT_0)return true;if(waited==WAIT_TIMEOUT)return false;
            fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot verify retained ComfyUI process exit before changing its installation.");
        };
        if(absent())return;
        const auto runtime=status(config,observation);
        if(!runtime.value("available",false))fail(Domain::ErrorCodes::Conflict,"Dependency changes require a confirmed absent provider or an available exact owned drained runtime; active or unverified provider files are retained.");
        static_cast<void>(requireOwner(config,observation,false));
        const auto queues=runtime.at("queue");if(!queues.at("queue_running").empty()||!queues.at("queue_pending").empty())fail(Domain::ErrorCodes::Conflict,"ComfyUI queue must drain before changing its installation.");
        const auto stopped=control({{"action","stop"}},config,authority,observation);
        if(!stopped.value("stopped",false)||!absent())fail(Domain::ErrorCodes::Conflict,"ComfyUI shutdown and listener absence were not confirmed; active provider files are retained.");
    };
    if(Fs::is_regular_file(manifestPath)){
        const auto previous=readJson(manifestPath);if(previous.at("operation_id")!=manifest.at("operation_id")||previous.at("installation")!=manifest.at("installation"))fail(Domain::ErrorCodes::IntegrityFailure,"Preparation receipt identity changed.");
        if(previous.value("download_budget_bytes",0ULL)!=config.downloadBudgetBytes||previous.value("request_sha256",digest(arguments.dump()))!=digest(arguments.dump()))fail(Domain::ErrorCodes::Conflict,"Preparation resume cannot change the saved request or cumulative download budget.");
        downloaded=previous.value("downloaded_bytes",0ULL);manifest["downloads"]=previous.value("downloads",Json::array());manifest["prior_attempts"]=previous.value("prior_attempts",Json::array());
        for(const auto* retained:{"resolution","metadata","metadata_retries","snapshot","activations","source_builds","build_interpreters","baseline_package_inventory_changed","baseline_package_inventory_unverified","receive_accounting_version","receive_admission","receive_uncertainties","uncertain_download_bytes","legacy_receive_uncertainty"})if(previous.contains(retained))manifest[retained]=previous.at(retained);
        if(manifest.contains("resolution")&&!manifest.at("resolution").contains("installed_node_sources")&&!manifest.at("resolution").value("installed_node_directories",Json::array()).empty())fail(Domain::ErrorCodes::Conflict,"Legacy preparation has referenced custom-node paths without saved source content; start a fresh preparation to reconcile those sources.");
        if(manifest.contains("resolution"))for(const auto& source:manifest.at("resolution").value("installed_node_sources",Json::array())){const Fs::path path{wide(text(source,"path"))};static_cast<void>(internalAuthorized(path,path,Domain::FileAccess::Read,authority,context));
            for(const auto& identity:source.value("identity_files",Json::array()))static_cast<void>(internalAuthorized(Fs::path{wide(text(identity,"path"))},path,Domain::FileAccess::Read,authority,context));
            if(sealCustomNodeSource(path,customNodeRoots(config,home),context,source.value("publisher_identity",Json::object()),source.value("identity_files",Json::array()))!=source)fail(Domain::ErrorCodes::IntegrityFailure,"Retained custom-node source changed before preparation resume.");}
        if(!previous.contains("receive_accounting_version"))manifest.erase("receive_accounting_version");
        manifest["prior_attempts"].push_back({{"state",previous.value("state",std::string{})},{"error",previous.value("error",Json(nullptr))},{"installed",previous.value("installed",Json::array())},{"rollback",previous.value("rollback",Json::array())}});
        std::vector<std::uint64_t> stagingBytes;
        for(auto& transfer:manifest["downloads"]){const auto path=Fs::path{wide(text(transfer,"path"))};if(!contained(transaction,path))fail(Domain::ErrorCodes::IntegrityFailure,"Retained preparation download escaped its transaction.");regularParents(path);
            stagingBytes.push_back(transfer.value("type",std::string{})!="metadata"&&Fs::is_regular_file(path)?Fs::file_size(path):0ULL);}
        manifest["downloaded_bytes"]=downloaded;reconcilePreparationReceives(manifest,previous.value("state",std::string{}),stagingBytes);downloaded=manifest.at("downloaded_bytes").get<std::uint64_t>();
        for(const auto& item:previous.value("installed",Json::array()))if(previous.value("state",std::string{})!="completed"&&!item.value("reused",false)&&item.contains("kind")){const Fs::path target{wide(text(item,"target"))};
            if(!(contained(home.parent_path()/L"forge-managed",target)||contained(root_/L"components",target)||contained(home/L"models",target)||(!config.modelStoragePath.empty()&&contained(Fs::path{wide(config.modelStoragePath)},target))))fail(Domain::ErrorCodes::IntegrityFailure,"Retained preparation activation escaped its installation scope.");
            if(Fs::exists(target)){bool matches=false;if(Fs::is_regular_file(target))matches=fileFacts(target,context).at("sha256")==item.at("sha256");else if(item.contains("tree")&&Fs::is_regular_file(target/L".forge-dependency.json")){
                    const auto marker=readJson(target/L".forge-dependency.json");matches=marker.value("sha256",std::string{})==text(item,"sha256",64U)&&marker.contains("tree")&&marker.at("tree")==item.at("tree")&&directoryFacts(target,MaximumArtifactBytes,context)==item.at("tree");}
                if(!matches)fail(Domain::ErrorCodes::IntegrityFailure,"Interrupted dependency content is changed or unsealed; retained in place for reconciliation.");committed.push_back(target);activationSeals[pathText(target)]=item;}}
        if(manifest.contains("activations"))for(auto& item:manifest["activations"]){if(previous.value("state",std::string{})=="completed")break;const Fs::path target{wide(text(item,"target"))};
            if(!(contained(home.parent_path()/L"forge-managed",target)||contained(root_/L"components",target)||contained(home/L"models",target)||(!config.modelStoragePath.empty()&&contained(Fs::path{wide(config.modelStoragePath)},target))))fail(Domain::ErrorCodes::IntegrityFailure,"Retained activation journal escaped its installation scope.");
            if(item.contains("staging_file")){const Fs::path staged{wide(text(item,"staging_file"))};const auto name=staged.filename().native();
                if(staged.parent_path()!=target.parent_path()||!name.starts_with(L"forge-copy-")||!name.ends_with(L".disabled"))fail(Domain::ErrorCodes::IntegrityFailure,"Retained dependency copy staging escaped its inactive destination namespace.");
                regularParents(staged);if(Fs::is_regular_file(staged))item["copied_bytes"]=Fs::file_size(staged);
            }
            if(!Fs::exists(target)||std::find(committed.begin(),committed.end(),target)!=committed.end())continue;
            const bool archive=item.value("archive",false);bool matches=false;if(archive&&item.contains("tree")&&Fs::is_regular_file(target/L".forge-dependency.json")){
                const auto marker=readJson(target/L".forge-dependency.json");matches=marker.value("sha256",std::string{})==text(item,"sha256",64U)&&marker.contains("tree")&&marker.at("tree")==item.at("tree")&&directoryFacts(target,MaximumArtifactBytes,context)==item.at("tree");}
            else if(!archive&&Fs::is_regular_file(target))matches=fileFacts(target,context).at("sha256")==item.at("sha256");
            if(!matches)fail(Domain::ErrorCodes::IntegrityFailure,"Interrupted dependency activation differs from its retained content seal.");committed.push_back(target);activationSeals[pathText(target)]=item;}
        if(previous.contains("package_overlay")&&previous.value("state",std::string{})!="completed"){
            const Fs::path overlay{wide(text(previous,"package_overlay"))};if(!contained(home.parent_path()/L"forge-managed"/wide(context.operationId.value()),overlay))fail(Domain::ErrorCodes::IntegrityFailure,"Retained package overlay escaped its preparation scope.");if(Fs::exists(overlay)){
                if(!previous.contains("package_overlay_tree"))manifest["rollback"].push_back({{"path",pathText(overlay)},{"moved",false},{"unreconciled",true},{"reason","Interrupted unsealed package staging remains inactive and is retained for review."}});
                else{if(directoryFacts(overlay,MaximumArtifactBytes,context)!=previous.at("package_overlay_tree"))fail(Domain::ErrorCodes::IntegrityFailure,"Interrupted package overlay content changed externally; retained in place.");
                    committed.push_back(overlay);activationSeals[pathText(overlay)]={{"kind","package_overlay"},{"tree",previous.at("package_overlay_tree")}};}}}
        if(previous.contains("python_path_backup")){const auto backup=previous.at("python_path_backup");const Fs::path file{wide(text(backup,"path"))};if(!contained(python.parent_path(),file))fail(Domain::ErrorCodes::IntegrityFailure,"Retained overlay backup escaped the portable interpreter.");
            std::ifstream source{file,std::ios::binary};const std::string content{std::istreambuf_iterator<char>{source},{}};source.close();
            if(previous.value("state",std::string{})!="completed"&&content!=text(backup,"replacement",65536U)&&content!=text(backup,"original",65536U))fail(Domain::ErrorCodes::Conflict,"Interrupted interpreter search path changed externally; it is retained for reconciliation.");
            if(previous.value("state",std::string{})!="completed"&&content==text(backup,"replacement",65536U)){pth=file;originalPth=text(backup,"original",65536U);changedPth=content;pthChanged=true;manifest["python_path_backup"]=backup;}
        }
    }
    manifest["request_sha256"]=digest(arguments.dump());
    if(!manifest.contains("metadata"))manifest["metadata"]=Json::object();
    const auto beforeRead=[&](std::size_t transfer,std::uint64_t proposed){
        const bool newWindow=!manifest.contains("receive_admission")||manifest.at("receive_admission").at("remaining_bytes")==0ULL;
        if(newWindow)reserve(transaction,static_cast<std::uint64_t>(manifest.dump().size())*2ULL+proposed);
        const auto admitted=admitPreparationReceive(manifest,transfer,proposed);if(newWindow)ledger();return admitted;
    };
    const auto account=[&](std::size_t transfer,std::uint64_t count){recordPreparationReceive(manifest,transfer,count);downloaded=manifest.at("downloaded_bytes").get<std::uint64_t>();
        if(manifest.at("receive_admission").at("remaining_bytes")==0ULL)ledger();};
    const auto remainingBudget=[&]{const auto charged=preparationDownloadCharge(manifest);if(charged>config.downloadBudgetBytes)fail(Domain::ErrorCodes::LimitExceeded,"Preparation exceeded its saved cumulative download budget.");return config.downloadBudgetBytes-charged;};
    const auto metadata=[&](const std::string& url)->Json {
        const auto key=digest(url);if(manifest["metadata"].contains(key)){const auto cached=retainedPublisherMetadata(manifest,key,url,transaction,context);if(!cached.is_null())return cached;ledger();}
        Infrastructure::Windows::WindowsUuidGenerator metadataAttempts;const auto path=transaction/(wide(key)+L"-"+wide(take(metadataAttempts.next()).value())+L".metadata.json");manifest["downloads"].push_back({{"type","metadata"},{"url",url},{"path",pathText(path)},{"received_bytes",0ULL},{"state","downloading"}});ledger();
        const auto index=manifest["downloads"].size()-1U;
        const auto response=download(url,16U*1024U*1024U,context,nullptr,0ULL,[&](std::uint64_t count){account(index,count);ledger();},[&](std::uint64_t proposed){return beforeRead(index,proposed);});
        finishPreparationReceive(manifest,true);ledger();
        const auto bodySha=digest(response.body);manifest["downloads"][index]["http_status"]=response.status;manifest["downloads"][index]["body_bytes"]=response.bytes;manifest["downloads"][index]["body_sha256"]=bodySha;ledger();
        const auto value=publisherMetadataDocument(response.status,response.body,bodySha,response.bytes);reserve(transaction,static_cast<std::uint64_t>(value.dump().size())*2ULL);writeJson(path,value);
        manifest["metadata"][key]={{"path",pathText(path)},{"sha256",fileFacts(path,context).at("sha256")},{"url",url}};manifest["downloads"][index]["state"]="retained";ledger();return value;
    };
    std::size_t contractSequence{};
    const auto buildInterpreter=[&](const Fs::path& overlay){
        static_cast<void>(authorizedBuildSource(overlay));const auto directory=overlay/L".forge-python";
        if(directory.native().size()>=MAX_PATH)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Source-build interpreter working directory exceeds the Windows child-process limit; use a shorter ComfyUI installation path.");
        auto staged=stageBuildInterpreter(python,overlay,directory,context,[&](const Fs::path& path,Domain::FileAccess access){
            const auto scope=contained(python.parent_path(),path)?python.parent_path():contained(buildWorkspace,path)?buildWorkspace:transaction;
            if(!contained(python.parent_path(),path)&&!inBuildScope(path))fail(Domain::ErrorCodes::IntegrityFailure,"Build interpreter component leaves its installed or inactive source scope.");
            static_cast<void>(internalAuthorized(path,scope,access,authority,context,access==Domain::FileAccess::Create||access==Domain::FileAccess::Write));
        },[&](std::uint64_t bytes){reserve(transaction);reserve(overlay,bytes);});
        const Fs::path interpreter{wide(text(staged,"python"))};
        if(staged.value("embedded",false)){
            if(!manifest.contains("build_interpreters"))manifest["build_interpreters"]=Json::object();const auto key=digest(pathText(directory));
            if(!manifest["build_interpreters"].contains(key)&&manifest["build_interpreters"].size()>=256U)fail(Domain::ErrorCodes::LimitExceeded,"Preparation exceeds its retained embedded build interpreter inventory.");
            if(manifest["build_interpreters"].contains(key))staged["copied_bytes"]=manifest["build_interpreters"][key].value("copied_bytes",0ULL)+staged.at("copied_bytes").get<std::uint64_t>();
            manifest["build_interpreters"][key]=staged;ledger();execution(interpreter,authority,context);
        }return interpreter;
    };
    const auto packageContract=[&](const Json& query){auto supplied=query;Infrastructure::Windows::WindowsUuidGenerator contractIds;const auto stem=std::to_wstring(contractSequence++)+L"-"+wide(take(contractIds.next()).value());const auto data=transaction/(L"package-contract-"+stem+L".json"),result=transaction/(L"package-contract-"+stem+L".result.json");const auto mode=text(query,"mode",64U);const bool build=mode.starts_with("build_")||mode=="wheel_metadata"||mode=="directory_metadata";if(build)supplied["result_path"]=pathText(result);
        std::vector<Fs::path> buildDirectories;if(build){for(const auto* field:{"source","overlay","output","path"})if(query.contains(field)){const Fs::path path{wide(text(query,field))};static_cast<void>(authorizedBuildSource(path));buildDirectories.push_back(Fs::is_directory(path)?path:path.parent_path());}for(const auto& directory:buildDirectories)if(directory.native().size()>=MAX_PATH)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Source-build working directory exceeds the Windows child-process limit; use a shorter ComfyUI installation path.");}
        const auto buildReserve=[&]{reserve(transaction);for(const auto& directory:buildDirectories)reserve(directory);};buildReserve();
        const auto bytes=static_cast<std::uint64_t>(supplied.dump().size());if(bytes>128ULL*1024ULL*1024ULL)fail(Domain::ErrorCodes::LimitExceeded,"Package metadata query exceeds its bounded staging inventory.");reserve(transaction,bytes*2ULL);writeJson(data,supplied);
        const bool hook=mode=="build_requires"||mode=="build_metadata"||mode=="build_wheel";
        auto interpreter=python;
        if(hook){
            interpreter=buildInterpreter(Fs::path{wide(text(query,"overlay"))});
            manifest["baseline_package_inventory_unverified"]=true;ledger();
        }
        Json observed;std::exception_ptr processFailure;try{observed=runProcess(interpreter,{"-I","-B","-c",std::string{PackageContracts},pathText(data)},interpreter.parent_path(),transaction/(L"package-contract-"+stem+L".log"),context,buildReserve);}catch(...){processFailure=std::current_exception();}
        if(mode=="build_requires"||mode=="build_metadata"||mode=="build_wheel"){auto audit=context;audit.cancellation={};audit.deadline=std::chrono::steady_clock::now()+std::chrono::seconds{30};const auto inventoryData=transaction/(L"build-inventory-"+stem+L".json");writeJson(inventoryData,{{"mode","inventory"}});
            try{const auto inventoryResult=runProcess(python,{"-I","-B","-c",std::string{PackageContracts},pathText(inventoryData)},python.parent_path(),transaction/(L"build-inventory-"+stem+L".log"),audit);requireSuccess(inventoryResult);if(parse(inventoryResult.at("output").get<std::string>())!=manifest.at("snapshot").at("packages")){manifest["baseline_package_inventory_changed"]=true;ledger();fail(Domain::ErrorCodes::Conflict,"Source build hook changed the installed package inventory; the prior runtime remains unreconciled.");}manifest["baseline_package_inventory_unverified"]=false;ledger();}
            catch(...){manifest["baseline_package_inventory_unverified"]=true;ledger();throw;}}
        if(processFailure)std::rethrow_exception(processFailure);requireSuccess(observed);if(build){static_cast<void>(internalAuthorized(result,transaction,Domain::FileAccess::Read,authority,context));return readJson(result);}return parse(observed.at("output").get<std::string>());};
    const auto pinnedPackage=[&](const Json& descriptor,const char* kind){const auto filename=text(descriptor,"filename",256U),sha=text(descriptor,"sha256",64U),url=text(descriptor,"url",16384U);const auto expected=descriptor.at("bytes").get<std::uint64_t>();
        if(!safeName(filename)||!Domain::Sha256Digest::parse(sha)||!url.starts_with("https://files.pythonhosted.org/"))fail(Domain::ErrorCodes::IntegrityFailure,"Package payload lacks a bounded identified PyPI publisher descriptor.");
        const auto directory=internalDirectory(transaction/L"package-downloads",transaction,authority,context),destination=directory/wide(filename);for(const auto& retained:{destination,transaction/L"wheels"/wide(filename)})if(Fs::is_regular_file(retained)){const auto facts=fileFacts(retained,context);if(facts.at("sha256")!=sha||facts.at("bytes")!=expected)fail(Domain::ErrorCodes::IntegrityFailure,"Retained package payload changed its pinned hash or length.");return retained;}
        for(const auto& retained:manifest["downloads"])if(retained.value("state",std::string{})=="verified"&&retained.value("url",std::string{})==url){const Fs::path path{wide(text(retained,"path"))};if(!contained(transaction,path))fail(Domain::ErrorCodes::IntegrityFailure,"Retained package payload leaves its preparation transaction.");if(!Fs::is_regular_file(path))continue;const auto facts=fileFacts(path,context);if(facts.at("sha256")!=sha||facts.at("bytes")!=expected)fail(Domain::ErrorCodes::IntegrityFailure,"Retained verified package payload differs from its publisher descriptor.");return path;}
        Infrastructure::Windows::WindowsUuidGenerator ids;const auto staged=directory/(wide(take(ids.next()).value())+L".download");reserve(directory,expected);Handle file{CreateFileW(staged.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};if(!file)fail(Domain::ErrorCodes::InternalFailure,"Cannot stage a pinned package payload.");
        const auto transfer=manifest["downloads"].size();manifest["downloads"].push_back({{"type",kind},{"url",url},{"path",pathText(staged)},{"sha256_expected",sha},{"received_bytes",0ULL},{"state","downloading"}});ledger();std::uint64_t sinceLedger{};
        const auto response=ComfyDetail::download(url,1024U*1024U,context,file.get(),remainingBudget(),[&](std::uint64_t count){account(transfer,count);reserve(directory,count);sinceLedger+=count;if(sinceLedger>=16ULL*1024ULL*1024ULL){ledger();sinceLedger=0ULL;}},[&](std::uint64_t proposed){return beforeRead(transfer,proposed);});finishPreparationReceive(manifest,true);ledger();
        if(response.status!=200U)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Package publisher "+url+" returned HTTP "+std::to_string(response.status));const auto facts=fileFacts(file.get(),context);file.reset();if(facts.at("sha256")!=sha||facts.at("bytes")!=expected)fail(Domain::ErrorCodes::IntegrityFailure,"Package payload failed its pinned hash or length check.");
        manifest["downloads"][transfer]["state"]="verified";manifest["downloads"][transfer]["bytes"]=facts.at("bytes");manifest["downloads"][transfer]["sha256"]=facts.at("sha256");ledger();if(!MoveFileExW(staged.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH))fail(Domain::ErrorCodes::Conflict,"Cannot publish the verified package staging file.");manifest["downloads"][transfer]["path"]=pathText(destination);ledger();return destination;
    };
    const auto archiveInventory=[&](const Fs::path& archive,const Fs::path& destination,const std::wstring& stem){const auto tar=executableOnPath(L"tar.exe");if(tar.empty())fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Windows tar is unavailable for package staging.");execution(tar,authority,context);
        const auto listing=runProcess(tar,{"-tf",pathText(archive)},transaction,transaction/(stem+L"-list.log"),context,[&]{reserve(transaction);});requireSuccess(listing);const auto names=listing.at("output").get<std::string>();if(names.size()>=1024U*1024U)fail(Domain::ErrorCodes::LimitExceeded,"Source package archive path inventory exceeds its bound.");
        std::istringstream paths{names};std::string path;while(std::getline(paths,path))if(!path.empty()&&(path.front()=='/'||path.find("..")!=std::string::npos||path.find_first_of("\\:")!=std::string::npos))fail(Domain::ErrorCodes::PathOutsideAuthority,"Source package archive contains an escaping path.");
        const auto types=runProcess(tar,{"-tvf",pathText(archive)},transaction,transaction/(stem+L"-types.log"),context,[&]{reserve(transaction);});requireSuccess(types);DWORD sectors{},cluster{},free{},total{};if(!GetDiskFreeSpaceW(destination.root_path().c_str(),&sectors,&cluster,&free,&total))fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot measure source package staging allocation size.");const auto expanded=archiveRequiredBytes(types.at("output").get<std::string>(),static_cast<std::uint64_t>(sectors)*cluster,MaximumArtifactBytes);reserve(destination,expanded);return expanded;
    };
    try{
        ledger();
        if(!committed.empty())requireIdle(context);
        if(pthChanged){requireIdle(context);std::ifstream current{pth,std::ios::binary};const std::string bytes{std::istreambuf_iterator<char>{current},{}};current.close();if(bytes!=changedPth)fail(Domain::ErrorCodes::Conflict,"Interrupted interpreter search path changed before idle recovery; it is retained for reconciliation.");replaceContents(pth,originalPth,context);pthChanged=false;manifest["rollback"].push_back({{"path",pathText(pth)},{"restored",true},{"recovered_before_preparation",true}});ledger();}
        if(preparationDownloadCharge(manifest)>config.downloadBudgetBytes)fail(Domain::ErrorCodes::LimitExceeded,"Resumed preparation has exceeded its cumulative download budget, including unresolved receive admissions.");
        reserve(transaction,16ULL*1024ULL*1024ULL);
        auto dependencies=arguments.value("dependencies",Json::array());if(!dependencies.is_array()||dependencies.size()>128U)fail(Domain::ErrorCodes::InvalidRequest,"Preparation accepts at most 128 explicit dependencies.");
        for(const auto& dependency:dependencies)if(!Domain::Sha256Digest::parse(text(dependency,"sha256",64U)))fail(Domain::ErrorCodes::InvalidRequest,"Explicit dependencies require a publisher SHA-256 checksum.");
        const auto inventory=packageContract({{"mode","inventory"}});
        if(manifest.value("baseline_package_inventory_changed",false)||manifest.value("baseline_package_inventory_unverified",false)){if(!manifest.contains("snapshot")||manifest.at("snapshot").at("packages")!=inventory)fail(Domain::ErrorCodes::Conflict,"Prior source-build package inventory has not been reconciled with the saved baseline inventory.");manifest["baseline_package_inventory_changed"]=false;manifest["baseline_package_inventory_unverified"]=false;manifest["baseline_package_inventory_reconciled"]=true;ledger();}
        if(!manifest.contains("snapshot")){
            Json snapshot{{"packages",inventory},{"runtime",status(config,context)},{"configuration",Json::array()},{"node_revisions",Json::array()}};
            for(const auto& path:{home/L"extra_model_paths.yaml",home/L"pyproject.toml",home/L"custom_nodes"/L"ComfyUI-Manager"/L"extension-node-map.json",home/L"custom_nodes"/L"ComfyUI-Manager"/L"custom-node-list.json",home/L"custom_nodes"/L"ComfyUI-Manager"/L"model-list.json",home.parent_path()/L"python_embeded"/L"python313._pth"})if(Fs::is_regular_file(path))snapshot["configuration"].push_back(fileFacts(path,context));
            for(const auto& base:customNodeRoots(config,home))if(Fs::is_directory(base))for(const auto& item:Fs::directory_iterator(base)){
                const auto name=pathText(item.path().filename());if(name=="__pycache__"||name.ends_with(".disabled"))continue;regularParents(item.path());
                if(item.is_regular_file()&&!_wcsicmp(item.path().extension().c_str(),L".py"))snapshot["node_revisions"].push_back(fileFacts(item.path(),context));
                else if(item.is_directory()){const auto seal=item.path()/L".forge-dependency.json",head=item.path()/L".git"/L"HEAD";if(Fs::is_regular_file(seal))snapshot["node_revisions"].push_back(readJson(seal));else if(Fs::is_regular_file(head))snapshot["node_revisions"].push_back(fileFacts(head,context));}}
            manifest["snapshot"]=std::move(snapshot);ledger();
        }
        Json packageRequirements=Json::array(),packageConstraints=Json::array();
        const auto requirementsFile=[&](const Fs::path& nodeDirectory){const auto path=nodeDirectory/L"requirements.txt";if(!Fs::is_regular_file(path))return;
            static_cast<void>(internalAuthorized(path,nodeDirectory,Domain::FileAccess::Read,authority,context));
            const auto parsed=packageContract({{"mode","requirements_file"},{"path",pathText(path)},{"root",pathText(nodeDirectory)}});
            for(const auto& requirement:parsed.at("requirements"))packageRequirements.push_back(requirement);for(const auto& constraint:parsed.at("constraints"))packageConstraints.push_back(constraint);
            for(const auto& file:parsed.at("files")){const Fs::path included{wide(text(file,"path"))};static_cast<void>(internalAuthorized(included,nodeDirectory,Domain::FileAccess::Read,authority,context));if(fileFacts(included,context).at("sha256")!=file.at("sha256"))fail(Domain::ErrorCodes::IntegrityFailure,"Included publisher requirements changed during introspection.");}
            if(!manifest.contains("requirement_files"))manifest["requirement_files"]=Json::array();manifest["requirement_files"].push_back(parsed);ledger();
        };
        Json installedNodeDirectories=Json::array(),installedNodeSources=Json::array();
        if(manifest.contains("resolution")){for(const auto& dependency:manifest.at("resolution").at("dependencies"))dependencies.push_back(dependency);packageRequirements=manifest.at("resolution").value("requirements",Json::array());}
        else if(arguments.contains("workflow")){
            const auto& graph=arguments.at("workflow");if(!graph.is_object()||graph.size()>4096U)fail(Domain::ErrorCodes::InvalidRequest,"Preparation workflow must be a bounded executable API graph.");
            const auto running=control({{"action","start"}},config,authority,context);if(!running.value("available",false))fail(Domain::ErrorCodes::HostCapabilityUnavailable,"ComfyUI discovery did not become ready.");
            const auto schemas=request({{"method","GET"},{"route","/object_info"}},config,context);
            const auto manager=home/L"custom_nodes"/L"ComfyUI-Manager";
            const auto mapping=Fs::is_regular_file(manager/L"extension-node-map.json")?readJson(manager/L"extension-node-map.json"):Json::object();
            const auto catalog=Fs::is_regular_file(manager/L"custom-node-list.json")?readJson(manager/L"custom-node-list.json"):Json::object();
            const auto models=modelPublisherCatalog(home,context);
            Json resolved=Json::array();std::set<std::string> packages,modelNames;const auto mediaKind=arguments.value("media_kind",std::string{});bool media=arguments.value("media_inspection_required",false)||mediaKind=="video"||mediaKind=="audio"||mediaKind=="mixed";
            for(const auto& [id,node]:graph.items()){
                const auto name=text(node,"class_type",256U);media=media||name.find("Video")!=std::string::npos||name.starts_with("VHS_")||name.find("Audio")!=std::string::npos||name=="SaveAnimatedWEBP"||name=="SaveAnimatedPNG";
                if(schemas.contains(name)){const auto module=schemas.at(name).value("python_module",std::string{});if(module.starts_with("custom_nodes.")){const auto sourceName=module.substr(13U);if(!safeName(sourceName))fail(Domain::ErrorCodes::MalformedMessage,"Provider custom-node module does not identify a local source name.");std::vector<Fs::path> candidates;const auto nodeRoots=customNodeRoots(config,home);
                    for(const auto& base:nodeRoots){if(Fs::is_directory(base/wide(sourceName)))candidates.push_back(base/wide(sourceName));const auto file=base/(wide(sourceName)+L".py");if(Fs::is_regular_file(file))candidates.push_back(file);}
                    if(candidates.size()!=1U)fail(Domain::ErrorCodes::Conflict,"Referenced provider custom-node module does not have one exact installed source: "+module);
                    const auto& source=candidates.front();static_cast<void>(internalAuthorized(source,source,Domain::FileAccess::Read,authority,context));const auto sealed=sealCustomNodeSource(source,nodeRoots,context);
                    if(std::none_of(installedNodeSources.begin(),installedNodeSources.end(),[&](const Json& existing){return existing.at("path")==sealed.at("path");}))installedNodeSources.push_back(sealed);
                    if(sealed.at("kind")=="directory"&&std::find(installedNodeDirectories.begin(),installedNodeDirectories.end(),sealed.at("path"))==installedNodeDirectories.end())installedNodeDirectories.push_back(sealed.at("path"));
                }}
                if(!schemas.contains(name)){
                    if(!config.automaticSetup)fail(Domain::ErrorCodes::Unauthorized,"Automatic dependency installation is disabled by the owner; missing node "+name+" requires preparation before publisher discovery.");
                    auto registry=metadata("https://api.comfy.org/comfy-nodes/"+encode(name)+"/node");std::string repository,packageId;Json registryNode;bool registryResolved=false;
                    if(registry.at("status")==200U){registryNode=registry.at("data");packageId=text(registryNode,"id",256U);repository=text(registryNode,"repository",4096U,false,"");}
                    else if(registry.at("status")==404U){std::set<std::string> repositories;for(const auto& [repo,map]:mapping.items())if(map.is_array()&&!map.empty()&&map[0].is_array())for(const auto& type:map[0])if(type==name)repositories.insert(repo);
                        if(repositories.size()!=1U)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Missing node "+name+" has no unique Registry or installed Manager publisher mapping.");repository=*repositories.begin();
                        for(const auto& item:catalog.value("custom_nodes",Json::array()))if(item.value("reference",std::string{})==repository&&item.contains("id")){auto candidate=metadata("https://api.comfy.org/nodes/"+encode(text(item,"id",256U)));
                            if(candidate.at("status")==200U&&candidate.at("data").value("repository",std::string{})==repository){registryNode=candidate.at("data");packageId=text(registryNode,"id",256U);break;}}}
                    else fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Comfy Registry node discovery returned HTTP "+registry.at("status").dump());
                    if(!packageId.empty()&&(registryNode.value("status",std::string{})!="NodeStatusActive"||registryNode.value("publisher",Json::object()).value("status",std::string{})!="PublisherStatusActive"))fail(Domain::ErrorCodes::Conflict,"Resolved Registry node or publisher is not active.");
                    const auto installedSource=installedPublisherSource(repository,packageId,registryNode,customNodeRoots(config,home),context,
                        [&](const Fs::path& path,const Fs::path& scope){static_cast<void>(internalAuthorized(path,scope,Domain::FileAccess::Read,authority,context));},
                        [&](const Fs::path& path){return packageContract({{"mode","node_project_identity"},{"path",pathText(path)}});});
                    if(!installedSource.is_null()){
                        const Fs::path source{wide(text(installedSource,"path"))};const auto sealed=sealCustomNodeSource(source,customNodeRoots(config,home),context,installedSource.at("publisher_identity"),installedSource.at("identity_files"));
                        if(installedSource.contains("expected_tree")&&sealed.at("tree")!=installedSource.at("expected_tree"))fail(Domain::ErrorCodes::IntegrityFailure,"Installed custom-node publisher content differs from its retained marker tree.");
                        bool retained=false;for(auto& binding:installedNodeSources)if(binding.at("path")==sealed.at("path")){
                            if(binding.at("kind")!=sealed.at("kind")||binding.value("tree",Json{})!=sealed.value("tree",Json{})||binding.value("sha256",Json{})!=sealed.value("sha256",Json{})||binding.value("bytes",Json{})!=sealed.value("bytes",Json{}))fail(Domain::ErrorCodes::IntegrityFailure,"Installed custom-node source changed between workflow requirements.");
                            if(!binding.value("publisher_identity",Json::object()).empty()&&binding.at("publisher_identity")!=sealed.at("publisher_identity"))fail(Domain::ErrorCodes::Conflict,"Workflow classes identify different publishers for the same installed source.");binding=sealed;retained=true;break;
                        }if(!retained)installedNodeSources.push_back(sealed);
                        if(sealed.at("kind")=="directory"&&std::find(installedNodeDirectories.begin(),installedNodeDirectories.end(),sealed.at("path"))==installedNodeDirectories.end())installedNodeDirectories.push_back(sealed.at("path"));registryResolved=true;
                    }
                    if(!packageId.empty()&&!registryResolved){
                        const auto version=registryNode.value("latest_version",Json::object());if(version.is_object()&&version.contains("version")){
                            const auto installed=metadata("https://api.comfy.org/nodes/"+encode(packageId)+"/install?version="+encode(text(version,"version",128U)));
                            if(installed.at("status")!=200U)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Pinned Registry version is not available for "+name);
                            const auto package=installed.at("data");if(package.value("deprecated",false)||package.value("status",std::string{})!="NodeVersionStatusActive")fail(Domain::ErrorCodes::Conflict,"Registry package version is deprecated or is not active.");
                            const auto url=text(package,"downloadUrl",16384U);if(!url.starts_with("https://cdn.comfy.org/"))fail(Domain::ErrorCodes::IntegrityFailure,"Registry package URL does not use its identified publisher CDN.");
                            if(packages.insert(packageId).second){resolved.push_back({{"kind","custom_node"},{"url",url},{"target",packageId},{"archive",true},{"revision",text(package,"id",128U)},
                                {"registry_version",text(package,"version",128U)},{"provenance",{{"kind","comfy_registry_immutable_version"},{"package_id",packageId},{"version_id",package.at("id")},{"publisher",registryNode.value("publisher",Json::object())},{"repository",repository}}}});
                                for(const auto& requirement:package.value("dependencies",Json::array()))packageRequirements.push_back(requirement);}registryResolved=true;
                        }
                    }
                    if(!registryResolved){if(!repository.starts_with("https://github.com/"))fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Missing node publisher has no versioned Registry package or identified GitHub source.");
                    auto repo=repository.substr(19U);if(repo.ends_with(".git"))repo.resize(repo.size()-4U);if(repo.empty()||repo.find_first_of("?#\\")!=std::string::npos||std::count(repo.begin(),repo.end(),'/')!=1)fail(Domain::ErrorCodes::InvalidRequest,"Manager mapping is not an exact GitHub repository.");
                    const auto commit=metadata("https://api.github.com/repos/"+repo+"/commits/HEAD");if(commit.at("status")!=200U)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot pin publisher source revision for "+name);
                    const auto revision=text(commit.at("data"),"sha",40U);if(revision.size()!=40U||revision.find_first_not_of("0123456789abcdef")!=std::string::npos)fail(Domain::ErrorCodes::IntegrityFailure,"Publisher commit identity is invalid.");
                    packageId=repo.substr(repo.find('/')+1U);if(packages.insert(repository).second)resolved.push_back({{"kind","custom_node"},{"url","https://codeload.github.com/"+repo+"/zip/"+revision},{"target",packageId},{"revision",revision},{"archive",true},
                        {"provenance",{{"kind","identified_publisher_commit"},{"repository",repository},{"revision",revision}}}});}
                }
                if(!node.contains("inputs")||!node.at("inputs").is_object())continue;
                for(const auto& choice:referencedModelChoices(Json{{id,node}},schemas,models)){
                    const auto selected=text(choice,"selected",4096U);const auto matches=choice.at("catalog_matches");
                    if(choice.at("enum_present")==true){if(matches.empty())continue;const auto roots=modelRoots(config,home);for(const auto& modelRoot:roots)if(Fs::is_directory(modelRoot))static_cast<void>(internalAuthorized(modelRoot,modelRoot,Domain::FileAccess::Read,authority,context));if(!selectedModelFiles(choice,roots,context,modelCategoryPaths(config,home)).empty())continue;}
                    if(!modelNames.insert(selected).second)continue;if(!config.automaticSetup)fail(Domain::ErrorCodes::Unauthorized,"Automatic dependency installation is disabled by the owner; missing model "+selected+" requires preparation before publisher discovery.");
                    if(matches.size()!=1U)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Missing model "+selected+" has no unique identified publisher entry in the installed model catalog.");
                    const auto model=matches[0];const auto url=text(model,"url",16384U);if(!url.starts_with("https://huggingface.co/"))fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Automatic model resolution currently requires publisher Hugging Face LFS metadata for "+selected);
                    const auto resolve=url.find("/resolve/",23U);if(resolve==std::string::npos)fail(Domain::ErrorCodes::InvalidRequest,"Publisher model URL has no file revision.");const auto repo=url.substr(23U,resolve-23U);const auto fileStart=url.find('/',resolve+9U);if(fileStart==std::string::npos)fail(Domain::ErrorCodes::InvalidRequest,"Publisher model URL has no file path.");const auto sourcePath=url.substr(fileStart+1U);
                    const auto repoInfo=metadata("https://huggingface.co/api/models/"+repo);if(repoInfo.at("status")!=200U)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Model publisher repository "+repo+" returned HTTP "+repoInfo.at("status").dump());
                    const auto revision=text(repoInfo.at("data"),"sha",40U);const auto slash=sourcePath.rfind('/');const auto directory=slash==std::string::npos?std::string{}:sourcePath.substr(0,slash);
                    const auto tree=metadata("https://huggingface.co/api/models/"+repo+"/tree/"+encode(revision)+(directory.empty()?"":"/"+directory));if(tree.at("status")!=200U||!tree.at("data").is_array())fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot inspect model publisher content identity.");
                    Json content;for(const auto& item:tree.at("data"))if(item.value("path",std::string{})==sourcePath)content=item;
                    if(!content.is_object()||!content.contains("lfs"))fail(Domain::ErrorCodes::IntegrityFailure,"Publisher model file has no SHA-256 LFS content seal.");
                    std::string category=model.value("storage_category",std::string{});if(category.empty()&&text(model,"save_path",256U)=="default"){
                        const auto mappingSource=manager/L"glob"/L"manager_server.py";static_cast<void>(internalAuthorized(mappingSource,manager,Domain::FileAccess::Read,authority,context));
                        const auto paths=packageContract({{"mode","model_paths"},{"path",pathText(mappingSource)}});auto type=text(model,"type",256U);std::transform(type.begin(),type.end(),type.begin(),[](unsigned char byte){return static_cast<char>(std::tolower(byte));});
                        category=paths.contains(type)?paths.at(type).get<std::string>():"etc";
                    }
                    if(category.empty())fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Selected model has no resolved publisher storage category: "+selected);
                    if(Fs::path{wide(selected)}.has_parent_path())category=category.substr(0,category.find('/'));resolved.push_back({{"kind","model"},{"url","https://huggingface.co/"+repo+"/resolve/"+revision+"/"+sourcePath},{"sha256",text(content.at("lfs"),"oid",64U)},
                        {"target",category+"/"+selected},{"bytes",content.at("size")},{"provenance",{{"kind","publisher_huggingface_lfs"},{"repository",repo},{"revision",revision},{"path",sourcePath},{"catalog_reference",model.value("reference",std::string{})}}}});
                }
            }
            if(media&&(component(L"ffprobe.exe",context).empty()||component(L"ffmpeg.exe",context).empty())){
                if(!config.automaticSetup)fail(Domain::ErrorCodes::Unauthorized,"Automatic dependency installation is disabled by the owner; missing media inspection components require preparation before publisher discovery.");
                const auto release=metadata("https://api.github.com/repos/BtbN/FFmpeg-Builds/releases");if(release.at("status")!=200U||!release.at("data").is_array())fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot identify the FFmpeg build publisher release.");
                resolved.push_back(resolveFfmpegRelease(release.at("data")));
            }
            manifest["resolution"]={{"dependencies",resolved},{"requirements",packageRequirements},{"installed_node_directories",installedNodeDirectories},{"installed_node_sources",installedNodeSources}};ledger();for(const auto& dependency:resolved)dependencies.push_back(dependency);
        }
        Json admitted=Json::array();
        for(const auto& dependency:dependencies){
            const auto kind=text(dependency,"kind",32U),url=text(dependency,"url",16384U),seal=text(dependency,"sha256",64U,false,""),target=text(dependency,"target",4096U);
            if(kind!="model"&&kind!="custom_node"&&kind!="component"&&kind!="package")fail(Domain::ErrorCodes::InvalidRequest,"Dependency kind must be model, custom_node, component, or package.");
            if((!url.starts_with("http://")&&!url.starts_with("https://"))||url.find_first_of("\r\n")!=std::string::npos)fail(Domain::ErrorCodes::InvalidRequest,"Dependencies require an explicit HTTP or HTTPS download URL.");
            const bool observedPublisherSeal=kind=="custom_node"&&seal.empty()&&dependency.contains("provenance")&&
                (dependency.at("provenance").value("kind",std::string{})=="comfy_registry_immutable_version"||dependency.at("provenance").value("kind",std::string{})=="identified_publisher_commit");
            if(!observedPublisherSeal&&!Domain::Sha256Digest::parse(seal))fail(Domain::ErrorCodes::InvalidRequest,"Dependency requires an exact SHA-256 checksum or a resolved immutable publisher package.");
            const Fs::path relative{wide(target)};
            if(relative.is_absolute()||relative.has_root_name()||target.find_first_of("\\:\r\n")!=std::string::npos||target.find("..")!=std::string::npos)fail(Domain::ErrorCodes::InvalidRequest,"Dependency target must be a safe relative path.");
            Fs::path base=kind=="model"?(config.modelStoragePath.empty()?home/L"models":Fs::path{wide(config.modelStoragePath)}):kind=="custom_node"?home.parent_path()/L"forge-managed"/L"custom_nodes":kind=="package"?transaction/L"wheels":root_/L"components";
            const auto destination=(base/relative).lexically_normal();if(!contained(base,destination))fail(Domain::ErrorCodes::PathOutsideAuthority,"Dependency target leaves its configured installation root.");
            if(kind=="custom_node"&&!dependency.value("archive",true))fail(Domain::ErrorCodes::InvalidRequest,"Custom-node dependencies require a checksum-sealed archive.");
            if(kind=="package"&&relative.extension()!=L".whl")fail(Domain::ErrorCodes::InvalidRequest,"Package dependencies require a wheel file.");
            if(kind=="custom_node"&&!dependency.contains("registry_version")){const auto revision=text(dependency,"revision",128U);if(revision.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos||revision.size()!=40U)fail(Domain::ErrorCodes::InvalidRequest,"Custom-node dependency requires a pinned full commit revision.");}
            static_cast<void>(internalAuthorized(destination,base,Domain::FileAccess::Write,authority,context,true));
            if(!Fs::exists(destination))static_cast<void>(internalAuthorized(destination,base,Domain::FileAccess::Create,authority,context,true));
            auto value=dependency;value["destination"]=pathText(destination);value["scope"]=pathText(base);admitted.push_back(std::move(value));
        }
        manifest["dependencies"]=admitted;ledger();
        if(!admitted.empty())requireIdle(context);
        Json wheels=Json::array();std::size_t index{};
        for(auto& dependency:admitted){
            check(context);const auto destination=Fs::path{wide(text(dependency,"destination"))};
            const bool archive=dependency.value("archive",dependency.at("kind")=="custom_node");
            if(Fs::exists(destination)){
                if(!archive&&Fs::is_regular_file(destination)&&fileFacts(destination,context).at("sha256")==dependency.at("sha256")){
                    manifest["installed"].push_back({{"target",pathText(destination)},{"reused",true},{"sha256",dependency.at("sha256")},{"kind",dependency.at("kind")}});ledger();if(dependency.at("kind")=="package")wheels.push_back(pathText(destination));++index;continue;}
                const auto seal=destination/L".forge-dependency.json";
                if(archive&&Fs::is_regular_file(seal)){const auto retained=readJson(seal);const bool sameSeal=dependency.contains("sha256")&&retained.value("sha256",std::string{})==text(dependency,"sha256",64U);
                    const bool samePublisher=!dependency.contains("sha256")&&retained.value("url",std::string{})==text(dependency,"url")&&retained.value("revision",std::string{})==dependency.value("revision",std::string{})&&retained.contains("sha256");
                    if(sameSeal||samePublisher){if(!retained.contains("tree"))fail(Domain::ErrorCodes::IntegrityFailure,"Existing dependency has no installed content seal; its publisher archive must be verified before adoption.");
                        if(directoryFacts(destination,MaximumArtifactBytes,context)!=retained.at("tree"))fail(Domain::ErrorCodes::IntegrityFailure,"Existing dependency content differs from its installed tree seal; retained for reconciliation.");
                        dependency["sha256"]=retained.at("sha256");manifest["installed"].push_back({{"target",pathText(destination)},{"reused",true},{"sha256",dependency.at("sha256")},{"kind",dependency.at("kind")},{"tree",retained.at("tree")}});ledger();++index;continue;}}
                fail(Domain::ErrorCodes::Conflict,"Existing dependency differs from the requested sealed content; it will not be overwritten.");
            }
            if(!config.automaticSetup)fail(Domain::ErrorCodes::Unauthorized,"Automatic dependency installation is disabled by the owner; missing dependency content is retained for preparation review.");
            Infrastructure::Windows::WindowsUuidGenerator attempts;Fs::path download;Json facts;std::size_t transferIndex{};
            for(std::size_t cursor=manifest["downloads"].size();cursor>0U;--cursor){const auto& retained=manifest["downloads"][cursor-1U];
                if(retained.value("type",std::string{})=="metadata"||retained.value("state",std::string{})!="verified"||retained.value("url",std::string{})!=text(dependency,"url"))continue;
                const auto seal=retained.value("sha256",retained.value("sha256_expected",std::string{}));if(seal.empty())continue;
                if(!Domain::Sha256Digest::parse(seal)||(dependency.contains("sha256")&&dependency.at("sha256")!=seal))fail(Domain::ErrorCodes::IntegrityFailure,"Retained verified download differs from the dependency pin.");
                const Fs::path candidate{wide(text(retained,"path"))};if(!contained(transaction,candidate))fail(Domain::ErrorCodes::IntegrityFailure,"Retained verified download escaped its preparation transaction.");
                if(!Fs::exists(candidate))continue;
                facts=fileFacts(candidate,context);if(facts.at("sha256")!=seal||!retained.contains("bytes")||facts.at("bytes")!=retained.at("bytes"))fail(Domain::ErrorCodes::IntegrityFailure,"Retained verified download content changed; its evidence is preserved for reconciliation.");
                download=candidate;transferIndex=cursor-1U;manifest["downloads"][transferIndex]["reused_after_verification"]=true;break;
            }
            if(download.empty()){
                download=transaction/(std::to_wstring(index)+L"-"+wide(take(attempts.next()).value())+L".download");regularParents(download);reserve(transaction);
                Handle file{CreateFileW(download.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
                if(!file)fail(Domain::ErrorCodes::Conflict,"Dependency download evidence already exists.");
                transferIndex=manifest["downloads"].size();manifest["downloads"].push_back({{"url",dependency.at("url")},{"path",pathText(download)},{"sha256_expected",dependency.value("sha256",std::string{})},{"state","downloading"},{"received_bytes",0ULL}});ledger();
                std::uint64_t sinceLedger{};
                const auto response=ComfyDetail::download(text(dependency,"url"),1024U*1024U,context,file.get(),remainingBudget(),[&](std::uint64_t count){
                    account(transferIndex,count);sinceLedger+=count;reserve(transaction,count);if(sinceLedger>=16U*1024U*1024U){ledger();sinceLedger=0;}},[&](std::uint64_t proposed){return beforeRead(transferIndex,proposed);});
                finishPreparationReceive(manifest,true);ledger();
                if(response.status!=200U)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Dependency source "+text(dependency,"url")+" returned HTTP "+std::to_string(response.status));
                facts=fileFacts(file.get(),context);file.reset();
            }
            if(dependency.contains("sha256")&&facts.at("sha256")!=dependency.at("sha256"))fail(Domain::ErrorCodes::IntegrityFailure,"Dependency download checksum does not match its pinned provenance.");
            if(dependency.contains("bytes")&&facts.at("bytes")!=dependency.at("bytes"))fail(Domain::ErrorCodes::IntegrityFailure,"Dependency download length differs from publisher metadata.");
            dependency["sha256"]=facts.at("sha256");dependency["seal_method"]=dependency.contains("provenance")&&manifest["downloads"][transferIndex]["sha256_expected"]==""?"observed_sha256_of_immutable_publisher_version":"publisher_sha256";
            manifest["dependencies"]=admitted;
            manifest["downloads"][transferIndex]["state"]="verified";manifest["downloads"][transferIndex]["bytes"]=facts.at("bytes");manifest["downloads"][transferIndex]["sha256"]=facts.at("sha256");ledger();
            internalDirectory(destination.parent_path(),Fs::path{wide(text(dependency,"scope"))},authority,context);reserve(destination.parent_path());
            if(!manifest.contains("activations"))manifest["activations"]=Json::array();manifest["activations"].push_back({{"target",pathText(destination)},{"sha256",dependency.at("sha256")},{"archive",archive},{"state","planned"}});ledger();
            if(archive){
                const auto tar=executableOnPath(L"tar.exe");if(tar.empty())fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Windows tar is unavailable for verified dependency extraction.");execution(tar,authority,context);
                const auto listLog=transaction/(std::to_wstring(index)+L".archive-list.log");
                const auto listing=runProcess(tar,{"-tf",pathText(download)},transaction,listLog,context);requireSuccess(listing);
                const auto types=runProcess(tar,{"-tvf",pathText(download)},transaction,transaction/(std::to_wstring(index)+L".archive-types.log"),context);requireSuccess(types);
                DWORD sectorsPerCluster{},bytesPerSector{},freeClusters{},totalClusters{};
                if(!GetDiskFreeSpaceW(destination.root_path().c_str(),&sectorsPerCluster,&bytesPerSector,&freeClusters,&totalClusters))fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot measure dependency staging allocation size.");
                const auto expandedReserve=archiveRequiredBytes(types.at("output").get<std::string>(),static_cast<std::uint64_t>(sectorsPerCluster)*bytesPerSector,MaximumArtifactBytes);
                const auto listingText=listing.at("output").get<std::string>();if(listingText.size()>=1024U*1024U)fail(Domain::ErrorCodes::LimitExceeded,"Dependency archive inventory exceeds its bound.");
                std::string top;std::istringstream names{listingText};std::string entry;
                while(std::getline(names,entry)){if(!entry.empty()&&entry.back()=='\r')entry.pop_back();if(entry.empty())continue;
                    if(entry.front()=='/'||entry.find("..")!=std::string::npos||entry.find_first_of("\\:")!=std::string::npos)fail(Domain::ErrorCodes::PathOutsideAuthority,"Dependency archive contains an unsafe path.");
                    const auto first=entry.substr(0,entry.find('/'));if(top.empty())top=first;else if(top!=first)top="*";
                }
                const auto staged=destination.parent_path()/(destination.filename().native()+L".forge-"+wide(take(attempts.next()).value())+L".disabled");
                if(Fs::exists(staged))fail(Domain::ErrorCodes::Conflict,"Retained dependency extraction already exists; its previous attempt needs reconciliation.");
                manifest["activations"].back()["staging_directory"]=pathText(staged);ledger();
                internalDirectory(staged,Fs::path{wide(text(dependency,"scope"))},authority,context);
                const auto extractLog=transaction/(std::to_wstring(index)+L".extract.log");
                if(expandedReserve>UINT64_MAX-16ULL*1024ULL*1024ULL)fail(Domain::ErrorCodes::LimitExceeded,"Dependency staging requirement overflows its disk bound.");
                manifest["activations"].back()["expanded_reserve_bytes"]=expandedReserve;ledger();reserve(staged,expandedReserve+16ULL*1024ULL*1024ULL);
                requireSuccess(runProcess(tar,{"-xf",pathText(download),"-C",pathText(staged),"--no-same-owner"},transaction,extractLog,context,[&]{reserve(staged,16ULL*1024ULL*1024ULL);}));
                std::uint64_t expanded{};for(const auto& item:Fs::recursive_directory_iterator(staged)){check(context);regularParents(item.path());if(item.is_regular_file()){const auto bytes=item.file_size();if(bytes>MaximumArtifactBytes-expanded)fail(Domain::ErrorCodes::LimitExceeded,"Expanded dependency exceeds the independent installed-content bound.");expanded+=bytes;}reserve(staged);}
                const auto payload=top!="*"&&!top.empty()&&Fs::is_directory(staged/wide(top))?staged/wide(top):staged;
                dependency["tree"]=directoryFacts(payload,MaximumArtifactBytes,context);manifest["activations"].back()["tree"]=dependency.at("tree");ledger();
                writeJson(payload/L".forge-dependency.json",dependency);
                requireIdle(context);
                if(!MoveFileExW(payload.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH))fail(Domain::ErrorCodes::Conflict,"Cannot activate staged dependency directory.");
                committed.push_back(destination);
                activationSeals[pathText(destination)]=dependency;
            }else{
                if(dependency.at("kind")!="package")requireIdle(context);
                if(!MoveFileExW(download.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH)){
                    if(GetLastError()!=ERROR_NOT_SAME_DEVICE)fail(Domain::ErrorCodes::Conflict,"Cannot publish verified dependency file.");
                    const auto staged=destination.parent_path()/(L"forge-copy-"+wide(take(attempts.next()).value())+L".disabled");
                    const auto scope=Fs::path{wide(text(dependency,"scope"))};const auto targetPath=internalAuthorized(staged,scope,Domain::FileAccess::Create,authority,context,true);
                    auto targetAnchors=take(Native::WindowsPathResolver::resolveAnchoredAuthorizedPath(targetPath,Domain::FileAccess::Create,Native::MissingPathPolicy::AllowLeaf,Native::AnchorSharePolicy::AllowConcurrentWrite));
                    auto source=take(Detail::openAuthorizedObject(internalAuthorized(download,transaction,Domain::FileAccess::Read,authority,context),Domain::FileAccess::Read,Native::MissingPathPolicy::Reject,context,GENERIC_READ,FILE_SHARE_READ));
                    const auto sourceBefore=fileFacts(source.handle.get(),context);if(sourceBefore.at("sha256")!=facts.at("sha256")||sourceBefore.at("bytes")!=facts.at("bytes"))fail(Domain::ErrorCodes::IntegrityFailure,"Verified dependency changed before cross-volume staging.");
                    reserve(destination.parent_path(),facts.at("bytes").get<std::uint64_t>());static_cast<void>(take(targetAnchors.revalidateDirectoryAnchors()));
                    Handle output{CreateFileW(targetAnchors.canonicalPath().c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
                    if(!output)fail(Domain::ErrorCodes::Conflict,"Cannot create authorized dependency copy staging.");
                    manifest["activations"].back()["staging_file"]=pathText(staged);manifest["activations"].back()["copied_bytes"]=0ULL;ledger();std::uint64_t recorded{};Json copiedFacts;
                    try{copiedFacts=copyFileContents(source.handle.get(),output.get(),facts.at("bytes").get<std::uint64_t>(),context,[&](std::uint64_t count){reserve(destination.parent_path(),count);LARGE_INTEGER size{};
                        if(!GetFileSizeEx(output.get(),&size)||size.QuadPart<0)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot measure dependency copy staging progress.");
                        const auto observed=static_cast<std::uint64_t>(size.QuadPart);manifest["activations"].back()["copied_bytes"]=observed;if(observed-recorded>=16ULL*1024ULL*1024ULL){ledger();recorded=observed;}});}
                    catch(...){LARGE_INTEGER size{};if(GetFileSizeEx(output.get(),&size)&&size.QuadPart>=0)manifest["activations"].back()["copied_bytes"]=static_cast<std::uint64_t>(size.QuadPart);throw;}
                    manifest["activations"].back()["copied_bytes"]=copiedFacts.at("bytes");
                    const auto sourceAfter=fileFacts(source.handle.get(),context);
                    if(copiedFacts.at("sha256")!=facts.at("sha256")||copiedFacts.at("bytes")!=facts.at("bytes")||sourceAfter.at("sha256")!=sourceBefore.at("sha256")||sourceAfter.at("bytes")!=sourceBefore.at("bytes"))fail(Domain::ErrorCodes::IntegrityFailure,"Dependency copy differs from its verified source seal; inactive staging is retained.");
                    manifest["activations"].back()["staging_identity"]=copiedFacts;ledger();output.reset();source.handle.reset();requireIdle(context);static_cast<void>(take(targetAnchors.revalidateDirectoryAnchors()));
                    if(!MoveFileExW(staged.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH))fail(Domain::ErrorCodes::Conflict,"Cannot atomically activate verified cross-volume dependency staging.");
                }
                committed.push_back(destination);activationSeals[pathText(destination)]=dependency;if(dependency.at("kind")=="package")wheels.push_back(pathText(destination));
            }
            manifest["activations"].back()["state"]="activated";Json installed{{"target",pathText(destination)},{"sha256",dependency.at("sha256")},{"kind",dependency.at("kind")}};if(dependency.contains("tree"))installed["tree"]=dependency.at("tree");manifest["installed"].push_back(std::move(installed));ledger();++index;
        }
        for(const auto& installed:manifest.at("installed"))if(installed.value("kind",std::string{})=="custom_node")installedNodeDirectories.push_back(installed.at("target"));
        if(manifest.contains("resolution"))for(const auto& directory:manifest.at("resolution").value("installed_node_directories",Json::array()))installedNodeDirectories.push_back(directory);
        std::set<std::string> inspectedNodeDirectories;
        for(const auto& installedDirectory:installedNodeDirectories){const auto nodeDirectory=Fs::path{wide(installedDirectory.get<std::string>())};if(!inspectedNodeDirectories.insert(pathText(nodeDirectory)).second)continue;
            const auto nodeRoots=customNodeRoots(config,home);if(std::none_of(nodeRoots.begin(),nodeRoots.end(),[&](const Fs::path& root){return contained(root,nodeDirectory)&&root!=nodeDirectory;}))fail(Domain::ErrorCodes::IntegrityFailure,"Referenced custom-node dependency source leaves the loaded node roots.");static_cast<void>(internalAuthorized(nodeDirectory,nodeDirectory,Domain::FileAccess::Read,authority,context));requirementsFile(nodeDirectory);
            const auto setup=nodeDirectory/L"setup.cfg";if(Fs::is_regular_file(setup)){
                static_cast<void>(internalAuthorized(setup,nodeDirectory,Domain::FileAccess::Read,authority,context));if(Fs::file_size(setup)>1024U*1024U)fail(Domain::ErrorCodes::PayloadTooLarge,"Custom-node setup metadata exceeds its bound.");
                Json requested=Json::array();for(const auto& dependency:admitted)if(text(dependency,"destination")==pathText(nodeDirectory)&&dependency.contains("extras"))requested=dependency.at("extras");
                if(!requested.is_array()||requested.size()>32U)fail(Domain::ErrorCodes::InvalidRequest,"Node setup extras must be a bounded array.");
                const auto declared=packageContract({{"mode","setup_requirements"},{"path",pathText(setup)},{"extras",requested}});for(const auto& requirement:declared)packageRequirements.push_back(requirement);
                if(!manifest.contains("setup_requirements"))manifest["setup_requirements"]=Json::array();manifest["setup_requirements"].push_back({{"path",pathText(setup)},{"sha256",fileFacts(setup,context).at("sha256")},{"requirements",declared}});ledger();
            }
            const auto project=nodeDirectory/L"pyproject.toml";if(Fs::is_regular_file(project)){
                static_cast<void>(internalAuthorized(project,nodeDirectory,Domain::FileAccess::Read,authority,context));if(Fs::file_size(project)>1024U*1024U)fail(Domain::ErrorCodes::PayloadTooLarge,"Custom-node project metadata exceeds its bound.");
                Json requestedExtras=Json::array();for(const auto& dependency:admitted)if(text(dependency,"destination")==pathText(nodeDirectory)&&dependency.contains("extras"))requestedExtras=dependency.at("extras");
                if(!requestedExtras.is_array()||requestedExtras.size()>32U)fail(Domain::ErrorCodes::InvalidRequest,"Node project extras must be a bounded array.");
                const auto requirements=packageContract({{"mode","project_requirements"},{"path",pathText(project)},{"extras",requestedExtras}});for(const auto& requirement:requirements)packageRequirements.push_back(requirement);
                if(!manifest.contains("project_requirements"))manifest["project_requirements"]=Json::array();manifest["project_requirements"].push_back({{"path",pathText(project)},{"sha256",fileFacts(project,context).at("sha256")},{"extras",requestedExtras},{"requirements",requirements}});ledger();
            }
        }
        if(!packageRequirements.empty()){
            Json requirements=Json::array(),selectedPackages=Json::object(),versions=Json::object();
            for(const auto& [name,item]:inventory.at("packages").items())versions[name]=item.at("version");
            if(config.automaticSetup){
                std::function<Json(const Json&,const Json&,unsigned)> resolvePackages;std::function<Json(const Json&,unsigned)> buildSource;
                resolvePackages=[&](const Json& requested,const Json& constraints,unsigned depth){if(depth>3U)fail(Domain::ErrorCodes::LimitExceeded,"Source-package build requirements exceed the bounded build depth.");Json publishers=Json::object();
                for(unsigned round=0U;round<1024U;++round){check(context);const auto resolution=packageContract({{"mode","resolve"},{"requirements",requested},{"constraints",constraints},{"environment",inventory},{"publishers",publishers},{"isolated_build",depth>0U}});
                    if(resolution.contains("error"))fail(resolution.at("error")=="conflict"?Domain::ErrorCodes::Conflict:Domain::ErrorCodes::LimitExceeded,"Python dependency resolution could not preserve every requested and baseline requirement: "+resolution.dump().substr(0,8192U));
                    if(resolution.contains("build")){const auto& descriptor=resolution.at("build");const auto name=text(descriptor,"name",256U),version=text(descriptor,"version",128U);auto& publisher=publishers[name];if(publisher.is_null())publisher=Json::object();if(publisher.value("built",Json::object()).contains(version))fail(Domain::ErrorCodes::MalformedMessage,"Package resolver repeatedly requested a sealed source build.");publisher["built"][version]=buildSource(descriptor,depth+1U);continue;}
                    if(!resolution.contains("need")){if(!resolution.at("selected").is_object()||resolution.at("selected").size()>128U||!resolution.at("requirements").is_array()||resolution.at("requirements").size()>65536U||!resolution.at("versions").is_object())fail(Domain::ErrorCodes::MalformedMessage,"Fixed package resolver returned an invalid bounded result.");return resolution;}
                    const auto& needed=resolution.at("need");const auto name=text(needed,"name",256U);if(name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-")!=std::string::npos)fail(Domain::ErrorCodes::MalformedMessage,"Fixed package resolver returned an invalid publisher name.");
                    if(!publishers.contains(name)&&publishers.size()>=128U)fail(Domain::ErrorCodes::LimitExceeded,"Python package resolution exceeds 128 publisher inventories.");
                    const auto version=text(needed,"version",128U,false,"");auto& publisher=publishers[name];const auto url="https://pypi.org/pypi/"+encode(name)+(version.empty()?"":"/"+encode(version))+"/json";
                    if((version.empty()&&publisher.contains("index"))||(!version.empty()&&publisher.value("versions",Json::object()).contains(version)))fail(Domain::ErrorCodes::MalformedMessage,"Fixed package resolver repeatedly requested retained publisher metadata.");
                    const auto response=metadata(url);if(response.at("status")!=200U)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Python package publisher "+url+" returned HTTP "+response.at("status").dump());
                    if(version.empty())publisher["index"]=response.at("data");else publisher["versions"][version]=response.at("data");
                }fail(Domain::ErrorCodes::LimitExceeded,"Python package resolution exceeds its bounded metadata rounds.");};
                buildSource=[&](const Json& descriptor,unsigned depth){if(depth>3U)fail(Domain::ErrorCodes::LimitExceeded,"Source-package build requirements exceed the bounded build depth.");const auto name=text(descriptor,"name",256U),version=text(descriptor,"version",128U),key=digest(descriptor.dump());
                    if(!manifest.contains("source_builds"))manifest["source_builds"]=Json::object();if(!manifest["source_builds"].contains(key)&&manifest["source_builds"].size()>=128U)fail(Domain::ErrorCodes::LimitExceeded,"Preparation exceeds its bounded source-build inventory.");
                    auto& record=manifest["source_builds"][key];if(record.is_object()&&record.value("state",std::string{})=="completed"){const Fs::path source{wide(text(record,"source"))},overlay{wide(text(record,"overlay"))},wheel{wide(text(record.at("wheel"),"local_wheel"))};
                        for(const auto& path:{source,overlay,wheel})static_cast<void>(authorizedBuildSource(path));
                        if(directoryFacts(source,MaximumArtifactBytes,context)!=record.at("source_tree")||directoryFacts(overlay,MaximumArtifactBytes,context)!=record.at("overlay_tree"))fail(Domain::ErrorCodes::IntegrityFailure,"Retained source or build requirements changed after wheel construction.");if(record.contains("initial_build_overlay")){const Fs::path initial{wide(text(record,"initial_build_overlay"))};static_cast<void>(authorizedBuildSource(initial));if(directoryFacts(initial,MaximumArtifactBytes,context)!=record.at("initial_build_overlay_tree"))fail(Domain::ErrorCodes::IntegrityFailure,"Retained initial build requirements changed after wheel construction.");}const auto facts=fileFacts(wheel,context);if(facts.at("sha256")!=record.at("wheel").at("digests").at("sha256")||facts.at("bytes")!=record.at("wheel").at("size"))fail(Domain::ErrorCodes::IntegrityFailure,"Retained built wheel changed its hash or byte count.");static_cast<void>(pinnedPackage(descriptor,"sdist"));record["reused"]=true;ledger();return record.at("wheel");}
                    const auto archive=pinnedPackage(descriptor,"sdist"),base=internalDirectory(buildWorkspace,home.parent_path(),authority,context);Infrastructure::Windows::WindowsUuidGenerator ids;const auto attempt=internalDirectory(base/wide(take(ids.next()).value()),buildWorkspace,authority,context),sourceTree=internalDirectory(attempt/L"source.disabled",buildWorkspace,authority,context);auto overlay=internalDirectory(attempt/L"build-packages.disabled",buildWorkspace,authority,context);const auto project=attempt/L"project";for(const auto& directory:{attempt,sourceTree,overlay,project,attempt/L"final-build-packages.disabled"})if(directory.native().size()>=MAX_PATH)fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Inactive source-build workspace exceeds the Windows child-process limit; use a shorter ComfyUI installation path.");
                    Json attempts=record.is_object()?record.value("prior_attempts",Json::array()):Json::array();if(record.is_object()&&!record.empty()){if(attempts.size()>=64U)fail(Domain::ErrorCodes::LimitExceeded,"Source build exceeds its bounded retained attempt inventory.");auto previousBuild=record;previousBuild.erase("prior_attempts");attempts.push_back(std::move(previousBuild));}record={{"state","extracting"},{"descriptor",descriptor},{"archive",pathText(archive)},{"attempt",pathText(attempt)},{"overlay",pathText(overlay)},{"prior_attempts",attempts}};ledger();record["expanded_reserve_bytes"]=archiveInventory(archive,sourceTree,L"source-"+wide(key));ledger();const auto tar=executableOnPath(L"tar.exe");
                    requireSuccess(runProcess(tar,{"-xf",pathText(archive),"-C",pathText(sourceTree)},transaction,attempt/L"extract.log",context,[&]{reserve(sourceTree);}));Fs::path source=sourceTree;
                    if(!Fs::is_regular_file(source/L"pyproject.toml")&&!Fs::is_regular_file(source/L"setup.py")){std::vector<Fs::path> entries;for(const auto& entry:Fs::directory_iterator(source))entries.push_back(entry.path());if(entries.size()!=1U||!Fs::is_directory(entries.front()))fail(Domain::ErrorCodes::MalformedMessage,"Source distribution does not identify one Python project root.");source=entries.front();}
                    if(fileFacts(archive,context).at("sha256")!=descriptor.at("sha256"))fail(Domain::ErrorCodes::IntegrityFailure,"Publisher source archive changed during extraction.");static_cast<void>(internalAuthorized(source,buildWorkspace,Domain::FileAccess::Write,authority,context));static_cast<void>(internalAuthorized(project,buildWorkspace,Domain::FileAccess::Create,authority,context,true));if(!MoveFileExW(source.c_str(),project.c_str(),MOVEFILE_WRITE_THROUGH))fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot move the extracted source project into its bounded build working directory.");source=project;record["source"]=pathText(source);record["publisher_tree"]=directoryFacts(source,MaximumArtifactBytes,context);const auto configuration=packageContract({{"mode","build_configuration"},{"source",pathText(source)},{"name",name}});record["configuration"]=configuration;ledger();
                    for(const auto& path:configuration.at("backend_path"))static_cast<void>(internalAuthorized(Fs::path{wide(path.get<std::string>())},source,Domain::FileAccess::Read,authority,context));
                    const auto install=[&](const Json& resolved){std::vector<std::string> args{"-I","-m","pip","--isolated","--disable-pip-version-check","install","--no-index","--no-deps","--no-compile","--no-cache-dir","--only-binary",":all:","--target",pathText(overlay)};std::uint64_t expanded{};
                        for(const auto& selected:resolved.at("selected")){Fs::path wheel;if(selected.contains("local_wheel")){wheel=Fs::path{wide(text(selected,"local_wheel"))};static_cast<void>(authorizedBuildSource(wheel));const auto facts=fileFacts(wheel,context);if(facts.at("sha256")!=selected.at("sha256")||facts.at("bytes")!=selected.at("bytes"))fail(Domain::ErrorCodes::IntegrityFailure,"Build requirement wheel changed its content seal.");}else wheel=pinnedPackage(selected,"build_wheel");const auto size=archiveInventory(wheel,overlay,L"build-wheel-"+wide(digest(pathText(wheel))));if(size>MaximumArtifactBytes-expanded)fail(Domain::ErrorCodes::LimitExceeded,"Build requirement expansion exceeds its content bound.");expanded+=size;args.push_back(pathText(wheel));}
                        reserve(overlay,expanded);if(!resolved.at("selected").empty()){const auto installer=buildInterpreter(overlay);requireSuccess(runProcess(installer,args,attempt,attempt/L"install-build-requirements.log",context,[&]{reserve(overlay);}));}record["build_requirements"]=resolved;ledger();};
                    auto buildRequirements=configuration.at("requirements");auto resolved=resolvePackages(buildRequirements,Json::array(),depth);install(resolved);auto hook=configuration;hook["source"]=pathText(source);hook["overlay"]=pathText(overlay);hook["mode"]="build_requires";record["state"]="discovering_build_requirements";ledger();const auto additional=packageContract(hook).at("requirements");if(!additional.is_array()||additional.size()>1024U)fail(Domain::ErrorCodes::MalformedMessage,"Source backend returned invalid build requirements.");
                    if(!additional.empty()){for(const auto& requirement:additional)buildRequirements.push_back(requirement);resolved=resolvePackages(buildRequirements,Json::array(),depth);record["initial_build_overlay"]=pathText(overlay);record["initial_build_overlay_tree"]=directoryFacts(overlay,MaximumArtifactBytes,context);overlay=internalDirectory(attempt/L"final-build-packages.disabled",buildWorkspace,authority,context);record["overlay"]=pathText(overlay);install(resolved);hook["overlay"]=pathText(overlay);}
                    record["state"]="building_metadata";ledger();hook["mode"]="build_metadata";hook["output"]=pathText(internalDirectory(attempt/L"metadata",buildWorkspace,authority,context));const auto generated=packageContract(hook);Json metadataInfo;
                    if(generated.contains("metadata_directory")){const Fs::path directory{wide(text(generated,"metadata_directory"))};static_cast<void>(internalAuthorized(directory,attempt,Domain::FileAccess::Read,authority,context));metadataInfo=packageContract({{"mode","directory_metadata"},{"path",pathText(directory)}});hook["metadata_directory"]=pathText(directory);}
                    record["state"]="building_wheel";ledger();hook["mode"]="build_wheel";hook["output"]=pathText(internalDirectory(attempt/L"wheels",buildWorkspace,authority,context));const Fs::path wheel{wide(text(packageContract(hook),"wheel"))};static_cast<void>(internalAuthorized(wheel,attempt/L"wheels",Domain::FileAccess::Read,authority,context));const auto metadata=packageContract({{"mode","wheel_metadata"},{"path",pathText(wheel)}});
                    if(metadata.at("name")!=name||!packageContract({{"mode","version_equal"},{"actual",metadata.at("version")},{"expected",version}}).value("equal",false)||(!metadataInfo.is_null()&&metadataInfo!=metadata))fail(Domain::ErrorCodes::IntegrityFailure,"Built wheel metadata differs from its pinned source package or prepared metadata.");const auto facts=fileFacts(wheel,context);const Json result{{"filename",pathText(wheel.filename())},{"url",descriptor.at("url")},{"digests",{{"sha256",facts.at("sha256")}}},{"size",facts.at("bytes")},{"local_wheel",pathText(wheel)},{"packagetype","bdist_wheel"},{"requires_dist",metadata.at("requires_dist")},{"requires_python",metadata.at("requires_python")}};
                    if(packageContract({{"mode","inventory"}})!=inventory){manifest["baseline_package_inventory_changed"]=true;ledger();fail(Domain::ErrorCodes::Conflict,"Source build changed the installed package inventory; retained transaction evidence requires reconciliation.");}record["wheel"]=result;record["source_tree"]=directoryFacts(source,MaximumArtifactBytes,context);record["overlay_tree"]=directoryFacts(overlay,MaximumArtifactBytes,context);record["state"]="completed";ledger();return result;
                };
                const auto resolution=resolvePackages(packageRequirements,packageConstraints,0U);selectedPackages=resolution.at("selected");requirements=resolution.at("requirements");versions=resolution.at("versions");
                for(const auto& selected:selectedPackages){if(!text(selected,"url",16384U).starts_with("https://files.pythonhosted.org/")||!Domain::Sha256Digest::parse(text(selected,"sha256",64U)))fail(Domain::ErrorCodes::IntegrityFailure,"Resolved wheel lacks an identified PyPI publisher URL and SHA-256 seal.");}
            }else{
            Json pending=packageRequirements;std::map<std::string,std::set<std::string>> constraints,extras;std::set<std::string> expanded;
            for(const auto& constraint:packageConstraints){const auto normalized=packageContract({{"mode","requirements"},{"requirements",Json::array({constraint})}});for(const auto& item:normalized)constraints[text(item,"name",256U)].insert(text(item,"requirement",4096U));}
            for(std::size_t cursor=0U;cursor<pending.size();++cursor){
                if(cursor>=1024U)fail(Domain::ErrorCodes::LimitExceeded,"Python dependency requirements exceed their resolution bound.");
                const auto normalized=packageContract({{"mode","requirements"},{"requirements",Json::array({pending[cursor]})}});
                for(const auto& requirement:normalized){const auto name=text(requirement,"name",256U),line=text(requirement,"requirement",4096U);constraints[name].insert(line);requirements.push_back(line);
                    for(const auto& extra:requirement.at("extras"))extras[name].insert(extra.get<std::string>());
                    Json required=Json::array();for(const auto& constraint:constraints[name])required.push_back(constraint);
                    const auto compatible=packageContract({{"mode","check"},{"requirements",required},{"versions",versions}});
                    if(inventory.at("packages").contains(name)){
                        if(!compatible.value("compatible",false))fail(Domain::ErrorCodes::Conflict,"Custom-node package requirement conflicts with the installed baseline: "+compatible.at("conflicts").dump()+". Baseline torch/NVIDIA packages remain unchanged.");
                        Json requestedExtras=Json::array();for(const auto& extra:extras[name])requestedExtras.push_back(extra);const auto key=name+requestedExtras.dump();
                        if(expanded.insert(key).second){const auto children=packageContract({{"mode","requirements"},{"requirements",inventory.at("packages").at(name).at("requires")},{"extras",requestedExtras}});for(const auto& child:children)pending.push_back(child.at("requirement"));}
                        continue;
                    }
                    fail(Domain::ErrorCodes::Unauthorized,"Automatic package installation is disabled by the owner; the installed baseline does not satisfy "+line);
                }
            }
            }
            // Validate both requested requirements and every installed package's
            // active requirements against the combined baseline plus new wheels.
            Json baselineRequirements=Json::array();for(const auto& [name,item]:inventory.at("packages").items())for(const auto& requirement:item.at("requires"))baselineRequirements.push_back(requirement);
            const auto baselineCheck=packageContract({{"mode","check"},{"requirements",baselineRequirements},{"versions",versions}});
            if(!baselineCheck.value("compatible",false))fail(Domain::ErrorCodes::Conflict,"Package environment has unresolved conflicts before activation: "+baselineCheck.at("conflicts").dump().substr(0,8192U));
            const auto requestedCheck=packageContract({{"mode","check"},{"requirements",requirements},{"versions",versions}});
            if(!requestedCheck.value("compatible",false))fail(Domain::ErrorCodes::Conflict,"Python package resolution did not satisfy every workflow requirement.");
            manifest["package_resolution"]={{"requirements",requirements},{"selected",selectedPackages},{"baseline_compatible",true}};ledger();
            for(const auto& selected:selectedPackages){
                const auto filename=text(selected,"filename",256U);if(!safeName(filename)||!filename.ends_with(".whl"))fail(Domain::ErrorCodes::InvalidRequest,"PyPI wheel filename is invalid.");
                if(selected.contains("local_wheel")){const Fs::path wheel{wide(text(selected,"local_wheel"))};static_cast<void>(authorizedBuildSource(wheel));const auto facts=fileFacts(wheel,context);if(facts.at("sha256")!=selected.at("sha256")||facts.at("bytes")!=selected.at("bytes"))fail(Domain::ErrorCodes::IntegrityFailure,"Generated package wheel changed its build seal.");wheels.push_back(pathText(wheel));}
                else wheels.push_back(pathText(pinnedPackage(selected,"wheel")));
            }
        }
        if(!wheels.empty()){
            if(!config.automaticSetup)fail(Domain::ErrorCodes::Unauthorized,"Automatic package installation is disabled by the owner; verified wheels remain staged without activation.");
            Infrastructure::Windows::WindowsUuidGenerator ids;const auto overlay=home.parent_path()/L"forge-managed"/wide(context.operationId.value())/(L"packages-"+wide(take(ids.next()).value()));internalDirectory(overlay,home.parent_path(),authority,context);
            manifest["package_overlay"]=pathText(overlay);ledger();
            const auto tar=executableOnPath(L"tar.exe");if(tar.empty())fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Windows tar is unavailable for sealed wheel disk inspection.");execution(tar,authority,context);
            DWORD sectorsPerCluster{},bytesPerSector{},freeClusters{},totalClusters{};
            if(!GetDiskFreeSpaceW(overlay.root_path().c_str(),&sectorsPerCluster,&bytesPerSector,&freeClusters,&totalClusters))fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Cannot measure wheel staging allocation size.");
            std::uint64_t expansion{};std::size_t wheelIndex{};
            for(const auto& wheel:wheels){const auto stem=L"wheel-"+std::to_wstring(wheelIndex++);const auto listing=runProcess(tar,{"-tf",wheel.get<std::string>()},transaction,transaction/(stem+L"-list.log"),context,[&]{reserve(transaction);});requireSuccess(listing);
                const auto namesText=listing.at("output").get<std::string>();if(namesText.size()>=1024U*1024U)fail(Domain::ErrorCodes::LimitExceeded,"Wheel archive path inventory exceeds its bound.");
                std::istringstream names{namesText};std::string name;while(std::getline(names,name))if(!name.empty()&&(name.front()=='/'||name.find("..")!=std::string::npos||name.find_first_of("\\:")!=std::string::npos))fail(Domain::ErrorCodes::PathOutsideAuthority,"Publisher wheel contains an unsafe path.");
                const auto inventoryLog=runProcess(tar,{"-tvf",wheel.get<std::string>()},transaction,transaction/(stem+L"-types.log"),context,[&]{reserve(transaction);});requireSuccess(inventoryLog);
                const auto bytes=archiveRequiredBytes(inventoryLog.at("output").get<std::string>(),static_cast<std::uint64_t>(sectorsPerCluster)*bytesPerSector,MaximumArtifactBytes);
                if(bytes>MaximumArtifactBytes-expansion)fail(Domain::ErrorCodes::LimitExceeded,"Wheel expansion exceeds its independent installed-content bound.");expansion+=bytes;
            }
            if(expansion>UINT64_MAX-16ULL*1024ULL*1024ULL)fail(Domain::ErrorCodes::LimitExceeded,"Wheel expansion overflows its disk bound.");manifest["package_expanded_reserve_bytes"]=expansion;ledger();reserve(overlay,expansion+16ULL*1024ULL*1024ULL);
            std::vector<std::string> args{"-I","-m","pip","--isolated","--disable-pip-version-check","install","--no-index","--no-deps","--no-compile","--only-binary",":all:","--target",pathText(overlay)};
            for(const auto& wheel:wheels)args.push_back(wheel.get<std::string>());
            requireSuccess(runProcess(python,args,home.parent_path(),transaction/L"packages-install.log",context,[&]{reserve(overlay,16ULL*1024ULL*1024ULL);}));
            manifest["package_overlay_tree"]=directoryFacts(overlay,MaximumArtifactBytes,context);ledger();committed.push_back(overlay);activationSeals[pathText(overlay)]={{"kind","package_overlay"},{"tree",manifest.at("package_overlay_tree")}};
            for(const auto& entry:Fs::directory_iterator(python.parent_path()))if(entry.path().extension()==L"._pth"){pth=entry.path();break;}
            if(pth.empty())fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Portable interpreter search-path file is unavailable; baseline environment remains intact.");
            static_cast<void>(internalAuthorized(pth,home.parent_path(),Domain::FileAccess::Read,authority,context));static_cast<void>(internalAuthorized(pth,home.parent_path(),Domain::FileAccess::Write,authority,context));
            std::ifstream original{pth,std::ios::binary};originalPth.assign(std::istreambuf_iterator<char>{original},{});original.close();if(originalPth.size()>65536U)fail(Domain::ErrorCodes::PayloadTooLarge,"Portable interpreter search-path file exceeds its bound.");
            changedPth=pathText(overlay)+"\r\n"+originalPth;manifest["python_path_backup"]={{"path",pathText(pth)},{"original",originalPth},{"replacement",changedPth}};ledger();
            requireIdle(context);std::ifstream current{pth,std::ios::binary};const std::string currentPth{std::istreambuf_iterator<char>{current},{}};current.close();if(currentPth!=originalPth)fail(Domain::ErrorCodes::Conflict,"Interpreter search path changed before package activation; baseline is retained.");reserve(pth.parent_path(),static_cast<std::uint64_t>(changedPth.size())+16ULL*1024ULL*1024ULL);replaceContents(pth,changedPth,context);pthChanged=true;
            const auto activatedInventory=packageContract({{"mode","inventory"}});manifest["activated_package_inventory"]=activatedInventory;ledger();
        }
        ledger();
        auto runtime=control({{"action","start"}},config,authority,context);manifest["runtime"]=runtime;
        if(arguments.contains("workflow")){
            const auto& graph=arguments.at("workflow");Json schemas=Json::object();
            for(const auto& [id,node]:graph.items()){if(!node.is_object())continue;const auto name=node.value("class_type",std::string{});if(name.empty())continue;
                if(!schemas.contains(name))schemas[name]=request({{"method","GET"},{"route","/object_info/"+encode(name)}},config,context);const auto& schema=schemas.at(name);
                if(!schema.contains(name))manifest["unresolved"].push_back({{"node_id",id},{"class_type",name},{"reason","Node is not available after preparation; resolve its pinned dependency provenance."}});
                else if(node.contains("inputs"))for(const auto& [input,value]:node.at("inputs").items()){
                    const auto& literal=value.is_object()&&value.size()==1U&&value.contains("__value__")?value.at("__value__"):value;
                    const auto contracts=schema.at(name).value("input",Json::object());for(const auto* section:{"required","optional"})if(contracts.contains(section)&&contracts.at(section).contains(input)){
                        const auto& contract=contracts.at(section).at(input);if(!contract.is_array()||contract.empty()||!contract[0].is_array()||!literal.is_string())continue;bool present=false;for(const auto& choice:contract[0])present=present||choice==literal;
                        if(!present&&contract.size()>1U&&contract[1].is_object()&&(contract[1].value("image_upload",false)||contract[1].value("audio_upload",false)||contract[1].value("video_upload",false))){
                            if(!manifest.contains("warnings"))manifest["warnings"]=Json::array();manifest["warnings"].push_back({{"node_id",id},{"class_type",name},{"input",input},{"value",value},{"reason","Private uploaded media is absent from the root-file widget enumeration; annotated path acceptance remains provider submission validation."}});
                        }else if(!present)manifest["unresolved"].push_back({{"node_id",id},{"class_type",name},{"input",input},{"value",value},{"reason","Requested model or enumeration is absent from the live node contract after preparation."}});}}
            }
        }
        for(const auto& path:committed)if(Fs::is_directory(path)&&activationSeals.contains(pathText(path))&&activationSeals.at(pathText(path)).contains("tree")){
            auto& retained=activationSeals[pathText(path)];const auto observed=directoryFacts(path,MaximumArtifactBytes,context);
            if(!retainsDirectoryFiles(retained.at("tree"),observed))fail(Domain::ErrorCodes::IntegrityFailure,"Provider startup changed installed publisher content; retained for reconciliation.");
            Json generated=Json::array();for(const auto& [name,facts]:observed.items()){static_cast<void>(facts);if(!retained.at("tree").contains(name))generated.push_back(name);}retained["tree"]=observed;
            if(!generated.empty()){if(!manifest.contains("provider_generated_files"))manifest["provider_generated_files"]=Json::array();manifest["provider_generated_files"].push_back({{"target",pathText(path)},{"files",generated}});}
            if(Fs::is_regular_file(path/L".forge-dependency.json")){auto marker=readJson(path/L".forge-dependency.json");marker["tree"]=observed;writeJson(path/L".forge-dependency.json",marker);}
            for(auto& installed:manifest["installed"])if(installed.at("target")==pathText(path))installed["tree"]=observed;
            if(manifest.contains("activations"))for(auto& activation:manifest["activations"])if(activation.at("target")==pathText(path))activation["tree"]=observed;
            if(manifest.value("package_overlay",std::string{})==pathText(path))manifest["package_overlay_tree"]=observed;ledger();
        }
        if(!manifest["unresolved"].empty())fail(Domain::ErrorCodes::HostCapabilityUnavailable,"Preparation did not resolve every requested node; inspect the manifest's unresolved entries.");
        manifest["state"]="completed";manifest["ok"]=true;ledger();
        const auto active=root_/L"active-environment.json";Json environment{{"python",pathText(python)},{"manifests",Json::array()},{"dependencies",Json::array()},{"package_overlays",Json::array()}};
        if(Fs::is_regular_file(active))environment=readJson(active);environment["python"]=pathText(python);environment["manifest"]=pathText(manifestPath);
        if(!environment.contains("manifests"))environment["manifests"]=Json::array();environment["manifests"].push_back(pathText(manifestPath));
        if(!environment.contains("dependencies"))environment["dependencies"]=Json::array();for(const auto& item:manifest.at("installed"))environment["dependencies"].push_back(item);
        if(manifest.contains("package_overlay")){if(!environment.contains("package_overlays"))environment["package_overlays"]=Json::array();environment["package_overlays"].push_back(manifest.at("package_overlay"));}writeJson(active,environment);
        return {{"ok",true},{"downloaded_bytes",downloaded},{"uncertain_download_bytes",manifest.at("uncertain_download_bytes")},{"budget_charged_bytes",manifest.at("budget_charged_bytes")},{"manifest_path",pathText(manifestPath)},{"manifest",manifest},{"available",runtime.value("available",false)}};
    }catch(...){
        Domain::Error cause;try{throw;}catch(const Failure& error){cause=error.error;}catch(const std::exception& error){cause=Domain::makeError(Domain::ErrorCodes::InternalFailure,error.what());}catch(...){cause=Domain::makeError(Domain::ErrorCodes::InternalFailure,"Preparation failed.");}
        finishPreparationReceive(manifest,false);manifest["state"]="failed";manifest["ok"]=false;manifest["error"]=errorJson(cause);
        auto cleanup=context;cleanup.cancellation={};cleanup.deadline=std::chrono::steady_clock::now()+std::chrono::seconds{30};
        const auto priorRuntime=manifest.value("snapshot",Json::object()).value("runtime",Json::object());
        bool unreconciledActive=manifest.value("baseline_package_inventory_changed",false)||manifest.value("baseline_package_inventory_unverified",false),pthRestored=!pthChanged,idleConfirmed=false;
        const auto activeChange=[&](const Fs::path& path){if(contained(transaction,path))return false;if(activationSeals.contains(pathText(path))&&activationSeals.at(pathText(path)).value("kind",std::string{})=="package_overlay")return !pthRestored;return true;};
        const auto confirmRollbackIdle=[&]{try{requireIdle(cleanup);idleConfirmed=true;manifest["rollback"].push_back({{"runtime_idle_confirmed",true}});return true;}catch(const Failure& error){manifest["rollback"].push_back({{"runtime_idle_confirmed",false},{"error",errorJson(error.error)}});}catch(const std::exception& error){manifest["rollback"].push_back({{"runtime_idle_confirmed",false},{"reason",error.what()}});}idleConfirmed=false;return false;};
        static_cast<void>(confirmRollbackIdle());
        // Retain downloads and manifest evidence. Roll back only this operation's
        // new activations, never an existing owner file or an unrelated node.
        if(pthChanged&&!idleConfirmed){unreconciledActive=true;manifest["rollback"].push_back({{"path",pathText(pth)},{"restored",false},{"unreconciled",true},{"reason","Provider idle state was not confirmed; active interpreter search path is retained."}});}
        else if(pthChanged){std::ifstream current{pth,std::ios::binary};const std::string bytes{std::istreambuf_iterator<char>{current},{}};current.close();
            if(bytes==changedPth){try{if(!confirmRollbackIdle())fail(Domain::ErrorCodes::Conflict,"Provider idle state changed before interpreter search-path restoration.");replaceContents(pth,originalPth,cleanup);pthRestored=true;manifest["rollback"].push_back({{"path",pathText(pth)},{"restored",true}});}catch(const Failure& error){unreconciledActive=true;manifest["rollback"].push_back({{"path",pathText(pth)},{"restored",false},{"unreconciled",true},{"error",errorJson(error.error)}});}}
            else{unreconciledActive=true;manifest["rollback"].push_back({{"path",pathText(pth)},{"restored",false},{"unreconciled",true},{"reason","Search-path file changed externally; retained for review."}});}}
        for(auto iterator=committed.rbegin();iterator!=committed.rend();++iterator){
            if(!idleConfirmed&&activeChange(*iterator)&&Fs::exists(*iterator)){unreconciledActive=true;manifest["rollback"].push_back({{"path",pathText(*iterator)},{"moved",false},{"unreconciled",true},{"reason","Provider idle state was not confirmed; active dependency is retained."}});continue;}
            const auto retained=iterator->parent_path()/(iterator->filename().native()+L".forge-"+wide(context.operationId.value())+L"-"+std::to_wstring(std::distance(committed.rbegin(),iterator))+L".disabled");
            bool inScope=contained(home.parent_path(),*iterator)||contained(root_,*iterator)||(!config.modelStoragePath.empty()&&contained(Fs::path{wide(config.modelStoragePath)},*iterator));
            bool moved=false;if(Fs::exists(*iterator)&&inScope){bool matches=false;
                try{if(activationSeals.contains(pathText(*iterator))){const auto& expected=activationSeals.at(pathText(*iterator));matches=Fs::is_directory(*iterator)?expected.contains("tree")&&directoryFacts(*iterator,MaximumArtifactBytes,cleanup)==expected.at("tree"):expected.contains("sha256")&&fileFacts(*iterator,cleanup).at("sha256")==expected.at("sha256");}}
                catch(const Failure& error){unreconciledActive=unreconciledActive||activeChange(*iterator);manifest["rollback"].push_back({{"path",pathText(*iterator)},{"moved",false},{"unreconciled",true},{"error",errorJson(error.error)}});continue;}
                catch(const std::exception& error){unreconciledActive=unreconciledActive||activeChange(*iterator);manifest["rollback"].push_back({{"path",pathText(*iterator)},{"moved",false},{"unreconciled",true},{"reason",error.what()}});continue;}
                if(!matches){unreconciledActive=unreconciledActive||activeChange(*iterator);manifest["rollback"].push_back({{"path",pathText(*iterator)},{"moved",false},{"unreconciled",true},{"reason","Activation content is changed or unsealed; retained in place for review."}});continue;}
                if(activeChange(*iterator)&&!confirmRollbackIdle()){unreconciledActive=true;manifest["rollback"].push_back({{"path",pathText(*iterator)},{"moved",false},{"unreconciled",true},{"reason","Provider idle state changed before dependency rollback; activation is retained."}});continue;}
                regularParents(*iterator);moved=MoveFileExW(iterator->c_str(),retained.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
                if(!moved)unreconciledActive=unreconciledActive||activeChange(*iterator);}
            manifest["rollback"].push_back({{"path",pathText(*iterator)},{"retained",pathText(retained)},{"moved",moved},{"unreconciled",!moved&&Fs::exists(*iterator)}});
        }
        if(priorRuntime.value("available",false)&&priorRuntime.value("managed",false)&&(unreconciledActive||!idleConfirmed))manifest["rollback"].push_back({{"prior_runtime_restored",false},{"reason","Unreconciled active changes or an unconfirmed provider idle state prevent restoration of the prior working runtime."}});
        else if(priorRuntime.value("available",false)&&priorRuntime.value("managed",false))try{const auto restored=control({{"action","start"}},config,authority,cleanup);manifest["rollback"].push_back({{"prior_runtime_restored",restored.value("available",false)},{"runtime",restored}});}
            catch(const Failure& error){manifest["rollback"].push_back({{"prior_runtime_restored",false},{"error",errorJson(error.error)}});}
        ledger();return {{"ok",false},{"downloaded_bytes",downloaded},{"uncertain_download_bytes",manifest.at("uncertain_download_bytes")},{"budget_charged_bytes",manifest.at("budget_charged_bytes")},{"manifest_path",pathText(manifestPath)},{"error",errorJson(cause)},{"manifest",manifest}};
    }
}

WindowsComfyUiBackend::WindowsComfyUiBackend(Contracts::IWorkspaceAuthority& authority,Domain::PathText root)
    :implementation_{std::make_unique<Impl>(authority,std::move(root))}{}
WindowsComfyUiBackend::~WindowsComfyUiBackend()=default;
Domain::Result<std::string> WindowsComfyUiBackend::perform(std::string_view operation,std::string_view arguments,const Domain::ComfyUiConfig& config,
    const Contracts::WorkspaceAuthority& authority,const Domain::OperationContext& context)noexcept{return implementation_->perform(operation,arguments,config,authority,context);}
void WindowsComfyUiBackend::shutdown()noexcept{implementation_->shutdown();}
} // namespace ForgeConductor::NativeTools::Windows
