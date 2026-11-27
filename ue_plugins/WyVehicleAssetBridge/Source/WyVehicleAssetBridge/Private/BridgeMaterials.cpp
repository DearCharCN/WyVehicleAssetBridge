#include "BridgeMaterials.h"
#include "WyPendingVehicleTextures.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace WyAssets
{
UObject* FindAsset(const FString& Package);
}

namespace
{
FString MaterialPurpose(const FString& MaxName)
{
    return MaxName.StartsWith(TEXT("MI_"), ESearchCase::CaseSensitive) ? MaxName.Mid(3) : MaxName;
}

const TCHAR* MasterName(const FString& Name)
{
    if (Name.Contains(TEXT("CarPaint"), ESearchCase::IgnoreCase))
        return TEXT("M_Vehicle_CarPaintWY_Master");
    if (Name.Contains(TEXT("Glass"), ESearchCase::IgnoreCase))
        return TEXT("M_Vehicle_Glass_Master");
    if (Name.Contains(TEXT("Carbon"), ESearchCase::IgnoreCase))
        return TEXT("M_Vehicle_CarbonFiber_Master");
    return WyVehiclePbr::MasterName;
}
}

FString WyVehiclePbr::MasterPackage(const FString& Name)
{
    return FString(TEXT("/Game/Material/Vehicle")) / Name;
}

bool WyVehiclePbr::GetTextureBindings(const FString& VehicleBase, const FString& VehicleName, const FString& InstanceName,
                                     TArray<FTextureBinding>& Out, FString& Why)
{
    Out.Reset();
    FString Purpose = InstanceName;
    if (Purpose.StartsWith(TEXT("MI_"), ESearchCase::CaseSensitive) &&
        !Purpose.RemoveFromStart(TEXT("MI_") + VehicleName + TEXT("_"), ESearchCase::CaseSensitive))
    {
        Why = TEXT("材质实例的名称与当前车辆不匹配：") + InstanceName;
        return false;
    }
    if (Purpose.IsEmpty())
    {
        Why = TEXT("材质实例名称缺少用途：") + InstanceName;
        return false;
    }
    struct FChannel { const TCHAR* Parameter; const TCHAR* Suffix; };
    static constexpr FChannel Channels[] = {
        {TEXT("Color_Tex"), TEXT("D")},
        {TEXT("M_Tex"), TEXT("MRO")},
        {TEXT("Normal_Tex"), TEXT("N")},
    };
    const FString Prefix = TEXT("T_") + VehicleName + TEXT("_") + Purpose + TEXT("_");
    for (const auto& Channel : Channels)
    {
        const FString Name = Prefix + Channel.Suffix;
        const FString Package = VehicleBase / TEXT("Texture") / Name;
        const FString Path = Package + TEXT(".") + Name;
        if (Package.Len() >= NAME_SIZE || !FPackageName::IsValidObjectPath(Path))
        {
            Out.Reset();
            Why = TEXT("贴图目标路径无效：") + Package;
            return false;
        }
        Out.Add({FName(Channel.Parameter), FSoftObjectPath(Path)});
    }
    return true;
}

UTexture2D* WyVehiclePbr::FindTexture(const FSoftObjectPath& Path)
{
    auto* Texture = Cast<UTexture2D>(WyAssets::FindAsset(Path.GetLongPackageName()));
    return IsValid(Texture) && FSoftObjectPath(Texture) == Path ? Texture : nullptr;
}

FString WyVehicleMaterialAssetName(const FString& VehicleName, const FString& MaxMaterialName)
{
    if (!MaxMaterialName.StartsWith(TEXT("MI_"), ESearchCase::CaseSensitive))
        return MaxMaterialName;
    return TEXT("MI_") + VehicleName + TEXT("_") + MaxMaterialName.Mid(3);
}

