#pragma once

#include <functional>
#include <vector>

/// Schedule a callback to run at the top of the next main loop iteration.
/// Safe to call from animation callbacks, event handlers, or anywhere else
/// where destroying the caller would be unsafe.
class DeferredCall {
public:
  static void callLater(std::function<void()> fn);
  static std::vector<std::function<void()>> takePending();
  static void drainWakeFd();
  static int wakeFd();

private:
  friend class MainLoop;
  static std::vector<std::function<void()>>& queue();
};
