#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include "BridgeClient.h"
#include "BridgeCompare.h"
#include "MaxScene.h"
#include <algorithm>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>

namespace
{
using Json = nlohmann::json;
constexpr uint32_t MaxFrame = 256u * 1024u * 1024u;
using Index = std::map<std::string, const Json*>;
std::array<unsigned char, 32> LocalVersion(const Json& item)
{
    auto bytes = Json::to_cbor(item);
    std::array<unsigned char, 32> digest{};
    if (bytes.size() > std::numeric_limits<ULONG>::max() ||
        BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()), digest.data(),
                   static_cast<ULONG>(digest.size())) < 0)
        throw std::runtime_error("计算 Max 节点快照失败。");
    return digest;
}
Index ById(const Json& items)
{
    Index result;
    for (const auto& item : items)
    {
        auto id = item.at("id").get<std::string>();
        if (!result.emplace(id, &item).second)
            throw std::runtime_error("存在重复作用域：" + id);
    }
    return result;
}
const Json* Lookup(const Index& items, const std::string& id)
{
    auto it = items.find(id);
    return it == items.end() ? nullptr : it->second;
}
std::string PartName(const std::string& name)
{
    const auto separator = name.find('_');
    return separator == std::string::npos ? name : name.substr(separator + 1);
}
const Json* ScopeForComparison(const Index& remote, const std::string& id, const Json* local)
{
    if (const auto* exact = Lookup(remote, id))
        return exact;
    if (local && local->at("kind") == "skeletalPart")
        return Lookup(remote, "LOD" + std::to_string(local->at("lod").get<int>()) + "_Unmapped");
    if (local && local->at("kind") == "staticMesh")
        if (const auto* baseLod = Lookup(remote, "LOD0_" + PartName(id)); baseLod && baseLod->value("assetConflict", false))
            return baseLod;
    return nullptr;
}
bool HasFields(const Json& fields)
{
    return !fields.value("type", "").empty() && !fields.value("name", "").empty() && !fields.value("variant", "").empty();
}
void CheckFields(const Json& fields)
{
    if (!HasFields(fields))
        throw std::runtime_error("请填写载具类型、载具名和变体名。");
    for (auto key : {"type", "name", "variant"})
    {
        const auto& text = fields.at(key).get_ref<const std::string&>();
        if (text.size() > 384 || text.find_first_of("\\/.:*?\"<>|,;'&!~@#(){}[]=^%$\x60 \t\r\n") != std::string::npos)
            throw std::runtime_error("载具字段包含 UE 路径不支持的字符。");
    }
}
std::string Asset(const Json& fields, const std::string& kind, const std::string& part, const Json* remote)
{
    if (!HasFields(fields) || (remote && remote->value("assetConflict", false)))
        return {};
    if (remote)
    {
        const auto package = remote->value("asset", "");
        if (package.starts_with("/Game/"))
            return "Content" + package.substr(5) + ".uasset";
    }
    const auto name = fields.at("name").get<std::string>(), variant = fields.at("variant").get<std::string>();
    auto base = "Content/Vehicle/" + fields.at("type").get<std::string>() + "/" + name + "/" + name + "_" + variant + "/";
    if (kind == "skeleton")
    {
        const auto suffix = variant == "Base" ? name : name + "_" + variant;
        return base + "Skeleton/SKEL_" + suffix + ".uasset";
    }
    if (kind == "physics")
        return base + "PhysicsAsset/PHYS_" + name + "_Body.uasset";
    if (kind == "skeletalPart")
        return base + "SkeletalMesh/SK_" + name + ".uasset";
    return base + "StaticMesh/SM_" + part + ".uasset";
}
std::string Compare(const Json* local, const Json* remote, bool online, const std::string& vehicleName)
{
    if (!online)
        return "offline";
    if ((remote && (remote->value("unmatched", false) || remote->contains("error") || !remote->contains("data"))) ||
        (local && (local->value("unmatched", false) || local->contains("error") || !local->contains("data"))))
        return "unmatched";
    if (!remote)
        return "ueMissing";
    if (!local)
        return "maxMissing";
    return WySameScope(*local, *remote, vehicleName) ? "equal" : "different";
}
struct OutlineScope
{
    std::string id, kind, part, status, asset, error;
    int lod;
    const Json* localNodes;
    const Json* remoteItem;
};
using ScopeList = std::vector<const OutlineScope*>;

