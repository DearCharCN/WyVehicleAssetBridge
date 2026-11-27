#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// The cross product is twice the triangle area (cm²). FVector::Normalize's
// default squared-length threshold rejects valid narrow facets on vehicle hulls.
inline bool WyHullFaceNormal(const FVector& A, const FVector& B, const FVector& C, FVector& Normal)
{
    Normal = FVector::CrossProduct(B - A, C - A);
    const double TwiceArea = Normal.Size();
    if (!FMath::IsFinite(TwiceArea) || TwiceArea <= 1.e-15)
        return false;
    Normal /= TwiceArea;
    return true;
}

// Must run on the editor game thread. Writes modify live assets and participate
// in the editor undo transaction; packages are deliberately not saved here.
TSharedPtr<FJsonObject> WyHandleAssets(const TSharedPtr<FJsonObject> &Request);
TSharedPtr<FJsonObject> WyBuildVehicleHulls(const TSharedPtr<FJsonObject> &Request);

// Scene nodes and wheel component names use the logical part; only UE assets carry SM_.
inline FString WyStaticMeshPartName(FString AssetName)
{
    AssetName.RemoveFromStart(TEXT("SM_"), ESearchCase::CaseSensitive);
    return AssetName;
}

// All model-part classifiers use the same optional four-digit variation suffix.
inline FString WyVehiclePartBase(const FString& Name)
{
    const int32 Separator = Name.Len() - 5;
    if (Separator <= 0 || Name[Separator] != TCHAR('_'))
        return Name;
    for (int32 I = Separator + 1; I < Name.Len(); ++I)
        if (Name[I] < TCHAR('0') || Name[I] > TCHAR('9'))
            return Name;
    return Name.Left(Separator);
}
