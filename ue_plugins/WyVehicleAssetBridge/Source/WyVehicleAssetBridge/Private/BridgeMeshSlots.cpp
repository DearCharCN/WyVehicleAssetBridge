#include "BridgeMeshSlots.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "StaticMeshAttributes.h"
#include "SkeletalMeshAttributes.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"

const FName WyLocalFaceMaterialID(TEXT("WyVehicleLocalMaterialID"));
const FName WyUnassignedMaterialSlot(TEXT("WyVehicle_Unassigned"));
static const TCHAR* ScopeKey = TEXT("WyVehicleMaterialSlotsV1");

FWyMeshSlotScopes WyLoadSlotScopes(UObject* Mesh)
{
    FWyMeshSlotScopes Result;
    const FString& Text = Mesh->GetOutermost()->GetMetaData().GetValue(Mesh, ScopeKey);
    TSharedPtr<FJsonObject> Root;
    if (Text.IsEmpty() || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root)
        return Result;
    for (const auto& Scope : Root->Values)
    {
        const TSharedPtr<FJsonObject>* Rows = nullptr;
        if (!Scope.Value->TryGetObject(Rows))
            continue;
        auto& Map = Result.Add(FString(*Scope.Key));
        for (const auto& Row : (*Rows)->Values)
        {
            FString Slot;
            if (Row.Value->TryGetString(Slot))
                Map.Add(FCString::Atoi(*Row.Key), FName(*Slot));
        }
    }
    return Result;
}

void WyStoreSlotScopes(UObject* Mesh, const FWyMeshSlotScopes& Scopes)
{
    auto& Meta = Mesh->GetOutermost()->GetMetaData();
    if (Scopes.IsEmpty())
    {
        Meta.RemoveValue(Mesh, ScopeKey);
        return;
    }
    auto Root = MakeShared<FJsonObject>();
    for (const auto& Scope : Scopes)
    {
        auto Obj = MakeShared<FJsonObject>();
        for (const auto& Row : Scope.Value)
            Obj->SetStringField(FString::FromInt(Row.Key), Row.Value.ToString());
        Root->SetObjectField(Scope.Key, Obj);
    }
    FString Text;
    FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Text));
    Meta.SetValue(Mesh, ScopeKey, *Text);
}

// Bone limits can split one source material into multiple render sections.
// Vertex order may change in a build; the imported vertex triplet survives.
static FIntVector4 TriangleKey(int32 Material, int32 A, int32 B, int32 C)
{
    if (A > B) Swap(A, B);
    if (B > C) Swap(B, C);
    if (A > B) Swap(A, B);
    return {Material, A, B, C};
}

