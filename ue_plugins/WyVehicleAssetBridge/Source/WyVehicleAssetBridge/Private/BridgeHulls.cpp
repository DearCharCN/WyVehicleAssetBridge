#include "BridgeAssets.h"
#include "CompGeom/ConvexHull3.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/SecureHash.h"

namespace WyHullCore
{
using FJsonValues = TArray<TSharedPtr<FJsonValue>>;
using FObject = TSharedRef<FJsonObject>;

FObject HullError(const FString &Message)
{
    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("ok"), false);
    R->SetStringField(TEXT("error"), Message);
    return R;
}

static TSharedPtr<FJsonValue> Row(double A, double B, double C)
{
    FJsonValues Values = {MakeShared<FJsonValueNumber>(A), MakeShared<FJsonValueNumber>(B),
                          MakeShared<FJsonValueNumber>(C)};
    return MakeShared<FJsonValueArray>(MoveTemp(Values));
}

struct FPart
{
    FString Name, Kind;
    TArray<FVector> Vertices;
    TArray<FIntVector> Faces;
};

static bool ReadTriple(const TSharedPtr<FJsonValue> &Value, FVector &Out)
{
    const FJsonValues *RowValues = nullptr;
    if (!Value || !Value->TryGetArray(RowValues) || RowValues->Num() != 3)
        return false;
    for (int I = 0; I < 3; ++I)
        if (!(*RowValues)[I]->TryGetNumber(Out[I]) || !FMath::IsFinite(Out[I]) || FMath::Abs(Out[I]) > 1.e8)
            return false;
    return true;
}

static bool ReadPart(const TSharedPtr<FJsonValue> &Value, FPart &Out, bool ExpectKind)
{
    if (!Value || Value->Type != EJson::Object)
        return false;
    auto O = Value->AsObject();
    const FJsonValues *Vertices = nullptr, *Faces = nullptr;
    if (!O->TryGetStringField(TEXT("name"), Out.Name) || !O->TryGetArrayField(TEXT("vertices"), Vertices) ||
        !O->TryGetArrayField(TEXT("faces"), Faces) || Faces->IsEmpty())
        return false;
    if (ExpectKind &&
        (!O->TryGetStringField(TEXT("kind"), Out.Kind) || (Out.Kind != TEXT("body") && Out.Kind != TEXT("spoiler"))))
        return false;
    for (const auto &V : *Vertices)
    {
        FVector P;
        if (!ReadTriple(V, P))
            return false;
        Out.Vertices.Emplace(P.X, P.Y, P.Z);
    }
    for (const auto &F : *Faces)
    {
        FVector D;
        if (!ReadTriple(F, D))
            return false;
        FIntVector T;
        for (int I = 0; I < 3; ++I)
        {
            T[I] = FMath::TruncToInt(D[I]);
            if (T[I] != D[I] || !Out.Vertices.IsValidIndex(T[I]))
                return false;
        }
        Out.Faces.Add(T);
    }
    return true;
}

using FHullSettings = UE::Geometry::TConvexHullSimplificationSettings<double>;
// Preserve the threshold used for Max's float coordinates before these settings were exposed.
static constexpr double DefaultDistanceFraction = 2.e-7;

static bool ReadSimplification(const FObject &Settings, FHullSettings &Out, FString &Error)
{
    Out.SkipAtHullDistanceAsFraction = DefaultDistanceFraction;
    if (Settings->HasField(TEXT("maxHullVertices")))
    {
        double Value;
        if (!Settings->HasTypedField<EJson::Number>(TEXT("maxHullVertices")) ||
            !Settings->TryGetNumberField(TEXT("maxHullVertices"), Value) || !FMath::IsFinite(Value) ||
            Value < MIN_int32 || Value > MAX_int32 || FMath::FloorToDouble(Value) != Value ||
            (Value > 0 && Value < 4))
        {
            Error = TEXT("每个凸包的最大顶点数必须是 32 位整数。非正值表示不限，正值至少为 4。");
            return false;
        }
        Out.MaxHullVertices = static_cast<int32>(Value);
    }
    auto ReadTolerance = [&](const TCHAR *Field, const TCHAR *Label, double &Target)
    {
        if (!Settings->HasField(Field))
            return true;
        if (!Settings->HasTypedField<EJson::Number>(Field) || !Settings->TryGetNumberField(Field, Target) ||
            !FMath::IsFinite(Target) || Target < 0)
        {
            Error = FString::Printf(TEXT("%s必须是大于等于 0 的有限数值。"), Label);
            return false;
        }
        return true;
    };
    if (!ReadTolerance(TEXT("degenerateEdgeTolerance"), TEXT("最短边容差"), Out.DegenerateEdgeTolerance) ||
        !ReadTolerance(TEXT("skipAtHullDistanceAbsolute"), TEXT("绝对距离阈值"), Out.SkipAtHullDistanceAbsolute) ||
        !ReadTolerance(TEXT("skipAtHullDistanceAsFraction"), TEXT("相对距离阈值"), Out.SkipAtHullDistanceAsFraction))
        return false;
    // GeometryCore uses a negative sentinel to disable its absolute plane-distance check.
    if (Out.SkipAtHullDistanceAbsolute == 0)
        Out.SkipAtHullDistanceAbsolute = FHullSettings{}.SkipAtHullDistanceAbsolute;
    return true;
}

