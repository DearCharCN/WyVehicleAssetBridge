#pragma once

#include "CoreMinimal.h"

class UAnimBlueprint;
class USkeletalMesh;
class USkeleton;

bool WyValidateAnimBlueprintDependencies(FString& Error);
bool WyCreateAnimBlueprint(UObject* Outer, FName Name, USkeleton* Skeleton, USkeletalMesh* PreviewMesh,
                           UAnimBlueprint*& OutBlueprint, FString& Error);
