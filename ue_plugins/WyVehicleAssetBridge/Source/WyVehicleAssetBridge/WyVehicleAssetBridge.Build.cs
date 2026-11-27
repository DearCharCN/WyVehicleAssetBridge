using UnrealBuildTool;

public class WyVehicleAssetBridge : ModuleRules
{
    public WyVehicleAssetBridge(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "Json", "JsonUtilities",
            "UnrealEd", "AssetRegistry", "MeshDescription", "StaticMeshDescription",
            "Slate", "SlateCore", "InputCore", "ToolMenus", "ContentBrowser", "ContentBrowserData", "MaterialEditor",
            "SkeletalMeshDescription", "SkeletalMeshEditor", "PhysicsCore", "GeometryCore", "Chaos",
            "RenderCore", "RHI", "Networking", "Sockets",
            "AnimGraph", "AnimGraphRuntime", "BlueprintGraph", "KismetCompiler"
        });
    }
}