static TSharedPtr<FJsonObject> MakeHull(const TCHAR *Name, const TSet<FVector> &Unique, const FHullSettings &Settings,
                                       FScopedSlowTask &Progress)
{
    if (Unique.Num() < 4)
        return nullptr;
    // Max Point3 stores floats. Solve on the positions the receiving mesh can
    // actually represent.
    TSet<FVector> Representable;
    Representable.Reserve(Unique.Num());
    for (const FVector &Source : Unique)
        Representable.Add(FVector(FVector3f(Source)));
    const TArray<FVector> Points = Representable.Array();
    if (Points.Num() < 4)
        return nullptr;
    UE::Geometry::TConvexHull3<double> Hull;
    Hull.SimplificationSettings = Settings;
    if (!Hull.Solve(Points.Num(), [&](int32 I) { return Points[I]; }) || Hull.GetDimension() != 3)
        return nullptr;

    TSet<FVector> HullPoints;
    for (const auto &Tri : Hull.GetTriangles())
    {
        HullPoints.Add(Points[Tri.A]);
        HullPoints.Add(Points[Tri.B]);
        HullPoints.Add(Points[Tri.C]);
    }
    if (HullPoints.Num() < 4 || (Settings.MaxHullVertices > 0 && HullPoints.Num() > Settings.MaxHullVertices))
        return nullptr;

    // The mean of the original cloud can lie outside an intentionally simplified hull.
    FVector Interior = FVector::ZeroVector;
    FBox Bounds(ForceInit);
    for (const auto &Point : HullPoints)
    {
        Interior += Point;
        Bounds += Point;
    }
    Interior /= HullPoints.Num();
    const double Tolerance = FMath::Max(1.e-6, Bounds.GetSize().Size() * 2.e-6);
    // UE simplification skips exterior points. Check convexity against the output in
    // that case; the default and tighter settings also check coverage of the original input.
    const bool bCanSkipSurface = (Settings.MaxHullVertices > 0 && Settings.MaxHullVertices < Points.Num()) ||
        Settings.DegenerateEdgeTolerance > 0 || Settings.SkipAtHullDistanceAbsolute > 0 ||
        Settings.SkipAtHullDistanceAsFraction > DefaultDistanceFraction;
    const TSet<FVector> &ContainmentPoints = bCanSkipSurface ? HullPoints : Unique;
    TMap<FIntPoint, FIntPoint> Edges;
    TMap<int32, int32> VertexIds;
    FJsonValues Vertices, Faces;
    auto ExportVertex = [&](int32 SourceIndex)
    {
        if (const int32 *Existing = VertexIds.Find(SourceIndex))
            return *Existing;
        const FVector &Point = Points[SourceIndex];
        const int32 NewIndex = Vertices.Add(Row(Point.X, Point.Y, Point.Z));
        VertexIds.Add(SourceIndex, NewIndex);
        return NewIndex;
    };
    int32 FaceIndex = 0;
    for (const auto &Tri : Hull.GetTriangles())
    {
        if ((FaceIndex++ % 64) == 0)
            Progress.TickProgress();
        int32 Ids[3] = {Tri.A, Tri.B, Tri.C};
        const FVector &A = Points[Ids[0]];
        FVector Normal;
        if (!WyHullFaceNormal(A, Points[Ids[1]], Points[Ids[2]], Normal))
            return nullptr;
        // The wire protocol is already in UE coordinates; keep the outward winding.
        if (FVector::DotProduct(Normal, Interior - A) > 0)
        {
            Swap(Ids[1], Ids[2]);
            Normal = -Normal;
        }
        for (const auto &P : ContainmentPoints)
            if (FVector::DotProduct(Normal, P - A) > Tolerance)
                return nullptr;
        for (int32 J = 0; J < 3; ++J)
        {
            const int32 V = Ids[J], W = Ids[(J + 1) % 3];
            auto &Edge = Edges.FindOrAdd({FMath::Min(V, W), FMath::Max(V, W)}, FIntPoint(0, 0));
            ++Edge.X;
            Edge.Y += V < W ? 1 : -1;
        }
        const int32 I0 = ExportVertex(Ids[0]), I1 = ExportVertex(Ids[1]), I2 = ExportVertex(Ids[2]);
        Faces.Add(Row(I0, I1, I2));
    }
    if (Vertices.Num() - Edges.Num() + Faces.Num() != 2)
        return nullptr;
    for (const auto &Pair : Edges)
        if (Pair.Value.X != 2 || Pair.Value.Y != 0)
            return nullptr;
    auto Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("name"), Name);
    Out->SetArrayField(TEXT("vertices"), Vertices);
    Out->SetArrayField(TEXT("faces"), Faces);
    return Out;
}