Json Row(const std::string& id, const std::string& parent, const std::string& name, const std::string& kind,
         const std::string& theme)
{
    return {{"id", id}, {"parent", parent}, {"name", name}, {"owner", id}, {"kind", kind}, {"theme", theme},
            {"status", "none"}, {"asset", ""}, {"canToUE", false}, {"canToMax", false}, {"scopes", Json::array()},
            {"maxNode", nullptr}};
}

// Scope IDs remain protocol IDs. Groups include all their descendants even while
// a search/filter hides rows; each receiving direction is resolved against the leaf status.
Json ActionRow(const std::string& id, const std::string& parent, const std::string& name, const std::string& kind,
               const std::string& theme, const ScopeList& scopes)
{
    Json row = Row(id, parent, name, kind, theme);
    if (scopes.empty())
        return row;
    const auto& first = *scopes.front();
    bool toUE = false, toMax = false, mixed = false, sharedAsset = true;
    for (const auto* scope : scopes)
    {
        row["scopes"].push_back(scope->id);
        toUE |= scope->status == "different" || scope->status == "ueMissing";
        toMax |= scope->status == "different" || scope->status == "maxMissing";
        mixed |= scope->status != first.status;
        sharedAsset &= scope->asset == first.asset;
    }
    row["canToUE"] = toUE;
    row["canToMax"] = toMax;
    row["status"] = mixed ? (toUE || toMax ? "different" : "unmatched") : first.status;
    if (sharedAsset)
        row["asset"] = first.asset;
    if (scopes.size() == 1 && !first.error.empty())
        row["error"] = first.error;
    return row;
}

