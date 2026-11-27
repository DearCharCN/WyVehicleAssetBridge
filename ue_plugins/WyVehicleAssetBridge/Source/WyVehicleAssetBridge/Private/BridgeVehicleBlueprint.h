#pragma once

#include "CoreMinimal.h"

class UAnimBlueprint;
class UBlueprint;
class USkeletalMesh;
class UStaticMesh;

UClass* WyResolveVehicleParent(FString& Error);
bool WyCreateVehicleBlueprint(UPackage* Outer, FName Name, UClass* ParentClass, USkeletalMesh* Mesh,
                              UAnimBlueprint* Anim, UStaticMesh* Hub, UStaticMesh* Tire, UStaticMesh* FrontBrake,
                              UStaticMesh* RearBrake, UBlueprint*& OutBlueprint, FString& Error);