struct FSplitPlane
{
    FVector Axis = FVector::UpVector;
    double WorldHeight = 0, WorldOriginZ = 0;
    int32 DominantAxis = 2;

    double HeightDelta(const FVector &Point) const
    {
        return FVector::DotProduct(Axis, Point) + (WorldOriginZ - WorldHeight);
    }

    FVector Intersect(const FVector &A, const FVector &B, double DeltaA, double DeltaB) const
    {
        // Give both windings of a shared edge the same calculation and snap the
        // best-conditioned local component back to the world-space cutting plane.
        const bool AIsLower = DeltaA < 0;
        const double Lo = AIsLower ? -DeltaA : -DeltaB, Hi = AIsLower ? DeltaB : DeltaA;
        const double Alpha = Lo < Hi ? (Lo / Hi) / (1 + Lo / Hi) : 1 / (1 + Hi / Lo);
        FVector Point = FMath::Lerp(AIsLower ? A : B, AIsLower ? B : A, Alpha);
        double Remaining = WorldHeight - WorldOriginZ;
        for (int32 I = 0; I < 3; ++I)
            if (I != DominantAxis)
                Remaining -= Axis[I] * Point[I];
        Point[DominantAxis] = Remaining / Axis[DominantAxis];
        return Point;
    }
};

static bool ReadWorldZ(const FObject &Settings, FSplitPlane &Plane)
{
    if (!Settings->HasField(TEXT("worldZ")))
        return true;
    const FJsonValues *Values = nullptr;
    if (!Settings->TryGetArrayField(TEXT("worldZ"), Values) || Values->Num() != 4)
        return false;
    double Coefficients[4];
    for (int32 I = 0; I < 4; ++I)
    {
        const auto &Value = (*Values)[I];
        if (!Value || Value->Type != EJson::Number || !Value->TryGetNumber(Coefficients[I]) ||
            !FMath::IsFinite(Coefficients[I]))
            return false;
    }
    Plane.Axis = FVector(Coefficients[0], Coefficients[1], Coefficients[2]);
    Plane.WorldOriginZ = Coefficients[3];
    for (int32 I = 0; I < 3; ++I)
        if (FMath::Abs(Plane.Axis[I]) > FMath::Abs(Plane.Axis[Plane.DominantAxis]))
            Plane.DominantAxis = I;
    return Plane.Axis[Plane.DominantAxis] != 0;
}

