#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"

class UMaterialInterface;
class UTexture2D;

namespace WyVehiclePbr
{
inline constexpr TCHAR MasterName[] = TEXT("M_Vehicle_PBR_BaseMat");
struct FTextureBinding
{
    FName Parameter;
    FSoftObjectPath Texture;
};
FString MasterPackage(const FString& Name);
bool GetTextureBindings(const FString& VehicleBase, const FString& VehicleName, const FString& InstanceName,
                        TArray<FTextureBinding>& Out, FString& Why);
UTexture2D* FindTexture(const FSoftObjectPath& Path);
}

// One plan per vehicle write. Resolve every name before publishing any assets.
struct FWyVehicleMaterialPlan
{
    struct FEntry
    {
        FString Package;
        UMaterialInterface* Material = nullptr;
        UMaterialInterface* Parent = nullptr;
        TMap<FName, FSoftObjectPath> PbrTextures;
    };
    TMap<FName, FEntry> Entries;
    TSet<FName> Packages;
};

FString WyVehicleMaterialAssetName(const FString& VehicleName, const FString& MaxMaterialName);
bool WyPlanVehicleMaterial(const FString& VehicleBase, const FString& VehicleName, const FString& MaxMaterialName,
                           FWyVehicleMaterialPlan& Plan, FString& Why);
void WyCreatePlannedVehicleMaterials(FWyVehicleMaterialPlan& Plan, TArray<UObject*>& Created);
UMaterialInterface* WyPlannedVehicleMaterial(const FWyVehicleMaterialPlan& Plan, FName MaterialName);
