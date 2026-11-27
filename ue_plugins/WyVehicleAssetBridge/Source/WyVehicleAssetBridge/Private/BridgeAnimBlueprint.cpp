#include "BridgeAnimBlueprint.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "AnimationGraphSchema.h"
#include "AnimGraphNode_ComponentToLocalSpace.h"
#include "AnimGraphNode_MeshRefPose.h"
#include "AnimGraphNode_ModifyBone.h"
#include "AnimGraphNode_Root.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimBlueprintFactory.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "UObject/UnrealType.h"

namespace
{
UClass* FGearAnimParent()
{
    return LoadClass<UAnimInstance>(nullptr, TEXT("/Script/FGearPlugin.FGearAnimInstance"), nullptr, LOAD_NoWarn);
}

template <class T>
T* AddNode(UEdGraph* Graph, int32 X, int32 Y)
{
    FGraphNodeCreator<T> Creator(*Graph);
    T* Node = Creator.CreateNode(false);
    Node->NodePosX = X;
    Node->NodePosY = Y;
    Creator.Finalize();
    return Node;
}

UEdGraphPin* SplitField(const UEdGraphSchema_K2* Schema, UEdGraphPin* Parent, const TCHAR* Field)
{
    if (!Parent)
        return nullptr;
    if (Parent->SubPins.IsEmpty() && Schema->CanSplitStructPin(*Parent))
        Schema->SplitPin(Parent, false);
    const FName Name(*FString::Printf(TEXT("%s_%s"), *Parent->PinName.ToString(), Field));
    for (UEdGraphPin* Child : Parent->SubPins)
        if (Child->PinName == Name)
            return Child;
    return nullptr;
}

UEdGraphPin* PoseOutput(UEdGraphNode* Node)
{
    for (UEdGraphPin* Pin : Node->Pins)
        if (Pin && Pin->Direction == EGPD_Output && UAnimationGraphSchema::IsPosePin(Pin->PinType))
            return Pin;
    return nullptr;
}

bool Connect(const UEdGraphSchema_K2* Schema, UEdGraphPin* From, UEdGraphPin* To)
{
    return From && To && Schema->TryCreateConnection(From, To);
}

bool MakeWheelGraph(UAnimBlueprint* Blueprint, FString& Error)
{
    UEdGraph* Graph = nullptr;
    UAnimGraphNode_Root* Root = nullptr;
    for (UEdGraph* Candidate : Blueprint->FunctionGraphs)
    {
        if (Candidate->GetFName() == UEdGraphSchema_K2::GN_AnimGraph)
        {
            TArray<UAnimGraphNode_Root*> Roots;
            Candidate->GetNodesOfClass(Roots);
            if (Roots.Num() == 1)
            {
                Graph = Candidate;
                Root = Roots[0];
            }
            break;
        }
    }
    if (!Root)
    {
        Error = TEXT("动画蓝图缺少默认 AnimGraph 输出节点。");
        return false;
    }
    const auto* Schema = CastChecked<UAnimationGraphSchema>(Graph->GetSchema());
    FGraphNodeCreator<UK2Node_VariableGet> AxlesCreator(*Graph);
    auto* Axles = AxlesCreator.CreateNode(false);
    Axles->VariableReference.SetSelfMember(TEXT("axles"));
    Axles->NodePosX = -1100;
    Axles->NodePosY = 490;
    AxlesCreator.Finalize();

    // FGear uses index 0 for the front axle and 1 for the rear. Its wheel positions
    // are suspension offsets in cm, so both channels are additive in component space.
    const FName Bones[] = {TEXT("D_WheelFL"), TEXT("D_WheelFR"), TEXT("D_WheelBL"), TEXT("D_WheelBR")};
    UAnimGraphNode_ModifyBone* Wheels[4] = {};
    for (int32 Axle = 0; Axle < 2; ++Axle)
    {
        auto* Get = AddNode<UK2Node_GetArrayItem>(Graph, -770, 195 + Axle * 560);
        if (!Connect(Schema, Axles->FindPin(TEXT("axles"), EGPD_Output), Get->GetTargetArrayPin()))
        {
            Error = TEXT("无法连入 FGear 动画实例的 Axles 数组。");
            return false;
        }
        Schema->TrySetDefaultValue(*Get->GetIndexPin(), FString::FromInt(Axle));
        for (int32 Side = 0; Side < 2; ++Side)
        {
            const int32 Index = Axle * 2 + Side;
            UEdGraphPin* Wheel = SplitField(Schema, Get->GetResultPin(), Side == 0 ? TEXT("left") : TEXT("right"));
            UEdGraphPin* Position = SplitField(Schema, Wheel, TEXT("position"));
            UEdGraphPin* Rotation = SplitField(Schema, Wheel, TEXT("rotation"));
            FGraphNodeCreator<UAnimGraphNode_ModifyBone> Creator(*Graph);
            auto* Modify = Creator.CreateNode(false);
            Modify->NodePosX = -330;
            Modify->NodePosY = 280 * Index;
            Modify->Node.BoneToModify.BoneName = Bones[Index];
            Modify->Node.TranslationMode = Modify->Node.RotationMode = BMM_Additive;
            Modify->Node.TranslationSpace = Modify->Node.RotationSpace = BCS_ComponentSpace;
            Modify->Node.ScaleMode = BMM_Ignore;
            Creator.Finalize();
            const int32 ScalePin = Modify->ShowPinForProperties.IndexOfByPredicate(
                [](const FOptionalPinFromProperty& P) { return P.PropertyName == TEXT("Scale"); });
            if (ScalePin != INDEX_NONE)
                Modify->SetPinVisibility(false, ScalePin);
            if (!Connect(Schema, Position, Modify->FindPin(TEXT("Translation"), EGPD_Input)) ||
                !Connect(Schema, Rotation, Modify->FindPin(TEXT("Rotation"), EGPD_Input)))
            {
                Error = FString::Printf(TEXT("无法连接车轮骨骼 %s 的位置或旋转。"), *Bones[Index].ToString());
                return false;
            }
            Wheels[Index] = Modify;
        }
    }

    auto* RefPose = AddNode<UAnimGraphNode_MeshRefPose>(Graph, -740, 1060);
    UEdGraphPin* Pose = PoseOutput(RefPose);
    for (int32 Index = 3; Index >= 0; --Index)
    {
        if (!Connect(Schema, Pose, Wheels[Index]->FindPin(TEXT("ComponentPose"), EGPD_Input)))
        {
            Error = TEXT("无法串接轮骨骼的组件空间姿势。");
            return false;
        }
        Pose = PoseOutput(Wheels[Index]);
    }
    auto* Local = AddNode<UAnimGraphNode_ComponentToLocalSpace>(Graph, 80, 155);
    Root->NodePosX = 420;
    Root->NodePosY = 390;
    if (!Connect(Schema, Pose, Local->FindPin(TEXT("ComponentPose"), EGPD_Input)) ||
        !Connect(Schema, PoseOutput(Local), Root->FindPin(TEXT("Result"), EGPD_Input)))
    {
        Error = TEXT("无法将车轮姿势连接到动画蓝图输出。");
        return false;
    }
    return true;
}
}

