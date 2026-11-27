#include "MaxScene.h"
#include <Windows.h>
#include <max.h>
#include <geom/dpoint3.h>
#include <triobj.h>
#include <stdmat.h>
#include <iparamb2.h>
#include <modstack.h>
#include <iskin.h>
#include <MeshNormalSpec.h>
#include <hold.h>
#include <ref.h>
#include <units.h>
#include <custcont.h>
#include <maxheapdirect.h>
#include <IGame/IGame.h>
#include <IGame/IGameObject.h>
#include <IGame/IConversionManager.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Json = nlohmann::json;
constexpr TimeValue ReferenceTime = 0;
const Class_ID BridgeId(0x13ed76a5, 0x681c4bd2);
constexpr DWORD TangentChunk = 0x57595431;
constexpr DWORD MaterialChunk = 0x57594D31;
constexpr wchar_t ColliderModeProperty[] = L"WY.BodyColliderMode";
constexpr wchar_t ColliderHeightProperty[] = L"WY.BodyColliderWorldZCm";
const std::pair<const char *, const wchar_t *> VehicleFieldKeys[] = {
    {"type", L"WY.VehicleType"}, {"name", L"WY.VehicleName"}, {"variant", L"WY.VariantName"}};
std::uint64_t selectionEpoch = 1;
thread_local const std::function<void()> *scenePaint = nullptr;

void PaintProgress(size_t index = 0)
{
    if (!scenePaint || (index & 255) != 0)
        return;
    static thread_local ULONGLONG next = 0;
    const auto now = GetTickCount64();
    if (now >= next)
    {
        next = now + 75;
        (*scenePaint)();
    }
}

[[noreturn]] void Fail(const std::string &message)
{
    throw std::runtime_error(message);
}
void Require(bool value, const std::string &message)
{
    if (!value)
        Fail(message);
}
std::string Utf8(const wchar_t *value)
{
    if (!value || !*value)
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    Require(n > 0, "Invalid Unicode node name");
    std::string result(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), n, nullptr, nullptr);
    result.pop_back();
    return result;
}
std::wstring Wide(const std::string &value)
{
    Require(value.find('\0') == std::string::npos, "Embedded NUL in name");
    if (value.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                nullptr, 0);
    Require(n > 0, "Invalid UTF-8 name");
    std::wstring result(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), n);
    return result;
}
void MainThread()
{
    Require(GetCurrentThreadId() == GetWindowThreadProcessId(GetCOREInterface()->GetMAXHWnd(), nullptr),
            "Scene access must run on the 3ds Max main thread");
}
double Cm()
{
    double scale = GetSystemUnitScale(UNITS_CENTIMETERS);
    Require(std::isfinite(scale) && scale > 0, "Invalid Max system units");
    return scale;
}
float Number(const Json &v)
{
    double d = v.get<double>();
    Require(std::isfinite(d) && std::abs(d) <= 1e20, "Non-finite or excessive numeric value");
    return static_cast<float>(d);
}
Point3 Point(const Json &v)
{
    Require(v.is_array() && v.size() == 3, "Expected a three-component vector");
    return Point3(Number(v[0]), Number(v[1]), Number(v[2]));
}
Json Vector(const Point3 &p)
{
    Require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z), "Non-finite mesh value");
    return Json::array({p.x, p.y, p.z});
}
Point3 Unit(Point3 p)
{
    float length = Length(p);
    Require(std::isfinite(length) && length > 1e-12f, "Zero normal or tangent");
    return p / length;
}
Point3 Reflect(Point3 p)
{
    p.y = -p.y;
    return p;
}
float Determinant(const Matrix3 &m)
{
    return DotProd(CrossProd(m.GetRow(0), m.GetRow(1)), m.GetRow(2));
}
Matrix3 Invert(const Matrix3 &m)
{
    Require(std::abs(Determinant(m)) > 1e-12f, "Singular node transform");
    return Inverse(m);
}
Point3 Direction(const Point3 &p, const Matrix3 &m)
{
    return VectorTransform(m, p);
}
Point3 Normal(const Point3 &p, const Matrix3 &m)
{
    return Unit(Direction(p, AffineTranspose(Invert(m))));
}
bool Near(const Matrix3 &a, const Matrix3 &b)
{
    for (int i = 0; i < 4; ++i)
        if (Length(a.GetRow(i) - b.GetRow(i)) > 1e-4f)
            return false;
    return true;
}

void Walk(INode *node, const std::function<void(INode *)> &visit)
{
    for (int i = 0; i < node->NumberOfChildren(); ++i)
    {
        INode *child = node->GetChildNode(i);
        visit(child);
        Walk(child, visit);
    }
}
INode *Find(const std::string &name)
{
    INode *result = nullptr;
    Walk(GetCOREInterface()->GetRootNode(), [&](INode *node) {
        if (Utf8(node->GetName()) == name)
        {
            Require(!result, "Ambiguous node name: " + name);
            result = node;
        }
    });
    return result;
}
INode *Collider()
{
    INode *current = Find("BodyCollider");
    INode *legacy = Find("BodyColider");
    Require(!current || !legacy, "Both BodyCollider and BodyColider exist; resolve the ambiguity first");
    return current ? current : legacy;
}
INode *FindBone(const std::string &name)
{
    INode *root = Find("Root");
    if (!root)
        return nullptr;
    if (name == "Root")
        return root;
    INode *result = nullptr;
    Walk(root, [&](INode *node) {
        if (Utf8(node->GetName()) == name)
        {
            Require(!result, "Duplicate bone name under Root: " + name);
            result = node;
        }
    });
    return result;
}
INode *VehicleParent()
{
    INode *parent = nullptr;
    for (INode *node : {Find("Root"), Find("LOD0"), Collider()})
        if (node)
        {
            Require(!parent || parent == node->GetParentNode(),
                    "Root, LOD0 and BodyCollider must share one vehicle parent");
            parent = node->GetParentNode();
        }
    return parent ? parent : GetCOREInterface()->GetRootNode();
}
Matrix3 Frame()
{
    auto *parent = VehicleParent();
    return parent == GetCOREInterface()->GetRootNode() ? Matrix3() : parent->GetNodeTM(ReferenceTime);
}
// Only value data crosses the boundary between posted SDK calls.
Json FrameRows(const Matrix3 &frame)
{
    Json rows = Json::array();
    for (int row = 0; row < 4; ++row)
        rows.push_back(Vector(frame.GetRow(row)));
    return rows;
}
Matrix3 ReadFrameRows(const Json &rows)
{
    Matrix3 frame;
    for (int row = 0; row < 4; ++row)
        frame.SetRow(row, Point(rows.at(row)));
    return frame;
}
Json NodeKey(INode *node)
{
    return {{"handle", node->GetHandle()}, {"name", Utf8(node->GetName())},
            {"parent", node->GetParentNode()->GetHandle()}};
}
INode *ResolveNode(const Json &key)
{
    auto *node = GetCOREInterface()->GetINodeByHandle(key.at("handle").get<ULONG>());
    Require(node && node->GetParentNode() && NodeKey(node) == key, "Max scene changed during reading; compare again");
    return node;
}
Json SelectionKey(INode *node)
{
    Json key = NodeKey(node);
    key["epoch"] = selectionEpoch;
    auto &ancestors = key["ancestors"] = Json::array();
    auto *sceneRoot = GetCOREInterface()->GetRootNode();
    for (INode *parent = node->GetParentNode(); parent && parent != sceneRoot; parent = parent->GetParentNode())
        ancestors.push_back({parent->GetHandle(), Utf8(parent->GetName())});
    return key;
}
struct SkinInfo
{
    Modifier *modifier = nullptr;
    ISkin *skin = nullptr;
};
SkinInfo Skin(INode *node, bool strict = true)
{
    SkinInfo result;
    Object *object = node->GetObjectRef();
    while (object && object->SuperClassID() == GEN_DERIVOB_CLASS_ID)
    {
        auto *derived = static_cast<IDerivedObject *>(object);
        for (int i = 0; i < derived->NumModifiers(); ++i)
        {
            Modifier *modifier = derived->GetModifier(i);
            if (modifier->ClassID() == SKIN_CLASSID)
            {
                Require(!result.skin, "Multiple Skin modifiers are not reversible");
                result = {modifier, static_cast<ISkin *>(modifier->GetInterface(I_SKIN))};
                Require(result.skin != nullptr, "Skin interface unavailable");
            }
            else if (strict)
                Fail("Additional modifier stack requires a reversible rule: " + Utf8(node->GetName()));
        }
        object = derived->GetObjRef();
    }
    return result;
}
Matrix3 ObjectFrame(INode *node, const Matrix3 &frame, const SkinInfo &skin)
{
    Matrix3 object = node->GetObjTMAfterWSM(ReferenceTime);
    if (skin.skin)
        Require(skin.skin->GetSkinInitTM(node, object, true) == SKIN_OK, "Missing Skin bind transform");
    return object * Invert(frame);
}
Mesh CopyMesh(INode *node, const SkinInfo &skin)
{
    node->EvalWorldState(ReferenceTime);
    Object *object = skin.skin ? node->GetObjectRef()->FindBaseObject()->Eval(ReferenceTime).obj
                               : node->EvalWorldState(ReferenceTime).obj;
    Require(object && object->CanConvertToType(triObjectClassID),
            "Node is not a triangle-convertible mesh: " + Utf8(node->GetName()));
    auto *tri = static_cast<TriObject *>(object->ConvertToType(ReferenceTime, triObjectClassID));
    Require(tri != nullptr, "Mesh conversion failed");
    Mesh mesh = tri->GetMesh();
    if (tri != object)
        tri->DeleteThis();
    Require(mesh.getNumVerts() > 0 && mesh.getNumFaces() > 0, "Mesh is empty");
    return mesh;
}
Point3 MapValue(Mesh &mesh, int channel, int face, int corner, Point3 fallback)
{
    if (!mesh.mapSupport(channel))
        return fallback;
    Require(mesh.mapFaces(channel) && mesh.mapVerts(channel) &&
                mesh.Map(channel).getNumFaces() == mesh.getNumFaces(),
            "Invalid map topology");
    int index = static_cast<int>(mesh.mapFaces(channel)[face].t[corner]);
    Require(index >= 0 && index < mesh.getNumMapVerts(channel), "Invalid map vertex index");
    return mesh.mapVerts(channel)[index];
}

using MaterialBindings = std::map<MtlID, Mtl *>;
// Autodesk's Multi/Sub-Object materialIDList validator accepts IDs [0,60000).
// The face channel itself stores the full unsigned 16-bit range.
constexpr int MaxMultiSlots = 60000;

ParamID MultiParam(IParamBlock2 *block, const wchar_t *name)
{
    Require(block != nullptr, "Missing Multi/Sub-Object parameter block");
    for (int i = 0; i < block->NumParams(); ++i)
    {
        const auto id = block->IndextoID(i);
        const auto *key = block->GetParamDef(id).int_name;
        if (key && std::wcscmp(key, name) == 0)
            return id;
    }
    Fail("Missing Multi/Sub-Object parameter: " + Utf8(name));
}

