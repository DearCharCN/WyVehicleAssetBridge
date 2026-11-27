#include "BridgeVehicleBlueprint.h"
#include "BridgeAssets.h"

#include "Animation/AnimBlueprint.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/CollisionProfile.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MeshDescription.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
struct FWheelBinding
{
    const TCHAR* Component;
    const TCHAR* Bone;
    bool bFront;
    double Yaw;
};

constexpr FWheelBinding Wheels[] = {
    {TEXT("Front Wheel Left"), TEXT("D_WheelFL"), true, 180.},
    {TEXT("Front Wheel Right"), TEXT("D_WheelFR"), true, 0.},
    {TEXT("Rear Wheel Left"), TEXT("D_WheelBL"), false, 180.},
    {TEXT("Rear Wheel Right"), TEXT("D_WheelBR"), false, 0.},
};
constexpr const TCHAR* WheelOptions[] = {TEXT("Front Wheel Options"), TEXT("Rear Wheel Options")};

// FGear and the managed vehicle class belong to the destination project. Resolving the
// native defaults by reflection also lets the bridge editor module load in an empty project.
struct FVehicleDefaults
{
    USkeletalMeshComponent* Mesh = nullptr;
    UObject* Wheel[UE_ARRAY_COUNT(Wheels)]{};
    UObject* Options[UE_ARRAY_COUNT(WheelOptions)]{};
    FNameProperty* BoneName = nullptr;
    FFloatProperty* Radius = nullptr;
    FFloatProperty* Width = nullptr;

    bool Read(UClass* Class, FString& Error)
    {
        AActor* CDO = Class ? Class->GetDefaultObject<AActor>() : nullptr;
        if (!CDO)
        {
            Error = TEXT("无法读取车辆类的默认 Actor。");
            return false;
        }
        const auto Component = [&](const TCHAR* Name, const TCHAR* ClassPath) -> UObject*
        {
            UObject* Object = CDO->GetDefaultSubobjectByName(FName(Name));
            UClass* ComponentClass = FindObject<UClass>(nullptr, ClassPath);
            if (!Object || !ComponentClass || !Object->IsA(ComponentClass))
            {
                Error = FString::Printf(TEXT("车辆类 %s 缺少 FGear 默认组件 %s。"), *Class->GetName(), Name);
                return nullptr;
            }
            return Object;
        };

        Mesh = Cast<USkeletalMeshComponent>(Component(TEXT("Mesh Component"), TEXT("/Script/FGearPlugin.FGearSkeletalMeshComponent")));
        if (!Mesh)
            return false;
        for (int32 I = 0; I < UE_ARRAY_COUNT(Wheels); ++I)
        {
            Wheel[I] = Component(Wheels[I].Component, TEXT("/Script/FGearPlugin.FGearWheel"));
            if (!Wheel[I])
                return false;
        }
        for (int32 I = 0; I < UE_ARRAY_COUNT(WheelOptions); ++I)
        {
            Options[I] = Component(WheelOptions[I], TEXT("/Script/FGearPlugin.FGearWheelOptions"));
            if (!Options[I])
                return false;
        }
        BoneName = FindFProperty<FNameProperty>(Wheel[0]->GetClass(), TEXT("mBoneName"));
        Radius = FindFProperty<FFloatProperty>(Options[0]->GetClass(), TEXT("mRadius"));
        Width = FindFProperty<FFloatProperty>(Options[0]->GetClass(), TEXT("mWidth"));
        if (!BoneName || !Radius || !Width)
        {
            Error = TEXT("FGear 车轮组件缺少 Bone Name、Radius 或 Width 属性。");
            return false;
        }
        return true;
    }
};

bool MeasureTire(UStaticMesh* Tire, float& RadiusMeters, float& WidthMeters, FString& Error)
{
    const FMeshDescription* Source = Tire ? Tire->GetMeshDescription(0) : nullptr;
    if (!Source || Source->Triangles().Num() == 0)
    {
        Error = TEXT("Tire_0000 缺少 LOD0 源三角网格，无法测量轮胎。");
        return false;
    }
    const auto Positions = Source->GetVertexPositions();
    // Wheel rotation axis is vehicle Y. Keep the artist's mounting offset intact
    // while measuring the tread radius in XZ and the full section width along Y.
    FBox Bounds(ForceInit);
    for (const FVertexID Vertex : Source->Vertices().GetElementIDs())
        Bounds += FVector(Positions[Vertex]);
    const FVector Center = Bounds.GetCenter();
    double RadiusSquared = 0.;
    for (const FVertexID Vertex : Source->Vertices().GetElementIDs())
    {
        const FVector Delta = FVector(Positions[Vertex]) - Center;
        RadiusSquared = FMath::Max(RadiusSquared, FMath::Square(Delta.X) + FMath::Square(Delta.Z));
    }
    WidthMeters = static_cast<float>(Bounds.GetSize().Y * .01);
    RadiusMeters = static_cast<float>(FMath::Sqrt(RadiusSquared) * .01);
    if (!FMath::IsFinite(WidthMeters) || !FMath::IsFinite(RadiusMeters) || WidthMeters <= 0 || RadiusMeters <= 0)
    {
        Error = TEXT("Tire_0000 的轮胎半径或宽度无效，请检查 LOD0 的尺寸和轮轴方向（UE 的 Y 轴）。");
        return false;
    }
    return true;
}
} // namespace

