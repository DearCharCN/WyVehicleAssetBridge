#pragma once

#include "BridgeClient.h"
#include <windows.h>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

class WyMaxSceneChanges;
// The worker owns comparison and transport. SDK access is dispatched to the Max main thread.
class WyMaxTaskRunner final
{
  public:
    explicit WyMaxTaskRunner(WyBridgeClient& client);
    ~WyMaxTaskRunner();
    static void Register(HINSTANCE module);
    static void Unregister(HINSTANCE module);
    void Start(HWND owner, HWND host, const std::wstring& title, std::function<void(const WyBridgeTask&)> work,
               std::function<void(const std::string&)> complete, bool showProgress = true);
    void Cancel();
    void Stop();

  private:
    static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);
    void Check() const;
    nlohmann::json OnMain(std::function<nlohmann::json()> call, bool writing);
    void PostProgress();
    void ShowProgress();
    void Layout();
    void RestoreHost();
    void Finish();
    WyBridgeClient& client_;
    std::unique_ptr<WyMaxSceneChanges> sceneChanges_;
    HWND dialog_ = nullptr, bar_ = nullptr, hostToEnable_ = nullptr;
    HFONT font_ = nullptr;
    std::thread worker_;
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    std::packaged_task<nlohmann::json()> pending_;
    std::wstring phase_, displayedPhase_;
    std::size_t done_ = 0, total_ = 0;
    bool showProgress_ = true, progressPosted_ = false, marquee_ = false;
    std::function<void(const std::string&)> complete_;
    std::string error_;
};
