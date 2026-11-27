#pragma once
#include <functional>
#include <optional>
#include <nlohmann/json.hpp>

// Runs one SDK callback on Max's main thread and waits for its result.
using WySceneCall = std::function<nlohmann::json(std::function<nlohmann::json()>)>;
// Scope a paint-only callback to one scheduled main-thread SDK call. The
// callback must not pump input or dispatch commands while an undo hold is open.
nlohmann::json WyRunSceneCall(const std::function<nlohmann::json()> &call,
                              const std::function<void()> &repaintProgress);
// The overloads without a dispatcher require the Max main thread.
// Positions are vehicle-space centimetres in UE axes; UVs use UE V.
nlohmann::json WyReadScene();
nlohmann::json WyReadSceneContext();
nlohmann::json WyReadScene(const WySceneCall &onMain);
nlohmann::json WyReadScene(const WySceneCall &onMain, bool includeOutlineNodes);
// These selection keys belong to the current Max scene and never go on the wire.
void WyInvalidateSceneSelectionKeys();
void WySelectSceneNode(const nlohmann::json &key);
void WyRevealSelectionInExplorers();
void WyWriteScene(const nlohmann::json &items);
struct WyColliderBuildSettings
{
    bool split = false;
    std::optional<double> worldZCm;
};
WyColliderBuildSettings WyReadColliderBuildSettings();
void WyWriteScene(const nlohmann::json &items, const WySceneCall &commit, const WyColliderBuildSettings *buildSettings = nullptr);
void WySetVehicleFields(const nlohmann::json &fields);
// Geometry-only input for hull generation; does not require metadata or Skin.
nlohmann::json WyReadColliderInput();
nlohmann::json WyReadColliderInput(const WySceneCall &onMain, nlohmann::json *worldZ = nullptr);
bool WyHasColliderMeshes();

// Centimetres per Max system unit when there is geometry to preview.
std::optional<double> WyColliderPreviewUnitScale(int time);
