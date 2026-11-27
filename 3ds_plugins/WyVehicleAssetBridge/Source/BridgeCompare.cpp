#include "BridgeCompare.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <numeric>
#include <string>
#include <vector>
#include <windows.h>

namespace
{
using Json = nlohmann::json;
bool Near(const Json& a, const Json& b, double epsilon)
{
    if (a.is_number() && b.is_number())
        return std::abs(a.get<double>() - b.get<double>()) <= epsilon;
    if (a.type() != b.type() || a.size() != b.size())
        return false;
    if (a.is_array())
    {
        for (size_t i = 0; i < a.size(); ++i)
            if (!Near(a[i], b[i], epsilon))
                return false;
        return true;
    }
    if (a.is_object())
    {
        for (auto it = a.begin(); it != a.end(); ++it)
            if (!b.contains(it.key()) || !Near(it.value(), b.at(it.key()), epsilon))
                return false;
        return true;
    }
    return a == b;
}
bool Weights(const Json& a, const Json& b)
{
    if (a.size() != b.size())
        return false;
    for (const auto& influence : a)
    {
        auto match = std::find_if(b.begin(), b.end(), [&](const Json& v) { return v.at("bone") == influence.at("bone"); });
        if (match == b.end() || !Near(influence.at("weight"), match->at("weight"), 2.0 / 65535))
            return false;
    }
    return true;
}
using Cell = std::array<long long, 3>;
Cell PositionCell(const Json& point)
{
    Cell cell;
    for (int axis = 0; axis < 3; ++axis)
        cell[axis] = static_cast<long long>(std::floor(point.at(axis).get<double>() * 10000));
    return cell;
}
using SlotTable = std::map<int, const Json*>;

bool SamePackage(const std::string& a, const std::string& b)
{
    if (a == b)
        return true;
    if (a.empty() || b.empty())
        return false;
    // Package identity in UE uses FName. Keep the display label comparison
    // separate so changing a slot's capitalization is still an editable change.
    const std::filesystem::path left(std::u8string(a.begin(), a.end()));
    const std::filesystem::path right(std::u8string(b.begin(), b.end()));
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

SlotTable NamedSlots(const Json& mesh)
{
    SlotTable result;
    if (const auto slots = mesh.find("materialSlots"); slots != mesh.end())
        for (const auto& slot : *slots)
            if (!slot.at("name").get_ref<const std::string&>().empty())
                result.emplace(slot.at("id").get<int>(), &slot);
    return result;
}

bool MaterialSlotsMatch(const Json& local, const Json& remote, const std::string& meshPackage, const std::string& vehicleName)
{
    const auto localSlots = NamedSlots(local), remoteSlots = NamedSlots(remote);
    if (localSlots.size() != remoteSlots.size())
        return false;
    const auto leaf = meshPackage.rfind('/');
    const auto folder = leaf == std::string::npos ? leaf : meshPackage.rfind('/', leaf - 1);
    const auto directory = meshPackage.substr(0, folder) + "/MaterialInstance/";
    const bool sameSource = !meshPackage.empty() && SamePackage(local.value("materialSlotSource", ""), meshPackage);
    for (const auto& [id, slot] : localSlots)
    {
        const auto found = remoteSlots.find(id);
        if (found == remoteSlots.end() || slot->at("name") != found->second->at("name"))
            return false;
        if (folder == std::string::npos || !found->second->contains("asset"))
            continue;
        const auto& assigned = found->second->at("asset").get_ref<const std::string&>();
        const auto& name = slot->at("name").get_ref<const std::string&>();
        const bool vehicleNamed = name.starts_with("MI_");
        const auto expected = directory + (vehicleNamed ? "MI_" + vehicleName + "_" + name.substr(3) : name);
        if (SamePackage(assigned, expected))
            continue;
        // A UE slot can have an artist-defined label and reference a differently
        // named asset. Accept a pull on this mesh only. Bindings accepted before
        // vehicle-qualified asset names existed still need a migration decision.
        const bool oldDefault = vehicleNamed && slot->value("legacyAssetNaming", false) && SamePackage(assigned, directory + name);
        if (oldDefault || !sameSource || !slot->contains("asset") || !SamePackage(slot->at("asset").get_ref<const std::string&>(), assigned))
            return false;
    }
    return true;
}

bool Mesh(const Json& a, const Json& b, const std::string& meshPackage = {}, const std::string& vehicleName = {})
{
    const auto& pa = a.at("positions");
    const auto& pb = b.at("positions");
    const auto& ta = a.at("triangles");
    const auto& tb = b.at("triangles");
    if (pa.size() != pb.size() || ta.size() != tb.size())
        return false;
    if (!MaterialSlotsMatch(a, b, meshPackage, vehicleName))
        return false;
    const bool compareFaceSlots = a.contains("materialSlots") && b.contains("materialSlots");
    if (compareFaceSlots && (!a.contains("materialIds") || a.at("materialIds").size() != ta.size() ||
                             !b.contains("materialIds") || b.at("materialIds").size() != tb.size()))
        return false;
    auto faceSlotMatches = [&](size_t left, size_t right)
    {
        return !compareFaceSlots || a.at("materialIds")[left] == b.at("materialIds")[right];
    };
    for (const auto* key : {"colors", "normals", "tangents", "uvs", "weights"})
        if (a.contains(key) != b.contains(key) || (a.contains(key) && a.at(key).size() != b.at(key).size()))
            return false;
    std::vector<size_t> remap(pa.size());
    std::iota(remap.begin(), remap.end(), 0);
    if (!Near(pa, pb, 1.e-4))
    {
        // Reordered distinct vertices are unambiguous. Coincident seams use the
        // original indexing when possible; never guess between duplicate points.
        std::multimap<Cell, size_t> cells;
        std::vector<bool> used(pb.size());
        for (size_t i = 0; i < pb.size(); ++i)
            cells.emplace(PositionCell(pb[i]), i);
        for (size_t i = 0; i < pa.size(); ++i)
        {
            auto cell = PositionCell(pa[i]);
            size_t found = pb.size();
            for (int z = -1; z <= 1; ++z)
                for (int y = -1; y <= 1; ++y)
                    for (int x = -1; x <= 1; ++x)
                    {
                        auto range = cells.equal_range(Cell{cell[0] + x, cell[1] + y, cell[2] + z});
                        for (auto it = range.first; it != range.second; ++it)
                            if (!used[it->second] && Near(pa[i], pb[it->second], 1.e-4))
                            {
                                if (found != pb.size())
                                    return false;
                                found = it->second;
                            }
                    }
            if (found == pb.size())
                return false;
            used[found] = true;
            remap[i] = found;
        }
    }
    if (a.contains("weights"))
        for (size_t i = 0; i < pa.size(); ++i)
            if (!Weights(a.at("weights").at(i), b.at("weights").at(remap[i])))
                return false;
    auto cornersMatch = [&](size_t ia, size_t ib)
    {
        for (const auto* channel : {"normals", "tangents", "colors"})
            if (a.contains(channel) && !Near(a.at(channel).at(ia), b.at(channel).at(ib), 1.e-4))
                return false;
        if (a.contains("uvs"))
            for (size_t uv = 0; uv < a.at("uvs").size(); ++uv)
                if (!Near(a.at("uvs")[uv].at(ia), b.at("uvs")[uv].at(ib), 1.e-5))
                    return false;
        return true;
    };
    if (ta == tb && std::is_sorted(remap.begin(), remap.end()))
    {
        for (size_t i = 0; i < ta.size(); ++i)
            if (!faceSlotMatches(i, i))
                return false;
        for (size_t i = 0; i < ta.size() * 3; ++i)
            if (!cornersMatch(i, i))
                return false;
        return true;
    }
    using Face = std::array<size_t, 3>;
    using Location = std::pair<size_t, int>;
    std::multimap<Face, Location> faces;
    std::vector<bool> used(tb.size());
    for (size_t i = 0; i < tb.size(); ++i)
        for (int rot = 0; rot < 3; ++rot)
            faces.emplace(
                Face{tb[i].at(rot).get<size_t>(), tb[i].at((rot + 1) % 3).get<size_t>(), tb[i].at((rot + 2) % 3).get<size_t>()},
                Location{i, rot});
    for (size_t i = 0; i < ta.size(); ++i)
    {
        Face face;
        for (int k = 0; k < 3; ++k)
            face[k] = remap.at(ta[i].at(k).get<size_t>());
        auto range = faces.equal_range(face);
        bool matched = false;
        for (auto it = range.first; it != range.second; ++it)
        {
            const auto [index, rot] = it->second;
            if (used[index] || !faceSlotMatches(i, index))
                continue;
            bool equal = true;
            for (int k = 0; k < 3; ++k)
                if (!cornersMatch(3 * i + k, 3 * index + (k + rot) % 3))
                {
                    equal = false;
                    break;
                }
            if (equal)
            {
                used[index] = true;
                matched = true;
                break;
            }
        }
        if (!matched)
            return false;
    }
    return true;
}
bool NamedItems(const Json& a, const Json& b, bool skeleton)
{
    if (a.size() != b.size())
        return false;
    for (const auto& item : a)
    {
        auto it = std::find_if(b.begin(), b.end(), [&](const Json& other) { return item.at("name") == other.at("name"); });
        if (it == b.end())
            return false;
        if (!skeleton)
        {
            if (!Mesh(item, *it))
                return false;
            continue;
        }
        if (item.at("parent") != it->at("parent") || !Near(item.at("translation"), it->at("translation"), 1.e-4) ||
            !Near(item.at("scale"), it->at("scale"), 1.e-5))
            return false;
        const auto& qa = item.at("rotation");
        const auto& qb = it->at("rotation");
        if (!Near(qa, qb, 1.e-5))
        {
            Json opposite = qb;
            for (auto& component : opposite)
                component = -component.get<double>();
            if (!Near(qa, opposite, 1.e-5))
                return false;
        }
    }
    return true;
}
} // namespace
bool WySameScope(const Json& a, const Json& b, const std::string& vehicleName)
{
    if (a.at("kind") != b.at("kind"))
        return false;
    const auto& l = a.at("data");
    const auto& r = b.at("data");
    if (a.at("kind") == "skeleton")
        return NamedItems(l.at("bones"), r.at("bones"), true);
    if (a.at("kind") == "physics")
        return NamedItems(l.at("shapes"), r.at("shapes"), false);
    return Mesh(l, r, b.value("asset", ""), vehicleName);
}
