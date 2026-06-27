#include "scripting/plugin_source_locks.h"

#include <string>
#include <unordered_map>
#include <utility>

namespace scripting::plugin_source_locks {

  namespace {
    std::mutex gLocksMutex;
    std::unordered_map<std::string, std::shared_ptr<std::recursive_mutex>> gLocks;
  } // namespace

  SourceLock::SourceLock(std::shared_ptr<std::recursive_mutex> mutex) : m_mutex(std::move(mutex)), m_lock(*m_mutex) {}

  SourceLock acquire(std::string_view sourceName) {
    std::shared_ptr<std::recursive_mutex> sourceMutex;
    {
      std::scoped_lock guard(gLocksMutex);
      auto& slot = gLocks[std::string(sourceName)];
      if (!slot) {
        slot = std::make_shared<std::recursive_mutex>();
      }
      sourceMutex = slot;
    }
    return SourceLock(std::move(sourceMutex));
  }

} // namespace scripting::plugin_source_locks
