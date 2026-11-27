#pragma once
#include <Windows.h>

namespace WyVehicleAssetBridgeUI
{
// These entry points run on 3ds Max's main thread. Initialize is idempotent and
// registers the action/menu before NOTIFY_CUI_REGISTER_MENUS is dispatched.
void Initialize(HINSTANCE module);
void Shutdown();
void Show();
} // namespace WyVehicleAssetBridgeUI