MaterialBindings BoundMaterials(Mtl *material, const std::set<MtlID> &faceIds)
{
    MaterialBindings result;
    if (!material)
        return result;
    if (material->ClassID() != MULTI_MATERIAL_CLASS_ID)
    {
        for (MtlID id : faceIds)
            result.emplace(id, material);
        return result;
    }
    // Multi/Sub's SDK accessors use the actual zero-based ID, even when its
    // rows are sparse or reordered. NumSubMtls() is max ID + 1, not row count.
    const int limit = std::clamp(material->NumSubMtls(), 0, 65536);
    auto *block = material->GetParamBlock(0);
    auto ids = MultiParam(block, L"materialIDList");
    for (int row = 0; row < block->Count(ids); ++row)
    {
        int id = block->GetInt(ids, ReferenceTime, row);
        if (id >= 0 && id <= 65535)
            if (auto *sub = material->GetSubMtl(id))
                result.emplace(static_cast<MtlID>(id), sub);
    }
    // Max shades out-of-range face IDs modulo max ID + 1. Holes and
    // disabled rows yield no material, and must never create a UE instance.
    for (MtlID id : faceIds)
        if (limit && id >= limit)
            if (auto *sub = material->GetSubMtl(id % limit))
                result.emplace(id, sub);
    return result;
}

Json ReadMaterialSlots(INode *node, const std::vector<MtlID> &faceIds, std::string &source)
{
    Json slots = Json::array();
    const auto bindings = BoundMaterials(node->GetMtl(), {faceIds.begin(), faceIds.end()});
    for (const auto &[id, material] : bindings)
    {
        auto name = Utf8(material->GetName());
        Require(!name.empty(), "请为 Max 材质命名：" + Utf8(node->GetName()) + "，材质 ID " + std::to_string(id + 1));
        slots.push_back({{"id", id}, {"name", name}});
    }
    // UE slot names and asset names can diverge after editing in UE. Remember
    // only an explicitly received binding whose live Max ID and name still match.
    auto *base = node->GetObjectRef()->FindBaseObject();
    if (auto *chunk = base->GetAppDataChunk(BridgeId, GUP_CLASS_ID, MaterialChunk); chunk && chunk->length)
    {
        const auto *bytes = static_cast<const char *>(chunk->data);
        auto cached = Json::parse(bytes, bytes + chunk->length, nullptr, false);
        if (cached.is_object() && cached.contains("slots") && cached["slots"].is_object() &&
            cached.contains("source") && cached["source"].is_string())
            for (auto &slot : slots)
            {
                const auto &table = cached["slots"];
                const auto found = table.find(std::to_string(slot.at("id").get<MtlID>()));
                if (found != table.end() && found->is_object() && found->contains("asset") &&
                    (*found)["asset"].is_string() && found->contains("name") && (*found)["name"] == slot["name"])
                {
                    slot["asset"] = (*found)["asset"];
                    if (!cached.contains("vehicleMaterialNames"))
                        slot["legacyAssetNaming"] = true;
                    source = cached["source"].get<std::string>();
                }
            }
    }
    return slots;
}

// SDK objects stay inside the posted main-thread work. Dense numeric buffers
// let the worker pay the per-corner JSON allocation/serialization cost.
struct MeshSnapshot
{
    std::vector<std::array<float, 3>> positions, normals;
    std::vector<std::array<DWORD, 3>> triangles;
    std::vector<std::array<float, 4>> colors, tangents;
    std::vector<std::vector<std::array<float, 2>>> uvs;
    std::vector<MtlID> materialIds;
    Json materialSlots = Json::array();
    std::string materialSlotSource;
    std::vector<std::string> bones;
    std::vector<size_t> weightEnds;
    std::vector<std::pair<int, float>> weights;
    std::string tangentCache;

    Json Data() const
    {
        Json data = Json::object();
        data["positions"] = positions;
        data["triangles"] = triangles;
        data["normals"] = normals;
        data["colors"] = colors;
        data["uvs"] = uvs;
        data["tangents"] = tangents;
        data["materialIds"] = materialIds;
        data["materialSlots"] = materialSlots;
        if (!materialSlotSource.empty())
            data["materialSlotSource"] = materialSlotSource;
        if (!weightEnds.empty())
        {
            auto &values = data["weights"] = Json::array();
            size_t offset = 0;
            for (size_t end : weightEnds)
            {
                Json vertex = Json::array();
                for (; offset < end; ++offset)
                {
                    const auto &[bone, weight] = weights[offset];
                    vertex.push_back({{"bone", bones[bone]}, {"weight", weight}});
                }
                values.push_back(std::move(vertex));
            }
        }
        return data;
    }
};
std::array<float, 3> Components(const Point3 &p)
{
    Require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z), "Non-finite mesh value");
    return {p.x, p.y, p.z};
}
enum class MeshKind { Static, Skeletal, Collision };

bool ReverseFaceOrder(const Matrix3 &objectFrame, MeshKind kind)
{
    // Collision faces use outward cross products on the wire. Reflecting Y
    // reverses their winding; a mirrored object-to-vehicle transform cancels it.
    // Render meshes use UE's render winding and explicit per-corner attributes.
    const bool mirrored = Determinant(objectFrame) < 0;
    return kind == MeshKind::Collision ? !mirrored : mirrored;
}

Matrix3 CollisionPositionFrame(INode *node, const Matrix3 &vehicleFrame, Matrix3 objectFrame, double cmPerUnit)
{
    const float unitsPerCm = static_cast<float>(1.0 / cmPerUnit);
    if (cmPerUnit != 1.0 && node->GetObjOffsetScale().s == Point3(unitsPerCm, unitsPerCm, unitsPerCm) &&
        node->GetObjTMAfterWSM(ReferenceTime) == node->GetObjectTM(ReferenceTime))
    {
        // A centimetre-space hull stores the reciprocal unit scale as a float.
        // Cancel that offset before composing transforms, so reading an untouched
        // hull does not shift distant vertices by an ULP. Preserve real node edits.
        const Matrix3 nodeTM = node->GetNodeTM(ReferenceTime);
        const Matrix3 relative = nodeTM == vehicleFrame ? Matrix3() : nodeTM * Invert(vehicleFrame);
        Matrix3 offsetRotation;
        node->GetObjOffsetRot().MakeMatrix(offsetRotation);
        Matrix3 result = offsetRotation * relative;
        result.SetTrans((node->GetObjOffsetPos() * relative) * static_cast<float>(cmPerUnit));
        return result;
    }
    for (int row = 0; row < 4; ++row)
        objectFrame.SetRow(row, objectFrame.GetRow(row) * static_cast<float>(cmPerUnit));
    return objectFrame;
}

