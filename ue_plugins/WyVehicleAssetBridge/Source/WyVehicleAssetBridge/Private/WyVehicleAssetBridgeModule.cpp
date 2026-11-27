#include "Modules/ModuleManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "IPAddress.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "BridgeAssets.h"
#include "SVehicleTexturePanel.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "ToolMenus.h"
#include "Widgets/SWindow.h"

DEFINE_LOG_CATEGORY_STATIC(LogWyVehicleBridge, Log, All);

namespace
{
using FObject = TSharedPtr<FJsonObject>;
constexpr int32 DefaultPort = 51976;
constexpr int32 MaxFrame = 256 * 1024 * 1024;
constexpr int32 TickBudget = 4 * 1024 * 1024;

FText VehicleToolsTitle()
{
    return NSLOCTEXT("WyVehicleAssetBridge", "VehicleToolsTitle", "望月载具工具");
}

FObject Reply(const FString& Error = {})
{
    FObject Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("ok"), Error.IsEmpty());
    if (!Error.IsEmpty())
        Result->SetStringField(TEXT("error"), Error);
    return Result;
}

FString Encode(const FObject& Object)
{
    FString Json;
    auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
    FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
    return Json;
}

bool VehiclePath(const FObject& Request, FString& PackagePath)
{
    const FObject* Fields = nullptr;
    FString Type, Name, Variant;
    if (!Request->TryGetObjectField(TEXT("vehicle"), Fields) || !(*Fields)->TryGetStringField(TEXT("type"), Type) ||
        !(*Fields)->TryGetStringField(TEXT("name"), Name) || !(*Fields)->TryGetStringField(TEXT("variant"), Variant))
        return false;
    for (const FString* Field : {&Type, &Name, &Variant})
    {
        if (Field->IsEmpty() || Field->Len() > 128 || Field->Contains(TEXT("/")) || Field->Contains(TEXT("\\")) ||
            Field->TrimStartAndEnd() != *Field)
            return false;
    }
    PackagePath = FString::Printf(TEXT("/Game/Vehicle/%s/%s/%s_%s"), *Type, *Name, *Name, *Variant);
    return FPackageName::IsValidLongPackageName(PackagePath);
}

FObject Hello()
{
    FObject Result = Reply();
    Result->SetStringField(TEXT("service"), TEXT("WyVehicleAssetBridge"));
    Result->SetNumberField(TEXT("protocol"), 1);
    Result->SetBoolField(TEXT("meshMaterialSlots"), true);
    Result->SetBoolField(TEXT("vehicleMaterialNames"), true);
    Result->SetStringField(TEXT("project"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
    TArray<FString> Names;
    IFileManager::Get().FindFiles(Names, *(FPaths::ProjectContentDir() / TEXT("Vehicle/*")), false, true);
    TArray<FString> SubPaths;
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
        .Get()
        .GetSubPaths(TEXT("/Game/Vehicle"), SubPaths, false);
    for (const FString& Path : SubPaths)
        Names.AddUnique(FPaths::GetCleanFilename(Path));
    Names.RemoveAll([](const FString& Name) { return Name.Equals(TEXT("CommonVH"), ESearchCase::IgnoreCase); });
    Names.Sort();
    TArray<TSharedPtr<FJsonValue>> Types;
    for (const FString& Name : Names)
        Types.Add(MakeShared<FJsonValueString>(Name));
    Result->SetArrayField(TEXT("types"), Types);
    return Result;
}

FObject Read(const FObject& Request, const FString& Path)
{
    FObject Query = MakeShared<FJsonObject>();
    Query->SetStringField(TEXT("op"), TEXT("read"));
    Query->SetObjectField(TEXT("vehicle"), Request->GetObjectField(TEXT("vehicle")));
    FObject Result = WyHandleAssets(Query);
    const TArray<TSharedPtr<FJsonValue>>* Items;
    if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("items"), Items))
        return Result;
    for (const auto& Value : *Items)
    {
        FObject Item = Value->AsObject();
        if (!Item.IsValid())
            return Reply(TEXT("无效的 UE 资源快照"));
        // Token represents this editor snapshot; compare it again in the same
        // game-thread dispatch that performs the write.
        const FString Snapshot = Encode(Item);
        const FTCHARToUTF8 Bytes(*Snapshot);
        Item->SetStringField(TEXT("revision"), FMD5::HashBytes(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length()));
    }
    bool CanCreate = Items->IsEmpty();
    if (CanCreate)
    {
        // Textures and existing material instances may be supplied before the
        // vehicle is built. All other packages still protect an occupied folder.
        const auto IsInput = [](const FString& Asset, const FString& Base)
        {
            return FPaths::IsUnderDirectory(Asset, Base / TEXT("Texture")) ||
                   FPaths::IsUnderDirectory(Asset, Base / TEXT("MaterialInstance"));
        };
        TArray<FAssetData> Assets;
        FAssetRegistryModule::GetRegistry().GetAssetsByPath(FName(*Path), Assets, true);
        CanCreate = !Assets.ContainsByPredicate([&](const FAssetData& A) { return !IsInput(A.PackageName.ToString(), Path); });
        if (CanCreate)
        {
            const FString Directory = FPackageName::LongPackageNameToFilename(Path);
            TArray<FString> Files;
            IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.uasset"), true, false);
            CanCreate = !Files.ContainsByPredicate([&](const FString& File) { return !IsInput(File, Directory); });
        }
    }
    Result->SetBoolField(TEXT("empty"), CanCreate);
    return Result;
}

