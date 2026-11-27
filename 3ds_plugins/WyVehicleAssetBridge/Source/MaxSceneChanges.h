#pragma once

#include <memory>

// Held for one bridge task. All methods, including destruction, run on Max's main thread.
class WyMaxSceneChanges final
{
  public:
    WyMaxSceneChanges();
    ~WyMaxSceneChanges();
    WyMaxSceneChanges(const WyMaxSceneChanges&) = delete;
    WyMaxSceneChanges& operator=(const WyMaxSceneChanges&) = delete;

    void Check();
    void AcceptOwnChanges();

  private:
    class Observer;
    std::unique_ptr<Observer> observer_;
};