void CaptureMesh(INode *node, const Matrix3 &frame, MeshKind kind, bool tangents, bool allowCache, MeshSnapshot &data)
{
    const bool skinned = kind == MeshKind::Skeletal;
    SkinInfo skin = Skin(node, skinned);
    Require(!skinned || skin.skin, "Skeletal part has no Skin modifier: " + Utf8(node->GetName()));
    Require(skinned || !skin.skin, "Static mesh unexpectedly has Skin");
    Mesh mesh = CopyMesh(node, skin);
    Matrix3 object = ObjectFrame(node, frame, skin);
    const bool flipped = ReverseFaceOrder(object, kind);
    const int order[2][3] = {{0, 1, 2}, {0, 2, 1}};
    const double cmPerUnit = Cm();
    const bool collision = kind == MeshKind::Collision;
    const Matrix3 positionFrame = collision ? CollisionPositionFrame(node, frame, object, cmPerUnit) : object;
    const float positionScale = collision ? 1.f : static_cast<float>(cmPerUnit);
    const size_t faces = mesh.getNumFaces(), corners = faces * 3;
    data.positions.reserve(mesh.getNumVerts());
    data.triangles.reserve(faces);
    data.materialIds.reserve(faces);
    data.normals.reserve(corners);
    data.colors.reserve(corners);
    for (int i = 0; i < mesh.getNumVerts(); ++i)
    {
        PaintProgress(i);
        data.positions.push_back(Components(Reflect(mesh.verts[i] * positionFrame) * positionScale));
    }
    int uvCount = 0;
    for (int ch = 1; ch < mesh.getNumMaps(); ++ch)
        if (mesh.mapSupport(ch))
            uvCount = ch;
    Require(uvCount <= 8, "More than eight UV channels cannot be transferred to UE");
    data.uvs.resize(uvCount);
    for (auto &channel : data.uvs)
        channel.reserve(corners);
    mesh.SpecifyNormals();
    auto *normals = mesh.GetSpecifiedNormals();
    Require(normals != nullptr, "Cannot evaluate normals");
    normals->SetParent(&mesh);
    if (normals->GetNumFaces() == 0)
        normals->SetNumFaces(mesh.getNumFaces());
    normals->CheckNormals();
    Require(normals->GetNumFaces() == mesh.getNumFaces(), "Normal face count mismatch");
    const Matrix3 normalMatrix = AffineTranspose(Invert(object));
    for (int f = 0; f < mesh.getNumFaces(); ++f)
    {
        PaintProgress(f);
        std::array<DWORD, 3> triangle{};
        for (int c = 0; c < 3; ++c)
        {
            int corner = order[flipped][c];
            auto vi = mesh.faces[f].v[corner];
            Require(vi < static_cast<DWORD>(mesh.getNumVerts()), "Invalid mesh index");
            triangle[c] = vi;
            data.normals.push_back(Components(Reflect(Unit(Direction(normals->GetNormal(f, corner), normalMatrix)))));
            Point3 rgb = MapValue(mesh, 0, f, corner, Point3(1, 1, 1));
            float alpha = MapValue(mesh, -2, f, corner, Point3(1, 1, 1)).x;
            data.colors.push_back({rgb.x, rgb.y, rgb.z, alpha});
            for (int ch = 1; ch <= uvCount; ++ch)
            {
                Point3 uv = MapValue(mesh, ch, f, corner, Point3(0, 0, 0));
                data.uvs[ch - 1].push_back({uv.x, 1.0f - uv.y});
            }
        }
        data.triangles.push_back(triangle);
        data.materialIds.push_back(mesh.faces[f].getMatID());
    }
    if (skin.skin)
    {
        auto *context = skin.skin->GetContextInterface(node);
        Require(context && context->GetNumPoints() == mesh.getNumVerts(),
                "Skin point count differs from base topology");
        std::map<std::string, INode *> allowed;
        INode *root = Find("Root");
        Require(root, "Skin requires Root");
        allowed.emplace("Root", root);
        Walk(root, [&](INode *n) {
            Require(allowed.emplace(Utf8(n->GetName()), n).second, "Duplicate bone name under Root");
        });
        data.bones.resize(skin.skin->GetNumBones());
        for (int i = 0; i < skin.skin->GetNumBones(); ++i)
        {
            INode *bone = skin.skin->GetBone(i);
            Require(bone, "Skin references a missing bone");
            auto name = Utf8(bone->GetName());
            auto found = allowed.find(name);
            Require(found != allowed.end() && found->second == bone, "Skin influence is outside Root");
            data.bones[i] = name;
        }
        data.weightEnds.reserve(mesh.getNumVerts());
        data.weights.reserve(mesh.getNumVerts());
        for (int v = 0; v < mesh.getNumVerts(); ++v)
        {
            PaintProgress(v);
            double sum = 0;
            for (int j = 0; j < context->GetNumAssignedBones(v); ++j)
            {
                int index = context->GetAssignedBone(v, j);
                Require(index >= 0 && static_cast<size_t>(index) < data.bones.size(),
                        "Invalid Skin bone index");
                float weight = context->GetBoneWeight(v, j);
                Require(std::isfinite(weight) && weight >= 0, "Invalid Skin weight");
                if (weight > 0)
                {
                    data.weights.emplace_back(index, weight);
                    sum += weight;
                }
            }
            Require(std::abs(sum - 1) <= 1e-4, "Skin weights must sum to one");
            data.weightEnds.push_back(data.weights.size());
        }
    }
    if (!tangents)
        return;
    Object *base = node->GetObjectRef()->FindBaseObject();
    if (allowCache)
    {
        if (auto *chunk = base->GetAppDataChunk(BridgeId, GUP_CLASS_ID, TangentChunk); chunk && chunk->length)
        {
            data.tangentCache.assign(static_cast<const char *>(chunk->data), chunk->length);
            return;
        }
    }
    if (skin.skin)
        for (int i = 0; i < skin.skin->GetNumBones(); ++i)
        {
            INode *bone = skin.skin->GetBone(i);
            Matrix3 bind;
            Require(bone && skin.skin->GetBoneInitTM(bone, bind, false) == SKIN_OK &&
                        Near(bind, bone->GetNodeTM(ReferenceTime)),
                    "IGame tangent extraction requires the Skin reference pose at frame zero");
        }
    // IGame exposes the real Max tangent/binormal; do not substitute a triangle derivative.
    auto *game = GetIGameInterface();
    auto *conversion = GetConversionManager();
    Require(game && conversion, "IGame tangent interface unavailable");
    struct IGameScope
    {
        IGameScene *game;
        IGameConversionManager *conversion;
        IGameConversionManager::CoordSystem previous;
        IGameNode *node = nullptr;
        IGameObject *object = nullptr;
        ~IGameScope()
        {
            if (node && object)
                node->ReleaseIGameObject();
            game->ReleaseIGame();
            conversion->SetCoordSystem(previous);
        }
    } scope{game, conversion, conversion->GetCoordSystem()};
    conversion->SetCoordSystem(IGameConversionManager::IGAME_MAX);
    Require(game->InitialiseIGame(node, false), "IGame initialization failed");
    game->SetStaticFrame(0);
    scope.node = game->GetIGameNode(node);
    scope.object = scope.node ? scope.node->GetIGameObject() : nullptr;
    Require(scope.object && scope.object->GetIGameType() == IGameObject::IGAME_MESH,
            "IGame mesh unavailable");
    auto *gm = static_cast<IGameMesh *>(scope.object);
    Require(gm->InitializeData() && gm->GetNumberOfFaces() == mesh.getNumFaces(), "IGame topology mismatch");
    if (uvCount == 0)
        Fail("Mesh needs UV channel 1 to preserve its tangent basis");
    gm->InitializeBinormalData();
    const Matrix3 inverseFrame = Invert(frame);
    data.tangents.reserve(corners);
    for (int f = 0; f < mesh.getNumFaces(); ++f)
    {
        PaintProgress(f);
        for (int c = 0; c < 3; ++c)
        {
            int corner = order[flipped][c];
            auto *face = gm->GetFace(f);
            int gameCorner = -1;
            // IGame reverses the corner order itself for a mirrored node transform.
            if (face)
                for (int k = 0; k < 3; ++k)
                    if (face->vert[k] == mesh.faces[f].v[corner])
                        gameCorner = k;
            Require(gameCorner >= 0, "IGame triangulation differs from the source mesh");
            int index = gm->GetFaceVertexTangentBinormal(f, gameCorner, 1);
            Point3 t(0, 0, 0), b(0, 0, 0);
            Require(index >= 0 && gm->GetTangent(index, t, 1) && gm->GetBinormal(index, b, 1),
                    "IGame tangent extraction failed");
            // IGame returns tangents/binormals in world coordinates. The static
            // rotation/parent-frame probe verifies that applying object TM again
            // would rotate them twice (GetNormal has a separate ObjectSpace flag).
            t = Unit(Reflect(Direction(t, inverseFrame)));
            b = -Unit(Reflect(Direction(b, inverseFrame)));
            const auto &normal = data.normals[f * 3 + c];
            Point3 n(normal[0], normal[1], normal[2]);
            float sign = DotProd(CrossProd(n, t), b) < 0 ? -1.0f : 1.0f;
            data.tangents.push_back({t.x, t.y, t.z, sign});
        }
    }
}

std::optional<std::string> SceneTextProperty(const wchar_t *name)
{
    PROPSPEC spec{};
    spec.ulKind = PRSPEC_LPWSTR;
    spec.lpwstr = const_cast<LPWSTR>(name);
    const int index = GetCOREInterface()->FindProperty(PROPSET_USERDEFINED, &spec);
    if (index < 0)
        return std::string{};
    const auto *v = GetCOREInterface()->GetPropertyVariant(PROPSET_USERDEFINED, index);
    if (v && v->vt == VT_LPWSTR && v->pwszVal)
        return Utf8(v->pwszVal);
    if (v && v->vt == VT_BSTR && v->bstrVal)
        return Utf8(v->bstrVal);
    if (v && v->vt == VT_LPSTR && v->pszVal)
    {
        int n = MultiByteToWideChar(CP_ACP, 0, v->pszVal, -1, nullptr, 0);
        std::wstring wide(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_ACP, 0, v->pszVal, -1, wide.data(), n);
        return Utf8(wide.c_str());
    }
    return std::nullopt;
}
Json ReadFields()
{
    Json result;
    for (const auto &[key, property] : VehicleFieldKeys)
    {
        const auto value = SceneTextProperty(property);
        Require(value.has_value(), "Vehicle metadata is not text");
        result[key] = *value;
    }
    if (result["variant"].get<std::string>().empty())
        result["variant"] = "Base";
    return result;
}
Json TransformData(Matrix3 matrix)
{
    Matrix3 reflection = ScaleMatrix(Point3(1, -1, 1));
    matrix = reflection * matrix * reflection;
    Point3 scale(Length(matrix.GetRow(0)), Length(matrix.GetRow(1)), Length(matrix.GetRow(2)));
    Require(scale.x > 1e-8f && scale.y > 1e-8f && scale.z > 1e-8f, "Zero bone scale");
    if (Determinant(matrix) < 0)
        scale.x = -scale.x;
    Matrix3 rotation;
    for (int i = 0; i < 3; ++i)
        rotation.SetRow(i, matrix.GetRow(i) / scale[i]);
    Require(std::abs(DotProd(rotation.GetRow(0), rotation.GetRow(1))) < 1e-5f &&
                std::abs(DotProd(rotation.GetRow(0), rotation.GetRow(2))) < 1e-5f &&
                std::abs(DotProd(rotation.GetRow(1), rotation.GetRow(2))) < 1e-5f,
            "Bone shear cannot be represented by UE TRS");
    Quat q(rotation);
    return {{"translation", Vector(matrix.GetTrans() * static_cast<float>(Cm()))},
            {"rotation", Json::array({q.x, q.y, q.z, q.w})},
            {"scale", Vector(scale)}};
}
Matrix3 ReadTransform(const Json &value)
{
    Point3 translation = Point(value.at("translation")) / static_cast<float>(Cm()),
           scale = Point(value.at("scale"));
    const auto &r = value.at("rotation");
    Require(r.is_array() && r.size() == 4, "Invalid bone quaternion");
    Quat q(Number(r[0]), Number(r[1]), Number(r[2]), Number(r[3]));
    double length = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    Require(std::abs(length - 1) <= 1e-4, "Bone quaternion must be normalized");
    Matrix3 rotation;
    q.MakeMatrix(rotation);
    Matrix3 matrix = ScaleMatrix(scale) * rotation;
    matrix.SetTrans(translation);
    Invert(matrix);
    Matrix3 reflection = ScaleMatrix(Point3(1, -1, 1));
    return reflection * matrix * reflection;
}
Json SkeletonData(INode *root, const Matrix3 &frame)
{
    Json bones = Json::array();
    std::vector<INode *> nodes{root};
    Walk(root, [&](INode *n) { nodes.push_back(n); });
    std::map<INode *, Matrix3> bind;
    Walk(GetCOREInterface()->GetRootNode(), [&](INode *n) {
        PaintProgress();
        auto skin = Skin(n, false);
        if (!skin.skin)
            return;
        for (auto *bone : nodes)
        {
            Matrix3 tm;
            if (skin.skin->GetBoneInitTM(bone, tm, false) == SKIN_OK)
            {
                auto old = bind.find(bone);
                Require(old == bind.end() || Near(old->second, tm),
                        "Skin modifiers disagree about bone bind pose");
                bind[bone] = tm;
            }
        }
    });
    auto world = [&](INode *node) {
        auto it = bind.find(node);
        return it == bind.end() ? node->GetNodeTM(ReferenceTime) : it->second;
    };
    std::set<std::string> names;
    for (auto *node : nodes)
    {
        std::string name = Utf8(node->GetName());
        Require(names.insert(name).second, "Duplicate bone name");
        Json bone = TransformData(world(node) * Invert(node == root ? frame : world(node->GetParentNode())));
        bone["name"] = name;
        bone["parent"] = node == root ? "" : Utf8(node->GetParentNode()->GetName());
        bones.push_back(bone);
    }
    return {{"bones", bones}};
}

struct SceneBatch
{
    bool finished = false;
    SceneBatch()
    {
        Require(!theHold.Holding(), "Finish the current Max operation before synchronizing");
        theHold.Begin();
    }
    ~SceneBatch()
    {
        if (!finished)
            theHold.Cancel();
    }
    void Accept(const wchar_t *name)
    {
        theHold.Accept(MSTR(name));
        finished = true;
        SetSaveRequiredFlag(TRUE);
        GetCOREInterface()->RedrawViews(GetCOREInterface()->GetTime());
    }
};
struct PauseHold
{
    PauseHold()
    {
        theHold.Suspend();
    }
    ~PauseHold()
    {
        theHold.Resume();
    }
};
void ApplyObject(ULONG handle, Object *object)
{
    if (auto *node = GetCOREInterface()->GetINodeByHandle(handle))
    {
        node->SetObjectRef(object);
        node->NotifyDependents(FOREVER, PART_ALL, REFMSG_CHANGE);
    }
}
class ObjectRestore final : public RestoreObj
{
    ULONG handle;
    SingleRefMaker before, after;