FWyMeshSlotView WyMeshSlotView(UObject* Object, int32 Lod, const FMeshDescription& Description)
{
    FWyMeshSlotView View;
    auto Add = [&View](const auto& Slot)
    {
        const FName Imported = Slot.ImportedMaterialSlotName.IsNone() ? Slot.MaterialSlotName : Slot.ImportedMaterialSlotName;
        FString Path = Slot.MaterialInterface ? Slot.MaterialInterface->GetOutermost()->GetName() : FString();
        // The old bridge wrote BOTH names as Material_<global index>. An editor
        // slot with just a matching display name (or no name) is still a real slot.
        const FString OldName = FString::Printf(TEXT("Material_%d"), View.Slots.Num());
        const bool LegacyPlaceholder = !Slot.MaterialInterface &&
            Slot.MaterialSlotName.ToString().Equals(OldName, ESearchCase::CaseSensitive) &&
            Slot.ImportedMaterialSlotName.ToString().Equals(OldName, ESearchCase::CaseSensitive);
        View.Slots.Add({Slot.MaterialSlotName, Imported, MoveTemp(Path), LegacyPlaceholder});
    };
    UStaticMesh* Static = Cast<UStaticMesh>(Object);
    USkeletalMesh* Skel = Cast<USkeletalMesh>(Object);
    TMap<int32, int32> SkeletalRemap;
    TSet<int32> SplitMaterials;
    TMap<FIntVector4, int32> ChunkFaces;
    if (Static)
        for (const auto& Slot : Static->GetStaticMaterials()) Add(Slot);
    if (Skel)
    {
        for (const auto& Slot : Skel->GetMaterials()) Add(Slot);
        const auto* Info = Skel->GetLODInfo(Lod);
        const auto* Model = Skel->GetImportedModel();
        if (Info && Model && Model->LODModels.IsValidIndex(Lod))
        {
            const auto& ModelLOD = Model->LODModels[Lod];
            const auto& Sections = ModelLOD.Sections;
            auto Actual = [&](int32 Section)
            {
                return Info->LODMaterialMap.IsValidIndex(Section) && View.Slots.IsValidIndex(Info->LODMaterialMap[Section])
                    ? Info->LODMaterialMap[Section] : int32(Sections[Section].MaterialIndex);
            };
            for (int32 I = 0; I < Sections.Num(); ++I)
            {
                const int32 Source = Sections[I].MaterialIndex, Target = Actual(I);
                if (const auto* Other = SkeletalRemap.Find(Source); Other && *Other != Target)
                    SplitMaterials.Add(Source);
                SkeletalRemap.Add(Source, Target);
            }
            for (int32 I = 0; I < Sections.Num() && !SplitMaterials.IsEmpty(); ++I)
            {
                const auto& Section = Sections[I];
                if (!SplitMaterials.Contains(Section.MaterialIndex))
                    continue;
                const int32 Target = Actual(I);
                for (uint32 Face = 0; Face < Section.NumTriangles; ++Face)
                {
                    const int32 First = Section.BaseIndex + Face * 3;
                    if (!ModelLOD.IndexBuffer.IsValidIndex(First + 2)) continue;
                    FIntVector V = FIntVector::ZeroValue;
                    bool Valid = true;
                    for (int32 K = 0; K < 3; ++K)
                    {
                        const int32 RenderVertex = ModelLOD.IndexBuffer[First + K];
                        if (!ModelLOD.MeshToImportVertexMap.IsValidIndex(RenderVertex)) { Valid = false; break; }
                        V[K] = ModelLOD.MeshToImportVertexMap[RenderVertex];
                    }
                    if (!Valid) continue;
                    const auto Key = TriangleKey(Section.MaterialIndex, V.X, V.Y, V.Z);
                    int32& Found = ChunkFaces.FindOrAdd(Key, Target);
                    if (Found != Target) Found = INDEX_NONE;
                }
            }
        }
    }
    const auto Names = FStaticMeshConstAttributes(Description).GetPolygonGroupMaterialSlotNames();
    int32 SectionIndex = 0;
    for (auto Group : Description.PolygonGroups().GetElementIDs())
    {
        if (!Static && Description.GetNumPolygonGroupTriangles(Group) == 0)
            continue;
        const FName Name = Names[Group];
        int32 Index = View.Slots.IndexOfByPredicate([&](const auto& S) { return S.Imported == Name; });
        if (Index == INDEX_NONE)
            Index = View.Slots.IndexOfByPredicate([&](const auto& S) { return S.Display == Name; });
        if (Index == INDEX_NONE && View.Slots.IsValidIndex(Group.GetValue()))
            Index = Group.GetValue();
        // UE's skeletal builder remaps imported names on LOD0 only. Higher
        // LODs render by polygon group index, then apply the optional section override.
        if (Skel && Lod > 0)
            Index = Group.GetValue();
        if (Static)
            Index = Static->GetSectionInfoMap().Get(Lod, SectionIndex).MaterialIndex;
        else if (SplitMaterials.Contains(Index))
        {
            const auto Import = Description.VertexAttributes().GetAttributesRef<int32>(MeshAttribute::Vertex::ImportPointIndex);
            for (auto Face : Description.GetPolygonGroupTriangles(Group))
            {
                const auto Vertices = Description.GetTriangleVertices(Face);
                auto Raw = [&](FVertexID V) { return Import.IsValid() && Import[V] != INDEX_NONE ? Import[V] : V.GetValue(); };
                const auto* Found = ChunkFaces.Find(TriangleKey(Index, Raw(Vertices[0]), Raw(Vertices[1]), Raw(Vertices[2])));
                View.FaceOverrides.Add(Face, Found ? *Found : INDEX_NONE);
            }
        }
        else if (const auto* Override = SkeletalRemap.Find(Index))
            Index = *Override;
        View.Groups.Add(Group, Index);
        ++SectionIndex;
    }
    return View;
}