void AddPreviews(Json& rows, const OutlineScope& scope, const std::string& category)
{
    const bool skeleton = scope.kind == "skeleton";
    const std::string kind = skeleton ? "bone" : "hull", owner = skeleton ? category : scope.id;
    auto append = [&](const std::string& id, const std::string& parent, const std::string& name, const Json* key)
    {
        auto row = Row(id, parent, name, kind, scope.kind);
        row["owner"] = owner;
        row["asset"] = scope.asset;
        if (key)
            row["maxNode"] = *key;
        rows.push_back(std::move(row));
    };
    const Json* localRoot = scope.localNodes ? &scope.localNodes->front() : nullptr;
    if (skeleton)
        append("Root", category, "Root", localRoot);

    // Max permits duplicate display names. Parent handles, not names, preserve
    // that hierarchy and keep each scene selection unambiguous even for invalid rigs.
    std::map<unsigned long, std::string> byHandle;
    std::map<std::string, std::string> byName;
    if (skeleton)
        byName.emplace("Root", scope.id);
    if (localRoot)
    {
        byHandle.emplace(localRoot->at("handle").get<unsigned long>(), scope.id);
        for (size_t i = 1; i < scope.localNodes->size(); ++i)
        {
            const Json& key = scope.localNodes->at(i);
            auto handle = key.at("handle").get<unsigned long>();
            const auto id = kind + ":node:" + std::to_string(handle);
            const auto& name = key.at("name").get_ref<const std::string&>();
            append(id, byHandle.at(key.at("parent").get<unsigned long>()), name, &key);
            byHandle.emplace(handle, id);
            byName.emplace(name, id);
        }
    }
    if (!scope.remoteItem || !scope.remoteItem->contains("data"))
        return;
    const Json& data = scope.remoteItem->at("data");
    auto members = data.find(skeleton ? "bones" : "shapes");
    if (members == data.end() || !members->is_array())
        return;
    for (const auto& member : *members)
    {
        const auto& name = member.at("name").get_ref<const std::string&>();
        if (byName.contains(name))
            continue;
        const auto id = kind + ":remote:" + name;
        const auto parent = byName.find(member.value("parent", ""));
        append(id, parent != byName.end() ? parent->second : scope.id, name, nullptr);
        byName.emplace(name, id);
    }
}
void AddMeshGroups(Json& rows, const std::string& group, const std::string& title, const std::string& theme,
                   const ScopeList& scopes)
{
    rows.push_back(ActionRow(group, "", title, "category", theme, scopes));
    std::map<std::string, ScopeList> parts;
    for (const auto* scope : scopes)
        parts[scope->part].push_back(scope);
    for (auto& [name, lods] : parts)
    {
        std::stable_sort(lods.begin(), lods.end(), [](const auto* a, const auto* b) { return a->lod < b->lod; });
        const auto partId = "part:" + theme + ":" + name;
        rows.push_back(ActionRow(partId, group, name, "part", theme, lods));
        for (const auto* lod : lods)
        {
            auto row = ActionRow(lod->id, partId, "LOD" + std::to_string(lod->lod), theme, theme, {lod});
            if (lod->localNodes)
                row["maxNode"] = lod->localNodes->front();
            rows.push_back(std::move(row));
        }
    }
}
Json Outline(const Json& vehicle, const Json& localItems, const Json& remoteItems, const Json& localNodes,
             bool online, const WyBridgeTask& task)
{
    auto local = ById(localItems), remote = ById(remoteItems);
    // A Max-only LOD still belongs to the UE asset used by the other LODs, including older unprefixed assets.
    Index remoteStaticParts;
    for (const auto& [id, item] : remote)
        if (item->at("kind") == "staticMesh" && item->contains("asset"))
            remoteStaticParts.try_emplace(PartName(id), item);
    Index all = remote;
    for (auto& [id, item] : local)
        all[id] = item;
    task.report(L"正在比对节点并生成资源大纲…");
    std::vector<OutlineScope> scopes;
    scopes.reserve(all.size());
    const auto vehicleName = vehicle.value("name", "");
    for (const auto& [id, item] : all)
    {
        task.checkpoint();
        task.progress(scopes.size(), all.size());
        const auto* localItem = Lookup(local, id);
        const auto* remoteItem = ScopeForComparison(remote, id, localItem);
        auto kind = item->at("kind").get<std::string>();
        auto part = PartName(item->value("name", id));
        const auto* assetItem = kind == "staticMesh" && !remoteItem ? Lookup(remoteStaticParts, part) : remoteItem;
        auto asset = Asset(vehicle, kind, part, assetItem);
        const auto keys = localNodes.find(id);
        const auto error = item->contains("error") ? item->at("error").get<std::string>() :
                           remoteItem ? remoteItem->value("reason", remoteItem->value("error", "")) : "";
        scopes.push_back({id, std::move(kind), std::move(part), Compare(localItem, remoteItem, online, vehicleName),
                          std::move(asset), error, item->value("lod", 0),
                          keys != localNodes.end() && !keys->empty() ? &*keys : nullptr, Lookup(remote, id)});
    }
    std::map<std::string, ScopeList> kinds;
    for (const auto& scope : scopes)
        kinds[scope.kind].push_back(&scope);
    Json rows = Json::array();
    const auto& skeletons = kinds["skeleton"];
    rows.push_back(ActionRow("group:skeleton", "", "骨骼", "category", "skeleton", skeletons));
    for (const auto* skeleton : skeletons)
        AddPreviews(rows, *skeleton, "group:skeleton");
    AddMeshGroups(rows, "group:skeletal", "骨骼网格体", "skeletalPart", kinds["skeletalPart"]);
    AddMeshGroups(rows, "group:static", "静态网格体", "staticMesh", kinds["staticMesh"]);
    const auto& collisions = kinds["physics"];
    rows.push_back(ActionRow("group:physics", "", "碰撞体", "category", "physics", collisions));
    for (const auto* collision : collisions)
    {
        rows.push_back(ActionRow(collision->id, "group:physics", collision->id, "physics", "physics", {collision}));
        AddPreviews(rows, *collision, "group:physics");
    }
    if (!all.empty())
        task.progress(all.size(), all.size());
    return rows;
}
void Transfer(SOCKET sock, char* data, size_t length, bool writing, const WyBridgeTask& task)
{
    while (length)
    {
        task.checkpoint();
        const int count = static_cast<int>((std::min)(length, size_t(1024 * 1024)));
        const int n = writing ? send(sock, data, count, 0) : recv(sock, data, count, 0);
        if (n <= 0)
            throw std::runtime_error("与 UE 的连接已中断或超时，请重新比对后再操作。");
        data += n;
        length -= n;
    }
}
} // namespace

