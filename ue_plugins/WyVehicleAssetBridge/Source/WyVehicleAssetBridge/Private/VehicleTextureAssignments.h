#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"

struct FWyVehicleTextureRow
{
    FAssetData Material;
    FName Parameter;
    FAssetData Texture;
    bool bApplied = false;
};

namespace WyVehicleTextures
{
FString VehicleRootForFolder(const FString& Folder);
void Scan(const FString& VehicleRoot, TArray<TSharedPtr<FWyVehicleTextureRow>>& OutRows);
FText Apply(const FString& VehicleRoot, const TArray<TSharedPtr<FWyVehicleTextureRow>>& Rows);
}
