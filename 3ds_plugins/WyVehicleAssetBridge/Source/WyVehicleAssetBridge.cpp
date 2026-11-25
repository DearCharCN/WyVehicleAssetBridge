#include <Windows.h>
#include <max.h>
#include <gup.h>
#include <maxversion.h>

// The package manifest targets Max 2025; reject another SDK at compile time.
#if MAX_RELEASE != 27000
#error WyVehicleAssetBridge currently requires the 3ds Max 2025 SDK.
#endif

namespace
{
HINSTANCE ModuleInstance = nullptr;

// Keep this identifier stable when implementing the bridge functionality.
const Class_ID BridgeClassId(0x13ed76a5, 0x681c4bd2);

class WyVehicleAssetBridge final : public GUP
{
public:
    DWORD Start() override
    {
        OutputDebugStringW(L"[WyVehicleAssetBridge] Native 3ds Max vehicle asset bridge 0.1.0 loaded.\n");
        return GUPRESULT_KEEP;
    }

    void Stop() override
    {
        OutputDebugStringW(L"[WyVehicleAssetBridge] Native 3ds Max vehicle asset bridge unloaded.\n");
    }

    void DeleteThis() override
    {
        delete this;
    }
};

class WyVehicleAssetBridgeClassDesc final : public ClassDesc
{
public:
    int IsPublic() override { return FALSE; }
    void* Create(BOOL /*loading*/ = FALSE) override { return new WyVehicleAssetBridge(); }
    const MCHAR* ClassName() override { return _T("WyVehicleAssetBridge"); }
    const MCHAR* NonLocalizedClassName() override { return _T("WyVehicleAssetBridge"); }
    const MCHAR* InternalName() override { return _T("WyVehicleAssetBridge"); }
    SClass_ID SuperClassID() override { return GUP_CLASS_ID; }
    Class_ID ClassID() override { return BridgeClassId; }
    const MCHAR* Category() override { return _T("WY Vehicle Tools"); }
    HINSTANCE HInstance() override { return ModuleInstance; }
};

WyVehicleAssetBridgeClassDesc BridgeClassDesc;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        ModuleInstance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) const MCHAR* LibDescription()
{
    return _T("WY Vehicle Asset Bridge 0.1.0");
}

extern "C" __declspec(dllexport) int LibNumberClasses()
{
    return 1;
}

extern "C" __declspec(dllexport) ClassDesc* LibClassDesc(int index)
{
    return index == 0 ? &BridgeClassDesc : nullptr;
}

extern "C" __declspec(dllexport) ULONG LibVersion()
{
    return VERSION_3DSMAX;
}

extern "C" __declspec(dllexport) ULONG CanAutoDefer()
{
    // GUP initialization belongs to application startup.
    return 0;
}