  public:
    ObjectRestore(INode *n, Object *replacement)
        : handle(n->GetHandle()), before(n->GetObjectRef()), after(replacement)
    {
    }
    void Restore(int) override
    {
        ApplyObject(handle, static_cast<Object *>(before.GetRef()));
    }
    void Redo() override
    {
        ApplyObject(handle, static_cast<Object *>(after.GetRef()));
    }
};
void ReplaceObject(INode *node, Object *object)
{
    theHold.Put(new ObjectRestore(node, object));
    theHold.Suspend();
    ApplyObject(node->GetHandle(), object);
    theHold.Resume();
}
INode *MakeNode(Object *object, const std::string &name, INode *parent, const Matrix3 &tm)
{
    INode *node = GetCOREInterface()->CreateObjectNode(object);
    Require(node != nullptr, "Cannot create Max node");
    auto wide = Wide(name);
    node->SetName(wide.c_str());
    parent->AttachChild(node, 1);
    node->SetNodeTM(ReferenceTime, tm);
    return node;
}
INode *MakeGroup(const std::string &name, INode *parent, const Matrix3 &tm)
{
    auto *object = static_cast<Object *>(
        GetCOREInterface()->CreateInstance(HELPER_CLASS_ID, Class_ID(DUMMY_CLASS_ID, 0)));
    Require(object != nullptr, "Cannot create group helper");
    return MakeNode(object, name, parent, tm);
}
void CheckMesh(const Json &data, std::set<std::string> *requiredBones)
{
    size_t checked = 0;
    const auto checkVector = [](const Json &v, size_t components, bool direction) {
        Require(v.is_array() && v.size() == components, "Invalid vector size");
        float x = Number(v[0]), y = Number(v[1]), z = Number(v[2]);
        if (direction)
        {
            float length = std::sqrt(x * x + y * y + z * z);
            Require(std::isfinite(length) && length > 1e-12f, "Zero normal or tangent");
        }
    };
    const auto &positions = data.at("positions");
    const auto &triangles = data.at("triangles");
    Require(positions.is_array() && !positions.empty() && positions.size() <= 10000000 &&
                triangles.is_array() && !triangles.empty() && triangles.size() <= 10000000,
            "Invalid mesh size");
    for (const auto &p : positions)
    {
        PaintProgress(checked++);
        checkVector(p, 3, false);
    }
    for (const auto &t : triangles)
    {
        PaintProgress(checked++);
        Require(t.is_array() && t.size() == 3, "Triangle must have three indices");
        std::set<int> unique;
        for (const auto &i : t)
        {
            Require(i.is_number_integer(), "Triangle index must be integer");
            int index = i.get<int>();
            Require(index >= 0 && static_cast<size_t>(index) < positions.size(),
                    "Triangle index out of range");
            unique.insert(index);
        }
        Require(unique.size() == 3, "Degenerate triangle indices");
    }
    size_t corners = triangles.size() * 3;
    for (const char *key : {"normals", "colors", "tangents"})
        if (data.contains(key))
            Require(data[key].is_array() && data[key].size() == corners,
                    std::string(key) + " count differs from triangle corners");
    if (data.contains("normals"))
        for (const auto &n : data["normals"])
        {
            PaintProgress(checked++);
            checkVector(n, 3, true);
        }
    if (data.contains("colors"))
        for (const auto &c : data["colors"])
        {
            PaintProgress(checked++);
            Require(c.is_array() && c.size() == 4, "Invalid RGBA");
            for (const auto &v : c)
            {
                float f = Number(v);
                Require(f >= 0 && f <= 1, "RGBA outside [0,1]");
            }
        }
    if (data.contains("tangents"))
        for (const auto &t : data["tangents"])
        {
            PaintProgress(checked++);
            checkVector(t, 4, true);
            Require(std::abs(Number(t[3])) == 1, "Tangent sign must be +/-1");
        }
    if (data.contains("uvs"))
    {
        Require(data["uvs"].is_array() && data["uvs"].size() <= 8, "Invalid UV channel count");
        for (const auto &channel : data["uvs"])
        {
            Require(channel.size() == corners, "UV corner count mismatch");
            for (const auto &uv : channel)
            {
                PaintProgress(checked++);
                Require(uv.is_array() && uv.size() == 2, "Invalid UV");
                Number(uv[0]);
                Number(uv[1]);
            }
        }
    }
    if (data.contains("materialIds"))
    {
        Require(data["materialIds"].is_array() && data["materialIds"].size() == triangles.size(),
                "Material ID count mismatch");
        for (const auto &id : data["materialIds"])
            Require(id.is_number_integer() && id >= 0 && id <= 65535, "Invalid material ID");
    }
    if (data.contains("materialSlots"))
    {
        const auto &slots = data["materialSlots"];
        Require(slots.is_array() && slots.size() <= 65536, "Invalid material slots");
        std::set<MtlID> ids;
        for (const auto &slot : slots)
        {
            const auto &id = slot.at("id");
            Require(id.is_number_integer() && id >= 0 && id <= 65535 && ids.insert(id.get<MtlID>()).second,
                    "Duplicate or invalid material slot ID");
            Wide(slot.at("name").get<std::string>());
            if (slot.contains("asset"))
                Wide(slot["asset"].get<std::string>());
        }
    }
    if (requiredBones)
    {
        Require(data.contains("weights") && data["weights"].size() == positions.size(),
                "Skin weight count mismatch");
        for (const auto &weights : data["weights"])
        {
            PaintProgress(checked++);
            Require(weights.is_array() && !weights.empty(), "Unweighted vertex");
            double sum = 0;
            std::set<std::string> names;
            for (const auto &w : weights)
            {
                auto name = w.at("bone").get<std::string>();
                Require(!name.empty() && names.insert(name).second, "Duplicate or empty influence");
                Wide(name);
                requiredBones->insert(std::move(name));
                float weight = Number(w.at("weight"));
                Require(weight > 0 && weight <= 1, "Invalid influence weight");
                sum += weight;
            }
            Require(std::abs(sum - 1) < 1e-4, "Skin weights must sum to one");
        }
    }
}
DPoint3 CollisionPoint(const Json &point)
{
    return {point[0].get<double>(), -point[1].get<double>(), point[2].get<double>()};
}

void SetCollisionPositions(Mesh &mesh, const Json &positions, double cmPerUnit, const DPoint3 &origin)
{
    for (int i = 0; i < mesh.getNumVerts(); ++i)
    {
        PaintProgress(i);
        const DPoint3 p = CollisionPoint(positions[i]) - origin;
        // The SDK's Point3 division multiplies by a float reciprocal. Round
        // only once, after the double-precision unit conversion.
        mesh.verts[i] = Point3(float(p.x / cmPerUnit), float(p.y / cmPerUnit), float(p.z / cmPerUnit));
    }
    mesh.InvalidateGeomCache();
}

bool CollisionFacesPointOut(const Mesh &mesh)
{
    DPoint3 interior(0.0, 0.0, 0.0);
    for (int i = 0; i < mesh.getNumVerts(); ++i)
        interior += DPoint3(mesh.verts[i]);
    interior /= mesh.getNumVerts();
    for (int i = 0; i < mesh.getNumFaces(); ++i)
    {
        const DPoint3 normal(mesh.FaceNormal(i, TRUE));
        const DPoint3 fromInterior = DPoint3(mesh.verts[mesh.faces[i].v[0]]) - interior;
        if (!(DotProd(normal, fromInterior) > 0.0))
            return false;
    }
    return true;
}