static FObject Build(const FJsonValues &PartsInput, const TOptional<FSplitPlane> &Plane,
                     const FHullSettings &Settings)
{
    FScopedSlowTask Progress(4.f, NSLOCTEXT("WyVehicleHulls", "Build", "构建车身凸包"));
    const float WorkPerPart = 1.f / FMath::Max(1, PartsInput.Num());
    TArray<FPart> Parts;
    double MinZ = TNumericLimits<double>::Max(), MaxZ = TNumericLimits<double>::Lowest();
    for (const auto &V : PartsInput)
    {
        Progress.EnterProgressFrame(WorkPerPart, NSLOCTEXT("WyVehicleHulls", "ReadParts", "读取 LOD0 网格"));
        FPart P;
        if (!ReadPart(V, P, true))
            return HullError(TEXT("Invalid mesh or triangle indices"));
        if (P.Kind == TEXT("body"))
            for (const FIntVector &T : P.Faces)
                for (int K = 0; K < 3; ++K)
                {
                    const FVector &Point = P.Vertices[T[K]];
                    const double Z = Plane.IsSet() ? Plane->HeightDelta(Point) : Point.Z;
                    if (!FMath::IsFinite(Z))
                        return HullError(TEXT("车身顶点转换为场景高度失败。"));
                    MinZ = FMath::Min(MinZ, Z);
                    MaxZ = FMath::Max(MaxZ, Z);
                }
        Parts.Add(MoveTemp(P));
    }
    if (MaxZ < MinZ)
        return HullError(TEXT("No body triangles"));
    if (Plane.IsSet() && (0 <= MinZ || 0 >= MaxZ))
        return HullError(TEXT("切面高度必须位于车身最低点和最高点之间。"));
    TSet<FVector> Body, Spoiler, Upper, Lower;
    for (const FPart &P : Parts)
    {
        Progress.EnterProgressFrame(WorkPerPart, FText::Format(
            NSLOCTEXT("WyVehicleHulls", "CollectPoints", "收集 {0} 的凸包顶点"), FText::FromString(P.Name)));
        for (const FIntVector &Face : P.Faces)
            for (int K = 0; K < 3; ++K)
            {
                const FVector V = P.Vertices[Face[K]];
                if (P.Kind == TEXT("spoiler"))
                {
                    Spoiler.Add(V);
                    continue;
                }
                if (!Plane.IsSet())
                {
                    Body.Add(V);
                    continue;
                }
                const double DeltaV = Plane->HeightDelta(V);
                if (DeltaV >= 0)
                    Upper.Add(V);
                if (DeltaV <= 0)
                    Lower.Add(V);
                const FVector W = P.Vertices[Face[(K + 1) % 3]];
                const double DeltaW = Plane->HeightDelta(W);
                if ((DeltaV < 0 && DeltaW > 0) || (DeltaV > 0 && DeltaW < 0))
                {
                    const FVector Intersection = Plane->Intersect(V, W, DeltaV, DeltaW);
                    Upper.Add(Intersection);
                    Lower.Add(Intersection);
                }
            }
    }
    FJsonValues Hulls;
    const int32 HullCount = (Plane.IsSet() ? 2 : 1) + (Spoiler.IsEmpty() ? 0 : 1);
    auto Add = [&](const TCHAR *Name, const TSet<FVector> &Points, const FText &Message)
    {
        Progress.EnterProgressFrame(2.f / HullCount, Message);
        const auto Hull = MakeHull(Name, Points, Settings, Progress);
        if (!Hull)
            return false;
        Hulls.Add(MakeShared<FJsonValueObject>(Hull));
        return true;
    };
    if (!Plane.IsSet() && !Add(TEXT("Body"), Body, NSLOCTEXT("WyVehicleHulls", "Body", "构建整体车身凸包")))
        return HullError(TEXT("Body has no 3D convex hull"));
    if (Plane.IsSet() && (!Add(TEXT("BodyLower"), Lower, NSLOCTEXT("WyVehicleHulls", "Lower", "构建车身下层凸包")) ||
                          !Add(TEXT("BodyUpper"), Upper, NSLOCTEXT("WyVehicleHulls", "Upper", "构建车身上层凸包"))))
        return HullError(TEXT("Body half has no 3D convex hull"));
    if (!Spoiler.IsEmpty() && !Add(TEXT("Spoiler"), Spoiler, NSLOCTEXT("WyVehicleHulls", "Spoiler", "构建尾翼凸包")))
        return HullError(TEXT("Spoiler has no 3D convex hull"));
    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("ok"), true);
    R->SetArrayField(TEXT("hulls"), Hulls);
    return R;
}

} // namespace WyHullCore

