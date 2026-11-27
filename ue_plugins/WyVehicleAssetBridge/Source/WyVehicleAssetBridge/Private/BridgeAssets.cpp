#include "BridgeAssets.h"
#include "BridgeMaterials.h"
#include "BridgeMeshSlots.h"
#include "Materials/MaterialInterface.h"
#include "BridgeAnimBlueprint.h"
#include "BridgeVehicleBlueprint.h"
#include "Animation/AnimBlueprint.h"
#include "BlueprintEditorSettings.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "StaticMeshAttributes.h"
#include "SkeletalMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "SkeletalMeshCompiler.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "MeshElementRemappings.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ReferenceSkeleton.h"
#include "ScopedTransaction.h"
#include "Misc/PackageName.h"
#include "Misc/Change.h"
#include "Misc/ScopedSlowTask.h"
#include "Editor/TransBuffer.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "Serialization/JsonSerializer.h"
#include "CompGeom/ConvexHull3.h"
#include "Chaos/Convex.h"

#define LOCTEXT_NAMESPACE "WyVehicleBridgeAssets"

namespace WyAssets
{
using JArray = TArray<TSharedPtr<FJsonValue>>;
using JObject = TSharedPtr<FJsonObject>;
static const FName OwnerAttribute(TEXT("WyVehiclePartOwner"));
static const FName PartNameAttribute(TEXT("WyVehiclePartName"));
static const FName UVCountAttribute(TEXT("WyVehicleOriginalUVCount"));

// Advance once per batch so a single dense mesh can repaint the progress window.
void EnterGeometryBatch(FScopedSlowTask &Progress, int32 Index, int32 Count)
{
    constexpr int32 BatchSize = 4096;
    if (Index % BatchSize == 0)
        Progress.EnterProgressFrame(FMath::Min(BatchSize, Count - Index));
}

JObject Error(const FString &Message)
{
    auto O = MakeShared<FJsonObject>();
    O->SetBoolField(TEXT("ok"), false);
    O->SetStringField(TEXT("error"), Message);
    return O;
}
TSharedPtr<FJsonValue> JV(const JObject &O)
{
    return MakeShared<FJsonValueObject>(O);
}
TSharedPtr<FJsonValue> JA(const JArray &A)
{
    return MakeShared<FJsonValueArray>(A);
}
TSharedPtr<FJsonValue> JN(double V)
{
    return MakeShared<FJsonValueNumber>(V);
}
JArray Vec(const FVector &V)
{
    return {JN(V.X), JN(V.Y), JN(V.Z)};
}
JArray Vec4(const FVector4f &V)
{
    return {JN(V.X), JN(V.Y), JN(V.Z), JN(V.W)};
}
bool Numbers(const TSharedPtr<FJsonValue> &V, int32 Count, TArray<double> &Out)
{
    const JArray *A = nullptr;
    if (!V || !V->TryGetArray(A) || A->Num() != Count)
        return false;
    Out.Reset();
    for (const auto &X : *A)
    {
        double N;
        if (!X || !X->TryGetNumber(N) || !FMath::IsFinite(N) || FMath::Abs(N) > 1.e8)
            return false;
        Out.Add(N);
    }
    return true;
}
bool VectorField(const JObject &O, const TCHAR *Name, int32 Count, TArray<double> &Out)
{
    const auto *Value = O->Values.Find(Name);
    return Value && Numbers(*Value, Count, Out);
}
bool IntegerField(const JObject &O, const TCHAR *Name, int32 &Out, int32 Min = 0, int32 Max = 64)
{
    double D;
    if (!O->TryGetNumberField(Name, D) || !FMath::IsFinite(D) || D < Min || D > Max || FMath::FloorToDouble(D) != D)
        return false;
    Out = int32(D);
    return true;
}
bool SafeSegment(const FString &S)
{
    return !S.IsEmpty() && !S.Contains(TEXT("/")) && !S.Contains(TEXT("\\")) && !S.Contains(TEXT(".")) &&
           FPackageName::IsValidLongPackageName(TEXT("/Game/") + S);
}
struct FVehicle
{
    FString Type, Name, Variant, Base, SkeletonPath;
    FString AssetSuffix() const
    {
        return Variant.Equals(TEXT("Base"), ESearchCase::CaseSensitive) ? Name : Name + TEXT("_") + Variant;
    }
    FString Blueprint(bool Animation) const
    {
        return Base / (Animation ? TEXT("AnimBlueprint") : TEXT("Blueprint")) /
               ((Animation ? TEXT("ABP_") : TEXT("BP_")) + AssetSuffix());
    }
    FString Skeletal() const
    {
        return Base / TEXT("SkeletalMesh") / (TEXT("SK_") + Name);
    }
    FString StaticMesh(const FString &Part) const
    {
        return Base / TEXT("StaticMesh") / (TEXT("SM_") + Part);
    }
    FString Physics() const
    {
        return Base / TEXT("PhysicsAsset") / (TEXT("PHYS_") + Name + TEXT("_Body"));
    }
};
bool Vehicle(const JObject &R, FVehicle &V)
{
    const JObject *O = nullptr;
    if (!R->TryGetObjectField(TEXT("vehicle"), O) || !(*O)->TryGetStringField(TEXT("type"), V.Type) ||
        !(*O)->TryGetStringField(TEXT("name"), V.Name) || !(*O)->TryGetStringField(TEXT("variant"), V.Variant))
        return false;
    if (!SafeSegment(V.Type) || !SafeSegment(V.Name) || !SafeSegment(V.Variant))
        return false;
    V.Base = TEXT("/Game/Vehicle") / V.Type / V.Name / (V.Name + TEXT("_") + V.Variant);
    return FPackageName::IsValidLongPackageName(V.Base);
}
UObject *FindAsset(const FString &Package)
{
    FSoftObjectPath AssetPath(Package + TEXT(".") + FPackageName::GetLongPackageAssetName(Package));
    AssetPath.FixupCoreRedirects();
    const FString ObjectPath = AssetPath.ToString();
    if (auto *Existing = FindObject<UObject>(nullptr, *ObjectPath))
        return Existing;
    // Loading a nonexistent package leaves an empty UPackage in the editor, even during preflight.
    if (!FPackageName::DoesPackageExist(AssetPath.GetLongPackageName()))
        return nullptr;
    return LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
}
FString ResolveSkeletonPath(const FVehicle &V)
{
    const FString Canonical = V.Base / TEXT("Skeleton") / (TEXT("SKEL_") + V.AssetSuffix());
    if (!V.Variant.Equals(TEXT("Base"), ESearchCase::CaseSensitive))
        return Canonical;
    const FString Legacy = Canonical + TEXT("_Base");
    if (auto *OldSkeleton = Cast<USkeleton>(FindAsset(Legacy)))
    {
        if (!FindAsset(Canonical))
            return Legacy;
        // Vehicles created with the old name may still have an explicitly bound skeleton.
        auto *Mesh = Cast<USkeletalMesh>(FindAsset(V.Skeletal()));
        if (Mesh && Mesh->GetSkeleton() == OldSkeleton)
            return Legacy;
    }
    return Canonical;
}
JObject Item(const FString &Kind, const FString &Name, int32 Lod, const JObject &Data)
{
    auto O = MakeShared<FJsonObject>();
    O->SetStringField(TEXT("id"), Name);
    O->SetStringField(TEXT("name"), Name);
    O->SetStringField(TEXT("kind"), Kind);
    O->SetNumberField(TEXT("lod"), Lod);
    O->SetObjectField(TEXT("data"), Data);
    return O;
}
JObject Unmatched(const FString &Kind, const FString &Name, int32 Lod, const FString &Why)
{
    auto O = Item(Kind, Name, Lod, MakeShared<FJsonObject>());
    O->SetBoolField(TEXT("unmatched"), true);
    O->SetStringField(TEXT("reason"), Why);
    return O;
}
JObject ReadBones(const FReferenceSkeleton &Ref)
{
    JArray Bones;
    for (int32 I = 0; I < Ref.GetRawBoneNum(); ++I)
    {
        auto B = MakeShared<FJsonObject>();
        const auto &Info = Ref.GetRawRefBoneInfo()[I];
        const FTransform &T = Ref.GetRawRefBonePose()[I];
        B->SetStringField(TEXT("name"), Info.Name.ToString());
        B->SetStringField(TEXT("parent"),
                          Info.ParentIndex < 0 ? TEXT("") : Ref.GetBoneName(Info.ParentIndex).ToString());
        B->SetArrayField(TEXT("translation"), Vec(T.GetTranslation()));
        const FQuat Q = T.GetRotation();
        B->SetArrayField(TEXT("rotation"), {JN(Q.X), JN(Q.Y), JN(Q.Z), JN(Q.W)});
        B->SetArrayField(TEXT("scale"), Vec(T.GetScale3D()));
        Bones.Add(JV(B));
    }
    auto D = MakeShared<FJsonObject>();
    D->SetArrayField(TEXT("bones"), Bones);
    return D;
}
bool ParseBones(const JObject &Data, FReferenceSkeleton &Ref, FString &Why)
{
    const JArray *Bones = nullptr;
    if (!Data->TryGetArrayField(TEXT("bones"), Bones) || Bones->IsEmpty() || Bones->Num() > 65535)
    {
        Why = TEXT("Invalid skeleton bones");
        return false;
    }
    Ref.Empty();
    FReferenceSkeletonModifier Modifier(Ref, nullptr);
    TMap<FString, int32> Names;
    for (const auto &Value : *Bones)
    {
        const JObject *B = nullptr;
        FString Name, Parent;
        TArray<double> T, Q, S;
        if (!Value->TryGetObject(B) || !(*B)->TryGetStringField(TEXT("name"), Name) ||
            !(*B)->TryGetStringField(TEXT("parent"), Parent) || Name.IsEmpty() || Names.Contains(Name) ||
            !VectorField(*B, TEXT("translation"), 3, T) || !VectorField(*B, TEXT("rotation"), 4, Q) ||
            !VectorField(*B, TEXT("scale"), 3, S))
        {
            Why = TEXT("Invalid bone or transform");
            return false;
        }
        const int32 *P = Names.Find(Parent);
        if ((Parent.IsEmpty() && !Names.IsEmpty()) || (!Parent.IsEmpty() && !P))
        {
            Why = TEXT("Bones require one root and parent-before-child order");
            return false;
        }
        FQuat Rot(Q[0], Q[1], Q[2], Q[3]);
        if (Rot.SizeSquared() < 1.e-12 || FMath::Abs(S[0]) < 1.e-8 || FMath::Abs(S[1]) < 1.e-8 ||
            FMath::Abs(S[2]) < 1.e-8)
        {
            Why = TEXT("Singular bone transform");
            return false;
        }
        Rot.Normalize();
        Modifier.Add(FMeshBoneInfo(FName(*Name), Name, P ? *P : INDEX_NONE),
                     FTransform(Rot, FVector(T[0], T[1], T[2]), FVector(S[0], S[1], S[2])));
        Names.Add(Name, Names.Num());
    }
    return true;
}
int32 VehicleBodyBone(const FReferenceSkeleton &Ref)
{
    const int32 Root = Ref.FindBoneIndex(TEXT("Root"));
    const int32 Body = Ref.FindBoneIndex(TEXT("Body"));
    return Root == 0 && Body != INDEX_NONE && Ref.GetParentIndex(Body) == Root ? Body : INDEX_NONE;
}
FTransform BoneTransform(const FReferenceSkeleton &Ref, int32 Bone)
{
    FTransform T = Ref.GetRefBonePose()[Bone];
    for (int32 P = Ref.GetParentIndex(Bone); P != INDEX_NONE; P = Ref.GetParentIndex(P))
        T = T * Ref.GetRefBonePose()[P];
    return T;
}

struct FMeshInput
{
    TArray<FVector3f> Positions, Normals;
    TArray<FVector4f> Colors, Tangents;
    TArray<FIntVector> Triangles;
    TArray<TArray<FVector2f>> UVs;
    TArray<TArray<UE::AnimationCore::FBoneWeight>> Weights;
    TArray<int32> MaterialIds, LocalFaceIds;
    TMap<int32, FString> SlotNames;
    bool HasNamedSlots = false;
};
bool ParseMesh(const JObject &D, const FReferenceSkeleton *Ref, FMeshInput &M, FString &Why)
{
    const JArray *P = nullptr, *T = nullptr, *N = nullptr, *C = nullptr, *Tang = nullptr, *UV = nullptr, *W = nullptr;
    if (!D->TryGetArrayField(TEXT("positions"), P) || !D->TryGetArrayField(TEXT("triangles"), T) ||
        !D->TryGetArrayField(TEXT("normals"), N) || !D->TryGetArrayField(TEXT("colors"), C) ||
        !D->TryGetArrayField(TEXT("tangents"), Tang) || !D->TryGetArrayField(TEXT("uvs"), UV) || P->IsEmpty() ||
        T->IsEmpty() || P->Num() > 2000000 || T->Num() > 2000000 || UV->Num() < 1 || UV->Num() > 8)
    {
        Why = TEXT("Incomplete or excessive mesh payload");
        return false;
    }
    const int32 Corners = T->Num() * 3;
    if (N->Num() != Corners || C->Num() != Corners || Tang->Num() != Corners)
    {
        Why = TEXT("Mesh corner channel lengths disagree");
        return false;
    }
    TArray<double> V;
    for (const auto &Value : *P)
    {
        if (!Numbers(Value, 3, V))
        {
            Why = TEXT("Invalid position");
            return false;
        }
        M.Positions.Emplace(V[0], V[1], V[2]);
    }
    for (const auto &Value : *T)
    {
        if (!Numbers(Value, 3, V))
        {
            Why = TEXT("Invalid triangle");
            return false;
        }
        FIntVector Face;
        for (int32 K = 0; K < 3; ++K)
        {
            if (V[K] < 0 || V[K] >= M.Positions.Num() || FMath::FloorToDouble(V[K]) != V[K])
            {
                Why = TEXT("Triangle index out of range");
                return false;
            }
            Face[K] = int32(V[K]);
        }
        if (Face.X == Face.Y || Face.Y == Face.Z || Face.Z == Face.X)
        {
            Why = TEXT("Repeated triangle vertex");
            return false;
        }
        M.Triangles.Add(Face);
    }
    for (int32 I = 0; I < Corners; ++I)
    {
        if (!Numbers((*N)[I], 3, V))
        {
            Why = TEXT("Invalid normal");
            return false;
        }
        M.Normals.Emplace(V[0], V[1], V[2]);
        if (!Numbers((*C)[I], 4, V))
        {
            Why = TEXT("Invalid color");
            return false;
        }
        M.Colors.Emplace(V[0], V[1], V[2], V[3]);
        if (!Numbers((*Tang)[I], 4, V) || FMath::Abs(FMath::Abs(V[3]) - 1) > 1.e-5)
        {
            Why = TEXT("Invalid tangent/sign");
            return false;
        }
        M.Tangents.Emplace(V[0], V[1], V[2], V[3]);
    }
    for (const auto &Channel : *UV)
    {
        const JArray *Values = nullptr;
        if (!Channel->TryGetArray(Values) || Values->Num() != Corners)
        {
            Why = TEXT("Invalid UV channel");
            return false;
        }
        auto &Out = M.UVs.AddDefaulted_GetRef();
        for (const auto &U : *Values)
        {
            if (!Numbers(U, 2, V))
            {
                Why = TEXT("Invalid UV");
                return false;
            }
            Out.Emplace(V[0], V[1]);
        }
    }
    M.HasNamedSlots = D->HasField(TEXT("materialSlots"));
    if (M.HasNamedSlots)
    {
        const JArray *Rows = nullptr;
        if (!D->TryGetArrayField(TEXT("materialSlots"), Rows) || Rows->Num() > 65536 || !D->HasTypedField<EJson::Array>(TEXT("materialIds")))
        {
            Why = TEXT("Expected materialSlots and per-triangle materialIds arrays");
            return false;
        }
        for (const auto& Row : *Rows)
        {
            const JObject* Obj = nullptr;
            FString Name;
            int32 Id;
            if (!Row->TryGetObject(Obj) || !IntegerField(*Obj, TEXT("id"), Id, 0, 65535) ||
                !(*Obj)->TryGetStringField(TEXT("name"), Name) || M.SlotNames.Contains(Id))
            {
                Why = TEXT("Invalid or duplicate local material slot");
                return false;
            }
            M.SlotNames.Add(Id, MoveTemp(Name));
        }
    }
    const JArray *Materials = nullptr;
    if (D->TryGetArrayField(TEXT("materialIds"), Materials))
    {
        if (Materials->Num() != T->Num())
        {
            Why = TEXT("materialIds must match triangle count");
            return false;
        }
        for (const auto &X : *Materials)
        {
            double Id;
            if (!X->TryGetNumber(Id) || !FMath::IsFinite(Id) || Id < 0 || Id > (M.HasNamedSlots ? 65535 : 4095) || FMath::FloorToDouble(Id) != Id)
            {
                Why = TEXT("Invalid material slot index");
                return false;
            }
            M.MaterialIds.Add(int32(Id));
        }
    }
    if (Ref)
    {
        if (!D->TryGetArrayField(TEXT("weights"), W) || W->Num() != M.Positions.Num())
        {
            Why = TEXT("Skin weights required for every source vertex");
            return false;
        }
        for (const auto &Value : *W)
        {
            const JArray *Influences = nullptr;
            if (!Value->TryGetArray(Influences) || Influences->IsEmpty() || Influences->Num() > 12)
            {
                Why = TEXT("Invalid skin influences");
                return false;
            }
            auto &Weights = M.Weights.AddDefaulted_GetRef();
            double Sum = 0;
            TSet<int32> Seen;
            for (const auto &Influence : *Influences)
            {
                const JObject *O = nullptr;
                FString Bone;
                double Weight;
                if (!Influence->TryGetObject(O) || !(*O)->TryGetStringField(TEXT("bone"), Bone) ||
                    !(*O)->TryGetNumberField(TEXT("weight"), Weight) || !FMath::IsFinite(Weight) || Weight <= 0 ||
                    Weight > 1)
                {
                    Why = TEXT("Invalid skin weight");
                    return false;
                }
                int32 Index = Ref->FindBoneIndex(FName(*Bone));
                if (Index < 0 || Seen.Contains(Index))
                {
                    Why = TEXT("Unknown or duplicate skin bone: ") + Bone;
                    return false;
                }
                Seen.Add(Index);
                Sum += Weight;
                Weights.Emplace(Index, float(Weight));
            }
            if (FMath::Abs(Sum - 1) > 1.e-3)
            {
                Why = TEXT("Skin weights must sum to one");
                return false;
            }
        }
    }
    return true;
}

bool PartOwners(const FMeshDescription &M, TMap<FVertexID, int32> &Owner, TArray<FString> &Names, FString &Why)
{
    Owner.Reset();
    Names.Reset();
    FSkeletalMeshConstAttributes A(M);
    const auto Stable = M.VertexAttributes().GetAttributesRef<FName>(PartNameAttribute);
    const auto Legacy = M.VertexAttributes().GetAttributesRef<int32>(OwnerAttribute);
    TArray<FName> LegacyNames;
    TMap<FVertexID, FName> RangeNames;
    TSet<FName> Unique;
    bool LegacyNamesValid = A.HasSourceGeometryParts();
    if (A.HasSourceGeometryParts())
    {
        const auto PartNames = A.GetSourceGeometryPartNames();
        const auto Ranges = A.GetSourceGeometryPartVertexOffsetAndCounts();
        for (auto P : A.SourceGeometryParts().GetElementIDs())
        {
            const FName Name = PartNames[P];
            LegacyNamesValid &= !Name.IsNone() && !Unique.Contains(Name);
            Unique.Add(Name);
            LegacyNames.Add(Name);
            // Range ownership is a compatibility route for untouched imported
            // meshes only. Never reinterpret a new vertex's empty persistent ID
            // by its current (possibly compacted or reordered) numeric index.
            if (!Stable.IsValid() && !Legacy.IsValid())
            {
                const auto Range = Ranges[P];
                if (Range.Num() != 2 || Range[0] < 0 || Range[1] < 0 || int64(Range[0]) + Range[1] > MAX_int32)
                {
                    Why = TEXT("Invalid legacy source part range");
                    return false;
                }
                for (int32 I = 0; I < Range[1]; ++I)
                {
                    const FVertexID V(Range[0] + I);
                    if (!M.IsVertexValid(V) || RangeNames.Contains(V))
                    {
                        Why = TEXT("Stale or overlapping legacy source part range");
                        return false;
                    }
                    RangeNames.Add(V, Name);
                }
            }
        }
    }

    TArray<FVertexID> Vertices;
    TArray<int32> Parents;
    TArray<FName> Known;
    TMap<FVertexID, int32> Dense;
    for (const auto V : M.Vertices().GetElementIDs())
    {
        const int32 Index = Vertices.Add(V);
        Dense.Add(V, Index);
        Parents.Add(Index);
        FName Name = Stable.IsValid() ? Stable[V] : NAME_None;
        if (Name.IsNone() && Legacy.IsValid() && Legacy[V] != INDEX_NONE)
        {
            if (!LegacyNamesValid || !LegacyNames.IsValidIndex(Legacy[V]))
            {
                Why = TEXT("Legacy vertex part identity is invalid");
                return false;
            }
            Name = LegacyNames[Legacy[V]];
        }
        if (Name.IsNone() && !Stable.IsValid() && !Legacy.IsValid())
        {
            if (!LegacyNamesValid)
            {
                Why = TEXT("Source part identity metadata is missing or ambiguous");
                return false;
            }
            Name = RangeNames.FindRef(V);
        }
        Known.Add(Name);
    }
    auto FindRoot = [&](int32 I)
    {
        while (Parents[I] != I)
        {
            Parents[I] = Parents[Parents[I]];
            I = Parents[I];
        }
        return I;
    };
    for (const auto T : M.Triangles().GetElementIDs())
    {
        const auto Vs = M.GetTriangleVertices(T);
        const int32 First = FindRoot(Dense[Vs[0]]);
        for (int32 C = 1; C < 3; ++C)
            Parents[FindRoot(Dense[Vs[C]])] = First;
    }
    TMap<int32, FName> ComponentNames;
    for (int32 I = 0; I < Vertices.Num(); ++I)
    {
        if (Known[I].IsNone())
            continue;
        const int32 Component = FindRoot(I);
        if (const auto *Existing = ComponentNames.Find(Component))
        {
            if (*Existing != Known[I])
            {
                Why = TEXT("Connected geometry contains more than one part identity");
                return false;
            }
        }
        else
            ComponentNames.Add(Component, Known[I]);
    }
    // Retain source-part display order when it is available, but not its ranges.
    TSet<FName> Used;
    for (const auto &Pair : ComponentNames)
        Used.Add(Pair.Value);
    for (const auto Name : LegacyNames)
        if (Used.Contains(Name))
            Names.AddUnique(Name.ToString());
    for (int32 I = 0; I < Vertices.Num(); ++I)
    {
        const auto *Name = ComponentNames.Find(FindRoot(I));
        if (!Name)
        {
            Why = TEXT("Connected geometry has no uniquely known part identity");
            return false;
        }
        const int32 Part = Names.AddUnique(Name->ToString());
        Owner.Add(Vertices[I], Part);
    }
    return true;
}
JObject ReadMesh(const FMeshDescription &M, const TSet<FVertexID> *Selected, const FReferenceSkeleton *Ref,
                 const FWyMeshSlotView& SlotView, const FWyLocalSlots* Scope, const FVehicle& Vehicle)
{
    FWyReadSlotScope Materials(SlotView, Scope);
    const auto LocalIds = M.TriangleAttributes().GetAttributesRef<int32>(WyLocalFaceMaterialID);
    FStaticMeshConstAttributes A(M);
    JArray Positions, Triangles, Normals, Colors, Tangents, Weights;
    TArray<JArray> UVs;
    UVs.SetNum(A.GetVertexInstanceUVs().GetNumChannels());
    // A shared skeletal LOD has one global UV channel count, while each Max
    // part may have fewer. Keep the original count only for zero-filled padding.
    const auto OriginalUVCounts = M.VertexAttributes().GetAttributesRef<int32>(UVCountAttribute);
    int32 UVCount = 1;
    bool KnownUVCount = OriginalUVCounts.IsValid();
    TMap<FVertexID, int32> Index;
    for (auto V : M.Vertices().GetElementIDs())
        if (!Selected || Selected->Contains(V))
        {
            if (KnownUVCount)
            {
                const int32 Count = OriginalUVCounts[V];
                if (Count < 1 || Count > UVs.Num())
                    KnownUVCount = false;
                else
                    UVCount = FMath::Max(UVCount, Count);
            }
            Index.Add(V, Positions.Num());
            Positions.Add(JA(Vec(FVector(A.GetVertexPositions()[V]))));
            if (Ref)
            {
                JArray Influences;
                FSkeletalMeshConstAttributes SA(M);
                for (auto W : SA.GetVertexSkinWeights().Get(V))
                {
                    auto O = MakeShared<FJsonObject>();
                    int32 B = W.GetBoneIndex();
                    O->SetStringField(TEXT("bone"),
                                      B >= 0 && B < Ref->GetNum() ? Ref->GetBoneName(B).ToString() : TEXT(""));
                    O->SetNumberField(TEXT("weight"), W.GetWeight());
                    Influences.Add(JV(O));
                }
                Weights.Add(JA(Influences));
            }
        }
    JArray MaterialIds;
    int32 TriangleIndex = 0;
    for (FScopedSlowTask Faces(M.Triangles().Num(), LOCTEXT("ReadFaces", "读取三角形与顶点属性"));
         auto T : M.Triangles().GetElementIDs())
    {
        EnterGeometryBatch(Faces, TriangleIndex++, M.Triangles().Num());
        auto V = M.GetTriangleVertices(T);
        if (!Index.Contains(V[0]) || !Index.Contains(V[1]) || !Index.Contains(V[2]))
            continue;
        Triangles.Add(JA({JN(Index[V[0]]), JN(Index[V[1]]), JN(Index[V[2]])}));
        const int32 MaterialId = Materials.ReadFace(T, M.GetTrianglePolygonGroup(T), LocalIds.IsValid() ? LocalIds[T] : INDEX_NONE);
        if (MaterialId == INDEX_NONE)
            return nullptr;
        MaterialIds.Add(JN(MaterialId));
        for (auto VI : M.GetTriangleVertexInstances(T))
        {
            Normals.Add(JA(Vec(FVector(A.GetVertexInstanceNormals()[VI]))));
            Colors.Add(JA(Vec4(A.GetVertexInstanceColors()[VI])));
            FVector3f Tangent = A.GetVertexInstanceTangents()[VI];
            Tangents.Add(JA({JN(Tangent.X), JN(Tangent.Y), JN(Tangent.Z), JN(A.GetVertexInstanceBinormalSigns()[VI])}));
            for (int32 Ch = 0; Ch < UVs.Num(); ++Ch)
            {
                FVector2f U = A.GetVertexInstanceUVs().Get(VI, Ch);
                if (U != FVector2f::ZeroVector)
                    UVCount = FMath::Max(UVCount, Ch + 1);
                UVs[Ch].Add(JA({JN(U.X), JN(U.Y)}));
            }
        }
    }
    if (KnownUVCount)
        UVs.SetNum(UVCount);
    JArray Channels;
    for (const auto &Channel : UVs)
        Channels.Add(JA(Channel));
    auto D = MakeShared<FJsonObject>();
    D->SetArrayField(TEXT("positions"), Positions);
    D->SetArrayField(TEXT("triangles"), Triangles);
    D->SetArrayField(TEXT("uvs"), Channels);
    D->SetArrayField(TEXT("normals"), Normals);
    D->SetArrayField(TEXT("colors"), Colors);
    D->SetArrayField(TEXT("tangents"), Tangents);
    D->SetArrayField(TEXT("materialIds"), MaterialIds);
    D->SetArrayField(TEXT("materialSlots"), Materials.ToJson(Vehicle.Base, Vehicle.Name));
    if (Ref)
        D->SetArrayField(TEXT("weights"), Weights);
    return D;
}

void RepairUVIndices(FMeshDescription &M)
{
    FStaticMeshAttributes A(M);
    auto UV = A.GetVertexInstanceUVs();
    M.SetNumUVChannels(UV.GetNumChannels());
    for (auto T : M.Triangles().GetElementIDs())
        for (int32 Ch = 0; Ch < UV.GetNumChannels(); ++Ch)
        {
            auto Existing = M.GetTriangleUVIndices(T, Ch);
            auto VI = M.GetTriangleVertexInstances(T);
            TArray<FUVID> Ids;
            for (int32 C = 0; C < 3; ++C)
            {
                FUVID I = Existing[C];
                if (!M.IsUVValid(I, Ch))
                {
                    I = M.CreateUV(Ch);
                    M.UVAttributes(Ch).GetAttributesRef<FVector2f>(MeshAttribute::UV::UVCoordinate)[I] =
                        UV.Get(VI[C], Ch);
                }
                Ids.Add(I);
            }
            M.SetTriangleUVIndices(T, Ids, Ch);
        }
    M.RebuildIndexers();
}
template <typename Container> void Identity(TSparseArray<int32> &Map, const Container &C)
{
    for (auto I : C.GetElementIDs())
        Map.Insert(I.GetValue(), I.GetValue());
}
bool ReplaceMesh(FMeshDescription &M, const FMeshInput &Input, const FString &Part, bool Skeletal,
                 const TArray<FName> &MaterialNames, FString &Why)
{
    M.VertexAttributes().RegisterAttribute<int32>(UVCountAttribute, 1, -1, EMeshAttributeFlags::None);
    TArray<FString> Names;
    TMap<FVertexID, int32> Owners;
    int32 Target = 0;
    if (Skeletal)
    {
        FSkeletalMeshAttributes SA(M);
        SA.Register();
        if (M.Vertices().Num() > 0 && !PartOwners(M, Owners, Names, Why))
            return false;
        Target = Names.IndexOfByKey(Part);
        if (Target == INDEX_NONE)
            Target = Names.Add(Part);
        auto OA = M.VertexAttributes().RegisterAttribute<int32>(OwnerAttribute, 1, -1, EMeshAttributeFlags::None);
        auto Stable =
            M.VertexAttributes().RegisterAttribute<FName>(PartNameAttribute, 1, NAME_None, EMeshAttributeFlags::None);
        for (auto V : M.Vertices().GetElementIDs())
        {
            OA[V] = Owners[V];
            Stable[V] = FName(*Names[Owners[V]]);
        }
    }
    FStaticMeshAttributes A(M);
    A.Register();
    RepairUVIndices(M);
    TArray<FTriangleID> Remove;
    TArray<FPolygonGroupID> Groups;
    for (auto T : M.Triangles().GetElementIDs())
        if (!Skeletal || Owners[M.GetTriangleVertices(T)[0]] == Target)
        {
            Remove.Add(T);
            Groups.Add(M.GetTrianglePolygonGroup(T));
        }
    M.DeleteTriangles(Remove);
    TArray<FVertexID> Orphans;
    for (auto V : M.Vertices().GetElementIDs())
        if ((!Skeletal || Owners.FindRef(V) == Target) && M.GetVertexVertexInstanceIDs(V).IsEmpty() &&
            M.GetVertexConnectedEdgeIDs(V).IsEmpty())
            Orphans.Add(V);
    for (auto V : Orphans)
        M.DeleteVertex(V);
    FPolygonGroupID Group = Groups.IsEmpty() ? FPolygonGroupID(INDEX_NONE) : Groups[0];
    if (Input.MaterialIds.IsEmpty() && !M.IsPolygonGroupValid(Group))
    {
        Group = M.CreatePolygonGroup();
        A.GetPolygonGroupMaterialSlotNames()[Group] = FName(TEXT("DefaultMaterial"));
    }
    auto UV = A.GetVertexInstanceUVs();
    UV.SetNumChannels(FMath::Max(UV.GetNumChannels(), Input.UVs.Num()));
    M.SetNumUVChannels(UV.GetNumChannels());
    TArray<FVertexID> NewVertices;
    auto Pos = A.GetVertexPositions();
    for (int32 I = 0; I < Input.Positions.Num(); ++I)
    {
        auto V = M.CreateVertex();
        M.VertexAttributes().GetAttributesRef<int32>(UVCountAttribute)[V] = Input.UVs.Num();
        Pos[V] = Input.Positions[I];
        NewVertices.Add(V);
        if (Skeletal)
        {
            M.VertexAttributes().GetAttributesRef<int32>(OwnerAttribute)[V] = Target;
            M.VertexAttributes().GetAttributesRef<FName>(PartNameAttribute)[V] = FName(*Part);
            FSkeletalMeshAttributes(M).GetVertexSkinWeights().Set(V, Input.Weights[I]);
        }
    }
    auto OriginalIds = M.TriangleAttributes().RegisterAttribute<int32>(WyLocalFaceMaterialID, 1, INDEX_NONE, EMeshAttributeFlags::None);
    TMap<FName, FPolygonGroupID> GroupsBySlot;
    for (auto G : M.PolygonGroups().GetElementIDs())
        GroupsBySlot.Add(A.GetPolygonGroupMaterialSlotNames()[G], G);
    FScopedSlowTask Faces(Input.Triangles.Num(), LOCTEXT("WriteFaces", "写入三角形与顶点属性"));
    for (int32 I = 0; I < Input.Triangles.Num(); ++I)
    {
        EnterGeometryBatch(Faces, I, Input.Triangles.Num());
        TArray<FVertexInstanceID> Corners;
        for (int32 C = 0; C < 3; ++C)
        {
            int32 K = I * 3 + C;
            auto VI = M.CreateVertexInstance(NewVertices[Input.Triangles[I][C]]);
            A.GetVertexInstanceNormals()[VI] = Input.Normals[K];
            A.GetVertexInstanceColors()[VI] = Input.Colors[K];
            const auto &T = Input.Tangents[K];
            A.GetVertexInstanceTangents()[VI] = FVector3f(T.X, T.Y, T.Z);
            A.GetVertexInstanceBinormalSigns()[VI] = T.W;
            for (int32 Ch = 0; Ch < UV.GetNumChannels(); ++Ch)
                UV.Set(VI, Ch, Input.UVs.IsValidIndex(Ch) ? Input.UVs[Ch][K] : FVector2f::ZeroVector);
            Corners.Add(VI);
        }
        FPolygonGroupID FaceGroup =
            Groups.Num() == Input.Triangles.Num() && M.IsPolygonGroupValid(Groups[I]) ? Groups[I] : Group;
        if (Input.MaterialIds.IsValidIndex(I))
        {
            const FName Name = MaterialNames[Input.MaterialIds[I]];
            if (const auto* Existing = GroupsBySlot.Find(Name))
                FaceGroup = *Existing;
            else
            {
                FaceGroup = M.CreatePolygonGroup();
                A.GetPolygonGroupMaterialSlotNames()[FaceGroup] = Name;
                GroupsBySlot.Add(Name, FaceGroup);
            }
        }
        auto T = M.CreateTriangle(FaceGroup, Corners);
        OriginalIds[T] = Input.LocalFaceIds.IsValidIndex(I) ? Input.LocalFaceIds[I] : INDEX_NONE;
        for (int32 Ch = 0; Ch < UV.GetNumChannels(); ++Ch)
        {
            TArray<FUVID> Ids;
            for (auto VI : Corners)
            {
                auto U = M.CreateUV(Ch);
                M.UVAttributes(Ch).GetAttributesRef<FVector2f>(MeshAttribute::UV::UVCoordinate)[U] = UV.Get(VI, Ch);
                Ids.Add(U);
            }
            M.SetTriangleUVIndices(T, Ids, Ch);
        }
    }
    if (Skeletal)
    {
        auto OA = M.VertexAttributes().GetAttributesRef<int32>(OwnerAttribute);
        TArray<FVertexID> Sorted;
        for (auto V : M.Vertices().GetElementIDs())
            Sorted.Add(V);
        Sorted.Sort([&](auto X, auto Y) { return OA[X] == OA[Y] ? X.GetValue() < Y.GetValue() : OA[X] < OA[Y]; });
        FElementIDRemappings Remap;
        for (int32 I = 0; I < Sorted.Num(); ++I)
            Remap.NewVertexIndexLookup.Insert(Sorted[I].GetValue(), I);
        Identity(Remap.NewVertexInstanceIndexLookup, M.VertexInstances());
        Identity(Remap.NewEdgeIndexLookup, M.Edges());
        Identity(Remap.NewTriangleIndexLookup, M.Triangles());
        Identity(Remap.NewPolygonIndexLookup, M.Polygons());
        Identity(Remap.NewPolygonGroupIndexLookup, M.PolygonGroups());
        M.Remap(Remap);
        FSkeletalMeshAttributes SA(M);
        SA.RegisterSourceGeometryPartsAttributes();
        TArray<FSourceGeometryPartID> Old;
        for (auto P : SA.SourceGeometryParts().GetElementIDs())
            Old.Add(P);
        for (auto P : Old)
            SA.DeleteSourceGeometryPart(P);
        OA = M.VertexAttributes().GetAttributesRef<int32>(OwnerAttribute);
        for (int32 I = 0; I < Names.Num(); ++I)
        {
            int32 Offset = 0, Count = 0;
            for (auto V : M.Vertices().GetElementIDs())
                if (OA[V] == I)
                {
                    if (!Count)
                        Offset = V.GetValue();
                    ++Count;
                }
            auto P = SA.CreateSourceGeometryPart();
            SA.GetSourceGeometryPartNames()[P] = FName(*Names[I]);
            SA.GetSourceGeometryPartVertexOffsetAndCounts().Set(P, {Offset, Count});
        }
    }
    TArray<FPolygonGroupID> EmptyGroups;
    for (auto G : M.PolygonGroups().GetElementIDs())
        if (!M.GetNumPolygonGroupTriangles(G))
            EmptyGroups.Add(G);
    for (auto G : EmptyGroups)
        M.DeletePolygonGroup(G);
    FElementIDRemappings Compact;
    M.Compact(Compact);
    M.RebuildIndexers();
    return true;
}
bool PhysicsReference(UPhysicsAsset *Physics, const FVehicle &V, FReferenceSkeleton &Ref)
{
    if (auto *S = Cast<USkeletalMesh>(FindAsset(V.Skeletal())))
    {
        Ref = S->GetRefSkeleton();
        return true;
    }
    if (auto *S = Cast<USkeleton>(FindAsset(V.SkeletonPath)))
    {
        Ref = S->GetReferenceSkeleton();
        return true;
    }
    if (auto *S = Physics->GetPreviewMesh())
    {
        Ref = S->GetRefSkeleton();
        return true;
    }
    return false;
}

class FVehiclePhysicsLinksChange final : public FCommandChange
{
    TObjectPtr<UPhysicsAsset> Physics;
    TSoftObjectPtr<USkeletalMesh> BeforePreview, AfterPreview;
    bool LinkMesh, LinkPreview, MeshWasDirty, PhysicsWasDirty;