TriObject *BuildMesh(const Json &data, const Matrix3 &objectFrame, MeshKind kind)
{
    auto *tri = CreateNewTriObject();
    Require(tri != nullptr, "Cannot allocate mesh");
    Mesh &mesh = tri->GetMesh();
    const int vertices = static_cast<int>(data["positions"].size()),
              faces = static_cast<int>(data["triangles"].size());
    mesh.setNumVerts(vertices);
    mesh.setNumFaces(faces);
    Matrix3 inverse = Invert(objectFrame);
    const bool flip = ReverseFaceOrder(objectFrame, kind);
    const int order[2][3] = {{0, 1, 2}, {0, 2, 1}};
    if (kind == MeshKind::Collision)
        SetCollisionPositions(mesh, data.at("positions"), Cm(), DPoint3(0.0, 0.0, 0.0));
    else
    {
        const float scale = static_cast<float>(Cm());
        for (int i = 0; i < vertices; ++i)
        {
            PaintProgress(i);
            mesh.verts[i] = Reflect(Point(data["positions"][i])) / scale * inverse;
        }
    }
    for (int f = 0; f < faces; ++f)
    {
        PaintProgress(f);
        auto &face = mesh.faces[f];
        face.setVerts(data["triangles"][f][order[flip][0]].get<int>(),
                      data["triangles"][f][order[flip][1]].get<int>(),
                      data["triangles"][f][order[flip][2]].get<int>());
        face.setEdgeVisFlags(1, 1, 1);
        face.setSmGroup(1);
        face.setMatID(data.contains("materialIds") ? data["materialIds"][f].get<MtlID>() : 0);
    }
    auto map = [&](int channel, const std::function<Point3(int)> &value) {
        mesh.setMapSupport(channel, TRUE);
        mesh.setNumMapVerts(channel, faces * 3);
        mesh.setNumMapFaces(channel, faces);
        for (int f = 0; f < faces; ++f)
        {
            PaintProgress(f);
            for (int c = 0; c < 3; ++c)
            {
                int id = f * 3 + c;
                mesh.mapFaces(channel)[f].t[c] = id;
                mesh.mapVerts(channel)[id] = value(f * 3 + order[flip][c]);
            }
        }
    };
    if (data.contains("uvs"))
        for (size_t ch = 0; ch < data["uvs"].size(); ++ch)
            map(static_cast<int>(ch + 1), [&](int i) {
                const auto &uv = data["uvs"][ch][i];
                return Point3(Number(uv[0]), 1.0f - Number(uv[1]), 0.0f);
            });
    if (data.contains("colors"))
    {
        map(0, [&](int i) {
            const auto &c = data["colors"][i];
            return Point3(Number(c[0]), Number(c[1]), Number(c[2]));
        });
        map(-2, [&](int i) {
            float a = Number(data["colors"][i][3]);
            return Point3(a, a, a);
        });
    }
    if (data.contains("normals"))
    {
        mesh.SpecifyNormals();
        auto *normals = mesh.GetSpecifiedNormals();
        normals->SetParent(&mesh);
        normals->SetNumFaces(faces);
        normals->SetNumNormals(faces * 3);
        for (int f = 0; f < faces; ++f)
        {
            PaintProgress(f);
            for (int c = 0; c < 3; ++c)
            {
                int id = f * 3 + c;
                normals->Normal(id) =
                    Normal(Reflect(Point(data["normals"][f * 3 + order[flip][c]])), inverse);
                normals->Face(f).SpecifyNormalID(c, id);
                normals->SetNormalExplicit(id, true);
            }
        }
    }
    mesh.InvalidateGeomCache();
    return tri;
}
void CacheMeshData(Object *base, DWORD id, const Json &value)
{
    const auto text = value.dump();
    Require(text.size() <= MAXDWORD, "Mesh cache is too large");
    void *memory = MAX_malloc(text.size());
    Require(memory != nullptr, "Cannot allocate mesh cache");
    std::memcpy(memory, text.data(), text.size());
    base->RemoveAppDataChunk(BridgeId, GUP_CLASS_ID, id);
    base->AddAppDataChunk(BridgeId, GUP_CLASS_ID, id, static_cast<DWORD>(text.size()), memory);
}
void CacheTangents(INode *node, const Matrix3 &frame, MeshKind kind, const Json &source)
{
    if (!source.contains("tangents"))
        return;
    MeshSnapshot actual;
    CaptureMesh(node, frame, kind, false, false, actual);
    Json cache = {{"version", 1}, {"tangents", source["tangents"]}};
    auto &geometry = cache["geometry"];
    geometry["positions"] = actual.positions;
    geometry["triangles"] = actual.triangles;
    geometry["uvs"] = actual.uvs;
    geometry["normals"] = actual.normals;
    CacheMeshData(node->GetObjectRef()->FindBaseObject(), TangentChunk, cache);
}
void InstallSkin(INode *node, Modifier *modifier, const Json &data)
{
    auto *importer = static_cast<ISkinImportData *>(modifier->GetInterface(I_SKINIMPORTDATA));
    Require(importer, "Skin import interface unavailable");
    auto *skin = static_cast<ISkin *>(modifier->GetInterface(I_SKIN));
    Require(skin, "Skin interface unavailable");
    node->EvalWorldState(ReferenceTime);
    std::set<INode *> existing;
    for (int i = 0; i < skin->GetNumBones(); ++i)
        existing.insert(skin->GetBone(i));
    std::map<std::string, INode *> bones;
    for (const auto &vertex : data["weights"])
        for (const auto &weight : vertex)
        {
            auto name = weight["bone"].get<std::string>();
            if (!bones.count(name))
            {
                INode *bone = FindBone(name);
                Require(bone, "Missing influence bone under Root: " + name);
                bones[name] = bone;
                if (!existing.count(bone))
                    Require(importer->AddBoneEx(bone, FALSE) != FALSE, "Cannot add Skin bone");
            }
        }
    Require(importer->SetSkinTm(node, node->GetObjTMBeforeWSM(ReferenceTime),
                                node->GetNodeTM(ReferenceTime)) != FALSE,
            "Cannot set Skin bind transform");
    for (const auto &bone : bones)
        Require(importer->SetBoneTm(bone.second, bone.second->GetObjTMBeforeWSM(ReferenceTime),
                                    bone.second->GetNodeTM(ReferenceTime)) != FALSE,
                "Cannot set bone bind transform");
    node->EvalWorldState(ReferenceTime);
    for (size_t i = 0; i < data["weights"].size(); ++i)
    {
        PaintProgress(i);
        Tab<INode *> list;
        Tab<float> weights;
        for (const auto &w : data["weights"][i])
        {
            INode *bone = bones.at(w["bone"].get<std::string>());
            float weight = Number(w["weight"]);
            list.Append(1, &bone);
            weights.Append(1, &weight);
        }
        Require(importer->AddWeights(node, static_cast<int>(i), list, weights) != FALSE,
                "Cannot write Skin weights");
    }
    node->NotifyDependents(FOREVER, PART_ALL, REFMSG_CHANGE);
}
// New bone-node deletion during Undo can clear Skin's internal influences even
// while Max keeps the modifier alive. Replay after all nodes return on Redo.
class BatchSkinRedo final : public RestoreObj
{
    std::vector<std::pair<ULONG, Json>> targets;

  public:
    explicit BatchSkinRedo(const Json &items)
    {
        for (const auto &item : items)
            if (item.at("kind") == "skeletalPart")
            {
                INode *node = Find(item.at("name").get<std::string>());
                Require(node, "Missing received Skin node");
                targets.emplace_back(node->GetHandle(), item.at("data"));
            }
    }
    void Restore(int) override
    {
    }
    void Redo() override
    {
        PauseHold pause;
        for (const auto &target : targets)
            if (auto *node = GetCOREInterface()->GetINodeByHandle(target.first))
            {
                auto skin = Skin(node);
                if (skin.modifier)
                    InstallSkin(node, skin.modifier, target.second);
            }
    }
};
// Copy the Multi/Sub container while leaving its child shaders (and their
// maps/controllers) shared. Only children that actually change name are cloned.
MultiMtl *CopyMulti(MultiMtl *original)
{
    auto *block = original->GetParamBlock(0);
    auto materials = MultiParam(block, L"materialList");
    auto release = [](RemapDir *remap) { remap->DeleteThis(); };
    std::unique_ptr<RemapDir, decltype(release)> remap(NewRemapDir(), release);
    std::set<Mtl *> registered;
    for (int row = 0; row < block->Count(materials); ++row)
        if (auto *child = block->GetMtl(materials, ReferenceTime, row); child && registered.insert(child).second)
            remap->AddEntry(child, child);
    auto *copy = static_cast<MultiMtl *>(remap->CloneRef(original));
    remap->Backpatch();
    Require(copy && copy != original, "Cannot copy Multi/Sub-Object material");
    return copy;
}

void ReceiveMaterials(INode *node, TriObject *tri, const Json &item, SingleRefMaker &owner)
{
    const auto &data = item.at("data");
    const auto &slots = data.at("materialSlots");
    Mtl *current = node ? node->GetMtl() : nullptr;
    std::set<MtlID> faceIds;
    const Mesh &mesh = tri->GetMesh();
    for (int f = 0; f < mesh.getNumFaces(); ++f)
        faceIds.insert(mesh.faces[f].getMatID());
    std::map<MtlID, std::string> wanted, effectiveNames;
    Json cached = Json::object();
    for (const auto &slot : slots)
    {
        const auto id = slot.at("id").get<MtlID>();
        const auto name = slot.at("name").get<std::string>();
        if (!name.empty())
        {
            wanted.emplace(id, name);
            if (slot.contains("asset"))
                cached[std::to_string(id)] = {{"name", name}, {"asset", slot["asset"]}};
        }
    }
    const std::string source = item.value("asset", "");
    if (!cached.empty() && !source.empty())
        CacheMeshData(tri, MaterialChunk, {{"source", source}, {"slots", cached}, {"vehicleMaterialNames", true}});
    for (const auto &[id, material] : BoundMaterials(current, faceIds))
        effectiveNames.emplace(id, Utf8(material->GetName()));
    if (effectiveNames == wanted)
    {
        owner.SetRef(current);
        return;
    }
    if (wanted.empty())
        return;

    auto *oldMulti = current && current->ClassID() == MULTI_MATERIAL_CLASS_ID ? static_cast<MultiMtl *>(current) : nullptr;
    std::set<MtlID> oldFaces;
    if (current)
    {
        const auto oldMesh = CopyMesh(node, Skin(node, false));
        for (int f = 0; f < oldMesh.getNumFaces(); ++f)
            oldFaces.insert(oldMesh.faces[f].getMatID());
    }
    auto original = BoundMaterials(current, oldFaces);
    const auto materialFor = [&](MtlID id, const std::string &name) -> Mtl * {
        const auto found = original.find(id);
        auto *existing = found == original.end() ? nullptr : found->second;
        if (existing && Utf8(existing->GetName()) == name)
            return existing;
        auto *material = existing ? static_cast<Mtl *>(CloneRefHierarchy(existing)) : NewDefaultStdMat();
        Require(material && material != existing, "Cannot create or copy Max material: " + name);
        material->SetName(Wide(name).c_str());
        return material;
    };
    // A single material shades every face regardless of that face's ID.
    const bool hasExistingId = original.count(wanted.begin()->first) != 0;
    if (!oldMulti && wanted.size() == faceIds.size() &&
        std::all_of(wanted.begin(), wanted.end(), [&](const auto &entry) {
            return faceIds.count(entry.first) && entry.second == wanted.begin()->second &&
                   (original.count(entry.first) != 0) == hasExistingId;
        }))
    {
        owner.SetRef(materialFor(wanted.begin()->first, wanted.begin()->second));
        return;
    }
    auto *multi = oldMulti ? CopyMulti(oldMulti) : NewDefaultMultiMtl();
    Require(multi, "Cannot create Multi/Sub-Object material");
    owner.SetRef(multi);
    if (!oldMulti)
    {
        multi->SetNumSubMtls(1);
        multi->SetSubMtl(0, nullptr);
    }
    auto aliasesMatch = [&](int length, auto firstFace) {
        return std::all_of(firstFace, faceIds.end(), [&](MtlID id) {
            auto target = wanted.find(id);
            auto wrapped = wanted.find(static_cast<MtlID>(id % length));
            if (target == wanted.end() || wrapped == wanted.end())
                return target == wrapped;
            return target->second == wrapped->second;
        });
    };
    const int oldPeriod = oldMulti ? oldMulti->NumSubMtls() : 0;
    const bool keepPeriod = oldPeriod > 0 && oldPeriod <= MaxMultiSlots &&
        std::all_of(wanted.lower_bound(static_cast<MtlID>(oldPeriod)), wanted.end(),
                    [&](const auto &entry) { return faceIds.count(entry.first) != 0; }) &&
        aliasesMatch(oldPeriod, faceIds.lower_bound(static_cast<MtlID>(oldPeriod)));
    const int explicitLimit = keepPeriod ? oldPeriod : MaxMultiSlots;
    auto *block = multi->GetParamBlock(0);
    const auto ids = MultiParam(block, L"materialIDList");
    const auto enabled = MultiParam(block, L"mapEnabled");
    const auto materials = MultiParam(block, L"materialList");
    for (int row = 0; row < block->Count(ids); ++row)
    {
        int rowId = block->GetInt(ids, ReferenceTime, row);
        if (rowId < 0 || rowId > 65535)
            continue;
        const auto id = static_cast<MtlID>(rowId);
        if (wanted.count(id))
        {
            // Even a disabled slot may contain a fully authored Max shader.
            if (auto *stored = block->GetMtl(materials, ReferenceTime, row))
                original[id] = stored;
            block->SetValue(enabled, ReferenceTime, TRUE, row);
        }
        else if (multi->GetSubMtl(id))
            multi->SetSubMtl(id, nullptr);
    }
    for (const auto &[id, name] : wanted)
    {
        if (id >= explicitLimit)
        {
            Require(faceIds.count(id) != 0, "3ds Max Multi/Sub 不支持高于 60000 的独立材质槽。");
            continue; // Such a face can only be shaded through a lower slot.
        }
        auto *material = materialFor(id, name);
        if (material != multi->GetSubMtl(id))
            multi->SetSubMtlAndName(id, material, MSTR(Wide(name).c_str()));
    }
    if (!oldMulti && !wanted.count(0))
        multi->RemoveMtl(0);
    if (keepPeriod)
        return;
    const auto firstHigh = faceIds.lower_bound(MaxMultiSlots);
    int period = std::max(multi->NumSubMtls(), 1);
    if (firstHigh != faceIds.begin())
        period = std::max(period, 1 + static_cast<int>(*std::prev(firstHigh)));
    // Keep sparse IDs and holes exact. For a raw face ID beyond Max's slot
    // limit, choose a valid table length whose native modulo selects its shader.
    auto matchesHighFaces = [&](int length) { return aliasesMatch(length, firstHigh); };
    if (period <= MaxMultiSlots && !matchesHighFaces(period))
    {
        if (matchesHighFaces(MaxMultiSlots))
            period = MaxMultiSlots;
        else
            do { ++period; } while (period < MaxMultiSlots && !matchesHighFaces(period));
    }
    Require(period <= MaxMultiSlots && matchesHighFaces(period),
            "UE 材质 ID 超出 3ds Max Multi/Sub 的 60000 槽上限，无法保留当前逐面分配。");
    if (period > multi->NumSubMtls())
        multi->SetSubMtl(period - 1, nullptr);
}

