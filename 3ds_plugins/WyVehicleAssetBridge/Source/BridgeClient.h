#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct WyColliderBuildSettings;
struct WyBridgeTask
{
    // onMain is synchronous. The dispatcher checks the scene/lifetime again
    // immediately before executing a queued SDK call on the Max UI thread.
    std::function<nlohmann::json(std::function<nlohmann::json()>)> onMain;
    std::function<void(const wchar_t*)> report;
    std::function<void()> checkpoint;
    std::function<void(std::size_t, std::size_t)> progress;
    std::function<nlohmann::json(std::function<nlohmann::json()>)> writeOnMain;
};

class WyBridgeClient
{
  public:
    WyBridgeClient();
    ~WyBridgeClient();
    WyBridgeClient(const WyBridgeClient&) = delete;
    WyBridgeClient& operator=(const WyBridgeClient&) = delete;
    // Operations run serially on the panel worker. Interrupt alone may be
    // called concurrently, to wake a worker blocked in network I/O.
    // The optional handshake notification runs on the main thread before scene comparison.
    using ConnectionChanged = std::function<void(bool, const std::string&)>;
    nlohmann::json Refresh(const WyBridgeTask& task, bool reconnect = false, const ConnectionChanged& onConnection = {});
    void SetVehicle(const nlohmann::json& vehicle, const WyBridgeTask& task);
    void Sync(const std::vector<std::string>& ids, bool toUE, const WyBridgeTask& task, bool firstCreate = false);
    void BuildCollider(const WyColliderBuildSettings& settings, const WyBridgeTask& task);
    void Interrupt();

  private:
    void Connect(const WyBridgeTask& task);
    void Disconnect();
    nlohmann::json Request(const nlohmann::json& request, const WyBridgeTask& task, bool guardLocalSource = false);
    // Only the worker replaces/closes the socket, under this mutex. The UI
    // thread can shut it down without closing or reusing its handle.
    std::mutex socketMutex_;
    std::uintptr_t socket_ = ~std::uintptr_t(0);
    bool winsock_ = false;
    // Keep only the comparison tokens between operations. A full-car snapshot
    // contains millions of corner values and is unnecessary once the tree is built.
    std::map<std::string, std::array<unsigned char, 32>> localVersions_;
    std::map<std::string, std::string> remoteVersions_;
    nlohmann::json vehicle_, hello_;
    bool compared_ = false;
};
