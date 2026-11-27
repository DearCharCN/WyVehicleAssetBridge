#pragma once

#include "Engine/AssetUserData.h"
#include "UObject/SoftObjectPath.h"
#include "WyPendingVehicleTextures.generated.h"

// Native material parameters require UTexture objects. Keep a record of the
// expected paths that were missing at creation for artists to consult later.
UCLASS(DisplayName = "望月载具材质贴图路径")
class UWyPendingVehicleTextures final : public UAssetUserData
{
    GENERATED_BODY()

  public:
    UPROPERTY(VisibleAnywhere, Category = "WyVehicle", meta = (DisplayName = "创建时缺失的贴图"))
    TMap<FName, FSoftObjectPath> PendingTextures;

    virtual bool IsEditorOnly() const override { return true; }
};