void WriteMesh(const Json &item, const Matrix3 &frame)
{
    std::string name = item.at("name").get<std::string>();
    const MeshKind kind = item.at("kind") == "skeletalPart" ? MeshKind::Skeletal : MeshKind::Static;
    const bool skinned = kind == MeshKind::Skeletal;
    INode *node = Find(name);
    Matrix3 object;
    if (node)
    {
        auto skin = Skin(node, skinned);
        object = ObjectFrame(node, frame, skin);
        if (skin.skin)
            Require(Near(node->GetObjTMBeforeWSM(ReferenceTime), object * frame),
                    "Receiving a part requires its reference pose at frame zero");
    }
    auto *tri = BuildMesh(item.at("data"), object, kind);
    SingleRefMaker owner(tri);
    SingleRefMaker receivedMaterial;
    const bool hasMaterialSlots = item.at("data").contains("materialSlots");
    if (hasMaterialSlots)
    {
        PauseHold pause;
        ReceiveMaterials(node, tri, item, receivedMaterial);
    }
    Object *replacement = tri;
    Modifier *modifier = nullptr;
    if (skinned)
    {
        modifier = static_cast<Modifier *>(GetCOREInterface()->CreateInstance(OSM_CLASS_ID, SKIN_CLASSID));
        Require(modifier, "Cannot create Skin modifier");
        auto *derived = CreateDerivedObject(tri);
        derived->AddModifier(modifier);
        replacement = derived;
    }
    SingleRefMaker replacementOwner(replacement);
    if (node)
        ReplaceObject(node, replacement);
    else
    {
        std::string group = "LOD" + std::to_string(item.at("lod").get<int>());
        INode *parent = Find(group);
        if (!parent)
            parent = MakeGroup(group, VehicleParent(), frame);
        node = MakeNode(replacement, name, parent, frame);
    }
    auto *material = static_cast<Mtl *>(receivedMaterial.GetRef());
    if (hasMaterialSlots && material != node->GetMtl())
        node->SetMtl(material);
    if (skinned)
    {
        PauseHold pause;
        InstallSkin(node, modifier, item.at("data"));
    }
    CacheTangents(node, frame, kind, item.at("data"));
}
void WritePhysics(const Json &data, const Matrix3 &frame)
{
    INode *parent = Collider();
    if (!parent)
        parent = MakeGroup("BodyCollider", VehicleParent(), frame);
    std::vector<INode *> old;
    Walk(parent, [&](INode *n) { old.push_back(n); });
    for (const auto &shape : data.at("shapes"))
    {
        auto *tri = BuildMesh(shape, Matrix3(), MeshKind::Collision);
        SingleRefMaker owner(tri);
        Mesh &mesh = tri->GetMesh();
        const auto name = shape.at("name").get<std::string>();
        DPoint3 origin(0.0, 0.0, 0.0);
        const bool useCm = !CollisionFacesPointOut(mesh);
        if (useCm)
        {
            // Preserve shared edges and triangle IDs on very narrow facets.
            // Change the mesh's coordinate frame instead of reversing individual faces.
            const auto &points = shape.at("positions");
            SetCollisionPositions(mesh, points, 1.0, origin);
            if (!CollisionFacesPointOut(mesh))
            {
                origin = CollisionPoint(points[0]);
                SetCollisionPositions(mesh, points, 1.0, origin);
                Require(CollisionFacesPointOut(mesh),
                        "碰撞网格“" + name + "”含有 Max 单精度无法保留朝向的三角面。原碰撞已保留。");
            }
        }
        INode *node = MakeNode(tri, name, parent, frame);
        if (useCm)
        {
            const float unitsPerCm = static_cast<float>(1.0 / Cm());
            node->SetObjOffsetScale(ScaleValue(Point3(unitsPerCm, unitsPerCm, unitsPerCm)));
            node->SetObjOffsetPos(static_cast<Point3>(origin / Cm()));
        }
    }
    for (auto it = old.rbegin(); it != old.rend(); ++it)
        Require(GetCOREInterface()->DeleteNode(*it, FALSE, TRUE) > 0, "Cannot remove old BodyCollider child");
}
void CheckSkeleton(const Json &data)
{
    Require(data.contains("bones") && data["bones"].is_array() && !data["bones"].empty(),
            "Skeleton has no bones");
    std::set<std::string> seen;
    for (const auto &bone : data["bones"])
    {
        std::string name = bone.at("name"), parent = bone.at("parent");
        Wide(name);
        Require(!name.empty() && seen.insert(name).second, "Duplicate bone name");
        Require((name == "Root" && parent.empty() && seen.size() == 1) ||
                    (!parent.empty() && parent != name && seen.count(parent)),
                "Bones must be parent-first under Root");
        ReadTransform(bone);
    }
    INode *root = Find("Root");
    for (const auto &name : seen)
        if (auto *existing = FindBone(name))
        {
            bool inside = false;
            for (INode *p = existing; p && p != GetCOREInterface()->GetRootNode(); p = p->GetParentNode())
                if (p == root)
                    inside = true;
            Require(inside, "Bone name collides with a node outside Root: " + name);
        }
    if (root)
        Walk(root, [&](INode *bone) {
            if (seen.count(Utf8(bone->GetName())))
                return;
            Walk(GetCOREInterface()->GetRootNode(), [&](INode *n) {
                auto skin = Skin(n, false);
                Matrix3 bind;
                Require(!skin.skin || skin.skin->GetBoneInitTM(bone, bind, false) != SKIN_OK,
                        "Cannot remove a bone referenced by Skin without an explicit remapping rule: " +
                            Utf8(bone->GetName()));
            });
        });
}
class SkinBindRestore final : public RestoreObj
{
    struct Pose
    {
        ULONG handle;
        Matrix3 beforeObject, beforeNode, afterObject, afterNode;
    };
    SingleRefMaker modifier;
    std::vector<Pose> poses;
    void Apply(bool after)
    {
        auto *mod = static_cast<Modifier *>(modifier.GetRef());
        auto *importer = mod ? static_cast<ISkinImportData *>(mod->GetInterface(I_SKINIMPORTDATA)) : nullptr;
        if (!importer)
            return;
        for (const auto &pose : poses)
            if (auto *bone = GetCOREInterface()->GetINodeByHandle(pose.handle))
                importer->SetBoneTm(bone, after ? pose.afterObject : pose.beforeObject,
                                    after ? pose.afterNode : pose.beforeNode);
        mod->NotifyDependents(FOREVER, PART_ALL, REFMSG_CHANGE);
    }

  public:
    SkinBindRestore(const SkinInfo &skin, const std::map<std::string, Matrix3> &desired)
        : modifier(skin.modifier)
    {
        Require(skin.modifier->GetInterface(I_SKINIMPORTDATA) != nullptr,
                "Skin bind transform is not writable");
        for (int i = 0; i < skin.skin->GetNumBones(); ++i)
        {
            INode *bone = skin.skin->GetBone(i);
            if (!bone)
                continue;
            if (FindBone(Utf8(bone->GetName())) != bone)
                continue;
            auto target = desired.find(Utf8(bone->GetName()));
            if (target == desired.end())
                continue;
            Pose pose;
            pose.handle = bone->GetHandle();
            Require(skin.skin->GetBoneInitTM(bone, pose.beforeObject, true) == SKIN_OK &&
                        skin.skin->GetBoneInitTM(bone, pose.beforeNode, false) == SKIN_OK,
                    "Cannot snapshot bone bind transform");
            pose.afterNode = target->second;
            // Preserve the bone object offset independently of its reference node TM.
            pose.afterObject = pose.beforeObject * Invert(pose.beforeNode) * pose.afterNode;
            poses.push_back(pose);
        }
    }
    void Restore(int) override
    {
        Apply(false);
    }
    void Redo() override
    {
        Apply(true);
    }
};
void WriteSkeleton(const Json &data, const Matrix3 &frame)
{
    std::map<std::string, Matrix3> world;
    for (const auto &bone : data["bones"])
    {
        std::string name = bone["name"], parent = bone["parent"];
        world[name] = ReadTransform(bone) * (parent.empty() ? frame : world.at(parent));
    }
    std::vector<INode *> obsolete;
    if (auto *root = Find("Root"))
        Walk(root, [&](INode *n) {
            if (!world.count(Utf8(n->GetName())))
                obsolete.push_back(n);
        });
    std::vector<std::unique_ptr<SkinBindRestore>> binds;
    std::set<Modifier *> seen;
    Walk(GetCOREInterface()->GetRootNode(), [&](INode *n) {
        auto skin = Skin(n, false);
        if (skin.skin && seen.insert(skin.modifier).second)
            binds.push_back(std::make_unique<SkinBindRestore>(skin, world));
    });
    for (const auto &bone : data["bones"])
    {
        std::string name = bone["name"], parent = bone["parent"];
        INode *node = FindBone(name);
        INode *parentNode = parent.empty() ? VehicleParent() : FindBone(parent);
        Require(parentNode, "Missing bone parent");
        if (!node)
        {
            node = MakeGroup(name, parentNode, world.at(name));
            node->SetBoneNodeOnOff(TRUE, ReferenceTime);
        }
        else
        {
            if (node->GetParentNode() != parentNode)
                parentNode->AttachChild(node, 1);
            node->SetNodeTM(ReferenceTime, world.at(name));
        }
    }
    for (auto &bind : binds)
    {
        auto *restore = bind.release();
        theHold.Put(restore);
        theHold.Suspend();
        restore->Redo();
        theHold.Resume();
    }
    for (auto it = obsolete.rbegin(); it != obsolete.rend(); ++it)
        Require(GetCOREInterface()->DeleteNode(*it, FALSE, TRUE) > 0, "Cannot remove obsolete bone");
}
using SceneTextFields = std::map<std::wstring, std::wstring>;
class FieldsRestore final : public RestoreObj
{
    struct Entry
    {
        std::wstring name;
        bool existed = false;
        PROPVARIANT value{};
        Entry(const wchar_t *key) : name(key)
        {
            PROPSPEC spec{};
            spec.ulKind = PRSPEC_LPWSTR;
            spec.lpwstr = name.data();
            int i = GetCOREInterface()->FindProperty(PROPSET_USERDEFINED, &spec);
            if (i >= 0)
            {
                existed = true;
                Require(SUCCEEDED(PropVariantCopy(
                            &value, GetCOREInterface()->GetPropertyVariant(PROPSET_USERDEFINED, i))),
                        "Cannot snapshot vehicle metadata");
            }
        }
        ~Entry()
        {
            PropVariantClear(&value);
        }
    };
    std::vector<std::unique_ptr<Entry>> before;
    SceneTextFields after;