TSharedPtr<FJsonObject> WyBuildVehicleHulls(const TSharedPtr<FJsonObject> &Request)
{
    using namespace WyHullCore;
    const FJsonValues *Items = nullptr;
    const TSharedPtr<FJsonObject> *Settings = nullptr;
    if (!Request || !Request->TryGetArrayField(TEXT("items"), Items) ||
        !Request->TryGetObjectField(TEXT("settings"), Settings))
        return HullError(TEXT("hulls requires items and settings"));
    FString Mode;
    if (!(*Settings)->TryGetStringField(TEXT("mode"), Mode) || (Mode != TEXT("single") && Mode != TEXT("split")))
        return HullError(TEXT("Invalid hull settings"));
    FHullSettings Simplification;
    FString Error;
    if (!ReadSimplification(Settings->ToSharedRef(), Simplification, Error))
        return HullError(Error);
    FSplitPlane Split;
    if (!ReadWorldZ(Settings->ToSharedRef(), Split))
        return HullError(TEXT("场景高度变换（worldZ）需要四个有限数值及非零的 Z 轴投影。"));
    TOptional<FSplitPlane> Cut;
    if (Mode == TEXT("split"))
    {
        FString Plane;
        if (!(*Settings)->TryGetStringField(TEXT("plane"), Plane) || Plane != TEXT("manual"))
            return HullError(TEXT("上下分层需要手动指定切面高度。"));
        if (!(*Settings)->HasTypedField<EJson::Number>(TEXT("splitZ")) ||
            !(*Settings)->TryGetNumberField(TEXT("splitZ"), Split.WorldHeight) || !FMath::IsFinite(Split.WorldHeight))
            return HullError(TEXT("上下分层需要有效的数值切面高度（splitZ）。"));
        Cut = Split;
    }
    static const TSet<FString> ExcludedParts = {TEXT("Tire"), TEXT("Hub"), TEXT("Brake"), TEXT("BrakeF"),
        TEXT("BrakeR"), TEXT("BrakeB")};
    FJsonValues Parts;
    for (const auto &Value : *Items)
    {
        const TSharedPtr<FJsonObject> *Item = nullptr;
        if (!Value || !Value->TryGetObject(Item))
            return HullError(TEXT("Invalid hull input item"));
        FString Kind, Name;
        double Lod;
        if (!(*Item)->TryGetStringField(TEXT("kind"), Kind) ||
            (Kind != TEXT("staticMesh") && Kind != TEXT("skeletalPart") && Kind != TEXT("hullInput")))
            continue;
        if (!(*Item)->TryGetNumberField(TEXT("lod"), Lod) || Lod != 0)
            continue;
        if (!(*Item)->TryGetStringField(TEXT("name"), Name))
            return HullError(TEXT("Mesh name missing"));
        FString PartName = Name;
        PartName.RemoveFromStart(TEXT("LOD0_"));
        PartName = WyVehiclePartBase(PartName);
        if (PartName.StartsWith(TEXT("Mirror"), ESearchCase::CaseSensitive) || ExcludedParts.Contains(PartName))
            continue;
        const TSharedPtr<FJsonObject> *Data = nullptr;
        const FJsonValues *Positions = nullptr, *Triangles = nullptr;
        if (!(*Item)->TryGetObjectField(TEXT("data"), Data) ||
            !(*Data)->TryGetArrayField(TEXT("positions"), Positions) ||
            !(*Data)->TryGetArrayField(TEXT("triangles"), Triangles))
            return HullError(TEXT("Mesh positions/triangles missing"));
        auto Part = MakeShared<FJsonObject>();
        Part->SetStringField(TEXT("name"), Name);
        Part->SetStringField(TEXT("kind"), PartName == TEXT("Spoiler") ? TEXT("spoiler") : TEXT("body"));
        Part->SetArrayField(TEXT("vertices"), *Positions);
        Part->SetArrayField(TEXT("faces"), *Triangles);
        Parts.Add(MakeShared<FJsonValueObject>(Part));
    }
    auto Built = Build(Parts, Cut, Simplification);
    if (!Built->GetBoolField(TEXT("ok")))
        return Built;
    FJsonValues Shapes;
    for (const auto &Value : Built->GetArrayField(TEXT("hulls")))
    {
        const auto Old = Value->AsObject();
        auto Shape = MakeShared<FJsonObject>();
        Shape->SetStringField(TEXT("name"), Old->GetStringField(TEXT("name")));
        Shape->SetArrayField(TEXT("positions"), Old->GetArrayField(TEXT("vertices")));
        Shape->SetArrayField(TEXT("triangles"), Old->GetArrayField(TEXT("faces")));
        Shapes.Add(MakeShared<FJsonValueObject>(Shape));
    }
    auto Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("shapes"), Shapes);
    auto Item = MakeShared<FJsonObject>();
    Item->SetStringField(TEXT("id"), TEXT("BodyCollider"));
    Item->SetStringField(TEXT("name"), TEXT("BodyCollider"));
    Item->SetStringField(TEXT("kind"), TEXT("physics"));
    Item->SetNumberField(TEXT("lod"), 0);
    Item->SetObjectField(TEXT("data"), Data);
    auto Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("ok"), true);
    Result->SetBoolField(TEXT("hullSimplification"), true);
    Result->SetArrayField(TEXT("items"), {MakeShared<FJsonValueObject>(Item)});
    if (Cut.IsSet())
        Result->SetNumberField(TEXT("splitZ"), Cut->WorldHeight);
    if ((*Settings)->HasField(TEXT("worldZ")))
        Result->SetArrayField(TEXT("worldZ"), (*Settings)->GetArrayField(TEXT("worldZ")));
    return Result;
}