WyBridgeClient::WyBridgeClient()
{
    WSADATA data{};
    winsock_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
}
WyBridgeClient::~WyBridgeClient()
{
    Disconnect();
    if (winsock_)
        WSACleanup();
}
void WyBridgeClient::Disconnect()
{
    std::lock_guard lock(socketMutex_);
    if (socket_ != INVALID_SOCKET)
        closesocket(socket_);
    socket_ = INVALID_SOCKET;
    compared_ = false;
}
void WyBridgeClient::Interrupt()
{
    std::lock_guard lock(socketMutex_);
    if (socket_ != INVALID_SOCKET)
        shutdown(socket_, SD_BOTH);
}
void WyBridgeClient::Connect(const WyBridgeTask& task)
{
    task.checkpoint();
    if (socket_ != INVALID_SOCKET)
        return;
    if (!winsock_)
        throw std::runtime_error("Windows TCP 初始化失败。");
    int port = 51976;
    wchar_t portText[12]{};
    DWORD n = GetEnvironmentVariableW(L"WY_VEHICLE_BRIDGE_PORT", portText, 12);
    if (n > 0 && n < 12)
    {
        port = _wtoi(portText);
        if (port < 1 || port > 65535)
            throw std::runtime_error("桥接端口无效。");
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        throw std::runtime_error("无法创建 TCP 套接字。");
    {
        std::lock_guard lock(socketMutex_);
        socket_ = s;
    }
    try
    {
        task.checkpoint();
        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
        {
            fd_set writable, failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(s, &writable);
            FD_SET(s, &failed);
            timeval deadline{1, 0};
            int err = 0, slen = sizeof(err);
            if (WSAGetLastError() != WSAEWOULDBLOCK || select(0, nullptr, &writable, &failed, &deadline) <= 0 ||
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &slen) == SOCKET_ERROR || err)
                throw std::runtime_error("UE 未连接，请打开已启用载具桥接插件的望月工程。");
        }
        nonblocking = 0;
        ioctlsocket(s, FIONBIO, &nonblocking);
        DWORD timeout = 60000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
        hello_ = Request({{"op", "hello"}}, task);
        if (hello_.value("service", "") != "WyVehicleAssetBridge" || hello_.value("protocol", 0) != 1)
            throw std::runtime_error("UE 插件协议版本不匹配。");
    }
    catch (...)
    {
        Disconnect();
        throw;
    }
}
nlohmann::json WyBridgeClient::Request(const Json& request, const WyBridgeTask& task, bool guardLocalSource)
{
    task.checkpoint();
    if (socket_ == INVALID_SOCKET)
        throw std::runtime_error("请先连接 UE。");
    auto bytes = request.dump();
    if (bytes.size() > MaxFrame)
        throw std::runtime_error("单次数据超过传输上限。");
    uint32_t length = htonl(static_cast<uint32_t>(bytes.size()));
    Json result;
    try
    {
        Transfer(socket_, reinterpret_cast<char*>(&length), 4, true, task);
        if (guardLocalSource)
        {
            // UE cannot dispatch an incomplete frame. Recheck Max after the large upload,
            // immediately before sending the final byte that allows UE to apply the edit.
            Transfer(socket_, bytes.data(), bytes.size() - 1, true, task);
            task.onMain([] { return Json(nullptr); });
            Transfer(socket_, &bytes.back(), 1, true, task);
        }
        else
            Transfer(socket_, bytes.data(), bytes.size(), true, task);
        Transfer(socket_, reinterpret_cast<char*>(&length), 4, false, task);
        length = ntohl(length);
        if (!length || length > MaxFrame)
            throw std::runtime_error("UE 响应帧长度无效。");
        bytes.resize(length);
        Transfer(socket_, bytes.data(), bytes.size(), false, task);
        result = Json::parse(bytes);
        task.checkpoint();
    }
    catch (...)
    {
        Disconnect();
        throw;
    }
    if (!result.value("ok", false))
        throw std::runtime_error(result.value("error", "UE 操作失败。"));
    return result;
}
Json WyBridgeClient::Refresh(const WyBridgeTask& task, bool reconnect, const ConnectionChanged& onConnection)
{
    task.checkpoint();
    task.report(L"正在连接 UE…");
    if (reconnect)
        Disconnect();
    compared_ = false;
    Json view = {{"connected", false},
                 {"error", ""},
                 {"project", ""},
                 {"types", Json::array()},
                 {"empty", false},
                 {"rows", Json::array()},
                 {"vehicle", {{"type", ""}, {"name", ""}, {"variant", "Base"}}}};
    Json scene = {{"items", Json::array()}, {"outlineNodes", Json::object()}}, remote = Json::array();
    localVersions_.clear();
    remoteVersions_.clear();
    try
    {
        // Refresh the type list on an existing connection. If the editor has
        // restarted, the handshake is safe to retry before taking a snapshot.
        if (socket_ != INVALID_SOCKET)
        {
            try
            {
                hello_ = Request({{"op", "hello"}}, task);
            }
            catch (const std::exception&)
            {
                task.checkpoint();
                if (socket_ != INVALID_SOCKET)
                    throw;
            }
        }
        Connect(task);
        view["connected"] = true;
        view["project"] = hello_.at("project");
        view["types"] = hello_.at("types");
    }
    catch (const std::exception& e)
    {
        task.checkpoint();
        view["error"] = e.what();
    }
    if (onConnection)
        task.onMain([&]
        {
            onConnection(view.at("connected").get<bool>(), view.at("project").get<std::string>());
            return Json(nullptr);
        });
    try
    {
        task.report(L"正在读取 Max 场景…");
        scene = WyReadScene(task.onMain, true);
        task.checkpoint();
        vehicle_ = scene.at("vehicle");
        view["vehicle"] = vehicle_;
        if (view["connected"] == true && HasFields(vehicle_))
        {
            CheckFields(vehicle_);
            task.report(L"正在读取 UE 资源…");
            auto reply = Request({{"op", "read"}, {"vehicle", vehicle_}}, task);
            remote = std::move(reply.at("items"));
            view["empty"] = reply.value("empty", false);
            view["blueprintPaths"] = reply.value("blueprintPaths", Json::array());
            task.report(L"正在计算 Max 节点指纹…");
            const auto& localItems = scene.at("items");
            for (const auto& item : localItems)
            {
                task.checkpoint();
                task.progress(localVersions_.size(), localItems.size());
                localVersions_.emplace(item.at("id").get<std::string>(), LocalVersion(item));
            }
            if (!localItems.empty())
                task.progress(localItems.size(), localItems.size());
            for (const auto& item : remote)
                remoteVersions_.emplace(item.at("id").get<std::string>(), item.at("revision").get<std::string>());
            compared_ = true;
        }
    }
    catch (const std::exception& e)
    {
        compared_ = false;
        task.checkpoint();
        view["error"] = e.what();
    }
    // A failed remote read still leaves the local outline and node selection usable.
    view["rows"] = Outline(view.at("vehicle"), scene.at("items"), remote, scene.at("outlineNodes"), compared_, task);
    if (socket_ == INVALID_SOCKET)
        view["connected"] = false;
    return view;
}
void WyBridgeClient::SetVehicle(const Json& fields, const WyBridgeTask& task)
{
    task.checkpoint();
    CheckFields(fields);
    task.report(L"正在向 UE 验证目标路径…");
    Connect(task);
    // Validate names against UE before recording the metadata in the Max scene.
    Request({{"op", "validate"}, {"vehicle", fields}}, task);
    task.checkpoint();
    task.report(L"正在应用 Max 载具信息…");
    task.writeOnMain([&fields]
    {
        WySetVehicleFields(fields);
        return Json(nullptr);
    });
    compared_ = false;
}
void WyBridgeClient::Sync(const std::vector<std::string>& ids, bool toUE, const WyBridgeTask& task, bool firstCreate)
{
    task.checkpoint();
    if (!compared_)
        throw std::runtime_error("请先重新比对。");
    // Sample Max after waiting for UE. All writes are guarded by the panel's
    // scene generation, and local/remote versions must still match the preview.
    task.report(L"正在读取 UE 资源…");
    auto reply = Request({{"op", "read"}, {"vehicle", vehicle_}}, task);
    if (firstCreate && !reply.contains("blueprintPaths"))
        throw std::runtime_error("当前 UE 插件不支持创建车辆蓝图，请更新 UE 插件。");
    if (firstCreate && !reply.value("empty", false))
        throw std::runtime_error("目标车辆目录已有资源，请重新比对。");
    task.report(L"正在复核 Max 场景与所选节点…");
    auto now = WyReadScene(task.onMain);
    task.checkpoint();
    if (now.at("vehicle") != vehicle_)
        throw std::runtime_error("当前车辆信息已改变，请重新比对。");
    auto local = ById(now.at("items")), ue = ById(reply.at("items"));
    Json items = Json::array(), expected = Json::object();
    std::set<std::string> visited;
    task.report(L"正在检查所选节点的版本…");
    std::size_t processed = 0;
    const auto& vehicleName = vehicle_.at("name").get_ref<const std::string&>();
    for (const auto& id : ids)
    {
        task.checkpoint();
        task.progress(processed++, ids.size());
        if (!visited.insert(id).second)
            continue;
        auto l = Lookup(local, id), r = Lookup(ue, id);
        auto ol = localVersions_.find(id);
        auto previousUE = remoteVersions_.find(id);
        if (bool(l) != (ol != localVersions_.end()) || (l && LocalVersion(*l) != ol->second) ||
            bool(r) != (previousUE != remoteVersions_.end()) || (r && r->at("revision") != previousUE->second))
            throw std::runtime_error("所选项在比对后已改变，请重新比对：" + id);
        if (!l && !r)
            continue;
        const auto status = Compare(l, ScopeForComparison(ue, id, l), true, vehicleName);
        if (status != "different" && status != (toUE ? "ueMissing" : "maxMissing"))
            continue;
        if (l && l->contains("data") && l->at("data").contains("materialSlots"))
        {
            const auto& slots = l->at("data").at("materialSlots");
            if (!slots.empty() && !hello_.value("meshMaterialSlots", false))
                throw std::runtime_error("当前 UE 插件不支持材质槽同步，请更新 UE 插件后重新连接。");
            if (toUE && !hello_.value("vehicleMaterialNames", false) &&
                std::any_of(slots.begin(), slots.end(), [](const Json& slot) { return slot.at("name").get_ref<const std::string&>().starts_with("MI_"); }))
                throw std::runtime_error("当前 UE 插件不支持按车名创建材质实例，请更新 UE 插件后重新连接。");
        }
        items.push_back(toUE ? *l : *r);
        expected[id] = r ? r->at("revision") : Json("");
    }
    if (!ids.empty())
        task.progress(ids.size(), ids.size());
    if (items.empty())
        return;
    if (toUE)
    {
        Json request = {{"op", "write"}, {"vehicle", vehicle_}, {"items", std::move(items)}, {"expected", std::move(expected)}};
        if (firstCreate)
            request["createVehicleBlueprints"] = true;
        if (const auto* root = Lookup(local, "Root"))
            request["skeleton"] = *root;
        task.report(L"正在写入 UE 资源…");
        Request(request, task, true);
    }
    else
    {
        const bool needsBones =
            std::any_of(items.begin(), items.end(), [](const Json& item) { return item.at("kind") == "skeletalPart"; });
        if (needsBones && !Lookup(local, "Root") && !visited.contains("Root"))
        {
            const Json* root = Lookup(ue, "Root");
            const auto previous = remoteVersions_.find("Root");
            if (!root || previous == remoteVersions_.end() || root->at("revision") != previous->second ||
                root->contains("error"))
                throw std::runtime_error("找不到已比对的参考骨架。请重新比对 Root 后再接收蒙皮部件。");
            items.push_back(*root);
        }
        task.checkpoint();
        task.report(L"正在写入 Max 场景…");
        WyWriteScene(items, task.writeOnMain);
    }
    compared_ = false;
}
void WyBridgeClient::BuildCollider(const WyColliderBuildSettings& settings, const WyBridgeTask& task)
{
    task.report(L"正在连接 UE…");
    Connect(task);
    task.report(L"正在读取 LOD0 碰撞源网格…");
    const bool split = settings.split;
    const Json hullParameters = settings.HullParameters();
    Json hullSettings = hullParameters;
    hullSettings["mode"] = split ? "split" : "single";
    hullSettings["plane"] = split ? "manual" : "auto";
    Json worldZ;
    auto input = WyReadColliderInput(task.onMain, split ? &worldZ : nullptr);
    if (split)
    {
        hullSettings["splitZ"] = settings.worldZCm.value();
        hullSettings["worldZ"] = worldZ;
    }
    task.checkpoint();
    task.report(L"正在由 UE 构建凸包…");
    auto reply = Request({{"op", "hulls"}, {"settings", std::move(hullSettings)}, {"items", std::move(input)}}, task);
    if (!reply.value("hullSimplification", false) && hullParameters != WyColliderBuildSettings{}.HullParameters())
        throw std::runtime_error("当前 UE 插件未确认高级凸包参数。请更新 UE 插件后重新生成。");
    // Older UE plugins always cut along the vehicle's local Z axis.
    if (split && worldZ != Json::array({0, 0, 1, 0}) && reply.value("worldZ", Json()) != worldZ)
        throw std::runtime_error("UE 插件未确认 Max 世界 Z 切面。请更新 UE 插件后重新生成。");
    const auto& result = reply.at("items");
    if (!result.is_array() || result.size() != 1 || result[0].value("id", "") != "BodyCollider" ||
        result[0].at("data").at("shapes").empty())
        throw std::runtime_error("UE 没有返回有效的车身碰撞结果。");
    task.checkpoint();
    task.report(L"正在将凸包写入 Max 场景…");
    WyWriteScene(result, task.writeOnMain, &settings);
    compared_ = false;
}