bool WyPlanVehicleMaterial(const FString& VehicleBase, const FString& VehicleName, const FString& MaterialName,
                           FWyVehicleMaterialPlan& Plan, FString& Why)
{
    const FString Purpose = MaterialPurpose(MaterialName);
    const FString AssetName = WyVehicleMaterialAssetName(VehicleName, MaterialName);
    const FString Target = VehicleBase / TEXT("MaterialInstance") / AssetName;
    if (Purpose.IsEmpty() || Target.Len() >= NAME_SIZE || AssetName.Contains(TEXT("/")) ||
        !FName::IsValidXName(AssetName, INVALID_OBJECTNAME_CHARACTERS) ||
        !FPackageName::IsValidObjectPath(Target + TEXT(".") + AssetName) || FName(*MaterialName).IsNone())
    {
        Why = FString::Printf(TEXT("Max 材质名“%s”不能作为 UE 资源名，请先在 Max 中改名。"), *MaterialName);
        return false;
    }
    const FName Key(*MaterialName);
    if (const auto* Planned = Plan.Entries.Find(Key))
    {
        if (Planned->Package.Equals(Target, ESearchCase::CaseSensitive))
            return true;
        Why = FString::Printf(TEXT("材质名仅大小写不同，UE 无法分别创建：%s、%s"),
                              *FPackageName::GetLongPackageAssetName(Planned->Package), *MaterialName);
        return false;
    }
    const FName PackageKey(*Target);
    if (Plan.Packages.Contains(PackageKey))
    {
        Why = TEXT("多个 Max 材质名映射到同一个 UE 资源：") + Target;
        return false;
    }
    FWyVehicleMaterialPlan::FEntry Entry;
    Entry.Package = Target;
    if (UObject* Existing = WyAssets::FindAsset(Target))
    {
        Entry.Material = Cast<UMaterialInterface>(Existing);
        if (!Entry.Material || !Existing->GetOutermost()->GetName().Equals(Target, ESearchCase::IgnoreCase))
        {
            Why = TEXT("材质目标路径已被其他类型的资源或重定向器占用：") + Target;
            return false;
        }
    }
    else
    {
        if (FPackageName::DoesPackageExist(Target))
        {
            Why = TEXT("目标包中找不到同名材质：") + Target;
            return false;
        }
        const FString Master = MasterName(Purpose);
        const FString ParentPath = WyVehiclePbr::MasterPackage(Master);
        Entry.Parent = Cast<UMaterialInterface>(WyAssets::FindAsset(ParentPath));
        if (!Entry.Parent)
        {
            Why = FString::Printf(TEXT("无法创建材质实例“%s”，找不到父材质 %s。"), *MaterialName, *ParentPath);
            return false;
        }
        if (Master == WyVehiclePbr::MasterName)
        {
            TArray<WyVehiclePbr::FTextureBinding> Bindings;
            if (!WyVehiclePbr::GetTextureBindings(VehicleBase, VehicleName, AssetName, Bindings, Why))
                return false;
            TArray<FMaterialParameterInfo> Parameters;
            TArray<FGuid> ParameterIds;
            Entry.Parent->GetAllTextureParameterInfo(Parameters, ParameterIds);
            for (const auto& Binding : Bindings)
            {
                if (!Parameters.Contains(FMaterialParameterInfo(Binding.Parameter)))
                {
                    Why = FString::Printf(TEXT("无法创建材质实例“%s”，父材质 %s 缺少贴图参数 %s。"), *AssetName, *ParentPath, *Binding.Parameter.ToString());
                    return false;
                }
                Entry.PbrTextures.Add(Binding.Parameter, Binding.Texture);
            }
        }
    }
    Plan.Entries.Add(Key, MoveTemp(Entry));
    Plan.Packages.Add(PackageKey);
    return true;
}

void WyCreatePlannedVehicleMaterials(FWyVehicleMaterialPlan& Plan, TArray<UObject*>& Created)
{
    for (auto& Pair : Plan.Entries)
    {
        auto& Entry = Pair.Value;
        if (Entry.Material)
            continue;
        UPackage* Package = CreatePackage(*Entry.Package);
        const FName AssetName(*FPackageName::GetLongPackageAssetName(Entry.Package));
        auto* Instance = NewObject<UMaterialInstanceConstant>(Package, AssetName, RF_Public | RF_Standalone | RF_Transactional);
        Instance->SetParentEditorOnly(Entry.Parent, false);
        UWyPendingVehicleTextures* Pending = nullptr;
        for (const auto& Channel : Entry.PbrTextures)
        {
            if (UTexture2D* Texture = WyVehiclePbr::FindTexture(Channel.Value))
                Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Channel.Key), Texture);
            else
            {
                if (!Pending)
                {
                    Pending = NewObject<UWyPendingVehicleTextures>(Instance, NAME_None, RF_Transactional);
                    Instance->AddAssetUserData(Pending);
                }
                Pending->PendingTextures.Add(Channel.Key, Channel.Value);
            }
        }
        Instance->PostEditChange();
        Entry.Material = Instance;
        Created.Add(Instance);
    }
}

UMaterialInterface* WyPlannedVehicleMaterial(const FWyVehicleMaterialPlan& Plan, FName MaterialName)
{
    return Plan.Entries.FindChecked(MaterialName).Material;
}

