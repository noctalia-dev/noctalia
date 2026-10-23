#include "auth/fingerprint_authenticator.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <sdbus-c++/Error.h>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <utility>
#include <vector>

namespace {
  constexpr Logger kLog("fingerprint");

  const sdbus::ServiceName kFprintBusName{"net.reactivated.Fprint"};
  const sdbus::ObjectPath kManagerPath{"/net/reactivated/Fprint/Manager"};
  constexpr auto kManagerInterface = "net.reactivated.Fprint.Manager";
  constexpr auto kDeviceInterface = "net.reactivated.Fprint.Device";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  const sdbus::ServiceName kLoginBusName{"org.freedesktop.login1"};
  const sdbus::ObjectPath kLoginPath{"/org/freedesktop/login1"};
  constexpr auto kLoginManagerInterface = "org.freedesktop.login1.Manager";

  constexpr int kMaxRetries = 3;
  constexpr auto kRetryDelay = std::chrono::milliseconds(250);

  enum class MatchResult {
    Invalid,
    NoMatch,
    Matched,
    Retry,
    SwipeTooShort,
    FingerNotCentered,
    RemoveAndRetry,
    Disconnected,
    UnknownError,
  };

  MatchResult parseVerifyResult(const std::string& result) {
    static const std::map<std::string, MatchResult> kResults = {
        {"verify-no-match", MatchResult::NoMatch},
        {"verify-match", MatchResult::Matched},
        {"verify-retry-scan", MatchResult::Retry},
        {"verify-swipe-too-short", MatchResult::SwipeTooShort},
        {"verify-finger-not-centered", MatchResult::FingerNotCentered},
        {"verify-remove-and-retry", MatchResult::RemoveAndRetry},
        {"verify-disconnected", MatchResult::Disconnected},
        {"verify-unknown-error", MatchResult::UnknownError},
    };
    const auto it = kResults.find(result);
    return it == kResults.end() ? MatchResult::Invalid : it->second;
  }

} // namespace

// All calls use the shell's sender, but address a unique fprintd owner. In particular,
// cleanup from an old daemon can never release a new daemon's claim at the same path.
struct FingerprintAuthenticator::Session {
  std::unique_ptr<sdbus::IProxy> proxy;
  std::string owner;
  std::string path;
  std::uint64_t generation = 0;
  bool retired = false;
  bool claimPending = true;
  bool claimed = false;
  bool startPending = false;
  bool verifying = false;
  bool stopPending = false;
  bool releaseSent = false;
  bool releasePending = false;
  bool uncertain = false;
  bool ownerGone = false;
};

struct FingerprintAuthenticator::Discovery {
  std::unique_ptr<sdbus::IProxy> names;
  std::unique_ptr<sdbus::IProxy> manager;
  std::uint64_t generation = 0;
  std::string owner;
  std::string preferred;
  std::vector<sdbus::ObjectPath> paths;
  int pending = 1;
};

namespace {
  constexpr auto kCallTimeout = std::chrono::seconds(2);
  constexpr int kDiscoveryRounds = 4;
  constexpr auto kDiscoveryDelay = std::chrono::milliseconds(500);
  const sdbus::ServiceName kDbusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kDbusPath{"/org/freedesktop/DBus"};
  constexpr auto kDbusInterface = "org.freedesktop.DBus";
} // namespace

// Survives the authenticator while its shared bus sender still has in-flight work.
// Only timer work sends cleanup or destroys proxies, never a D-Bus callback. Sleep
// pauses cleanup (not a frozen destructor-time decision); each RPC remains bounded.
struct FingerprintAuthenticator::Cleanup : std::enable_shared_from_this<Cleanup> {
  std::vector<std::unique_ptr<Session>> sessions;
  std::unique_ptr<sdbus::IProxy> login;
  std::unique_ptr<sdbus::IProxy> names;
  bool sleeping = false;