  public:
    explicit FieldsRestore(SceneTextFields fields) : after(std::move(fields))
    {
        for (const auto &field : after)
            before.push_back(std::make_unique<Entry>(field.first.c_str()));
    }
    void Restore(int) override
    {
        for (const auto &e : before)
        {
            PROPSPEC spec{};
            spec.ulKind = PRSPEC_LPWSTR;
            spec.lpwstr = e->name.data();
            GetCOREInterface()->DeleteProperty(PROPSET_USERDEFINED, &spec);
            if (e->existed)
                GetCOREInterface()->AddProperty(PROPSET_USERDEFINED, &spec, &e->value);
        }
    }
    void Redo() override
    {
        for (auto &[name, value] : after)
        {
            PROPSPEC spec{};
            spec.ulKind = PRSPEC_LPWSTR;
            spec.lpwstr = const_cast<LPWSTR>(name.c_str());
            PROPVARIANT v{};
            v.vt = VT_LPWSTR;
            v.pwszVal = value.data();
            GetCOREInterface()->DeleteProperty(PROPSET_USERDEFINED, &spec);
            GetCOREInterface()->AddProperty(PROPSET_USERDEFINED, &spec, &v);
        }
    }
};
void SetSceneTextFields(SceneTextFields fields)
{
    auto *restore = new FieldsRestore(std::move(fields));
    theHold.Put(restore);
    restore->Redo();
}
Json SceneIndex(bool includeOutlineNodes)
{
    MainThread();
    Json result = {{"vehicle", ReadFields()}, {"parts", Json::array()}, {"tree", Json::array()},
                   {"frame", FrameRows(Frame())}, {"scale", Cm()}};
    auto record = [&](const std::string &scope, INode *node) {
        if (includeOutlineNodes)
            result["outlineNodes"][scope].push_back(SelectionKey(node));
    };
    if (includeOutlineNodes)
        result["outlineNodes"] = Json::object();
    auto add = [&](INode *node, const std::string &name, const char *kind, int lod) -> Json& {
        record(name, node);
        result["parts"].push_back({{"id", name}, {"name", name}, {"kind", kind},
                                   {"lod", lod}, {"node", NodeKey(node)}});
        return result["parts"].back();
    };
    if (auto *root = Find("Root"))
    {
        add(root, "Root", "skeleton", 0);
        if (includeOutlineNodes)
            Walk(root, [&](INode *child) { record("Root", child); });
    }
    if (auto *collider = Collider())
    {
        auto &part = add(collider, "BodyCollider", "physics", 0);
        auto &children = part["children"] = Json::array();
        Walk(collider, [&](INode *child) {
            record("BodyCollider", child);
            children.push_back(NodeKey(child));
        });
    }
    static const std::regex pattern("^LOD([0-9]+)_(.+)$"), statics("^((Tire|Hub)_[0-9]{4}|BrakeB|BrakeF|BrakeR)$"),
        skeletal("^(Body|DoorModelFL|DoorModelFR|DoorWindowFL|DoorWindowFR|Engine|FrontKit|RearKit|Hood|"
                 "Interior|MirrorFL|MirrorFR|SeatFL|SeatFR|Sidepedals|Spoiler|SteerWheel)(_[0-9]{4})?$");
    std::set<std::string> names;
    Walk(GetCOREInterface()->GetRootNode(), [&](INode *node) {
        std::string name = Utf8(node->GetName());
        std::smatch match;
        if (!std::regex_match(name, match, pattern))
            return;
        int lod = std::stoi(match[1].str());
        Require(lod <= 7, "LOD index exceeds supported UE range");
        std::string group = "LOD" + std::to_string(lod);
        bool under = false;
        for (INode *p = node->GetParentNode(); p && p != GetCOREInterface()->GetRootNode();
             p = p->GetParentNode())
            if (Utf8(p->GetName()) == group)
                under = true;
        if (!under)
            return;
        Require(names.insert(name).second, "Duplicate LOD part name: " + name);
        bool isStatic = std::regex_match(match[2].str(), statics);
        if (!isStatic && !std::regex_match(match[2].str(), skeletal))
            return;
        add(node, name, isStatic ? "staticMesh" : "skeletalPart", lod);
        result["tree"].push_back({{"name", name}, {"id", name}, {"parent", group}});
    });
    return result;
}
Json CaptureIndexedNode(const Json &key, const std::string &kind, const Json &frameRows, bool allowCache,
                        MeshSnapshot &snapshot)
{
    MainThread();
    auto *node = ResolveNode(key);
    const Matrix3 frame = ReadFrameRows(frameRows);
    Json result = Json::object();
    try
    {
        if (kind == "skeleton")
            result["data"] = SkeletonData(node, frame);
        else
        {
            MeshKind meshKind = kind == "skeletalPart" ? MeshKind::Skeletal : MeshKind::Static;
            if (kind == "physics")
            {
                meshKind = MeshKind::Collision;
                Object *object = node->EvalWorldState(ReferenceTime).obj;
                if (!object || !object->CanConvertToType(triObjectClassID))
                    return result;
            }
            CaptureMesh(node, frame, meshKind, meshKind != MeshKind::Collision, allowCache, snapshot);
            if (meshKind != MeshKind::Collision)
                snapshot.materialSlots = ReadMaterialSlots(node, snapshot.materialIds, snapshot.materialSlotSource);
            return true;
        }
    }
    catch (const std::exception &e)
    {
        result["error"] = e.what();
        result["data"] = Json::object();
    }
    return result;
}
bool UseCachedTangents(const std::string &cache, Json &data)
{
    auto stored = Json::parse(cache, nullptr, false);
    if (!stored.is_object() || stored.value("version", 0) != 1 || !stored.contains("geometry") ||
        !stored.contains("tangents") || stored["tangents"].size() != data["normals"].size())
        return false;
    const auto &geometry = stored["geometry"];
    for (const char *key : {"positions", "triangles", "uvs", "normals"})
        if (!geometry.contains(key) || geometry.at(key) != data.at(key))
            return false;
    data["tangents"] = std::move(stored["tangents"]);
    return true;
}
Json ReadIndexedNode(const WySceneCall &onMain, const Json &key, const std::string &kind, const Json &rows)
{
    bool allowCache = true;
    for (;;)
    {
        auto snapshot = std::make_shared<MeshSnapshot>();
        auto status = onMain([key, kind, rows, allowCache, snapshot] {
            return CaptureIndexedNode(key, kind, rows, allowCache, *snapshot);
        });
        if (!status.is_boolean())
            return status;
        Json result = Json::object();
        auto &data = result["data"];
        if (kind == "physics")
        {
            data["name"] = key.at("name");
            data["positions"] = snapshot->positions;
            data["triangles"] = snapshot->triangles;
            return result;
        }
        data = snapshot->Data();
        if (snapshot->tangentCache.empty() || UseCachedTangents(snapshot->tangentCache, data))
            return result;
        // A stale cache requires native IGame tangents. Capture geometry and
        // tangents together again to avoid mixing different scene revisions.
        allowCache = false;
    }
}
Json RunHere(std::function<Json()> call)
{
    return call();
}
} // namespace

nlohmann::json WyRunSceneCall(const std::function<Json()> &call, const std::function<void()> &repaintProgress)
{
    MainThread();
    struct Restore
    {
        const std::function<void()> *previous;
        ~Restore() { scenePaint = previous; }
    } restore{scenePaint};
    scenePaint = repaintProgress ? &repaintProgress : nullptr;
    return call();
}