FObject Dispatch(const FObject& Request)
{
    FString Op, Path;
    if (!Request->TryGetStringField(TEXT("op"), Op))
        return Reply(TEXT("缺少操作名称"));
    if (Op == TEXT("hello"))
        return Hello();
    if (Op == TEXT("hulls"))
        return WyHandleAssets(Request);
    if (!VehiclePath(Request, Path))
        return Reply(TEXT("载具类型、名称或变体不是有效的 UE 路径段"));
    if (Op == TEXT("validate"))
        return Reply();
    if (Op == TEXT("read"))
        return Read(Request, Path);
    if (Op != TEXT("write"))
        return Reply(TEXT("未知操作"));

    const FObject* Expected;
    const TArray<TSharedPtr<FJsonValue>>* Changes;
    if (!Request->TryGetObjectField(TEXT("expected"), Expected) || !Request->TryGetArrayField(TEXT("items"), Changes) ||
        Changes->IsEmpty())
        return Reply(TEXT("写入前必须提供所选资源的比对版本"));
    FObject Current = Read(Request, Path);
    const TArray<TSharedPtr<FJsonValue>>* Items;
    if (!Current.IsValid() || !Current->TryGetArrayField(TEXT("items"), Items))
        return Current;
    bool CreateBlueprints = false;
    if (Request->HasField(TEXT("createVehicleBlueprints")) && !Request->TryGetBoolField(TEXT("createVehicleBlueprints"), CreateBlueprints))
        return Reply(TEXT("首次创建标记无效"));
    if (CreateBlueprints && !Current->GetBoolField(TEXT("empty")))
        return Reply(TEXT("目标车辆目录已有其他资源，请重新比对。首次创建允许预先放入 Texture 和 MaterialInstance 资源。"));
    TMap<FString, FString> Revisions;
    for (const auto& Value : *Items)
    {
        const FObject Item = Value->AsObject();
        Revisions.Add(Item->GetStringField(TEXT("id")), Item->GetStringField(TEXT("revision")));
    }
    TSet<FString> Unique;
    for (const auto& Change : *Changes)
    {
        const FObject* Item = nullptr;
        FString Id, Before;
        if (!Change->TryGetObject(Item) || !(*Item)->TryGetStringField(TEXT("id"), Id) || Unique.Contains(Id) ||
            !(*Expected)->TryGetStringField(Id, Before))
            return Reply(TEXT("写入项缺少版本或重复"));
        if (Revisions.FindRef(Id) != Before)
            return Reply(TEXT("UE 数据在比对后已改变，请重新比对"));
        Unique.Add(Id);
    }
    return WyHandleAssets(Request);
}

