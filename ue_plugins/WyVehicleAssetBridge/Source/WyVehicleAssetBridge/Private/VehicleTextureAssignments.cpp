#include "VehicleTextureAssignments.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BridgeMaterials.h"
#include "Engine/Texture2D.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

namespace
{
bool MaterialBindings(const FString& VehicleRoot, const FAssetData& Material, UMaterialInstanceConstant*& OutInstance,
                      TArray<WyVehiclePbr::FTextureBinding>& OutBindings)
{
    OutInstance = Cast<UMaterialInstanceConstant>(Material.GetAsset());
    if (!IsValid(OutInstance) || FSoftObjectPath(OutInstance) != Material.GetSoftObjectPath() || !IsValid(OutInstance->Parent))
        return false;
    static const FSoftObjectPath ParentPath(WyVehiclePbr::MasterPackage(WyVehiclePbr::MasterName) + TEXT(".") + WyVehiclePbr::MasterName);
    if (FSoftObjectPath(OutInstance->Parent.Get()) != ParentPath)
        return false;

    const FString VehicleName = FPackageName::GetLongPackageAssetName(FPackageName::GetLongPackagePath(VehicleRoot));
    FString Why;
    if (!WyVehiclePbr::GetTextureBindings(VehicleRoot, VehicleName, OutInstance->GetName(), OutBindings, Why))
        return false;

    // A removed parameter can leave an obsolete override on an existing instance.
    TArray<FMaterialParameterInfo> Parameters;
    TArray<FGuid> ParameterIds;
    OutInstance->Parent->GetAllTextureParameterInfo(Parameters, ParameterIds);
    OutBindings.RemoveAll([&Parameters](const auto& Binding) {
        return !Parameters.Contains(FMaterialParameterInfo(Binding.Parameter));
    });
    return true;
}

bool UsesTexture(UMaterialInstanceConstant* Instance, FName Parameter, const UTexture2D* Texture)
{
    UTexture* Current = nullptr;
    return Instance->GetTextureParameterValue(FMaterialParameterInfo(Parameter), Current) && Current == Texture;
}
}

FString WyVehicleTextures::VehicleRootForFolder(const FString& Folder)
{
    FString Path = Folder;
    if (Path.StartsWith(TEXT("/All/Game/"), ESearchCase::CaseSensitive))
        Path.RightChopInline(4);
    Path.RemoveFromEnd(TEXT("/"));
    if (!Path.StartsWith(TEXT("/Game/Vehicle/"), ESearchCase::CaseSensitive) || !FPackageName::IsValidLongPackageName(Path))
        return {};
    TArray<FString> Segments;
    Path.ParseIntoArray(Segments, TEXT("/"), true);
    if (Segments.Num() < 5)
        return {};
    const FString& VehicleName = Segments[3];
    const FString& VariantFolder = Segments[4];
    if (!VariantFolder.StartsWith(VehicleName + TEXT("_"), ESearchCase::CaseSensitive) || VariantFolder.Len() <= VehicleName.Len() + 1)
        return {};
    return FString(TEXT("/Game/Vehicle")) / Segments[2] / VehicleName / VariantFolder;
}

void WyVehicleTextures::Scan(const FString& VehicleRoot, TArray<TSharedPtr<FWyVehicleTextureRow>>& OutRows)
{
    OutRows.Reset();
    if (VehicleRoot.IsEmpty() || VehicleRootForFolder(VehicleRoot) != VehicleRoot)
        return;

    auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    const FString Folder = VehicleRoot / TEXT("MaterialInstance");
    if (Registry.IsLoadingAssets())
        Registry.ScanPathsSynchronous({Folder});
    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*Folder));
    Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
    Filter.bRecursivePaths = true;
    Filter.bRecursiveClasses = true;
    TArray<FAssetData> Materials;
    Registry.GetAssets(Filter, Materials);
    Materials.Sort([](const FAssetData& A, const FAssetData& B) {
        return A.AssetName == B.AssetName ? A.PackageName.LexicalLess(B.PackageName) : A.AssetName.LexicalLess(B.AssetName);
    });
    for (const FAssetData& Material : Materials)
    {
        UMaterialInstanceConstant* Instance = nullptr;
        TArray<WyVehiclePbr::FTextureBinding> Bindings;
        if (!MaterialBindings(VehicleRoot, Material, Instance, Bindings))
            continue;
        for (const auto& Binding : Bindings)
        {
            UTexture2D* Texture = WyVehiclePbr::FindTexture(Binding.Texture);
            if (!Texture || UsesTexture(Instance, Binding.Parameter, Texture))
                continue;
            auto Row = MakeShared<FWyVehicleTextureRow>();
            Row->Material = Material;
            Row->Parameter = Binding.Parameter;
            Row->Texture = FAssetData(Texture, FAssetData::ECreationFlags::SkipAssetRegistryTagsGathering);
            OutRows.Add(MoveTemp(Row));
        }
    }
}

FText WyVehicleTextures::Apply(const FString& VehicleRoot, const TArray<TSharedPtr<FWyVehicleTextureRow>>& Rows)
{
    if (VehicleRoot.IsEmpty() || VehicleRootForFolder(VehicleRoot) != VehicleRoot)
        return NSLOCTEXT("WyVehicleTextures", "InvalidRoot", "当前目录不属于具体车辆。");

    const FString Folder = VehicleRoot / TEXT("MaterialInstance");
    TSet<UMaterialInstanceConstant*> Changed;
    int32 Stale = 0;
    TOptional<FScopedTransaction> Transaction;
    for (const auto& Row : Rows)
    {
        if (!Row || Row->bApplied)
            continue;
        const FString MaterialFolder = Row->Material.PackagePath.ToString();
        UMaterialInstanceConstant* Instance = nullptr;
        TArray<WyVehiclePbr::FTextureBinding> Bindings;
        if ((MaterialFolder != Folder && !MaterialFolder.StartsWith(Folder + TEXT("/"))) ||
            !MaterialBindings(VehicleRoot, Row->Material, Instance, Bindings))
        {
            ++Stale;
            continue;
        }
        const auto* Binding = Bindings.FindByPredicate([&Row](const auto& Candidate) {
            return Candidate.Parameter == Row->Parameter && Candidate.Texture == Row->Texture.GetSoftObjectPath();
        });
        UTexture2D* Texture = Binding ? WyVehiclePbr::FindTexture(Binding->Texture) : nullptr;
        if (!Texture)
        {
            ++Stale;
            continue;
        }
        if (!UsesTexture(Instance, Row->Parameter, Texture))
        {
            if (!Transaction.IsSet())
                Transaction.Emplace(NSLOCTEXT("WyVehicleTextures", "AssignTextures", "将载具贴图赋值到材质"));
            if (!Changed.Contains(Instance))
            {
                Instance->SetFlags(RF_Transactional);
                Instance->Modify();
                Changed.Add(Instance);
            }
            Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Row->Parameter), Texture);
        }
        Row->bApplied = true;
    }
    for (auto* Instance : Changed)
        UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
    if (Stale > 0)
        return FText::Format(NSLOCTEXT("WyVehicleTextures", "Stale", "{0} 项的材质、字段或贴图已变化，请刷新列表。"), FText::AsNumber(Stale));
    return FText::GetEmpty();
}