nlohmann::json WyReadScene(const WySceneCall &onMain, bool includeOutlineNodes)
{
    const auto collectIndex = [includeOutlineNodes] { return SceneIndex(includeOutlineNodes); };
    const Json index = onMain(collectIndex);
    const Json &rows = index.at("frame");
    Json result = {{"vehicle", index.at("vehicle")}, {"tree", index.at("tree")}, {"items", Json::array()}};
    if (includeOutlineNodes)
        result["outlineNodes"] = index.at("outlineNodes");
    for (const auto &part : index.at("parts"))
    {
        const std::string kind = part.at("kind");
        Json payload;
        if (kind == "physics")
        {
            auto &shapes = payload["data"]["shapes"] = Json::array();
            for (const auto &child : part.at("children"))
            {
                auto shape = ReadIndexedNode(onMain, child, kind, rows);
                if (shape.contains("error"))
                {
                    payload = std::move(shape);
                    break;
                }
                if (shape.contains("data"))
                    shapes.push_back(std::move(shape["data"]));
            }
        }
        else
            payload = ReadIndexedNode(onMain, part.at("node"), kind, rows);
        for (const char *key : {"id", "name", "kind", "lod"})
            payload[key] = part.at(key);
        result["items"].push_back(std::move(payload));
    }
    Require(index == onMain(collectIndex), "Max scene hierarchy or vehicle fields changed during reading; compare again");
    return result;
}
nlohmann::json WyReadScene(const WySceneCall &onMain)
{
    return WyReadScene(onMain, false);
}
void WyInvalidateSceneSelectionKeys()
{
    ++selectionEpoch;
}
void WySelectSceneNode(const Json &key)
{
    MainThread();
    auto *core = GetCOREInterface();
    auto *node = core->GetINodeByHandle(key.at("handle").get<ULONG>());
    Require(node && node->GetParentNode() && SelectionKey(node) == key, "节点已改变，请重新比对后再选中。");
    const auto targetSelected = [&] { return core->GetSelNodeCount() == 1 && core->GetSelNode(0) == node; };
    if (targetSelected())
    {
        WyRevealSelectionInExplorers();
        return;
    }
    Require(!theHold.Holding(), "请先结束当前 Max 操作，再选中节点。");
    theHold.Begin();
    try
    {
        core->SelectNode(node, TRUE);
        if (!targetSelected())
        {
            // Max expands a closed group member into a whole-group selection.
            // Open the containing groups from outside to inside using Max's undoable group operation.
            std::vector<INode *> groups;
            for (auto *ancestor = node; ancestor && !ancestor->IsRootNode(); ancestor = ancestor->GetParentNode())
                if (ancestor->IsGroupHead() && !ancestor->IsOpenGroupHead())
                    groups.push_back(ancestor);
            for (auto it = groups.rbegin(); it != groups.rend(); ++it)
            {
                auto *head = *it;
                INodeTab members;
                members.Append(1, &head);
                for (int i = 0; i < head->NumberOfChildren(); ++i)
                {
                    auto *member = head->GetChildNode(i);
                    members.Append(1, &member);
                }
                core->OpenGroup(&members, FALSE);
            }
            core->SelectNode(node, TRUE);
            Require(targetSelected(), "Max 无法单独选中该节点，请检查节点的选择限制。");
        }
        theHold.Accept(MSTR(L"在大纲中选中"));
    }
    catch (...)
    {
        theHold.Cancel();
        throw;
    }
    core->RedrawViews(core->GetTime());
    WyRevealSelectionInExplorers();
}
nlohmann::json WyReadScene()
{
    return WyReadScene(RunHere);
}
nlohmann::json WyReadSceneContext()
{
    MainThread();
    return {{"vehicle", ReadFields()}, {"scale", Cm()}, {"time", GetCOREInterface()->GetTime()}};
}
WyColliderBuildSettings WyReadColliderBuildSettings()
{
    MainThread();
    WyColliderBuildSettings settings;
    settings.split = SceneTextProperty(ColliderModeProperty) == "split";
    const auto height = Json::parse(SceneTextProperty(ColliderHeightProperty).value_or(""), nullptr, false);
    if (height.is_number() && std::isfinite(height.get<double>()))
        settings.worldZCm = height.get<double>();
    return settings;
}
void WyWriteScene(const nlohmann::json &items, const WySceneCall &commit, const WyColliderBuildSettings *buildSettings)
{
    Require(items.is_array(), "Expected an item array");
    if (items.empty())
        return;
    std::set<std::string> ids, requiredBones;
    const Json *skeleton = nullptr;
    for (const auto &item : items)
    {
        PaintProgress();
        std::string id = item.at("id"), name = item.at("name"), kind = item.at("kind");
        Require(ids.insert(id).second, "Duplicate batch target");
        Require(id == name, "Item id must match its Max node name");
        Wide(name);
        if (kind == "skeleton")
        {
            Require(id == "Root", "Invalid skeleton target");
            skeleton = &item.at("data");
        }
        else if (kind == "physics")
        {
            Require(id == "BodyCollider", "Invalid physics target");
            std::set<std::string> names;
            Require(item["data"].at("shapes").is_array(), "Expected collision shapes");
            for (const auto &shape : item["data"]["shapes"])
            {
                auto n = shape.at("name").get<std::string>();
                Wide(n);
                Require(!n.empty() && names.insert(n).second, "Duplicate collision shape name");
                CheckMesh(shape, nullptr);
            }
        }
        else
        {
            Require(kind == "staticMesh" || kind == "skeletalPart", "Unknown item kind");
            int lod = item.at("lod");
            Require(lod >= 0 && lod <= 7 && name.rfind("LOD" + std::to_string(lod) + "_", 0) == 0,
                    "Invalid LOD target");
            CheckMesh(item.at("data"), kind == "skeletalPart" ? &requiredBones : nullptr);
        }
    }
    commit([&] {
        MainThread();
        if (skeleton)
        {
            CheckSkeleton(*skeleton);
            for (const auto &bone : skeleton->at("bones"))
                requiredBones.erase(bone.at("name").get_ref<const std::string &>());
        }
        for (const auto &name : requiredBones)
            Require(FindBone(name) != nullptr, "Missing influence bone: " + name);
        const Matrix3 frame = Frame();
        SceneBatch hold;
        if (skeleton)
            WriteSkeleton(*skeleton, frame);
        for (const auto &item : items)
            if (item["kind"] == "staticMesh" || item["kind"] == "skeletalPart")
                WriteMesh(item, frame);
        for (const auto &item : items)
            if (item["kind"] == "physics")
                WritePhysics(item["data"], frame);
        if (buildSettings)
        {
            SceneTextFields fields{{ColliderModeProperty, buildSettings->split ? L"split" : L"single"}};
            if (buildSettings->worldZCm)
                fields.emplace(ColliderHeightProperty, Wide(Json(*buildSettings->worldZCm).dump()));
            SetSceneTextFields(std::move(fields));
        }
        theHold.Put(new BatchSkinRedo(items));
        hold.Accept(L"WY Vehicle Asset Bridge — Receive batch");
        return Json(nullptr);
    });
}
void WyWriteScene(const nlohmann::json &items)
{
    MainThread();
    WyWriteScene(items, RunHere);
}
void WySetVehicleFields(const nlohmann::json &fields)
{
    MainThread();
    SceneTextFields properties;
    for (const auto &[key, property] : VehicleFieldKeys)
    {
        Require(fields.contains(key) && fields[key].is_string(), "Vehicle fields must be text");
        auto value = fields[key].get<std::string>();
        Require(!value.empty(), "Vehicle fields cannot be empty");
        properties.emplace(property, Wide(value));
    }
    SceneBatch hold;
    SetSceneTextFields(std::move(properties));
    hold.Accept(L"WY Vehicle Asset Bridge — Vehicle fields");
}
namespace
{
bool IsColliderSource(INode *node)
{
    static const std::regex excluded("^(Tire|Hub|Brake[BRF]?)(_[0-9]{4})?$");
    const std::string name = Utf8(node->GetName());
    const std::string part = name.rfind("LOD0_", 0) == 0 ? name.substr(5) : name;
    return part.rfind("Mirror", 0) != 0 && !std::regex_match(part, excluded);
}
Json ColliderInputIndex()
{
    MainThread();
    INode *lod = Find("LOD0");
    Require(lod, "LOD0 is missing");
    INode *parent = VehicleParent();
    const TimeValue time = GetCOREInterface()->GetTime();
    Matrix3 frame = parent == GetCOREInterface()->GetRootNode() ? Matrix3() : parent->GetNodeTM(time);
    Json index = {{"time", time}, {"frame", FrameRows(frame)}, {"scale", Cm()}, {"nodes", Json::array()}};
    Walk(lod, [&](INode *node) {
        if (IsColliderSource(node))
            index["nodes"].push_back(NodeKey(node));
    });
    return index;
}
bool CaptureColliderMesh(const Json &key, TimeValue time, const Json &rows, float scale, MeshSnapshot &snapshot)
{
    MainThread();
    auto *node = ResolveNode(key);
    Object *object = node->EvalWorldState(time).obj;
    if (!object || !object->CanConvertToType(triObjectClassID))
        return false;
    auto *tri = static_cast<TriObject *>(object->ConvertToType(time, triObjectClassID));
    Require(tri, "Cannot convert collider source");
    Mesh mesh = tri->GetMesh();
    if (tri != object)
        tri->DeleteThis();
    if (!mesh.getNumVerts() || !mesh.getNumFaces())
        return true;
    Matrix3 transform = node->GetObjTMAfterWSM(time) * Invert(ReadFrameRows(rows));
    const bool flip = Determinant(transform) < 0;
    snapshot.positions.reserve(mesh.getNumVerts());
    snapshot.triangles.reserve(mesh.getNumFaces());
    for (int i = 0; i < mesh.getNumVerts(); ++i)
    {
        PaintProgress(i);
        snapshot.positions.push_back(Components(Reflect(mesh.verts[i] * transform) * scale));
    }
    for (int i = 0; i < mesh.getNumFaces(); ++i)
    {
        PaintProgress(i);
        const Face &face = mesh.faces[i];
        for (int c = 0; c < 3; ++c)
            Require(face.v[c] < static_cast<DWORD>(mesh.getNumVerts()), "Invalid collider source triangle");
        snapshot.triangles.push_back({face.v[0], face.v[flip ? 2 : 1], face.v[flip ? 1 : 2]});
    }
    return true;
}
} // namespace
std::optional<double> WyColliderPreviewUnitScale(int time)
{
    MainThread();
    INode *lod = Find("LOD0");
    if (!lod)
        return std::nullopt;
    std::optional<double> scale;
    Walk(lod, [&](INode *node) {
        if (scale || !IsColliderSource(node))
            return;
        Object *object = node->EvalWorldState(time).obj;
        if (!object || !object->CanConvertToType(triObjectClassID))
            return;
        Matrix3 toWorld = node->GetObjTMAfterWSM(time);
        Box3 part;
        object->GetDeformBBox(time, part, &toWorld);
        if (!part.IsEmpty())
            scale = Cm();
    });
    return scale;
}
nlohmann::json WyReadColliderInput(const WySceneCall &onMain, Json *worldZ)
{
    const Json index = onMain(ColliderInputIndex);
    const TimeValue time = index.at("time");
    const float scale = index.at("scale");
    const auto &rows = index.at("frame");
    Json items = Json::array();
    std::set<std::string> names;
    for (const auto &key : index.at("nodes"))
    {
        auto snapshot = std::make_shared<MeshSnapshot>();
        auto isMesh = onMain([key, rows, time, scale, snapshot] {
            return Json(CaptureColliderMesh(key, time, rows, scale, *snapshot));
        });
        if (!isMesh.get<bool>())
            continue;
        const std::string name = key.at("name");
        Require(names.insert(name).second, "Duplicate collider source node: " + name);
        if (!snapshot->positions.empty())
        {
            Json item = {{"id", name}, {"name", name}, {"kind", "hullInput"}, {"lod", 0}};
            item["data"]["positions"] = snapshot->positions;
            item["data"]["triangles"] = snapshot->triangles;
            items.push_back(std::move(item));
        }
    }
    Require(index == onMain(ColliderInputIndex), "LOD0 hierarchy, vehicle frame or active time changed while reading collider inputs");
    Require(!items.empty(), "LOD0 has no eligible body geometry");
    if (worldZ)
    {
        // Wire positions are vehicle-local centimeters with Y reflected. Max uses
        // row-vector transforms, so dot([wireX,wireY,wireZ,1], worldZ) is world Z in cm.
        *worldZ = {rows[0][2], -rows[1][2].get<double>(), rows[2][2],
                   rows[3][2].get<double>() * index.at("scale").get<double>()};
    }
    return items;
}
nlohmann::json WyReadColliderInput()
{
    return WyReadColliderInput(RunHere);
}
bool WyHasColliderMeshes()
{
    MainThread();
    INode *parent = Collider();
    if (!parent)
        return false;
    bool found = false;
    Walk(parent, [&](INode *node) {
        Object *object = node->EvalWorldState(GetCOREInterface()->GetTime()).obj;
        if (object && object->CanConvertToType(triObjectClassID))
            found = true;
    });
    return found;
}