  public:
    FVehiclePhysicsLinksChange(USkeletalMesh *Mesh, UPhysicsAsset *Asset, bool SetMesh, bool SetPreview)
        : Physics(Asset), BeforePreview(Asset->PreviewSkeletalMesh), AfterPreview(Mesh),
          LinkMesh(SetMesh), LinkPreview(SetPreview),
          MeshWasDirty(Mesh->GetPackage()->IsDirty()), PhysicsWasDirty(Asset->GetPackage()->IsDirty()) {}

    void Apply(UObject *O) override
    {
        if (LinkMesh)
        {
            CastChecked<USkeletalMesh>(O)->SetPhysicsAsset(Physics);
            O->MarkPackageDirty();
        }
        if (LinkPreview)
        {
            Physics->PreviewSkeletalMesh = AfterPreview;
            Physics->MarkPackageDirty();
        }
    }
    void Revert(UObject *O) override
    {
        if (LinkMesh)
        {
            CastChecked<USkeletalMesh>(O)->SetPhysicsAsset(nullptr);
            O->GetPackage()->SetDirtyFlag(MeshWasDirty);
        }
        if (LinkPreview)
        {
            // A prior cube or unresolved soft path cannot pass SetPreviewMesh's bone validation.
            Physics->PreviewSkeletalMesh = BeforePreview;
            Physics->GetPackage()->SetDirtyFlag(PhysicsWasDirty);
        }
    }
    void AddReferencedObjects(FReferenceCollector &Collector) override { Collector.AddReferencedObject(Physics); }
    FString ToString() const override { return TEXT("Link vehicle physics asset and skeletal mesh"); }
};

void LinkVehiclePhysics(USkeletalMesh *Mesh, UPhysicsAsset *Physics, const TArray<UObject *> &Created)
{
    if (!Mesh || !Physics || VehicleBodyBone(Mesh->GetRefSkeleton()) == INDEX_NONE ||
        Physics->FindBodyIndex(TEXT("Body")) == INDEX_NONE)
        return;
    for (const auto &Body : Physics->SkeletalBodySetups)
        if (!Body || Mesh->GetRefSkeleton().FindBoneIndex(Body->BoneName) == INDEX_NONE)
            return;

    // PhAT assigns this fallback when opening a legacy asset without a preview.
    static const FSoftObjectPath SkeletalCube(TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    const bool LinkMesh = !Mesh->GetPhysicsAsset();
    const bool LinkPreview = Physics->PreviewSkeletalMesh.IsNull() || Physics->PreviewSkeletalMesh.ToSoftObjectPath() == SkeletalCube;

    // New objects are retained whole by FCreatedAssetChange, including their links.
    if (LinkMesh && Created.Contains(Mesh))
        Mesh->SetPhysicsAsset(Physics);
    if (LinkPreview && Created.Contains(Physics))
        Physics->SetPreviewMesh(Mesh, false);
    const bool UpdateMesh = LinkMesh && !Created.Contains(Mesh);
    const bool UpdatePreview = LinkPreview && !Created.Contains(Physics);
    if (UpdateMesh || UpdatePreview)
    {
        auto Change = MakeUnique<FVehiclePhysicsLinksChange>(Mesh, Physics, UpdateMesh, UpdatePreview);
        Change->Apply(Mesh);
        if (GUndo)
            GUndo->StoreUndo(Mesh, MoveTemp(Change));
    }
}
bool ClosedConvexSurface(const TArray<FVector> &Points, const TArray<FIntVector> &Faces)
{
    if (Points.Num() < 4 || Faces.Num() < 4)
        return false;
    FVector Center = FVector::ZeroVector;
    FBox Bounds(ForceInit);
    for (const auto &P : Points)
    {
        Center += P;
        Bounds += P;
    }
    Center /= Points.Num();
    const double Tolerance = FMath::Max(1.e-6, Bounds.GetSize().Size() * 2.e-6);
    TMap<FIntPoint, FIntPoint> Edges;
    TSet<int32> Used;
    double Volume6 = 0;
    for (const auto &F : Faces)
    {
        for (int32 I = 0; I < 3; ++I)
            if (!Points.IsValidIndex(F[I]))
                return false;
        const FVector A = Points[F.X], B = Points[F.Y], C = Points[F.Z];
        FVector N;
        Volume6 += FVector::DotProduct(A - Center, FVector::CrossProduct(B - Center, C - Center));
        if (!WyHullFaceNormal(A, B, C, N) || FVector::DotProduct(N, Center - A) > Tolerance)
            return false;
        for (const auto &P : Points)
            if (FVector::DotProduct(N, P - A) > Tolerance)
                return false;
        for (int32 I = 0; I < 3; ++I)
        {
            const int32 X = F[I], Y = F[(I + 1) % 3];
            Used.Add(X);
            auto &E = Edges.FindOrAdd(FIntPoint(FMath::Min(X, Y), FMath::Max(X, Y)), FIntPoint(0, 0));
            ++E.X;
            E.Y += X < Y ? 1 : -1;
        }
    }
    if (Volume6 <= UE_DOUBLE_SMALL_NUMBER || Used.Num() - Edges.Num() + Faces.Num() != 2)
        return false;
    for (const auto &E : Edges)
        if (E.Value.X != 2 || E.Value.Y != 0)
            return false;
    return true;
}
bool BuildConvexSurface(TArray<FVector> &Points, TArray<FIntVector> &Faces, FString &Why)
{
    if (ClosedConvexSurface(Points, Faces))
        return true;
    if (Points.Num() < 4)
    {
        Why = TEXT("三角面引用的顶点少于 4 个。请检查 Max 碰撞网格是否有足够的顶点和厚度。");
        return false;
    }
    UE::Geometry::TConvexHull3<double> Hull;
    Hull.SimplificationSettings.SkipAtHullDistanceAsFraction = 1.e-8;
    if (!Hull.Solve(Points.Num(), [&](int32 I) { return Points[I]; }) || Hull.GetDimension() != 3)
    {
        Why = Hull.GetDimension() < 3
                  ? TEXT("无法找到 4 个不共面的有效顶点。请检查 Max 碰撞网格的厚度；分层生成时也可调整切面高度。")
                  : TEXT("UE 未能从顶点构建凸包。请检查 Max 碰撞网格或重新构建 BodyCollider。");
        return false;
    }
    FVector Center = FVector::ZeroVector;
    for (const auto &P : Points)
        Center += P;
    Center /= Points.Num();
    Faces.Reset();
    for (const auto &F : Hull.GetTriangles())
    {
        FIntVector T(F.A, F.B, F.C);
        if (FVector::DotProduct(FVector::CrossProduct(Points[T.Y] - Points[T.X], Points[T.Z] - Points[T.X]),
                                Center - Points[T.X]) > 0)
            Swap(T.Y, T.Z);
        Faces.Add(T);
    }
    // Validate containment against every referenced input before dropping interior points.
    if (!ClosedConvexSurface(Points, Faces))
    {
        Why = TEXT("重建后的凸包未通过体积、闭合性或包含性检查。请检查 Max 碰撞网格或重新构建 BodyCollider。");
        return false;
    }
    TMap<int32, int32> Remap;
    TArray<FVector> Compact;
    for (auto &F : Faces)
        for (int32 I = 0; I < 3; ++I)
        {
            const int32 Old = F[I];
            if (!Remap.Contains(Old))
                Remap.Add(Old, Compact.Add(Points[Old]));
            F[I] = Remap[Old];
        }
    Points = MoveTemp(Compact);
    return true;
}
JObject ReadPhysics(UPhysicsAsset *Physics, const FVehicle &V)
{
    int32 BodyIndex = Physics->FindBodyIndex(TEXT("Body"));
    if (!Physics->SkeletalBodySetups.IsValidIndex(BodyIndex))
        return nullptr;
    const auto *B = Physics->SkeletalBodySetups[BodyIndex].Get();
    if (!B)
        return nullptr;
    if (B->AggGeom.GetElementCount() != B->AggGeom.ConvexElems.Num())
        return Unmatched(TEXT("physics"), TEXT("BodyCollider"), 0,
                         TEXT("Body contains non-convex primitive types; explicit conversion is required"));
    FReferenceSkeleton Ref;
    const int32 Bone = PhysicsReference(Physics, V, Ref) ? VehicleBodyBone(Ref) : INDEX_NONE;
    if (Bone == INDEX_NONE)
        return Unmatched(TEXT("physics"), TEXT("BodyCollider"), 0, TEXT("Root/Body reference transform is unavailable"));
    const FTransform Body = BoneTransform(Ref, Bone);
    JArray Shapes;
    for (const auto &Elem : B->AggGeom.ConvexElems)
    {
        TArray<FVector> Points = Elem.VertexData;
        TArray<FIntVector> Faces;
        // PhAT draws VertexData and IndexData in element space. Cooking can
        // simplify that surface even when the author has made no changes.
        const bool HasEditorVertices = !Points.IsEmpty();
        if (HasEditorVertices)
        {
            if (Elem.IndexData.Num() % 3 == 0)
                for (int32 I = 0; I < Elem.IndexData.Num(); I += 3)
                    Faces.Emplace(Elem.IndexData[I], Elem.IndexData[I + 1], Elem.IndexData[I + 2]);
        }
        else if (const auto &Convex = Elem.GetChaosConvexMesh())
        {
            // GetCookInfo already applied the element transform to these points.
            for (int32 I = 0; I < Convex->NumVertices(); ++I)
                Points.Add(FVector(Convex->GetVertex(I)));
        }
        FString HullError;
        if (!BuildConvexSurface(Points, Faces, HullError))
            return Unmatched(TEXT("physics"), TEXT("BodyCollider"), 0,
                             FString::Printf(TEXT("碰撞子节点“%s”：%s"), *Elem.GetName().ToString(), *HullError));
        JArray Positions, Triangles;
        const FTransform Element = HasEditorVertices ? Elem.GetTransform() : FTransform::Identity;
        for (const auto &P : Points)
            Positions.Add(JA(Vec(Body.TransformPosition(Element.TransformPosition(P)))));
        const bool Reflected = Body.GetDeterminant() * Element.GetDeterminant() < 0;
        for (auto F : Faces)
        {
            if (Reflected)
                Swap(F.Y, F.Z);
            Triangles.Add(JA({JN(F.X), JN(F.Y), JN(F.Z)}));
        }
        auto Shape = MakeShared<FJsonObject>();
        Shape->SetStringField(TEXT("name"), Elem.GetName().ToString());
        Shape->SetArrayField(TEXT("positions"), Positions);
        Shape->SetArrayField(TEXT("triangles"), Triangles);
        Shapes.Add(JV(Shape));
    }
    auto Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("shapes"), Shapes);
    return Item(TEXT("physics"), TEXT("BodyCollider"), 0, Data);
}
bool IsVehicleMeshPart(const FString &Name, bool Skeletal)
{
    // Tire/Hub need the four digits. On a skeletal part the suffix is optional.
    const FString Base = WyVehiclePartBase(Name);
    const bool Numbered = Base.Len() != Name.Len();
    if (!Skeletal)
        return Numbered ? Base == TEXT("Tire") || Base == TEXT("Hub")
                        : Base == TEXT("BrakeB") || Base == TEXT("BrakeR") || Base == TEXT("BrakeF");
    static const TSet<FString> BodyParts = {
        TEXT("Body"),     TEXT("DoorModelFL"), TEXT("DoorModelFR"), TEXT("DoorWindowFL"), TEXT("DoorWindowFR"),
        TEXT("Engine"),   TEXT("FrontKit"),    TEXT("RearKit"),     TEXT("Hood"),         TEXT("Interior"),
        TEXT("MirrorFL"), TEXT("MirrorFR"),    TEXT("SeatFL"),      TEXT("SeatFR"),       TEXT("Sidepedals"),
        TEXT("Spoiler"),  TEXT("SteerWheel")};
    return BodyParts.Contains(Base);
}
struct FStaticMeshMatch
{
    UStaticMesh *Mesh = nullptr;
    int32 ConflictLods = 0;
};
FStaticMeshMatch MatchStaticMesh(const FVehicle &V, const FString &Part)
{
    // A redirector left behind by an editor rename is not a second mesh.
    const auto MeshAt = [](const FString &Package) -> UStaticMesh *
    {
        auto *Mesh = Cast<UStaticMesh>(FindAsset(Package));
        return Mesh && Mesh->GetOutermost()->GetFName() == FName(*Package) ? Mesh : nullptr;
    };
    auto *Current = MeshAt(V.StaticMesh(Part));
    auto *Legacy = MeshAt(V.Base / TEXT("StaticMesh") / Part);
    if (Current && Legacy)
    {
        TArray<UStaticMesh *> Wait = {Current, Legacy};
        FStaticMeshCompilingManager::Get().FinishCompilation(Wait);
        return {nullptr, FMath::Max3(1, Current->GetNumSourceModels(), Legacy->GetNumSourceModels())};
    }
    return {Current ? Current : Legacy, 0};
}
FString StaticMeshConflict(const FString &Part)
{
    return FString::Printf(TEXT("同一车辆目录内存在 SM_%s 和 %s 两个静态网格，无法唯一匹配 Max 部件。"), *Part, *Part);
}
JObject Read(const FVehicle &V)
{
    FScopedSlowTask Progress(5.f, LOCTEXT("ReadAssets", "读取车辆资源"));
    Progress.Visibility = ESlowTaskVisibility::ForceVisible;
    Progress.EnterProgressFrame(1.f, LOCTEXT("ReadSkeleton", "读取骨骼"));
    JArray Items;
    if (auto *Skeleton = Cast<USkeleton>(FindAsset(V.SkeletonPath)))
    {
        auto Root = Item(TEXT("skeleton"), TEXT("Root"), 0, ReadBones(Skeleton->GetReferenceSkeleton()));
        Root->SetStringField(TEXT("asset"), Skeleton->GetOutermost()->GetName());
        Items.Add(JV(Root));
    }
    Progress.EnterProgressFrame(2.f, LOCTEXT("ReadSkinned", "读取骨骼网格体"));
    if (auto *Mesh = Cast<USkeletalMesh>(FindAsset(V.Skeletal())))
    {
        TArray<USkinnedAsset *> Wait = {Mesh};
        FSkinnedAssetCompilingManager::Get().FinishCompilation(Wait);
        const auto Scopes = WyLoadSlotScopes(Mesh);
        FScopedSlowTask Lods(Mesh->GetNumSourceModels());
        for (int32 L = 0; L < Mesh->GetNumSourceModels(); ++L)
        {
            Lods.EnterProgressFrame(1.f, FText::Format(LOCTEXT("ReadSkeletalLOD", "读取车身 LOD{0}"), L));
            auto *M = Mesh->GetMeshDescription(L);
            FString Why;
            TMap<FVertexID, int32> Owners;
            TArray<FString> Names;
            if (!M || !PartOwners(*M, Owners, Names, Why))
            {
                Items.Add(JV(Unmatched(TEXT("skeletalPart"), FString::Printf(TEXT("LOD%d_Unmapped"), L), L,
                                       M ? Why : TEXT("LOD has no source MeshDescription"))));
                continue;
            }
            const auto Materials = WyMeshSlotView(Mesh, L, *M);
            FScopedSlowTask Parts(Names.Num());
            for (int32 P = 0; P < Names.Num(); ++P)
            {
                FString Name = Names[P];
                Parts.EnterProgressFrame(1.f, FText::FromString(Name));
                const FString Prefix = FString::Printf(TEXT("LOD%d_"), L);
                if (!Name.StartsWith(Prefix))
                {
                    if (Name.StartsWith(TEXT("LOD")))
                    {
                        Items.Add(
                            JV(Unmatched(TEXT("skeletalPart"), Name, L, TEXT("Source part name disagrees with LOD"))));
                        continue;
                    }
                    Name = Prefix + Name;
                }
                if (!IsVehicleMeshPart(Name.Mid(Prefix.Len()), true))
                {
                    Items.Add(JV(
                        Unmatched(TEXT("skeletalPart"), Name, L, TEXT("No vehicle part rule for this source name"))));
                    continue;
                }
                TSet<FVertexID> Selected;
                for (const auto &Pair : Owners)
                    if (Pair.Value == P)
                        Selected.Add(Pair.Key);
                const auto Data = ReadMesh(*M, &Selected, &Mesh->GetRefSkeleton(), Materials, Scopes.Find(Name), V);
                auto Out = Data ? Item(TEXT("skeletalPart"), Name, L, Data) :
                    Unmatched(TEXT("skeletalPart"), Name, L, TEXT("骨骼网格分段的材质覆盖无法唯一映射到源三角形。"));
                Out->SetStringField(TEXT("asset"), V.Skeletal());
                Items.Add(JV(Out));
            }
        }
    }
    Progress.EnterProgressFrame(1.f, LOCTEXT("ReadStatic", "读取静态网格体"));
    TArray<FAssetData> Assets;
    FAssetRegistryModule::GetRegistry().GetAssetsByPath(FName(*(V.Base / TEXT("StaticMesh"))), Assets, false);
    TSet<FString> KnownParts;
    for (const auto &Asset : Assets)
    {
        FString Part = WyStaticMeshPartName(Asset.AssetName.ToString());
        if (IsVehicleMeshPart(Part, false))
            KnownParts.Add(MoveTemp(Part));
    }
    TArray<FString> Parts = KnownParts.Array();
    Parts.Sort();
    for (FScopedSlowTask StaticMeshes(Parts.Num()); const FString &Part : Parts)
    {
        StaticMeshes.EnterProgressFrame(1.f, FText::FromString(Part));
        const FStaticMeshMatch Match = MatchStaticMesh(V, Part);
        if (Match.ConflictLods)
        {
            const FString Why = StaticMeshConflict(Part);
            for (int32 L = 0; L < Match.ConflictLods; ++L)
            {
                auto Out = Unmatched(TEXT("staticMesh"), FString::Printf(TEXT("LOD%d_%s"), L, *Part), L, Why);
                Out->SetBoolField(TEXT("assetConflict"), true);
                Items.Add(JV(Out));
            }
            continue;
        }
        UStaticMesh *Mesh = Match.Mesh;
        if (!Mesh)
            continue;
        TArray<UStaticMesh *> Wait = {Mesh};
        FStaticMeshCompilingManager::Get().FinishCompilation(Wait);
        const auto Scopes = WyLoadSlotScopes(Mesh);
        FScopedSlowTask Lods(Mesh->GetNumSourceModels());
        for (int32 L = 0; L < Mesh->GetNumSourceModels(); ++L)
        {
            const FString Name = FString::Printf(TEXT("LOD%d_%s"), L, *Part);
            Lods.EnterProgressFrame(1.f, FText::FromString(Name));
            const auto *M = Mesh->GetMeshDescription(L);
            auto Out = M ? Item(TEXT("staticMesh"), Name, L, ReadMesh(*M, nullptr, nullptr, WyMeshSlotView(Mesh, L, *M), Scopes.Find(Name), V))
                         : Unmatched(TEXT("staticMesh"), Name, L, TEXT("LOD has no source MeshDescription"));
            Out->SetStringField(TEXT("asset"), Mesh->GetOutermost()->GetName());
            Items.Add(JV(Out));
        }
    }
    Progress.EnterProgressFrame(1.f, LOCTEXT("ReadCollision", "读取车身碰撞"));
    if (auto *Physics = Cast<UPhysicsAsset>(FindAsset(V.Physics())))
    {
        auto Out = ReadPhysics(Physics, V);
        if (Out)
            Items.Add(JV(Out));
    }
    auto Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("ok"), true);
    Result->SetArrayField(TEXT("items"), Items);
    Result->SetBoolField(TEXT("empty"), Items.IsEmpty());
    JArray BlueprintPaths;
    for (bool Animation : {false, true})
        BlueprintPaths.Add(MakeShared<FJsonValueString>(TEXT("Content") + V.Blueprint(Animation).RightChop(5) + TEXT(".uasset")));
    Result->SetArrayField(TEXT("blueprintPaths"), BlueprintPaths);
    Progress.EnterProgressFrame(0.f);
    return Result;
}