struct FPeer
{
    FSocket* Socket = nullptr;
    TArray<uint8> Input, Output;
    int32 FrameBytes = 0, Sent = 0;
    double LastIO = FPlatformTime::Seconds();
    bool HasRequest = false;
    void Respond()
    {
        FObject Request, Result;
        FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Input.GetData() + 4), FrameBytes);
        auto Reader = TJsonReaderFactory<>::Create(FString(Text.Length(), Text.Get()));
        if (FJsonSerializer::Deserialize(Reader, Request) && Request.IsValid())
            Result = Dispatch(Request);
        if (!Result.IsValid())
            Result = Reply(TEXT("无效的桥接请求或响应"));
        FString Json = Encode(Result);
        if (FTCHARToUTF8(*Json).Length() > MaxFrame)
            Json = Encode(Reply(TEXT("资源快照超过单次传输上限")));
        FTCHARToUTF8 Data(*Json);
        const uint32 Size = Data.Length();
        Output.SetNumUninitialized(Size + 4);
        for (int32 I = 0; I < 4; ++I)
            Output[I] = uint8(Size >> (24 - 8 * I));
        FMemory::Memcpy(Output.GetData() + 4, Data.Get(), Size);
        Input.Empty();
        FrameBytes = 0;
        Sent = 0;
        LastIO = FPlatformTime::Seconds();
        HasRequest = true;
    }
    bool Pump(ISocketSubsystem& Subsystem)
    {
        if ((!HasRequest || !Input.IsEmpty() || !Output.IsEmpty()) && FPlatformTime::Seconds() - LastIO > 300)
            return false;
        if (!Output.IsEmpty())
        {
            int32 Count = 0;
            if (!Socket->Send(Output.GetData() + Sent, FMath::Min(TickBudget, Output.Num() - Sent), Count))
                return Subsystem.GetLastErrorCode() == SE_EWOULDBLOCK;
            if (Count > 0)
            {
                Sent += Count;
                LastIO = FPlatformTime::Seconds();
            }
            if (Sent == Output.Num())
            {
                Output.Empty();
                Sent = 0;
            }
            return true;
        }
        int32 Budget = TickBudget;
        while (Budget > 0)
        {
            int32 Wanted = FrameBytes == 0 ? 4 : FrameBytes + 4;
            int32 Old = Input.Num(), Count = 0;
            int32 Amount = FMath::Min3(Wanted - Old, Budget, 65536);
            Input.AddUninitialized(Amount);
            bool Success = Socket->Recv(Input.GetData() + Old, Amount, Count);
            Input.SetNum(Old + FMath::Max(0, Count), EAllowShrinking::No);
            // UE stream Recv reports would-block as success with zero bytes.
            if (!Success)
                return false;
            if (Count == 0)
                return true;
            Budget -= Count;
            LastIO = FPlatformTime::Seconds();
            if (Input.Num() < Wanted)
                continue;
            if (FrameBytes)
            {
                Respond();
                break;
            }
            const uint32 Length =
                (uint32(Input[0]) << 24) | (uint32(Input[1]) << 16) | (uint32(Input[2]) << 8) | uint32(Input[3]);
            if (!Length || Length > MaxFrame)
                return false;
            FrameBytes = int32(Length);
        }
        return true;
    }
};
} // namespace

class FWyVehicleAssetBridgeModule final : public IModuleInterface
{
    ISocketSubsystem* Subsystem = nullptr;
    FSocket* Listener = nullptr;
    TArray<FPeer> Peers;
    FTSTicker::FDelegateHandle Ticker;
    bool bRegisteredEditorTools = false;
    TWeakPtr<SWindow> ToolsWindow;

