#include "MaxSceneChanges.h"
#include "MaxScene.h"
#include <max.h>
#include <ISceneEventManager.h>
#include <stdexcept>

class WyMaxSceneChanges::Observer final : public INodeEventCallback
{
  public:
    Observer() : context_(WyReadSceneContext()), manager_(GetISceneEventManager())
    {
        if (!manager_ || !(key_ = manager_->RegisterCallback(this, TRUE)))
            throw std::runtime_error("无法监听 Max 场景更改。");
        manager_->ResetMessages(key_);
    }
    ~Observer()
    {
        manager_->UnRegisterCallback(key_);
    }
    void Check()
    {
        manager_->TriggerMessages(key_);
        if (changed_ || context_ != WyReadSceneContext())
            throw std::runtime_error("任务期间 Max 场景数据已更改，请重新比对。");
    }
    void AcceptOwnChanges()
    {
        // Only a mutation initiated by the bridge may advance the baseline.
        // Its undo record and dependent-object notifications belong to that same edit.
        manager_->ResetMessages(key_);
        changed_ = false;
        context_ = WyReadSceneContext();
    }

    void Added(NodeKeyTab&) override { changed_ = true; }
    void Deleted(NodeKeyTab&) override { changed_ = true; }
    void LinkChanged(NodeKeyTab&) override { changed_ = true; }
    void GroupChanged(NodeKeyTab&) override { changed_ = true; }
    void HierarchyOtherEvent(NodeKeyTab&) override { changed_ = true; }
    void NameChanged(NodeKeyTab&) override { changed_ = true; }
    void ModelStructured(NodeKeyTab&) override { changed_ = true; }
    void GeometryChanged(NodeKeyTab&) override { changed_ = true; }
    void TopologyChanged(NodeKeyTab&) override { changed_ = true; }
    void MappingChanged(NodeKeyTab&) override { changed_ = true; }
    void ExtentionChannelChanged(NodeKeyTab&) override { changed_ = true; }
    void ModelOtherEvent(NodeKeyTab&) override { changed_ = true; }
    void ControllerStructured(NodeKeyTab&) override { changed_ = true; }
    void ControllerOtherEvent(NodeKeyTab&) override { changed_ = true; }
    // Selection, viewport display and material changes do not modify bridge geometry.

  private:
    nlohmann::json context_;
    ISceneEventManager* manager_;
    ISceneEventManager::CallbackKey key_ = 0;
    bool changed_ = false;
};

WyMaxSceneChanges::WyMaxSceneChanges() : observer_(std::make_unique<Observer>()) {}
WyMaxSceneChanges::~WyMaxSceneChanges() = default;
void WyMaxSceneChanges::Check() { observer_->Check(); }
void WyMaxSceneChanges::AcceptOwnChanges() { observer_->AcceptOwnChanges(); }