struct FMeshState
{
    TArray<FMeshDescription> Lods;
    FReferenceSkeleton Ref;
    FWyMeshSlotScopes MaterialScopes;
    FMeshSectionInfoMap Sections, OriginalSections;
    TArray<FStaticMaterial> StaticMaterials;
    TArray<FSkeletalMaterial> SkeletalMaterials;
    TSet<int32> ChangedLods;
    TArray<FMeshBuildSettings> StaticBuild;
    TArray<FSkeletalMeshLODInfo> SkeletalLodInfo;
    TObjectPtr<USkeleton> Skeleton = nullptr;
    bool Dirty = false;
};
FMeshState Capture(UObject *Object)
{
    FMeshState S;
    S.Dirty = Object->GetOutermost()->IsDirty();
    S.MaterialScopes = WyLoadSlotScopes(Object);
    if (auto *M = Cast<UStaticMesh>(Object))
    {
        TArray<UStaticMesh *> Wait = {M};
        FStaticMeshCompilingManager::Get().FinishCompilation(Wait);
        S.StaticMaterials = M->GetStaticMaterials();
        S.Sections = M->GetSectionInfoMap();
        S.OriginalSections = M->GetOriginalSectionInfoMap();
        for (int32 I = 0; I < M->GetNumSourceModels(); ++I)
        {
            auto *D = M->GetMeshDescription(I);
            S.Lods.Add(D ? *D : FMeshDescription());
            S.StaticBuild.Add(M->GetSourceModel(I).BuildSettings);
        }
    }
    if (auto *M = Cast<USkeletalMesh>(Object))
    {
        TArray<USkinnedAsset *> Wait = {M};
        FSkinnedAssetCompilingManager::Get().FinishCompilation(Wait);
        S.Ref = M->GetRefSkeleton();
        S.Skeleton = M->GetSkeleton();
        S.SkeletalMaterials = M->GetMaterials();
        const int32 Count = FMath::Max(M->GetNumSourceModels(), M->GetLODNum());
        for (int32 I = 0; I < Count; ++I)
        {
            auto *D = I < M->GetNumSourceModels() ? M->GetMeshDescription(I) : nullptr;
            S.Lods.Add(D ? *D : FMeshDescription());
            S.SkeletalLodInfo.Add(M->GetLODInfo(I) ? *M->GetLODInfo(I) : FSkeletalMeshLODInfo());
        }
    }
    return S;
}
void ApplyMesh(UObject *Object, const FMeshState &S)
{
    WyStoreSlotScopes(Object, S.MaterialScopes);
    if (auto *M = Cast<UStaticMesh>(Object))
    {
        M->SetNumSourceModels(S.Lods.Num());
        M->GetStaticMaterials() = S.StaticMaterials;
        M->GetSectionInfoMap() = S.Sections;
        M->GetOriginalSectionInfoMap() = S.OriginalSections;
        for (int32 I = 0; I < S.Lods.Num(); ++I)
            if (S.ChangedLods.Contains(I))
            {
                M->GetSourceModel(I).BuildSettings = S.StaticBuild[I];
                M->CreateMeshDescription(I, FMeshDescription(S.Lods[I]));
                M->CommitMeshDescription(I);
            }
        if (!S.Lods.IsEmpty())
        {
            M->Build(true);
            TArray<UStaticMesh *> Wait = {M};
            FStaticMeshCompilingManager::Get().FinishCompilation(Wait);
        }
        M->PostEditChange();
    }
    if (auto *M = Cast<USkeletalMesh>(Object))
    {
        M->SetRefSkeleton(S.Ref);
        M->SetSkeleton(S.Skeleton);
        M->GetMaterials() = S.SkeletalMaterials;
        M->SetNumSourceModels(S.Lods.Num());
        // CommitMeshDescription updates the imported LOD's bulk-data identity.
        // New source LODs need their imported-model slot before that commit.
        auto *Imported = M->GetImportedModel();
        while (Imported->LODModels.Num() < S.Lods.Num())
            Imported->LODModels.Add(new FSkeletalMeshLODModel());
        if (Imported->LODModels.Num() > S.Lods.Num())
            Imported->LODModels.RemoveAt(S.Lods.Num(), Imported->LODModels.Num() - S.Lods.Num());
        FBox Bounds(ForceInit);
        for (int32 I = 0; I < S.Lods.Num(); ++I)
            if (S.ChangedLods.Contains(I))
            {
                *M->GetLODInfo(I) = S.SkeletalLodInfo[I];
                M->CreateMeshDescription(I, FMeshDescription(S.Lods[I]));
                M->CommitMeshDescription(I);
                if (I == 0)
                    for (auto V : S.Lods[I].Vertices().GetElementIDs())
                        Bounds += FVector(S.Lods[I].GetVertexPositions()[V]);
            }
        if (Bounds.IsValid)
            M->SetImportedBounds(FBoxSphereBounds(Bounds));
        M->CalculateInvRefMatrices();
        if (!S.Lods.IsEmpty())
        {
            M->Build();
            TArray<USkinnedAsset *> Wait = {M};
            FSkinnedAssetCompilingManager::Get().FinishCompilation(Wait);
        }
        M->PostEditChange();
        if (!S.Lods.IsEmpty())
        {
            TArray<USkinnedAsset *> Wait = {M};
            FSkinnedAssetCompilingManager::Get().FinishCompilation(Wait);
            const auto *Render = M->GetResourceForRendering();
            for (int32 L : S.ChangedLods)
                if (S.Lods.IsValidIndex(L))
                {
                    const uint32 Vertices =
                        Render && Render->LODRenderData.IsValidIndex(L) ? Render->LODRenderData[L].GetNumVertices() : 0;
                    UE_LOG(LogTemp, Display,
                           TEXT("WyVehicle bridge built %s LOD%d: source triangles=%d render vertices=%u"),
                           *M->GetPathName(), L, S.Lods[L].Triangles().Num(), Vertices);
                }
        }
    }
    Object->GetOutermost()->SetDirtyFlag(S.Dirty);
}
class FMeshChange final : public FCommandChange
{
    FMeshState Before, After;

