using UnrealBuildTool;

public class WyVehicleAssetBridge : ModuleRules
{
    public WyVehicleAssetBridge(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.Add("Core");
    }
}