UClass* WyResolveVehicleParent(FString& Error)
{
    UClass* Parent = FindObject<UClass>(nullptr, TEXT("/Script/UnrealSharp.WYAutomobileCS_C"));
    UClass* FGearVehicle = FindObject<UClass>(nullptr, TEXT("/Script/FGearPlugin.FGearVehicle"));
    if (!Parent || !FGearVehicle || !Parent->IsChildOf(FGearVehicle) || !FKismetEditorUtilities::CanCreateBlueprintOfClass(Parent))
    {
        Error = TEXT("无法使用望月车辆父类 WYAutomobileCS，请确认当前工程已加载 FGear 和望月 UnrealSharp 脚本。");
        return nullptr;
    }
    FVehicleDefaults Defaults;
    return Defaults.Read(Parent, Error) ? Parent : nullptr;
}

bool WyCreateVehicleBlueprint(UPackage* Outer, FName Name, UClass* ParentClass, USkeletalMesh* Mesh,
                              UAnimBlueprint* Anim, UStaticMesh* Hub, UStaticMesh* Tire, UStaticMesh* FrontBrake,
                              UStaticMesh* RearBrake, UBlueprint*& OutBlueprint, FString& Error)
{
    OutBlueprint = nullptr;
    if (!Outer || !ParentClass || !Mesh || !Hub || !Tire || !FrontBrake || !RearBrake ||
        !Anim || !Anim->GeneratedClass || !Mesh->GetSkeleton() || Anim->TargetSkeleton != Mesh->GetSkeleton())
    {
        Error = TEXT("车辆蓝图需要已生成的车身、轮毂、胎皮、前后刹车和使用同一骨骼的动画蓝图。");
        return false;
    }
    for (const FWheelBinding& Wheel : Wheels)
        if (Mesh->GetRefSkeleton().FindBoneIndex(FName(Wheel.Bone)) == INDEX_NONE)
        {
            Error = FString::Printf(TEXT("车身骨骼缺少车轮挂点 %s。"), Wheel.Bone);
            return false;
        }
    float RadiusMeters, WidthMeters;
    if (!MeasureTire(Tire, RadiusMeters, WidthMeters, Error))
        return false;

    OutBlueprint = FKismetEditorUtilities::CreateBlueprint(ParentClass, Outer, Name, BPTYPE_Normal);
    if (!OutBlueprint || !OutBlueprint->SimpleConstructionScript)
    {
        Error = TEXT("UE 未能建立车辆蓝图的组件树。");
        return false;
    }
    OutBlueprint->Modify();
    FVehicleDefaults Defaults;
    if (!Defaults.Read(OutBlueprint->GeneratedClass, Error))
        return false;
    USimpleConstructionScript* SCS = OutBlueprint->SimpleConstructionScript;
    for (const FWheelBinding& Wheel : Wheels)
    {
        const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
        const FTransform SocketPose = Skeleton.GetBoneAbsoluteTransform(Skeleton.FindBoneIndex(FName(Wheel.Bone)));
        // Wheel parts use a shared right-side mounting pivot. Match the WangYue
        // vehicle-frame orientation, even on rigs whose wheel bones are already turned.
        const FTransform WheelPose(FRotator(0., Wheel.Yaw, 0.), SocketPose.GetTranslation());
        const FTransform SocketOffset = WheelPose.GetRelativeTransform(SocketPose);
        UStaticMesh* Parts[] = {Hub, Tire, Wheel.bFront ? FrontBrake : RearBrake};
        for (UStaticMesh* Part : Parts)
        {
            const FName ComponentName(*FString::Printf(TEXT("%s_%s"), *WyStaticMeshPartName(Part->GetName()), Wheel.Bone));
            USCS_Node* Node = SCS->CreateNode(UStaticMeshComponent::StaticClass(), ComponentName);
            UStaticMeshComponent* Visual = Node ? Cast<UStaticMeshComponent>(Node->ComponentTemplate) : nullptr;
            if (!Visual)
            {
                Error = FString::Printf(TEXT("创建车轮可视组件 %s 失败。"), *ComponentName.ToString());
                return false;
            }
            Visual->SetStaticMesh(Part);
            Visual->SetMobility(EComponentMobility::Movable);
            Visual->SetRelativeTransform(SocketOffset);
            Visual->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
            Visual->SetGenerateOverlapEvents(false);
            Node->SetParent(Defaults.Mesh);
            Node->AttachToName = FName(Wheel.Bone);
            SCS->AddNode(Node);
        }
    }

    FKismetEditorUtilities::CompileBlueprint(OutBlueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
    if (!OutBlueprint->GeneratedClass || OutBlueprint->Status == BS_Error)
    {
        Error = FString::Printf(TEXT("车辆蓝图 %s 编译失败，请查看 UE 的蓝图编译日志。"), *Name.ToString());
        return false;
    }
    // Compiling the SCS reinstantiates the generated CDO. Write the inherited
    // native component defaults on the final child CDO, never the parent's CDO.
    if (!Defaults.Read(OutBlueprint->GeneratedClass, Error))
        return false;
    Defaults.Mesh->Modify();
    Defaults.Mesh->SetSkeletalMesh(Mesh);
    Defaults.Mesh->SetAnimInstanceClass(Anim->GeneratedClass);
    for (int32 I = 0; I < UE_ARRAY_COUNT(Wheels); ++I)
    {
        Defaults.Wheel[I]->Modify();
        Defaults.BoneName->SetPropertyValue_InContainer(Defaults.Wheel[I], FName(Wheels[I].Bone));
    }
    for (UObject* Options : Defaults.Options)
    {
        Options->Modify();
        Defaults.Radius->SetPropertyValue_InContainer(Options, RadiusMeters);
        Defaults.Width->SetPropertyValue_InContainer(Options, WidthMeters);
    }
    return true;
}