  public:
    FMeshChange(FMeshState A, FMeshState B) : Before(MoveTemp(A)), After(MoveTemp(B))
    {
        Before.ChangedLods = After.ChangedLods;
    }
    void Apply(UObject *O) override
    {
        ApplyMesh(O, After);
    }
    void Revert(UObject *O) override
    {
        ApplyMesh(O, Before);
    }
    void AddReferencedObjects(FReferenceCollector& Collector) override
    {
        for (auto* State : {&Before, &After})
        {
            Collector.AddReferencedObject(State->Skeleton);
            for (auto& Slot : State->StaticMaterials)
                Collector.AddReferencedObject(Slot.MaterialInterface);
            for (auto& Slot : State->SkeletalMaterials)
                Collector.AddReferencedObject(Slot.MaterialInterface);
        }
    }
    FString ToString() const override
    {
        return TEXT("WyVehicle mesh scope synchronization");
    }
};
bool ApplySkeleton(USkeleton *Skeleton, const FReferenceSkeleton &Ref)
{
    auto *Temporary = NewObject<USkeletalMesh>(GetTransientPackage());
    Temporary->SetRefSkeleton(Ref);
    TMap<FName, EBoneTranslationRetargetingMode::Type> Retargeting;
    const auto &Old = Skeleton->GetReferenceSkeleton();
    for (int32 I = 0; I < Old.GetNum(); ++I)
        Retargeting.Add(Old.GetBoneName(I), Skeleton->GetBoneTranslationRetargetingMode(I));
    if (!Skeleton->RecreateBoneTree(Temporary))
        return false;
    const auto &Updated = Skeleton->GetReferenceSkeleton();
    for (int32 I = 0; I < Updated.GetNum(); ++I)
        if (auto *Mode = Retargeting.Find(Updated.GetBoneName(I)))
            Skeleton->SetBoneTranslationRetargetingMode(I, *Mode, false);
    Skeleton->PostEditChange();
    Skeleton->MarkPackageDirty();
    return true;
}
class FSkeletonChange final : public FCommandChange
{
    FReferenceSkeleton Before, After;
    bool BeforeDirty;