bool WyBakeSkeletalSectionOverrides(FMeshDescription& Description, const FWyMeshSlotView& View, FString& Why)
{
    auto Names = FStaticMeshAttributes(Description).GetPolygonGroupMaterialSlotNames();
    TMap<int32, FPolygonGroupID> TargetGroups;
    for (const auto& Group : View.Groups)
        if (View.Slots.IsValidIndex(Group.Value))
        {
            Names[Group.Key] = View.Slots[Group.Value].Imported;
            TargetGroups.Add(Group.Value, Group.Key);
        }
    TMap<FPolygonID, int32> Moves;
    for (const auto& Face : View.FaceOverrides)
    {
        const FPolygonID Polygon = Description.GetTrianglePolygon(Face.Key);
        if (!View.Slots.IsValidIndex(Face.Value) || Moves.FindOrAdd(Polygon, Face.Value) != Face.Value)
        {
            Why = TEXT("骨骼网格分段的材质覆盖无法唯一映射到源多边形。");
            return false;
        }
    }
    for (const auto& Move : Moves)
    {
        auto* Target = TargetGroups.Find(Move.Value);
        if (!Target)
        {
            auto Group = Description.CreatePolygonGroup();
            Names[Group] = View.Slots[Move.Value].Imported;
            Target = &TargetGroups.Add(Move.Value, Group);
        }
        if (Description.GetPolygonPolygonGroup(Move.Key) != *Target)
            Description.SetPolygonPolygonGroup(Move.Key, *Target);
    }
    return true;
}

FWyReadSlotScope::FWyReadSlotScope(const FWyMeshSlotView& InView, const FWyLocalSlots* Source) : View(InView), Managed(Source != nullptr)
{
    if (Source)
    {
        TArray<int32> Keys;
        Source->GetKeys(Keys);
        Keys.Sort();
        for (int32 Local : Keys)
        {
            FName Imported = Source->FindChecked(Local);
            int32 Index = View.Slots.IndexOfByPredicate([&](const auto& S) { return S.Imported == Imported; });
            if (Index == INDEX_NONE)
                continue;
            LocalToGlobal.Add(Local, Index);
            if (!GlobalToLocal.Contains(Index))
                GlobalToLocal.Add(Index, Local);
        }
    }
}

int32 FWyReadSlotScope::ReadFace(FTriangleID Triangle, FPolygonGroupID Group, int32 Original)
{
    const int32* Global = View.FaceOverrides.Find(Triangle);
    if (Global && *Global == INDEX_NONE) return INDEX_NONE;
    if (!Global) Global = View.Groups.Find(Group);
    if (!Global || !View.Slots.IsValidIndex(*Global))
        return Original >= 0 ? Original : 0;
    if (const auto* Old = LocalToGlobal.Find(Original); Old && *Old == *Global)
        return Original;
    if (const auto* Existing = GlobalToLocal.Find(*Global))
        return *Existing;
    int32 Local = *Global;
    if (LocalToGlobal.Contains(Local))
    {
        while (LocalToGlobal.Contains(NextFree)) ++NextFree;
        Local = NextFree++;
    }
    LocalToGlobal.Add(Local, *Global);
    GlobalToLocal.Add(*Global, Local);
    if (Managed) EditorSlots.Add(Local);
    return Local;
}

FString WyMaxMaterialSlotName(FName Display, const FString& AssetPackage, const FString& VehicleBase, const FString& VehicleName)
{
    const FString Label = Display.ToString();
    const FString Prefix = TEXT("MI_") + VehicleName + TEXT("_");
    // Artists may give a slot its bound asset's full UE name. Collapse that
    // case only: a short Max purpose can itself start with the vehicle name.
    if (Label.Len() > Prefix.Len() && Label.StartsWith(Prefix, ESearchCase::CaseSensitive) &&
        AssetPackage.Equals(VehicleBase / TEXT("MaterialInstance") / Label, ESearchCase::IgnoreCase))
        return TEXT("MI_") + Label.Mid(Prefix.Len());
    return Label;
}

TArray<TSharedPtr<FJsonValue>> FWyReadSlotScope::ToJson(const FString& VehicleBase, const FString& VehicleName) const
{
    TArray<TSharedPtr<FJsonValue>> Result;
    TArray<int32> Keys;
    LocalToGlobal.GetKeys(Keys);
    Keys.Sort();
    for (int32 Local : Keys)
    {
        const auto& Slot = View.Slots[LocalToGlobal.FindChecked(Local)];
        FString Name = WyMaxMaterialSlotName(Slot.Display, Slot.Asset, VehicleBase, VehicleName);
        const bool Unassigned = Managed && !EditorSlots.Contains(Local) && Slot.Display == WyUnassignedMaterialSlot;
        if ((!Managed && Slot.LegacyPlaceholder) || (Unassigned && Slot.Asset.IsEmpty()))
            continue;
        if (Slot.Display.IsNone())
            Name = FString::Printf(TEXT("Material_%d"), Local);
        auto Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("id"), Local);
        Row->SetStringField(TEXT("name"), Name);
        Row->SetStringField(TEXT("asset"), Slot.Asset);
        Result.Add(MakeShared<FJsonValueObject>(Row));
    }
    return Result;
}