  void schedule() {
    TimerManager::instance().start(0, std::chrono::milliseconds(10), [self = shared_from_this()]() { self->work(); });
  }

  void work() {
    for (auto& s : sessions) {
      if (s->ownerGone) {
        s->claimed = false;
        s->uncertain = false;
      }
      if (sleeping
          || s->ownerGone
          || s->claimPending
          || s->startPending
          || s->stopPending
          || s->releasePending
          || s->releaseSent
          || !s->claimed)
        continue;
      s->releaseSent = true;
      try {
        // No subsequent operation will use this retired state. Sending on the same
        // sender is sufficient; do not keep a borrowed connection for its reply.
        s->proxy->callMethod("Release").onInterface(kDeviceInterface).dontExpectReply();
      } catch (const sdbus::Error& e) {
        kLog.debug("could not release retired fingerprint device: {}", e.what());
      }
    }
    std::erase_if(sessions, [](const auto& s) {
      return !s->claimPending
          && !s->startPending
          && !s->stopPending
          && !s->releasePending
          && (!s->claimed || s->releaseSent);
    });
    // RPCs are bounded; an already-claimed reader can intentionally survive a long
    // sleep until resume. Poll only local flags; never contact the reader asleep.
    if (!sessions.empty())
      schedule();
  }
};