  public:
    FSkeletonChange(const FReferenceSkeleton &A, const FReferenceSkeleton &B, bool Dirty)
        : Before(A), After(B), BeforeDirty(Dirty)
    {
    }
    void Apply(UObject *O) override
    {
        ApplySkeleton(CastChecked<USkeleton>(O), After);
    }
    void Revert(UObject *O) override
    {
        ApplySkeleton(CastChecked<USkeleton>(O), Before);
        O->GetOutermost()->SetDirtyFlag(BeforeDirty);
    }
    FString ToString() const override
    {
        return TEXT("WyVehicle whole skeleton synchronization");
    }
};
void ApplyPhysics(UPhysicsAsset *Asset, const FKAggregateGeom &Shapes, bool Exists, bool Dirty)
{
    int32 Index = Asset->FindBodyIndex(TEXT("Body"));
    if (!Exists)
    {
        if (Asset->SkeletalBodySetups.IsValidIndex(Index))
            Asset->SkeletalBodySetups.RemoveAt(Index);
    }
    else
    {
        USkeletalBodySetup *Body =
            Asset->SkeletalBodySetups.IsValidIndex(Index) ? Asset->SkeletalBodySetups[Index].Get() : nullptr;
        if (!Body)
        {
            Body = NewObject<USkeletalBodySetup>(Asset, NAME_None, RF_Transactional);
            Body->BoneName = TEXT("Body");
            Body->PhysicsType = PhysType_Simulated;
            Body->CollisionTraceFlag = CTF_UseSimpleAsComplex;
            Asset->SkeletalBodySetups.Add(Body);
        }
        Body->AggGeom = Shapes;
        Body->InvalidatePhysicsData();
        Body->CreatePhysicsMeshes();
    }
    Asset->UpdateBodySetupIndexMap();
    Asset->UpdateBoundsBodiesArray();
    Asset->PostEditChange();
    Asset->GetOutermost()->SetDirtyFlag(Dirty);
}
class FPhysicsChange final : public FCommandChange
{
    FKAggregateGeom Before, After;
    bool BeforeExists, BeforeDirty;