bool WyValidateAnimBlueprintDependencies(FString& Error)
{
    UClass* Parent = FGearAnimParent();
    const auto* Axles = Parent ? FindFProperty<FArrayProperty>(Parent, TEXT("axles")) : nullptr;
    const auto* Axle = Axles ? CastField<FStructProperty>(Axles->Inner) : nullptr;
    for (const TCHAR* Side : {TEXT("left"), TEXT("right")})
    {
        const auto* Wheel = Axle ? FindFProperty<FStructProperty>(Axle->Struct, Side) : nullptr;
        const auto* Position = Wheel ? FindFProperty<FStructProperty>(Wheel->Struct, TEXT("position")) : nullptr;
        const auto* Rotation = Wheel ? FindFProperty<FStructProperty>(Wheel->Struct, TEXT("rotation")) : nullptr;
        if (!Position || Position->Struct != TBaseStructure<FVector>::Get() ||
            !Rotation || Rotation->Struct != TBaseStructure<FRotator>::Get())
        {
            Error = TEXT("请确认当前工程已启用 FGearPlugin，且 FGearAnimInstance 的 Axles 含有左右轮位置与旋转。");
            return false;
        }
    }
    return true;
}

bool WyCreateAnimBlueprint(UObject* Outer, FName Name, USkeleton* Skeleton, USkeletalMesh* PreviewMesh,
                           UAnimBlueprint*& OutBlueprint, FString& Error)
{
    OutBlueprint = nullptr;
    if (!WyValidateAnimBlueprintDependencies(Error))
        return false;
    if (!Skeleton || !PreviewMesh)
    {
        Error = TEXT("创建动画蓝图需要车辆骨骼和骨骼网格体。");
        return false;
    }
    auto* Factory = NewObject<UAnimBlueprintFactory>();
    Factory->ParentClass = FGearAnimParent();
    Factory->TargetSkeleton = Skeleton;
    Factory->PreviewSkeletalMesh = PreviewMesh;
    OutBlueprint = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), Outer, Name,
        RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
    if (!OutBlueprint)
    {
        Error = TEXT("UE 未能创建 FGear 动画蓝图。");
        return false;
    }
    if (!MakeWheelGraph(OutBlueprint, Error))
        return false;
    FBlueprintEditorUtils::MarkBlueprintAsModified(OutBlueprint);
    FCompilerResultsLog Log;
    FKismetEditorUtilities::CompileBlueprint(OutBlueprint,
        EBlueprintCompileOptions::SkipGarbageCollection | EBlueprintCompileOptions::SkipSave, &Log);
    if (Log.NumErrors || OutBlueprint->Status == BS_Error || !Cast<UAnimBlueprintGeneratedClass>(OutBlueprint->GeneratedClass))
    {
        Error = TEXT("车轮动画蓝图编译失败。");
        for (const auto& Message : Log.Messages)
            if (Message->GetSeverity() == EMessageSeverity::Error)
                Error += TEXT(" ") + Message->ToText().ToString();
        return false;
    }
    return true;
}