    void OpenVehicleTools()
    {
        if (const TSharedPtr<SWindow> Existing = ToolsWindow.Pin())
        {
            if (Existing->IsWindowMinimized())
                Existing->Restore();
            Existing->BringToFront();
            return;
        }
        const TSharedRef<SWindow> Window = SNew(SWindow)
            .Title(VehicleToolsTitle())
            .ClientSize(FVector2D(1000, 600))
            .SupportsMinimize(true)
            .SupportsMaximize(true)
            [ SNew(SWyVehicleTexturePanel) ];
        ToolsWindow = Window;
        if (const TSharedPtr<SWindow> Parent = FGlobalTabmanager::Get()->GetRootWindow())
            FSlateApplication::Get().AddWindowAsNativeChild(Window, Parent.ToSharedRef());
        else
            FSlateApplication::Get().AddWindow(Window);
    }

    void RegisterToolsMenu()
    {
        FToolMenuOwnerScoped Owner(this);
        UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
        ToolsMenu->FindOrAddSection("WyVehicleTools").AddMenuEntry(
            "WyVehicleAssetBridge.OpenVehicleTools", VehicleToolsTitle(),
            NSLOCTEXT("WyVehicleAssetBridge", "OpenVehicleToolsTip", "打开当前载具的编辑工具"),
            FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FWyVehicleAssetBridgeModule::OpenVehicleTools)));
    }
    void Close(FSocket*& Socket)
    {
        if (Socket)
        {
            Socket->Close();
            Subsystem->DestroySocket(Socket);
            Socket = nullptr;
        }
    }
    bool Tick(float)
    {
        bool Pending = false;
        if (Listener->HasPendingConnection(Pending) && Pending)
        {
            FSocket* Socket = Listener->Accept(TEXT("WyVehicleAssetBridge client"));
            if (Socket && Peers.Num() < 4 && Socket->SetNonBlocking(true))
            {
                Socket->SetNoDelay(true);
                Peers.AddDefaulted_GetRef().Socket = Socket;
            }
            else
                Close(Socket);
        }
        for (int32 I = Peers.Num() - 1; I >= 0; --I)
            if (!Peers[I].Pump(*Subsystem))
            {
                Close(Peers[I].Socket);
                Peers.RemoveAtSwap(I);
            }
        return true;
    }

  public:
    void StartupModule() override
    {
        if (!GIsEditor || (IsRunningCommandlet() && !FParse::Param(FCommandLine::Get(), TEXT("WyBridgeServe"))))
            return;
        if (!IsRunningCommandlet() && FSlateApplication::IsInitialized())
        {
            UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FWyVehicleAssetBridgeModule::RegisterToolsMenu));
            bRegisteredEditorTools = true;
        }
        int32 Port = DefaultPort;
        FParse::Value(FCommandLine::Get(), TEXT("WyVehicleBridgePort="), Port);
        Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        if (!Subsystem || Port < 1 || Port > 65535)
            return;
        auto Local = Subsystem->CreateInternetAddr();
        bool Valid = false;
        Local->SetIp(TEXT("127.0.0.1"), Valid);
        Local->SetPort(Port);
        Listener = Subsystem->CreateSocket(NAME_Stream, TEXT("WyVehicleAssetBridge"), Local->GetProtocolType());
        if (!Listener || !Valid || !Listener->SetNonBlocking(true) || !Listener->Bind(*Local) || !Listener->Listen(4))
        {
            Close(Listener);
            UE_LOG(LogWyVehicleBridge, Error, TEXT("Cannot listen on 127.0.0.1:%d"), Port);
            return;
        }
        Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FWyVehicleAssetBridgeModule::Tick));
        UE_LOG(LogWyVehicleBridge, Display, TEXT("Vehicle bridge listening on 127.0.0.1:%d"), Port);
    }
    void ShutdownModule() override
    {
        if (bRegisteredEditorTools)
        {
            UToolMenus::UnRegisterStartupCallback(this);
            UToolMenus::UnregisterOwner(this);
            if (const TSharedPtr<SWindow> Window = ToolsWindow.Pin(); Window && FSlateApplication::IsInitialized())
                FSlateApplication::Get().DestroyWindowImmediately(Window.ToSharedRef());
        }
        if (Ticker.IsValid())
            FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
        for (FPeer& Peer : Peers)
            Close(Peer.Socket);
        Peers.Empty();
        Close(Listener);
    }
};
IMPLEMENT_MODULE(FWyVehicleAssetBridgeModule, WyVehicleAssetBridge)