  public:
    FPhysicsChange(const FKAggregateGeom &A, const FKAggregateGeom &B, bool Exists, bool Dirty)
        : Before(A), After(B), BeforeExists(Exists), BeforeDirty(Dirty)
    {
    }
    void Apply(UObject *O) override
    {
        ApplyPhysics(CastChecked<UPhysicsAsset>(O), After, true, true);
    }
    void Revert(UObject *O) override
    {
        ApplyPhysics(CastChecked<UPhysicsAsset>(O), Before, BeforeExists, BeforeDirty);
    }
    FString ToString() const override
    {
        return TEXT("WyVehicle Body collision group synchronization");
    }
};
class FCreatedAssetChange final : public FCommandChange
{
    FString PackageName, Name;
    bool OldDirty;

  public:
    explicit FCreatedAssetChange(UObject *O, bool Dirty)
        : PackageName(O->GetOutermost()->GetName()), Name(O->GetName()), OldDirty(Dirty)
    {
    }
    void Apply(UObject *O) override
    {
        O->Rename(*Name, CreatePackage(*PackageName), REN_DontCreateRedirectors | REN_NonTransactional);
        O->SetFlags(RF_Public | RF_Standalone);
        FAssetRegistryModule::AssetCreated(O);
        O->MarkPackageDirty();
    }
    void Revert(UObject *O) override
    {
        auto *Package = O->GetOutermost();
        FAssetRegistryModule::AssetDeleted(O);
        O->ClearFlags(RF_Public | RF_Standalone);
        const FName TemporaryName = MakeUniqueObjectName(GetTransientPackage(), O->GetClass(), O->GetFName());
        O->Rename(*TemporaryName.ToString(), GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
        Package->SetDirtyFlag(OldDirty);
    }
    FString ToString() const override
    {
        return TEXT("WyVehicle asset creation");
    }
};

struct FPlan
{
    FString Path, Kind;
    UObject *Object = nullptr;
    FMeshState Mesh;
    TMap<int32, FName> PendingMaterials;
    TSet<int32> BakedSectionLods;
    FKAggregateGeom Shapes;
    bool New = false;
};
template<typename MaterialType>
bool PrepareSlots(FPlan& P, FMeshInput& Input, TArray<MaterialType>& Materials, const FString& Scope,
                  const FVehicle& Vehicle, FWyVehicleMaterialPlan& Plan, TArray<FName>& ImportedNames, FString& Why)
{
    TMap<FName, int32> Named, Empty;
    for (int32 I = 0; I < Materials.Num(); ++I)
    {
        const auto& M = Materials[I];
        ImportedNames.Add(M.ImportedMaterialSlotName.IsNone() ? M.MaterialSlotName : M.ImportedMaterialSlotName);
        const FName Display = M.MaterialSlotName;
        if (const FName* Binding = P.PendingMaterials.Find(I))
            Named.FindOrAdd(*Binding, I);
        else if (M.MaterialInterface)
        {
            const FString Package = M.MaterialInterface->GetOutermost()->GetName();
            const FString MaxName = WyMaxMaterialSlotName(Display, Package, Vehicle.Base, Vehicle.Name);
            const FString Expected = Vehicle.Base / TEXT("MaterialInstance") / WyVehicleMaterialAssetName(Vehicle.Name, MaxName);
            if (Package.Equals(Expected, ESearchCase::IgnoreCase))
                Named.FindOrAdd(FName(*MaxName), I);
        }
        else
            Empty.FindOrAdd(Display, I);
    }
    auto Append = [&](FName Display)
    {
        FName Imported = Display;
        for (int32 Suffix = 1; ImportedNames.Contains(Imported); ++Suffix)
            Imported = FName(*FString::Printf(TEXT("WyVehicleSlot_%d_%d"), Materials.Num(), Suffix));
        auto& Slot = Materials.AddDefaulted_GetRef();
        Slot.MaterialSlotName = Display;
        Slot.ImportedMaterialSlotName = Imported;
        return ImportedNames.Add(Imported);
    };
    if (!Input.HasNamedSlots)
    {
        int32 Required = 1;
        for (int32 Id : Input.MaterialIds)
            Required = FMath::Max(Required, Id + 1);
        while (ImportedNames.Num() < Required)
            Append(FName(*FString::Printf(TEXT("Material_%d"), ImportedNames.Num())));
        P.Mesh.MaterialScopes.Remove(Scope);
        return true;
    }

    auto& ScopeSlots = P.Mesh.MaterialScopes.FindOrAdd(Scope);
    ScopeSlots.Reset();
    TSet<int32> AllIds(Input.MaterialIds);
    for (const auto& Pair : Input.SlotNames) AllIds.Add(Pair.Key);
    auto Ids = AllIds.Array();
    Ids.Sort();
    TMap<int32, int32> LocalToGlobal;
    for (int32 Id : Ids)
    {
        const FString* Name = Input.SlotNames.Find(Id);
        int32 Index;
        if (Name && !Name->IsEmpty())
        {
            if (!WyPlanVehicleMaterial(Vehicle.Base, Vehicle.Name, *Name, Plan, Why))
                return false;
            const FName Desired(**Name);
            if (const int32* Found = Named.Find(Desired))
                Index = *Found;
            else
            {
                Index = Append(Desired);
                Named.Add(Desired, Index);
            }
            P.PendingMaterials.Add(Index, Desired);
        }
        else if (const int32* Found = Empty.Find(WyUnassignedMaterialSlot))
            Index = *Found;
        else
        {
            Index = Append(WyUnassignedMaterialSlot);
            Empty.Add(WyUnassignedMaterialSlot, Index);
        }
        if (Index > MAX_uint16)
        {
            Why = TEXT("UE mesh exceeds 65536 material slots");
            return false;
        }
        ScopeSlots.Add(Id, ImportedNames[Index]);
        LocalToGlobal.Add(Id, Index);
    }
    Input.LocalFaceIds = Input.MaterialIds;
    for (int32& Id : Input.MaterialIds)
        Id = LocalToGlobal.FindChecked(Id);
    return true;
}

// FLODUtilities::AdjustImportDataFaceMaterialIndex only translates imported slot
// names for LOD0. Give higher skeletal LODs the actual asset slot index in source.
bool AlignSkeletalLODGroups(FMeshDescription& Mesh, const TArray<FName>& Slots, FString& Why)
{
    auto Names = FStaticMeshAttributes(Mesh).GetPolygonGroupMaterialSlotNames();
    TMap<FPolygonGroupID, int32> Targets;
    int32 Highest = 0;
    for (auto Group : Mesh.PolygonGroups().GetElementIDs())
    {
        int32 Target = Slots.IndexOfByKey(Names[Group]);
        if (Target == INDEX_NONE)
        {
            Why = TEXT("Cannot resolve existing skeletal LOD polygon group: ") + Names[Group].ToString();
            return false;
        }
        Targets.Add(Group, Target);
        Highest = FMath::Max(Highest, Target);
    }
    while (Mesh.PolygonGroups().Num() <= Highest)
        Mesh.CreatePolygonGroup();
    for (auto Polygon : Mesh.Polygons().GetElementIDs())
    {
        const auto Old = Mesh.GetPolygonPolygonGroup(Polygon);
        const FPolygonGroupID Target(Targets.FindChecked(Old));
        if (Old != Target)
            Mesh.SetPolygonPolygonGroup(Polygon, Target);
    }
    for (int32 I = Mesh.PolygonGroups().Num() - 1; I > Highest; --I)
        Mesh.DeletePolygonGroup(FPolygonGroupID(I));
    for (int32 I = 0; I <= Highest; ++I)
        Names[FPolygonGroupID(I)] = Slots[I];
    return true;
}

void SetStaticSectionMaterials(FMeshState& State, int32 Lod, const TArray<FName>& Slots)
{
    const FMeshDescription& Description = State.Lods[Lod];
    const auto Names = FStaticMeshConstAttributes(Description).GetPolygonGroupMaterialSlotNames();
    int32 Section = 0;
    for (auto Group : Description.PolygonGroups().GetElementIDs())
    {
        const int32 Global = Slots.IndexOfByKey(Names[Group]);
        if (Global == INDEX_NONE)
            continue;
        for (auto* Map : {&State.Sections, &State.OriginalSections})
        {
            auto Info = Map->Get(Lod, Section);
            Info.MaterialIndex = Global;
            Map->Set(Lod, Section, Info);
        }
        ++Section;
    }
    for (auto* Map : {&State.Sections, &State.OriginalSections})
    {
        const int32 OldCount = Map->GetSectionNumber(Lod);
        for (int32 I = Section; I < OldCount; ++I)
            Map->Remove(Lod, I);
    }
}

bool ConvexShapes(const JObject &Data, const FReferenceSkeleton &Ref, FKAggregateGeom &Out, FString &Why)
{
    const int32 Bone = VehicleBodyBone(Ref);
    const JArray *Shapes = nullptr;
    if (Bone < 0 || !Data->TryGetArrayField(TEXT("shapes"), Shapes) || Shapes->Num() > 512)
    {
        Why = TEXT("Body reference bone and valid shapes are required");
        return false;
    }
    const FTransform Body = BoneTransform(Ref, Bone);
    TSet<FString> Names;
    for (const auto &Value : *Shapes)
    {
        const JObject *Shape = nullptr;
        const JArray *Positions = nullptr, *Triangles = nullptr;
        FString Name;
        if (!Value->TryGetObject(Shape) || !(*Shape)->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty() ||
            Names.Contains(Name) || !(*Shape)->TryGetArrayField(TEXT("positions"), Positions) ||
            !(*Shape)->TryGetArrayField(TEXT("triangles"), Triangles) ||
            Positions->Num() > 100000 || Triangles->IsEmpty() || Triangles->Num() > 200000)
        {
            Why = TEXT("Invalid or duplicate collision mesh");
            return false;
        }
        Names.Add(Name);
        TArray<double> V;
        TArray<FVector> Input;
        TArray<FIntVector> Faces;
        TSet<int32> Used;
        for (const auto &P : *Positions)
        {
            if (!Numbers(P, 3, V))
            {
                Why = TEXT("Invalid collision position");
                return false;
            }
            Input.Add(Body.InverseTransformPosition(FVector(V[0], V[1], V[2])));
        }
        for (const auto &T : *Triangles)
        {
            if (!Numbers(T, 3, V))
            {
                Why = TEXT("Invalid collision triangle");
                return false;
            }
            FIntVector F;
            for (int32 K = 0; K < 3; ++K)
            {
                if (V[K] < 0 || V[K] >= Input.Num() || FMath::FloorToDouble(V[K]) != V[K])
                {
                    Why = TEXT("Collision triangle index out of range");
                    return false;
                }
                F[K] = int32(V[K]);
                Used.Add(F[K]);
            }
            if (Body.GetDeterminant() < 0)
                Swap(F.Y, F.Z);
            Faces.Add(F);
        }
        // Only triangle-referenced vertices participate. A hand-made open or concave
        // mesh becomes one UE convex element; its name and grouping remain intact.
        FKConvexElem Elem;
        Elem.SetName(FName(*Name));
        TMap<int32, int32> Remap;
        for (int32 I = 0; I < Input.Num(); ++I)
            if (Used.Contains(I))
                Remap.Add(I, Elem.VertexData.Add(Input[I]));
        for (auto &F : Faces)
            for (int32 K = 0; K < 3; ++K)
                F[K] = Remap[F[K]];
        FString HullError;
        if (!BuildConvexSurface(Elem.VertexData, Faces, HullError))
        {
            Why = FString::Printf(TEXT("碰撞子节点“%s”：%s"), *Name, *HullError);
            return false;
        }
        for (const auto &F : Faces)
        {
            Elem.IndexData.Add(F.X);
            Elem.IndexData.Add(F.Y);
            Elem.IndexData.Add(F.Z);
        }
        // UpdateElemBox also discards IndexData and re-triangulates the hull.
        // These validated faces are the editor surface that Max will read back.
        Elem.ElemBox = FBox(Elem.VertexData);
        Out.ConvexElems.Add(MoveTemp(Elem));
    }
    return true;
}
bool RemapSkin(FMeshState &State, const FReferenceSkeleton &NewRef, FString &Why)
{
    bool Same = State.Ref.GetRawBoneNum() == NewRef.GetRawBoneNum();
    if (Same)
        for (int32 I = 0; I < NewRef.GetRawBoneNum(); ++I)
            if (State.Ref.GetBoneName(I) != NewRef.GetBoneName(I) ||
                State.Ref.GetParentIndex(I) != NewRef.GetParentIndex(I) ||
                !State.Ref.GetRefBonePose()[I].Equals(NewRef.GetRefBonePose()[I], 0))
            {
                Same = false;
                break;
            }
    if (Same)
        return true;
    for (int32 L = 0; L < State.Lods.Num(); ++L)
    {
        auto &M = State.Lods[L];
        if (M.Vertices().Num() == 0)
            continue;
        State.ChangedLods.Add(L);
        FSkeletalMeshAttributes A(M);
        for (auto V : M.Vertices().GetElementIDs())
        {
            TArray<UE::AnimationCore::FBoneWeight> Weights;
            for (auto W : A.GetVertexSkinWeights().Get(V))
            {
                int32 Old = W.GetBoneIndex();
                if (Old < 0 || Old >= State.Ref.GetNum())
                {
                    Why = TEXT("Existing skin bone index is invalid");
                    return false;
                }
                int32 New = NewRef.FindBoneIndex(State.Ref.GetBoneName(Old));
                if (New < 0)
                {
                    Why = TEXT("Cannot remove a bone still used by an existing mesh");
                    return false;
                }
                Weights.Emplace(New, W.GetWeight());
            }
            A.GetVertexSkinWeights().Set(V, Weights);
        }
    }
    // The engine only refreshes source bone metadata automatically when the bone
    // count changes. Explicit Root edits may change poses or parents at equal count.
    for (int32 L : State.ChangedLods)
    {
        FSkeletalMeshAttributes A(State.Lods[L]);
        A.Register(true);
        A.Bones().Reset(NewRef.GetRawBoneNum());
        auto Names = A.GetBoneNames();
        auto Parents = A.GetBoneParentIndices();
        auto Poses = A.GetBonePoses();
        for (int32 I = 0; I < NewRef.GetRawBoneNum(); ++I)
        {
            const auto B = A.CreateBone();
            Names.Set(B, NewRef.GetRawRefBoneInfo()[I].Name);
            Parents.Set(B, NewRef.GetRawRefBoneInfo()[I].ParentIndex);
            Poses.Set(B, NewRef.GetRawRefBonePose()[I]);
        }
    }
    State.Ref = NewRef;
    return true;
}
JObject Write(const JObject &R, const FVehicle &V)
{
    bool CreateBlueprints = false;
    R->TryGetBoolField(TEXT("createVehicleBlueprints"), CreateBlueprints);
    const JArray *Items = nullptr;
    if (!R->TryGetArrayField(TEXT("items"), Items) || Items->IsEmpty() || Items->Num() > 4096)
        return Error(TEXT("write requires nonempty items"));
    FScopedSlowTask Progress(CreateBlueprints ? 14.f : 12.f, LOCTEXT("WriteAssets", "写入车辆资源"));
    Progress.Visibility = ESlowTaskVisibility::ForceVisible;
    Progress.EnterProgressFrame(1.f, LOCTEXT("CheckSkeleton", "检查载具骨骼与输入列表"));
    FString Why;
    FReferenceSkeleton Ref;
    bool WriteSkeleton = false, HaveRef = false, NeedsRef = false;
    const JObject *Supplied = nullptr;
    auto *ExistingSkeleton = Cast<USkeleton>(FindAsset(V.SkeletonPath));
    auto *ExistingMesh = Cast<USkeletalMesh>(FindAsset(V.Skeletal()));
    if (ExistingSkeleton)
    {
        Ref = ExistingSkeleton->GetReferenceSkeleton();
        HaveRef = true;
    }
    if (ExistingMesh)
    {
        Ref = ExistingMesh->GetRefSkeleton();
        HaveRef = true;
    }
    TSet<FString> Ids;
    TArray<JObject> Input;
    for (const auto &Value : *Items)
    {
        const JObject *O = nullptr;
        FString Id, Kind;
        bool Unmapped = false;
        if (!Value->TryGetObject(O) || !(*O)->TryGetStringField(TEXT("id"), Id) ||
            !(*O)->TryGetStringField(TEXT("kind"), Kind) || Ids.Contains(Id))
            return Error(TEXT("Invalid or duplicate item identity"));
        Ids.Add(Id);
        (*O)->TryGetBoolField(TEXT("unmatched"), Unmapped);
        if (Unmapped)
            return Error(TEXT("Unmatched items cannot be overwritten"));
        Input.Add(*O);
        NeedsRef |= Kind == TEXT("skeletalPart") || Kind == TEXT("physics");
        if (Kind == TEXT("skeleton"))
        {
            const JObject *D = nullptr;
            if (Id != TEXT("Root") || !(*O)->TryGetObjectField(TEXT("data"), D) || !ParseBones(*D, Ref, Why))
                return Error(Why.IsEmpty() ? TEXT("Invalid Root item") : Why);
            WriteSkeleton = true;
            HaveRef = true;
        }
    }
    if (NeedsRef && !HaveRef && R->TryGetObjectField(TEXT("skeleton"), Supplied))
    {
        const JObject *D = nullptr;
        if (!(*Supplied)->TryGetObjectField(TEXT("data"), D) || !ParseBones(*D, Ref, Why))
            return Error(Why);
        HaveRef = true;
        WriteSkeleton = true;
    }
    FWyVehicleMaterialPlan MaterialPlan;
    TArray<FPlan> Plans;
    auto GetPlan = [&](const FString &Path, const FString &Kind) -> FPlan &
    {
        for (auto &P : Plans)
            if (P.Path == Path)
                return P;
        auto &P = Plans.AddDefaulted_GetRef();
        P.Path = Path;
        P.Kind = Kind;
        P.Object = FindAsset(Path);
        P.New = !P.Object;
        if (P.Object)
            P.Mesh = Capture(P.Object);
        P.Mesh.Dirty = true;
        return P;
    };
    Input.Sort(
        [](const JObject &A, const JObject &B)
        {
            double L = 0, R = 0;
            A->TryGetNumberField(TEXT("lod"), L);
            B->TryGetNumberField(TEXT("lod"), R);
            return L < R;
        });
    const float WorkPerItem = 4.f / Input.Num();
    for (const auto &O : Input)
    {
        FString Kind, Name, Id;
        O->TryGetStringField(TEXT("kind"), Kind);
        O->TryGetStringField(TEXT("name"), Name);
        O->TryGetStringField(TEXT("id"), Id);
        Progress.EnterProgressFrame(WorkPerItem, FText::Format(LOCTEXT("PreparePart", "校验并准备 {0}"), FText::FromString(Id)));
        const JObject *Data = nullptr;
        if (!O->TryGetObjectField(TEXT("data"), Data))
            return Error(TEXT("Item data missing"));
        if (Kind == TEXT("skeleton"))
            continue;
        if (Kind == TEXT("physics"))
        {
            if (Id != TEXT("BodyCollider") || !HaveRef)
                return Error(TEXT("BodyCollider requires the target skeleton or request.skeleton"));
            auto &P = GetPlan(V.Physics(), Kind);
            if (P.Object && !Cast<UPhysicsAsset>(P.Object))
                return Error(TEXT("PhysicsAsset path is occupied by another class"));
            if (P.Object)
            {
                int32 Index = Cast<UPhysicsAsset>(P.Object)->FindBodyIndex(TEXT("Body"));
                if (Index >= 0)
                {
                    auto *B = Cast<UPhysicsAsset>(P.Object)->SkeletalBodySetups[Index].Get();
                    if (B && B->AggGeom.GetElementCount() != B->AggGeom.ConvexElems.Num())
                        return Error(TEXT("Existing non-convex Body shapes require explicit mapping/conversion"));
                }
            }
            if (!ConvexShapes(*Data, Ref, P.Shapes, Why))
                return Error(Why);
            continue;
        }
        if (Kind != TEXT("staticMesh") && Kind != TEXT("skeletalPart"))
            return Error(TEXT("Unknown item kind"));
        int32 Lod;
        if (!IntegerField(O, TEXT("lod"), Lod, 0, 7))
            return Error(TEXT("Invalid mesh LOD"));
        const FString Prefix = FString::Printf(TEXT("LOD%d_"), Lod);
        FString Part = Name;
        if (Id != Name || !Part.RemoveFromStart(Prefix) || !SafeSegment(Part))
            return Error(TEXT("Mesh identity must be its original LODn_part node name"));
        const bool Skel = Kind == TEXT("skeletalPart");
        if (!IsVehicleMeshPart(Part, Skel))
            return Error(TEXT("No vehicle part rule for: ") + Part);
        if (Skel && !HaveRef)
            return Error(TEXT("SkeletalMesh requires the target skeleton or request.skeleton"));
        FString Path = V.Skeletal();
        if (!Skel)
        {
            const FStaticMeshMatch Match = MatchStaticMesh(V, Part);
            if (Match.ConflictLods)
                return Error(StaticMeshConflict(Part));
            Path = Match.Mesh ? Match.Mesh->GetOutermost()->GetName() : V.StaticMesh(Part);
        }
        auto &P = GetPlan(Path, Kind);
        if (!Skel && P.Object && P.Object->GetOutermost()->GetFName() != FName(*Path))
            return Error(TEXT("目标包已重定向至其他资源：") + Path);
        if (P.Object && ((Skel && !Cast<USkeletalMesh>(P.Object)) || (!Skel && !Cast<UStaticMesh>(P.Object))))
            return Error(TEXT("Mesh path is occupied by another class"));
        if (Skel)
        {
            if (P.Mesh.Lods.IsEmpty())
                P.Mesh.Ref = Ref;
            else if (!RemapSkin(P.Mesh, Ref, Why))
                return Error(Why);
        }
        FMeshInput Mesh;
        if (!ParseMesh(*Data, Skel ? &Ref : nullptr, Mesh, Why))
            return Error(Name + TEXT(": ") + Why);
        if (Lod > P.Mesh.Lods.Num())
            return Error(TEXT("Create LODs in sequence; a preceding source LOD is missing"));
        if (Lod < P.Mesh.Lods.Num() && P.Mesh.Lods[Lod].Vertices().Num() == 0)
            return Error(TEXT("Selected LOD has no editable source description; generated LODs are not overwritten"));
        if (Lod == P.Mesh.Lods.Num())
        {
            P.Mesh.Lods.AddDefaulted();
            if (Skel)
                P.Mesh.SkeletalLodInfo.AddDefaulted();
            else
                P.Mesh.StaticBuild.AddDefaulted();
        }
        P.Mesh.ChangedLods.Add(Lod);
        if (Skel)
        {
            auto &Info = P.Mesh.SkeletalLodInfo[Lod];
            auto &Reduction = Info.ReductionSettings;
            Reduction.BaseLOD = Lod;
            Reduction.TerminationCriterion = SMTC_NumOfTriangles;
            Reduction.NumOfTrianglesPercentage = Reduction.NumOfVertPercentage = 1.0f;
            Reduction.MaxNumOfTriangles = Reduction.MaxNumOfVerts = MAX_uint32;
            Reduction.MaxNumOfTrianglesPercentage = Reduction.MaxNumOfVertsPercentage = MAX_uint32;
            Reduction.MaxDeviationPercentage = 0.0f;
            Info.bHasBeenSimplified = false;
            auto &B = Info.BuildSettings;
            B.bRecomputeNormals = B.bRecomputeTangents = B.bRemoveDegenerates = false;
            B.bUseFullPrecisionUVs = B.bUseHighPrecisionTangentBasis = true;
        }
        else
        {
            auto &B = P.Mesh.StaticBuild[Lod];
            B.bRecomputeNormals = B.bRecomputeTangents = B.bRemoveDegenerates = B.bGenerateLightmapUVs = false;
            B.bUseFullPrecisionUVs = B.bUseHighPrecisionTangentBasis = true;
        }
        TArray<FName> Slots;
        if (!(Skel ? PrepareSlots(P, Mesh, P.Mesh.SkeletalMaterials, Name, V, MaterialPlan, Slots, Why)
                   : PrepareSlots(P, Mesh, P.Mesh.StaticMaterials, Name, V, MaterialPlan, Slots, Why)))
            return Error(Name + TEXT(": ") + Why);
        // A skeletal LOD section may be shared by several Max parts. Bake effective
        // assignments into the staged source before replacing just one of those parts.
        if (Skel && Mesh.HasNamedSlots && !P.BakedSectionLods.Contains(Lod) &&
            !P.Mesh.SkeletalLodInfo[Lod].LODMaterialMap.IsEmpty() && P.Object)
        {
            auto& Description = P.Mesh.Lods[Lod];
            if (!WyBakeSkeletalSectionOverrides(Description, WyMeshSlotView(P.Object, Lod, Description), Why))
                return Error(Name + TEXT(": ") + Why);
            P.Mesh.SkeletalLodInfo[Lod].LODMaterialMap.Reset();
            P.BakedSectionLods.Add(Lod);
        }
        FString SourcePart = Name;
        if (Skel && P.Mesh.Lods[Lod].Vertices().Num() > 0)
        {
            TMap<FVertexID, int32> Owners;
            TArray<FString> Parts;
            if (!PartOwners(P.Mesh.Lods[Lod], Owners, Parts, Why))
                return Error(Name + TEXT(": ") + Why);
            if (Parts.Contains(Part) && Parts.Contains(Name))
                return Error(TEXT("Ambiguous legacy part names"));
            if (Parts.Contains(Part))
                SourcePart = Part;
        }
        if (!ReplaceMesh(P.Mesh.Lods[Lod], Mesh, SourcePart, Skel, Slots, Why))
            return Error(Name + TEXT(": ") + Why);
        if (Skel && Lod > 0 && Mesh.HasNamedSlots && !AlignSkeletalLODGroups(P.Mesh.Lods[Lod], Slots, Why))
            return Error(Name + TEXT(": ") + Why);
        if (!Skel && Mesh.HasNamedSlots)
            SetStaticSectionMaterials(P.Mesh, Lod, Slots);
    }
    Progress.EnterProgressFrame(1.f, LOCTEXT("CheckDependencies", "检查目标资源和依赖"));
    if (WriteSkeleton)
    {
        if (UObject *Occupant = FindAsset(V.SkeletonPath))
            if (!Cast<USkeleton>(Occupant))
                return Error(TEXT("Skeleton path is occupied by another class"));
        if (ExistingMesh)
        {
            auto &P = GetPlan(V.Skeletal(), TEXT("skeletalPart"));
            if (!RemapSkin(P.Mesh, Ref, Why))
                return Error(Why);
        }
    }
    auto MeshPlan = [&](const FString &Path) -> FPlan *
    {
        return Plans.FindByPredicate([&](const FPlan &P) { return P.Path == Path; });
    };
    FPlan *BodyMesh = MeshPlan(V.Skeletal()), *BodyPhysics = MeshPlan(V.Physics());
    FPlan *Hub = nullptr, *Tire = nullptr, *FrontBrake = nullptr, *RearBrake = nullptr;
    UClass *VehicleParent = nullptr;
    if (CreateBlueprints)
    {
        // First creation may be retried. Never modify an existing asset on this path.
        if (ExistingSkeleton || ExistingMesh || Plans.ContainsByPredicate([](const FPlan &P) { return !P.New; }) ||
            FindAsset(V.Blueprint(false)) || FindAsset(V.Blueprint(true)))
            return Error(TEXT("车辆目录已有资产，请重新比对。"));
        Hub = MeshPlan(V.StaticMesh(TEXT("Hub_0000")));
        Tire = MeshPlan(V.StaticMesh(TEXT("Tire_0000")));
        FrontBrake = MeshPlan(V.StaticMesh(TEXT("BrakeF")));
        RearBrake = MeshPlan(V.StaticMesh(TEXT("BrakeR")));
        if (!RearBrake)
            RearBrake = MeshPlan(V.StaticMesh(TEXT("BrakeB")));
        TArray<FString> Missing;
        if (!WriteSkeleton) Missing.Add(TEXT("Root"));
        if (!BodyMesh) Missing.Add(TEXT("车身骨骼网格体"));
        if (!Hub) Missing.Add(TEXT("LOD0_Hub_0000"));
        if (!Tire) Missing.Add(TEXT("LOD0_Tire_0000"));
        if (!FrontBrake) Missing.Add(TEXT("LOD0_BrakeF"));
        if (!RearBrake) Missing.Add(TEXT("LOD0_BrakeR（或 BrakeB）"));
        for (const TCHAR *Bone : {TEXT("D_WheelFL"), TEXT("D_WheelFR"), TEXT("D_WheelBL"), TEXT("D_WheelBR")})
            if (Ref.FindBoneIndex(Bone) == INDEX_NONE)
                Missing.Add(Bone);
        if (!Missing.IsEmpty())
            return Error(TEXT("首次创建车辆蓝图缺少：") + FString::Join(Missing, TEXT("、")));
        VehicleParent = WyResolveVehicleParent(Why);
        if (!VehicleParent || !WyValidateAnimBlueprintDependencies(Why))
            return Error(Why);
    }
    // For new vehicles the complete objects are the undo payload. Start that
    // transaction after both compilers succeed so redo preserves their CDOs and graphs.
    // New instances are restored as complete objects by FCreatedAssetChange.
    // Their initialization must not also serialize their original outer/name in
    // this transaction, or the engine can move them back after creation undo.
    TArray<UObject *> Created;
    Progress.EnterProgressFrame(1.f, LOCTEXT("ApplyMaterials", "准备车辆材质实例"));
    WyCreatePlannedVehicleMaterials(MaterialPlan, Created);
    TOptional<FScopedTransaction> Transaction;
    if (!CreateBlueprints)
        Transaction.Emplace(NSLOCTEXT("WyVehicleAssetBridge", "SyncAssets", "Synchronize vehicle assets"));
    auto Create = [&](const FString &Path, UClass *Class) -> UObject *
    {
        UPackage *Package = CreatePackage(*Path);
        return NewObject<UObject>(Package, Class, FName(*FPackageName::GetLongPackageAssetName(Path)),
                                  RF_Public | RF_Standalone | RF_Transactional);
    };
    USkeleton *Skeleton = ExistingSkeleton ? ExistingSkeleton : (ExistingMesh ? ExistingMesh->GetSkeleton() : nullptr);
    const float WorkPerAsset = 4.f / (Plans.Num() + (WriteSkeleton ? 1 : 0));
    if (WriteSkeleton)
    {
        Progress.EnterProgressFrame(WorkPerAsset, LOCTEXT("WriteSkeleton", "写入车辆骨骼"));
        Skeleton = ExistingSkeleton;
        if (!Skeleton)
        {
            Skeleton = CastChecked<USkeleton>(Create(V.SkeletonPath, USkeleton::StaticClass()));
            Created.Add(Skeleton);
        }
        auto Change =
            MakeUnique<FSkeletonChange>(Skeleton->GetReferenceSkeleton(), Ref, Skeleton->GetOutermost()->IsDirty());
        Change->Apply(Skeleton);
        if (GUndo && !CreateBlueprints)
            GUndo->StoreUndo(Skeleton, MoveTemp(Change));
    }
    for (auto &P : Plans)
    {
        Progress.EnterProgressFrame(WorkPerAsset, FText::Format(LOCTEXT("BuildAsset", "构建 {0}"),
            FText::FromString(FPackageName::GetLongPackageAssetName(P.Path))));
        if (!P.Object)
        {
            UClass *Class =
                P.Kind == TEXT("staticMesh")
                    ? UStaticMesh::StaticClass()
                    : (P.Kind == TEXT("skeletalPart") ? USkeletalMesh::StaticClass() : UPhysicsAsset::StaticClass());
            P.Object = Create(P.Path, Class);
            Created.Add(P.Object);
        }
        if (P.Kind == TEXT("physics"))
        {
            auto *Physics = CastChecked<UPhysicsAsset>(P.Object);
            int32 Index = Physics->FindBodyIndex(TEXT("Body"));
            bool Exists = Physics->SkeletalBodySetups.IsValidIndex(Index);
            FKAggregateGeom Before;
            if (Exists)
                Before = Physics->SkeletalBodySetups[Index]->AggGeom;
            auto Change = MakeUnique<FPhysicsChange>(Before, P.Shapes, Exists, Physics->GetOutermost()->IsDirty());
            Change->Apply(Physics);
            if (GUndo && !CreateBlueprints)
                GUndo->StoreUndo(Physics, MoveTemp(Change));
        }
        else
        {
            for (const auto& Binding : P.PendingMaterials)
            {
                auto* Material = WyPlannedVehicleMaterial(MaterialPlan, Binding.Value);
                if (P.Kind == TEXT("skeletalPart"))
                    P.Mesh.SkeletalMaterials[Binding.Key].MaterialInterface = Material;
                else
                    P.Mesh.StaticMaterials[Binding.Key].MaterialInterface = Material;
            }
            if (P.New || WriteSkeleton)
                P.Mesh.Skeleton = Skeleton;
            auto Change = MakeUnique<FMeshChange>(Capture(P.Object), P.Mesh);
            Change->Apply(P.Object);
            if (GUndo && !CreateBlueprints)
                GUndo->StoreUndo(P.Object, MoveTemp(Change));
        }
        P.Object->MarkPackageDirty();
    }
    Progress.EnterProgressFrame(0.5f, LOCTEXT("LinkPhysics", "关联骨骼网格体与物理资产"));
    if (BodyMesh || BodyPhysics)
    {
        auto *Mesh = BodyMesh ? CastChecked<USkeletalMesh>(BodyMesh->Object) : ExistingMesh;
        auto *Physics = Cast<UPhysicsAsset>(BodyPhysics ? BodyPhysics->Object : FindAsset(V.Physics()));
        LinkVehiclePhysics(Mesh, Physics, Created);
    }
    if (CreateBlueprints)
    {
        const FString AnimPath = V.Blueprint(true), ActorPath = V.Blueprint(false);
        UAnimBlueprint *Animation = nullptr;
        UBlueprint *Actor = nullptr;
        auto *Mesh = CastChecked<USkeletalMesh>(BodyMesh->Object);
        // Factories perform an initial compile internally without a SkipSave option.
        TGuardValue<TEnumAsByte<ESaveOnCompile>> NoAutoSave(GetMutableDefault<UBlueprintEditorSettings>()->SaveOnCompile, SoC_Never);
        Progress.EnterProgressFrame(1.f, LOCTEXT("CreateAnimBlueprint", "创建并编译动画蓝图"));
        bool Ok = WyCreateAnimBlueprint(CreatePackage(*AnimPath), FName(*FPackageName::GetLongPackageAssetName(AnimPath)),
                                        Skeleton, Mesh, Animation, Why);
        if (Ok)
        {
            Progress.EnterProgressFrame(1.f, LOCTEXT("CreateVehicleBlueprint", "创建并编译车辆蓝图"));
            Ok = WyCreateVehicleBlueprint(CreatePackage(*ActorPath), FName(*FPackageName::GetLongPackageAssetName(ActorPath)),
                VehicleParent, Mesh, Animation, CastChecked<UStaticMesh>(Hub->Object), CastChecked<UStaticMesh>(Tire->Object),
                CastChecked<UStaticMesh>(FrontBrake->Object), CastChecked<UStaticMesh>(RearBrake->Object), Actor, Why);
        }
        if (Animation) Created.Add(Animation);
        if (Actor) Created.Add(Actor);
        if (!Ok)
        {
            // Only brand-new objects can reach this branch. They have not been published.
            for (int32 Index = Created.Num() - 1; Index >= 0; --Index)
            {
                UObject *Object = Created[Index];
                UPackage *Package = Object->GetOutermost();
                Object->ClearFlags(RF_Public | RF_Standalone);
                const FName TemporaryName = MakeUniqueObjectName(GetTransientPackage(), Object->GetClass(), Object->GetFName());
                Object->Rename(*TemporaryName.ToString(), GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
                Package->SetDirtyFlag(false);
            }
            return Error(Why);
        }
        Transaction.Emplace(NSLOCTEXT("WyVehicleAssetBridge", "CreateVehicle", "Create vehicle assets and blueprints"));
    }
    Progress.EnterProgressFrame(0.5f, LOCTEXT("RegisterAssets", "登记资源与撤销记录"));
    for (auto *O : Created)
    {
        O->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(O);
        if (GUndo)
            GUndo->StoreUndo(O, MakeUnique<FCreatedAssetChange>(O, false));
    }
    auto Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("ok"), true);
    Progress.EnterProgressFrame(0.f);
    return Result;
}
} // namespace WyAssets

TSharedPtr<FJsonObject> WyHandleAssets(const TSharedPtr<FJsonObject> &Request)
{
    if (!IsInGameThread())
        return WyAssets::Error(TEXT("Asset operations require the editor game thread"));
    FString Op;
    if (!Request || !Request->TryGetStringField(TEXT("op"), Op))
        return WyAssets::Error(TEXT("Missing operation"));
    if (Op == TEXT("hulls"))
        return WyBuildVehicleHulls(Request);
    WyAssets::FVehicle Vehicle;
    if (!WyAssets::Vehicle(Request, Vehicle))
        return WyAssets::Error(TEXT("Invalid vehicle type/name/variant"));
    Vehicle.SkeletonPath = WyAssets::ResolveSkeletonPath(Vehicle);
    if (Op == TEXT("read"))
        return WyAssets::Read(Vehicle);
    if (Op == TEXT("write"))
        return WyAssets::Write(Request, Vehicle);
    return WyAssets::Error(TEXT("Unknown asset operation"));
}

#undef LOCTEXT_NAMESPACE