FingerprintAuthenticator::FingerprintAuthenticator(SystemBus& bus)
    : m_bus(bus), m_cleanup(std::make_shared<Cleanup>()) {
  try {
    m_loginManager = sdbus::createProxy(m_bus.connection(), kLoginBusName, kLoginPath);
    m_loginManager->uponSignal("PrepareForSleep")
        .onInterface(kLoginManagerInterface)
        .call([this, lifetime = std::weak_ptr<int>{m_lifetime},
               cleanup = std::weak_ptr<Cleanup>{m_cleanup}](bool sleeping) {
          if (auto state = cleanup.lock())
            state->sleeping = sleeping;
          if (lifetime.expired())
            return;
          m_sleeping = sleeping;
          // Invalidate immediately, including pending Claim/VerifyStart and signals. Do not
          // touch fprintd on sleep (#3935): it may already have powered down the hardware.
          invalidate();
          if (!sleeping && m_active && !m_abort) {
            m_discoveryRounds = 0;
            discover();
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("could not monitor PrepareForSleep: {}", e.what());
  }
  m_nameProxy = sdbus::createProxy(m_bus.connection(), kDbusName, kDbusPath);
  m_nameProxy->uponSignal("NameOwnerChanged")
      .onInterface(kDbusInterface)
      .call([this, lifetime = std::weak_ptr<int>{m_lifetime}, cleanup = std::weak_ptr<Cleanup>{m_cleanup}](
                const std::string& name, const std::string& oldOwner, const std::string& owner
            ) {
        if (owner.empty() && name == oldOwner) {
          auto markGone = [&](auto& sessions) {
            for (auto& s : sessions) {
              if (s->owner == name) {
                s->ownerGone = true;
                s->claimed = false;
                s->uncertain = false;
              }
            }
          };
          if (lifetime.expired()) {
            if (auto state = cleanup.lock())
              markGone(state->sessions);
          } else {
            markGone(m_sessions);
            deferWork();
          }
        }
        if (lifetime.expired() || name != kFprintBusName)
          return;
        invalidate();
        m_owner = owner;
        // The discovery budget belongs to the lock/resume attempt, not to each
        // owner-change signal. Repeated daemon crashes cannot create an endless loop.
        if (m_active && !m_sleeping && !m_abort)
          retryDiscovery();
      });
}

FingerprintAuthenticator::~FingerprintAuthenticator() {
  stop();
  m_workTimer.stop();
  m_retryTimer.stop();
  // Discovery replies are cancelled; session replies keep only detached state alive.
  m_lifetime.reset();
  m_discoveries.clear();
  // Preserve the existing login/name subscriptions without a handoff gap. Their
  // weak lifetime guards now update only the surviving cleanup state.
  // Release already sent on the shared connection needs no further work; cancelling
  // its reply here also avoids retaining a borrowed bus for completed cleanup.
  std::erase_if(m_sessions, [](const auto& s) {
    return s->releaseSent || (!s->claimPending && !s->startPending && !s->stopPending && !s->claimed);
  });
  m_cleanup->sessions = std::move(m_sessions);
  m_cleanup->login = std::move(m_loginManager);
  m_cleanup->names = std::move(m_nameProxy);
  if (!m_cleanup->sessions.empty())
    m_cleanup->schedule();
}

void FingerprintAuthenticator::setAuthenticatedCallback(AuthenticatedCallback callback) {
  m_onAuthenticated = std::move(callback);
}

void FingerprintAuthenticator::setStatusCallback(StatusCallback callback) { m_onStatus = std::move(callback); }

void FingerprintAuthenticator::invalidate() {
  ++m_generation;
  m_retryTimer.stop();
  m_discovering = false;
  m_selecting = false;
  m_waitingForCleanup = false;
  m_candidates.clear();
  m_current = nullptr;
  for (auto& s : m_sessions) {
    s->retired = true;
    s->verifying = false;
  }
  deferWork();
}

void FingerprintAuthenticator::start() {
  if (m_active)
    return;
  m_active = true;
  m_abort = false;
  m_retries = 0;
  m_discoveryRounds = 0;
  invalidate();
  if (!m_sleeping)
    discover();
}

void FingerprintAuthenticator::stop() {
  m_active = false;
  invalidate();
  // Password completion must not wait for hardware. Release is async and per-path.
  if (!m_sleeping) {
    for (auto& s : m_sessions)
      releaseDevice(*s);
  }
}

bool FingerprintAuthenticator::current(const Session& s) const {
  return m_active
      && !m_sleeping
      && !m_abort
      && m_current == &s
      && !s.retired
      && s.generation == m_generation
      && s.owner == m_owner;
}

void FingerprintAuthenticator::discover() {
  if (!m_active || m_sleeping || m_abort || m_discovering || m_current)
    return;
  if (m_discoveryRounds >= kDiscoveryRounds)
    return;
  ++m_discoveryRounds;
  m_discovering = true;
  m_selecting = false;
  auto discovery = std::make_unique<Discovery>();
  auto* d = discovery.get();
  d->generation = m_generation;
  d->names = sdbus::createProxy(m_bus.connection(), kDbusName, kDbusPath);
  m_discoveries.push_back(std::move(discovery));
  d->names->callMethodAsync("GetNameOwner")
      .onInterface(kDbusInterface)
      .withTimeout(kCallTimeout)
      .withArguments(std::string{kFprintBusName})
      .uponReplyInvoke([this, d](std::optional<sdbus::Error> error, const std::string& owner) {
        d->pending = 0;
        if (d->generation != m_generation || !m_active || m_sleeping) {
          deferWork();
          return;
        }
        if (error) {
          // GetNameOwner does not activate an idle fprintd. Request activation without
          // blocking the shell, then let the bounded discovery retry resolve its owner.
          d->pending = 1;
          d->names->callMethodAsync("StartServiceByName")
              .onInterface(kDbusInterface)
              .withTimeout(kCallTimeout)
              .withArguments(std::string{kFprintBusName}, std::uint32_t{0})
              .uponReplyInvoke([this, d](std::optional<sdbus::Error>, std::uint32_t) {
                d->pending = 0;
                deferWork();
              });
          return;
        }
        d->owner = owner;
        d->manager = sdbus::createProxy(m_bus.connection(), sdbus::ServiceName{owner}, kManagerPath);
        d->pending = 2;
        d->manager->callMethodAsync("GetDefaultDevice")
            .onInterface(kManagerInterface)
            .withTimeout(kCallTimeout)
            .uponReplyInvoke([this, d](std::optional<sdbus::Error> e, const sdbus::ObjectPath& path) {
              if (!e)
                d->preferred = path;
              --d->pending;
              deferWork();
            });
        d->manager->callMethodAsync("GetDevices")
            .onInterface(kManagerInterface)
            .withTimeout(kCallTimeout)
            .uponReplyInvoke([this, d](std::optional<sdbus::Error> e, const std::vector<sdbus::ObjectPath>& paths) {
              if (!e)
                d->paths = paths;
              --d->pending;
              deferWork();
            });
      });
}

void FingerprintAuthenticator::retryDiscovery() {
  if (m_active && !m_sleeping && !m_abort && m_discoveryRounds < kDiscoveryRounds) {
    m_retryTimer.start(kDiscoveryDelay, [this]() { discover(); });
  } else if (!m_current) {
    emitStatus({}, false);
  }
}

void FingerprintAuthenticator::deferWork() {
  // Never destroy a proxy inside one of its own reply/signal callbacks.
  m_workTimer.start(std::chrono::milliseconds(0), [this]() {
    for (auto it = m_discoveries.begin(); it != m_discoveries.end();) {
      auto& d = **it;
      if (d.pending != 0) {
        ++it;
        continue;
      }
      if (d.generation == m_generation && m_active && !m_sleeping && !m_abort) {
        m_discovering = false;
        m_owner = d.owner;
        m_candidates.clear();
        // Prefer the default only if it was actually enumerated. Preserve manager order
        // for the remainder; object path numbers are not a freshness or liveness signal.
        if (std::ranges::find(d.paths, d.preferred) != d.paths.end())
          m_candidates.push_back(d.preferred);
        for (const auto& p : d.paths) {
          if (std::ranges::find(m_candidates, p) == m_candidates.end())
            m_candidates.push_back(p);
        }
        m_selecting = true;
      }
      it = m_discoveries.erase(it);
    }
    if (!m_sleeping) {
      for (auto& s : m_sessions)
        if (s->retired)
          releaseDevice(*s);
    }
    // Completed proxies can now be destroyed, outside the callback stack. Uncertain
    // cleanup stays as a same-path barrier for the lifetime of that daemon owner.
    std::erase_if(m_sessions, [](const auto& s) {
      return s->retired
          && !s->claimPending
          && !s->startPending
          && !s->stopPending
          && !s->releasePending
          && !s->claimed
          && !s->uncertain;
    });
    if (m_waitingForCleanup && !std::ranges::any_of(m_sessions, [this](const auto& s) {
          return s->owner == m_owner
              && s->retired
              && (s->claimPending || s->startPending || s->stopPending || s->releasePending);
        })) {
      m_waitingForCleanup = false;
      retryDiscovery();
    }
    if (m_selecting && !m_current && !m_sleeping)
      tryCandidate();
  });
}

void FingerprintAuthenticator::tryCandidate() {
  if (!m_active || m_sleeping || m_abort || m_current)
    return;
  bool cleanupPending = false;
  while (!m_candidates.empty()) {
    const auto path = m_candidates.front();
    m_candidates.erase(m_candidates.begin());
    const bool blocked = std::ranges::any_of(m_sessions, [&](const auto& s) {
      return s->owner == m_owner
          && s->path == path
          && (s->claimPending || s->claimed || s->releasePending || s->uncertain);
    });
    if (blocked) {
      cleanupPending = cleanupPending || std::ranges::any_of(m_sessions, [&](const auto& s) {
                         return s->owner == m_owner
                             && s->path == path
                             && (s->claimPending || s->startPending || s->stopPending || s->releasePending);
                       });
      continue;
    }
    auto session = std::make_unique<Session>();
    auto* s = session.get();
    s->owner = m_owner;
    s->path = path;
    s->generation = m_generation;
    s->proxy = sdbus::createProxy(m_bus.connection(), sdbus::ServiceName{s->owner}, sdbus::ObjectPath{path});
    m_current = s;
    m_sessions.push_back(std::move(session));
    s->proxy->uponSignal("VerifyStatus")
        .onInterface(kDeviceInterface)
        .call([this, s, lifetime = std::weak_ptr<int>{m_lifetime}](const std::string& result, bool done) {
          if (!lifetime.expired() && current(*s) && s->verifying)
            handleVerifyStatus(result, done);
        });
    s->proxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this, s, lifetime = std::weak_ptr<int>{m_lifetime}](
                  const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changed,
                  const std::vector<std::string>&
              ) {
          if (lifetime.expired() || !current(*s) || !s->verifying || interfaceName != kDeviceInterface)
            return;
          const auto it = changed.find("finger-present");
          if (it == changed.end() || it->second.get<bool>())
            return;
          m_retryTimer.start(kRetryDelay, [this, s]() {
            if (current(*s) && s->verifying)
              emitStatus(i18n::tr("auth.fingerprint.ready"), false);
          });
        });
    s->proxy->callMethodAsync("Claim")
        .onInterface(kDeviceInterface)
        .withTimeout(kCallTimeout)
        .withArguments(std::string{})
        .uponReplyInvoke([this, s, lifetime = std::weak_ptr<int>{m_lifetime}](std::optional<sdbus::Error> error) {
          s->claimPending = false;
          s->claimed = !error && !s->ownerGone;
          if (error) {
            const auto& name = error->getName();
            // A timeout is not proof that fprintd refused the claim. Do not Release
            // without success, but do not send another Claim on this sender/path either.
            s->uncertain = name == sdbus::Error::Name{"org.freedesktop.DBus.Error.NoReply"}
                || name == sdbus::Error::Name{"org.freedesktop.DBus.Error.Timeout"}
                || name == sdbus::Error::Name{"org.freedesktop.DBus.Error.Disconnected"};
          }
          if (s->ownerGone)
            s->uncertain = false;
          if (lifetime.expired())
            return;
          if (error || !current(*s)) {
            if (error)
              kLog.info("could not claim fingerprint device {}: {}", s->path, error->what());
            if (m_current == s)
              m_current = nullptr;
            s->retired = true;
            deferWork();
            return;
          }
          startVerify(false);
        });
    return;
  }
  m_selecting = false;
  if (cleanupPending) {
    // Do not spend all hotplug rounds while the same-path barrier is still awaiting
    // its bounded reply. Its completion will wake selection; other paths were tried.
    m_waitingForCleanup = true;
    return;
  }
  retryDiscovery();
}

void FingerprintAuthenticator::startVerify(bool isRetry) {
  auto* s = m_current;
  if (!s || !current(*s) || !s->claimed || s->startPending || s->stopPending || s->verifying)
    return;
  s->startPending = true;
  s->proxy->callMethodAsync("VerifyStart")
      .onInterface(kDeviceInterface)
      .withTimeout(kCallTimeout)
      .withArguments(std::string{"any"})
      .uponReplyInvoke([this, s, isRetry,
                        lifetime = std::weak_ptr<int>{m_lifetime}](std::optional<sdbus::Error> error) {
        s->startPending = false;
        if (lifetime.expired())
          return;
        if (!current(*s)) {
          deferWork();
          return;
        }
        if (error) {
          kLog.info("could not start fingerprint verification on {}: {}", s->path, error->what());
          s->retired = true;
          m_current = nullptr;
          deferWork();
          return;
        }
        s->verifying = true;
        m_selecting = false;
        if (isRetry)
          ++m_retries;
        emitStatus(i18n::tr("auth.fingerprint.ready"), false);
      });
}

void FingerprintAuthenticator::stopVerify(std::function<void()> continuation) {
  m_retryTimer.stop();
  auto* s = m_current;
  if (!s || !s->claimed || s->stopPending)
    return;
  s->verifying = false;
  s->stopPending = true;
  s->proxy->callMethodAsync("VerifyStop")
      .onInterface(kDeviceInterface)
      .withTimeout(kCallTimeout)
      .uponReplyInvoke([this, s, continuation = std::move(continuation),
                        lifetime = std::weak_ptr<int>{m_lifetime}](std::optional<sdbus::Error> error) {
        s->stopPending = false;
        if (lifetime.expired())
          return;
        if (error) {
          // Never re-arm a verification whose stop is uncertain.
          if (current(*s)) {
            m_abort = true;
            emitStatus({}, false);
          }
        } else if (current(*s) && continuation) {
          continuation();
        }
        deferWork();
      });
}

void FingerprintAuthenticator::releaseDevice(Session& s) {
  if (m_sleeping
      || s.ownerGone
      || !s.retired
      || s.claimPending
      || s.startPending
      || s.stopPending
      || !s.claimed
      || s.releaseSent)
    return;
  s.releaseSent = true;
  s.releasePending = true;
  // Release stops an active operation itself. Waiting for a separate VerifyStop here
  // would serialize password completion (and replacement discovery) behind old hardware.
  s.proxy->callMethodAsync("Release")
      .onInterface(kDeviceInterface)
      .withTimeout(kCallTimeout)
      .uponReplyInvoke([this, p = &s, lifetime = std::weak_ptr<int>{m_lifetime}](std::optional<sdbus::Error> error) {
        p->releasePending = false;
        p->uncertain = error.has_value() && !p->ownerGone;
        p->claimed = p->uncertain;
        if (lifetime.expired())
          return;
        if (error)
          kLog.debug("could not release fingerprint device {}: {}", p->path, error->what());
        deferWork();
      });
}

void FingerprintAuthenticator::handleVerifyStatus(const std::string& result, bool done) {
  if (!m_current || !current(*m_current) || !m_current->verifying)
    return;
  const MatchResult match = parseVerifyResult(result);
  const auto retry = [this]() { m_retryTimer.start(kRetryDelay, [this]() { startVerify(true); }); };
  switch (match) {
  case MatchResult::Matched:
    stopVerify();
    if (m_onAuthenticated)
      m_onAuthenticated();
    return;
  case MatchResult::NoMatch:
  case MatchResult::UnknownError:
    if (m_retries >= kMaxRetries) {
      stopVerify();
      m_abort = true;
      emitStatus(i18n::tr("auth.fingerprint.too-many-attempts"), true);
    } else {
      stopVerify(retry);
      emitStatus(
          i18n::tr(match == MatchResult::NoMatch ? "auth.fingerprint.no-match" : "auth.fingerprint.error"), true
      );
    }
    return;
  case MatchResult::Disconnected:
    stopVerify();
    // Includes libfprint TOO_HOT: never turn this into an automatic hardware retry.
    m_abort = true;
    emitStatus(i18n::tr("auth.fingerprint.disconnected"), true);
    return;
  case MatchResult::Retry:
    emitStatus(i18n::tr("auth.fingerprint.retry"), false);
    return;
  case MatchResult::SwipeTooShort:
    emitStatus(i18n::tr("auth.fingerprint.swipe-too-short"), false);
    return;
  case MatchResult::FingerNotCentered:
    emitStatus(i18n::tr("auth.fingerprint.not-centered"), false);
    return;
  case MatchResult::RemoveAndRetry:
    emitStatus(i18n::tr("auth.fingerprint.remove-and-retry"), false);
    return;
  case MatchResult::Invalid:
    kLog.warn("unknown fingerprint verify status: {}", result);
    if (done)
      stopVerify(retry);
    return;
  }
}

void FingerprintAuthenticator::emitStatus(const std::string& message, bool isError) {
  if (m_onStatus)
    m_onStatus(message, isError);
}
