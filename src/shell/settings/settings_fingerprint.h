#pragma once

#include <functional>
#include <string>

class Flex;
class FingerprintManager;

namespace settings {

  struct FingerprintBlockActions {
    std::function<void()> requestRebuild;
    std::function<void()> openEnrollSheet;
  };

  // The Security page's fingerprint card: the reader (a selector when there are several), the
  // enrolled fingers with delete, and Add / Test. <pendingDelete> is the finger whose delete
  // awaits confirmation; it lives in the window so it survives content rebuilds.
  void buildFingerprintBlock(
      Flex& block, FingerprintManager& manager, std::string& pendingDelete, const FingerprintBlockActions& actions,
      float scale
  );

  // Sheet body: the finger picker, then the enrollment progress.
  void
  buildFingerprintEnrollSheet(Flex& body, FingerprintManager& manager, const std::function<void()>& close, float scale);

} // namespace settings
