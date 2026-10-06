#pragma once

#include <functional>
#include <memory>

class SessionBus;

class SecretCollectionProbe {
public:
  using ResultCallback = std::function<void(bool unlocked)>;

  SecretCollectionProbe(SessionBus& bus, ResultCallback callback);
  ~SecretCollectionProbe();

  SecretCollectionProbe(const SecretCollectionProbe&) = delete;
  SecretCollectionProbe& operator=(const SecretCollectionProbe&) = delete;

  void request();
  void invalidate();

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};
