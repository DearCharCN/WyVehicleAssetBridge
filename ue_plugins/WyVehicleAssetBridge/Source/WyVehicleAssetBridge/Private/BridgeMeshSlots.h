#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MeshDescription.h"

// Each Max node has its own sparse material ID space, even inside a shared skeletal mesh.
using FWyLocalSlots = TMap<int32, FName>;
using FWyMeshSlotScopes = TMap<FString, FWyLocalSlots>;
extern const FName WyLocalFaceMaterialID;
extern const FName WyUnassignedMaterialSlot;

FWyMeshSlotScopes WyLoadSlotScopes(UObject* Mesh);
void WyStoreSlotScopes(UObject* Mesh, const FWyMeshSlotScopes& Scopes);

struct FWyMeshSlotView
{
    struct FSlot
    {
        FName Display, Imported;
        FString Asset;
        bool LegacyPlaceholder;
    };
    TArray<FSlot> Slots;
    TMap<FPolygonGroupID, int32> Groups;
    // Only populated for source groups split into chunks with different overrides.
    TMap<FTriangleID, int32> FaceOverrides;
};

FWyMeshSlotView WyMeshSlotView(UObject* Object, int32 Lod, const FMeshDescription& Description);
FString WyMaxMaterialSlotName(FName Display, const FString& AssetPackage, const FString& VehicleBase, const FString& VehicleName);
bool WyBakeSkeletalSectionOverrides(FMeshDescription& Description, const FWyMeshSlotView& View, FString& Why);

// Preserve declared unused IDs; resolve faces from current editor section assignments.
class FWyReadSlotScope
{
public:
    FWyReadSlotScope(const FWyMeshSlotView& InView, const FWyLocalSlots* Source);
    int32 ReadFace(FTriangleID Triangle, FPolygonGroupID Group, int32 Original);
    TArray<TSharedPtr<FJsonValue>> ToJson(const FString& VehicleBase, const FString& VehicleName) const;

private:
    const FWyMeshSlotView& View;
    TMap<int32, int32> LocalToGlobal;
    TMap<int32, int32> GlobalToLocal;
    TSet<int32> EditorSlots;
    bool Managed = false;
    int32 NextFree = 0;
};
