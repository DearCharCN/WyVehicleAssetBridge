#include "Logging/LogMacros.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogWyVehicleAssetBridge, Log, All);

class FWyVehicleAssetBridgeModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        UE_LOG(LogWyVehicleAssetBridge, Log, TEXT("WY Vehicle Asset Bridge 0.1.0 loaded. Transport and import rules are not configured yet."));
    }

    virtual void ShutdownModule() override
    {
        UE_LOG(LogWyVehicleAssetBridge, Log, TEXT("WY Vehicle Asset Bridge unloaded."));
    }
};

IMPLEMENT_MODULE(FWyVehicleAssetBridgeModule, WyVehicleAssetBridge)
