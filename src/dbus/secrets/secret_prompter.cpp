#include "dbus/secrets/secret_prompter.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <algorithm>
#include <deque>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// GcrSecretExchange is the only piece of GCR4 used here: it implements the
// "sx-aes-1" handshake that keeps the secret off the bus in plaintext. Everything
// else (D-Bus registration, queueing, UI) is native. Reimplementing the handshake
// by hand would mean hand-rolling the DH/AES wire format for no benefit.
#define GCR_API_SUBJECT_TO_CHANGE
#include <gcr/gcr.h>

namespace {

  constexpr Logger kLog("secret-prompter");

  const sdbus::ServiceName kBusName{"org.gnome.keyring.SystemPrompter"};
  const sdbus::ObjectPath kObjectPath{"/org/gnome/keyring/Prompter"};
  constexpr auto kPrompterInterface = "org.gnome.keyring.internal.Prompter";
  constexpr auto kCallbackInterface = "org.gnome.keyring.internal.Prompter.Callback";

  constexpr auto kMethodReady = "PromptReady";
  constexpr auto kMethodDone = "PromptDone";

  constexpr auto kTypePassword = "password";
  constexpr auto kTypeConfirm = "confirm";

  // Wire values for the PromptReady `reply` argument (gcr-dbus-constants.h).
  constexpr auto kReplyNone = "";
  constexpr auto kReplyYes = "yes";
  constexpr auto kReplyNo = "no";

  const sdbus::Error::Name kErrorFailed{"org.freedesktop.DBus.Error.Failed"};
  const sdbus::Error::Name kErrorInvalidArgs{"org.freedesktop.DBus.Error.InvalidArgs"};

  using VariantMap = std::map<std::string, sdbus::Variant>;

  // Identifies one client prompt. gcr keys on both the caller's unique bus name
  // and the object path it exported the Callback interface on, because a single
  // client may drive several prompts from different objects.
  struct CallbackId {
    std::string name;
    std::string path;

    auto operator<=>(const CallbackId&) const = default;
  };

  // Owns a GcrSecretExchange. The prompter is the side that calls begin() first:
  // our public key goes out with the initial PromptReady, the client's comes back
  // in PerformPrompt, and only then can send() encrypt a secret.
  class SecretExchange {
  public:
    SecretExchange() : m_exchange(gcr_secret_exchange_new(nullptr)) {}

    ~SecretExchange() {
      if (m_exchange != nullptr) {
        g_object_unref(m_exchange);
      }
    }

    SecretExchange(const SecretExchange&) = delete;
    SecretExchange& operator=(const SecretExchange&) = delete;

    [[nodiscard]] std::string begin() {
      if (m_exchange == nullptr) {
        return {};
      }
      return takeString(gcr_secret_exchange_begin(m_exchange));
    }

    [[nodiscard]] bool receive(const std::string& exchange) {
      return m_exchange != nullptr && gcr_secret_exchange_receive(m_exchange, exchange.c_str()) != FALSE;
    }

    // `secret` may be null: a cancelled password prompt and every confirm prompt
    // reply carry no secret, exactly as gcr's own prompter does.
    [[nodiscard]] std::string send(const char* secret) {
      if (m_exchange == nullptr) {
        return {};
      }
      return takeString(gcr_secret_exchange_send(m_exchange, secret, -1));
    }

  private:
    static std::string takeString(gchar* owned) {
      if (owned == nullptr) {
        return {};
      }
      std::string out(owned);
      g_free(owned);
      return out;
    }

    GcrSecretExchange* m_exchange = nullptr;
  };

  template <typename T> bool readInto(const sdbus::Variant& variant, T& out) {
    try {
      out = variant.get<T>();
      return true;
    } catch (const sdbus::Error& e) {
      kLog.debug("ignoring prompt property of unexpected type: {}", e.what());
      return false;
    }
  }

  void applyProperties(SecretPromptRequest& request, const VariantMap& properties) {
    for (const auto& [key, value] : properties) {
      if (key == "title") {
        readInto(value, request.title);
      } else if (key == "message") {
        readInto(value, request.message);
      } else if (key == "description") {
        readInto(value, request.description);
      } else if (key == "warning") {
        readInto(value, request.warning);
      } else if (key == "choice-label") {
        readInto(value, request.choiceLabel);
      } else if (key == "choice-chosen") {
        readInto(value, request.choiceChosen);
      } else if (key == "password-new") {
        readInto(value, request.passwordNew);
      } else if (key == "continue-label") {
        readInto(value, request.continueLabel);
      } else if (key == "cancel-label") {
        readInto(value, request.cancelLabel);
      } else if (key == "caller-window") {
        readInto(value, request.callerWindow);
      }
      // password-strength is prompter->client only; nothing to apply.
    }
  }

  // A client that has called BeginPrompting. The proxy is kept for the lifetime
  // of the registration: async PromptReady/PromptDone calls must not outlive it.
  struct Client {
    std::unique_ptr<sdbus::IProxy> proxy;
  };

  // The one prompt currently owning the screen (GCR_SYSTEM_PROMPTER_SINGLE).
  //
  // Declared here rather than nested inside Impl on purpose: clang defers parsing
  // a nested class's default member initializers to the end of the *outermost*
  // class, which leaves this type not-default-constructible for an
  // std::optional::emplace() call in an enclosing member function.
  struct Active {
    CallbackId id;
    SecretExchange exchange;
    // The client's half of the handshake has arrived, so send() may encrypt.
    bool received = false;
    // The client has acked our last PromptReady and may call PerformPrompt.
    bool ready = false;
    // A prompt is on screen awaiting the user.
    bool prompting = false;
    // choice-chosen was toggled by the user and must be reported back.
    bool choiceChanged = false;
    SecretPromptRequest request;
  };

  // Keeps a proxy alive across an in-flight PromptDone. Destroying a proxy from
  // inside its own reply handler is not safe, so entries are only marked here
  // and reaped later from ordinary call sites.
  struct PendingDone {
    std::unique_ptr<sdbus::IProxy> proxy;
    bool finished = false;
  };

} // namespace

struct SecretPrompter::Impl {
  SessionBus& bus;
  std::unique_ptr<sdbus::IObject> object;
  std::unique_ptr<sdbus::IProxy> dbusProxy;
  bool nameAcquired = false;
  StateCallback stateCallback;

  std::map<CallbackId, Client> clients;
  std::deque<CallbackId> waiting;
  std::optional<Active> active;
  std::list<PendingDone> pendingDone;

  explicit Impl(SessionBus& b) : bus(b) {}

  void notifyState() const {
    if (stateCallback) {
      stateCallback();
    }
  }

  void reapPendingDone() {
    std::erase_if(pendingDone, [](const PendingDone& entry) { return entry.finished; });
  }

  // Takes a deferred result so the method reply is written to the bus *before* the
  // first PromptReady. gcr's client (GcrSystemPrompt, i.e. what gnome-keyring and
  // seahorse use) asserts on a PromptReady that overtakes the BeginPrompting reply
  // and then refuses the session with "Another prompt is already in progress".
  void onBeginPrompting(sdbus::Result<>&& result, const sdbus::ObjectPath& callbackPath, const std::string& sender) {
    reapPendingDone();

    const CallbackId id{sender, callbackPath};
    if (clients.contains(id)) {
      result.returnError(sdbus::Error{kErrorFailed, "Already begun prompting for this prompt callback"});
      return;
    }

    Client client;
    try {
      client.proxy = sdbus::createProxy(bus.connection(), sdbus::ServiceName{id.name}, sdbus::ObjectPath{id.path});
    } catch (const sdbus::Error& e) {
      result.returnError(sdbus::Error{kErrorFailed, std::string("Cannot reach prompt callback: ") + e.what()});
      return;
    }

    kLog.debug("BeginPrompting from {}@{}", id.path, id.name);
    clients.emplace(id, std::move(client));
    waiting.push_back(id);

    result.returnResults();
    nextReady();
  }

  void onPerformPrompt(
      const sdbus::ObjectPath& callbackPath, const std::string& type, const VariantMap& properties,
      const std::string& exchange, const std::string& sender
  ) {
    const CallbackId id{sender, callbackPath};

    if (!active.has_value() || active->id != id) {
      throw sdbus::Error(kErrorFailed, "Not begun prompting for this prompt callback");
    }
    if (!active->ready) {
      throw sdbus::Error(kErrorFailed, "Already performing a prompt for this prompt callback");
    }

    SecretPromptRequest::Type promptType{};
    if (type == kTypePassword) {
      promptType = SecretPromptRequest::Type::Password;
    } else if (type == kTypeConfirm) {
      promptType = SecretPromptRequest::Type::Confirm;
    } else {
      throw sdbus::Error(kErrorInvalidArgs, "Invalid type argument");
    }

    applyProperties(active->request, properties);

    if (!active->exchange.receive(exchange)) {
      throw sdbus::Error(kErrorInvalidArgs, "Invalid secret exchange received");
    }

    kLog.debug("PerformPrompt type={} from {}@{}", type, id.path, id.name);
    active->received = true;
    active->ready = false;
    active->prompting = true;
    active->choiceChanged = false;
    active->request.type = promptType;
    notifyState();
  }

  // Same ordering constraint as BeginPrompting: the next client's PromptReady must
  // not overtake this client's StopPrompting reply.
  void onStopPrompting(sdbus::Result<>&& result, const sdbus::ObjectPath& callbackPath, const std::string& sender) {
    stopPrompting(CallbackId{sender, callbackPath}, true);
    result.returnResults();
    nextReady();
  }

  // Hands the next queued client its turn. No-op while a prompt is active.
  void nextReady() {
    if (active.has_value()) {
      return;
    }
    while (!waiting.empty()) {
      const CallbackId id = waiting.front();
      waiting.pop_front();
      if (!clients.contains(id)) {
        continue;
      }
      active.emplace();
      active->id = id;
      sendReady(kReplyNone, nullptr);
      return;
    }
  }

  // Sends PromptReady. Before the client's half of the handshake has arrived this
  // carries our public key; afterwards it carries the (possibly absent) secret.
  void sendReady(const char* reply, const char* secret) {
    if (!active.has_value()) {
      return;
    }

    const auto it = clients.find(active->id);
    if (it == clients.end() || it->second.proxy == nullptr) {
      active.reset();
      return;
    }

    const std::string payload = active->received ? active->exchange.send(secret) : active->exchange.begin();

    VariantMap changed;
    if (active->choiceChanged) {
      changed.emplace("choice-chosen", sdbus::Variant{active->request.choiceChosen});
      active->choiceChanged = false;
    }

    const CallbackId id = active->id;
    active->ready = false;

    try {
      it->second.proxy->callMethodAsync(kMethodReady)
          .onInterface(kCallbackInterface)
          .withArguments(std::string(reply), changed, payload)
          .uponReplyInvoke([this, id](std::optional<sdbus::Error> err) { onReadyComplete(id, err); });
    } catch (const sdbus::Error& e) {
      kLog.warn("PromptReady dispatch to {}@{} failed: {}", id.path, id.name, e.what());
      stopPrompting(id, false);
      nextReady();
    }
  }

  void onReadyComplete(const CallbackId& id, const std::optional<sdbus::Error>& err) {
    if (!err.has_value()) {
      if (active.has_value() && active->id == id) {
        active->ready = true;
      }
      return;
    }

    // The client went away or rejected the call; drop it and move the queue on.
    kLog.debug("PromptReady to {}@{} returned an error: {}", id.path, id.name, err->what());
    stopPrompting(id, false);
    nextReady();
  }

  void stopPrompting(const CallbackId& id, bool sendDone) {
    reapPendingDone();

    const auto it = clients.find(id);
    if (it == clients.end()) {
      return;
    }

    std::unique_ptr<sdbus::IProxy> proxy = std::move(it->second.proxy);
    clients.erase(it);
    std::erase(waiting, id);

    const bool wasActive = active.has_value() && active->id == id;
    if (wasActive) {
      active.reset();
    }

    if (proxy != nullptr) {
      // Park the proxy rather than let it die here. stopPrompting can run from
      // inside this very proxy's PromptReady reply handler (client vanished, call
      // rejected), and destroying a proxy from its own callback is undefined.
      // reapPendingDone() collects it later, from an unrelated call.
      auto& entry = pendingDone.emplace_back(PendingDone{.proxy = std::move(proxy), .finished = !sendDone});
      if (sendDone) {
        auto* slot = &entry;
        try {
          entry.proxy->callMethodAsync(kMethodDone)
              .onInterface(kCallbackInterface)
              .uponReplyInvoke([slot](std::optional<sdbus::Error>) { slot->finished = true; });
        } catch (const sdbus::Error& e) {
          kLog.debug("PromptDone dispatch to {}@{} failed: {}", id.path, id.name, e.what());
          entry.finished = true;
        }
      }
    }

    if (wasActive) {
      notifyState();
    }
  }

  void onNameOwnerLost(const std::string& owner) {
    std::vector<CallbackId> lost;
    for (const auto& [id, client] : clients) {
      if (id.name == owner) {
        lost.push_back(id);
      }
    }
    for (const CallbackId& id : lost) {
      kLog.debug("prompt caller {} vanished", id.name);
      stopPrompting(id, false);
    }
    if (!lost.empty()) {
      nextReady();
    }
  }
};

SecretPrompter::SecretPrompter(SessionBus& bus) : m_impl(std::make_unique<Impl>(bus)) {
  // Fails when gnome-shell or a legacy gcr-prompter already serves this session.
  m_impl->bus.connection().requestName(kBusName);
  m_impl->nameAcquired = true;

  try {
    m_impl->object = sdbus::createObject(bus.connection(), kObjectPath);
    auto* objectPtr = m_impl->object.get();

    m_impl->object
        ->addVTable(
            sdbus::registerMethod("BeginPrompting")
                .withInputParamNames("callback")
                .implementedAs([this, objectPtr](sdbus::Result<>&& result, const sdbus::ObjectPath& callback) {
                  m_impl->onBeginPrompting(
                      std::move(result), callback, objectPtr->getCurrentlyProcessedMessage().getSender()
                  );
                }),
            sdbus::registerMethod("PerformPrompt")
                .withInputParamNames("callback", "type", "properties", "exchange")
                .implementedAs([this, objectPtr](
                                   const sdbus::ObjectPath& callback, const std::string& type,
                                   const std::map<std::string, sdbus::Variant>& properties, const std::string& exchange
                               ) {
                  m_impl->onPerformPrompt(
                      callback, type, properties, exchange, objectPtr->getCurrentlyProcessedMessage().getSender()
                  );
                }),
            sdbus::registerMethod("StopPrompting")
                .withInputParamNames("callback")
                .implementedAs([this, objectPtr](sdbus::Result<>&& result, const sdbus::ObjectPath& callback) {
                  m_impl->onStopPrompting(
                      std::move(result), callback, objectPtr->getCurrentlyProcessedMessage().getSender()
                  );
                })
        )
        .forInterface(kPrompterInterface);

    // Clients are tracked by unique name; a disconnect must tear their prompt down
    // rather than leave the queue wedged behind a caller that no longer exists.
    m_impl->dbusProxy = sdbus::createProxy(
        bus.connection(), sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"}
    );
    m_impl->dbusProxy->uponSignal("NameOwnerChanged")
        .onInterface("org.freedesktop.DBus")
        .call([this](const std::string& /*name*/, const std::string& oldOwner, const std::string& newOwner) {
          if (newOwner.empty() && !oldOwner.empty()) {
            m_impl->onNameOwnerLost(oldOwner);
          }
        });

    kLog.info("listening on {}", std::string(kBusName));
  } catch (...) {
    try {
      m_impl->bus.connection().releaseName(kBusName);
    } catch (const sdbus::Error& e) {
      kLog.debug("prompter name release after init failure failed: {}", e.what());
    }
    m_impl->nameAcquired = false;
    throw;
  }
}

SecretPrompter::~SecretPrompter() {
  if (m_impl == nullptr) {
    return;
  }

  // Tell every client we are done so none of them hangs waiting on a reply.
  std::vector<CallbackId> ids;
  ids.reserve(m_impl->clients.size());
  for (const auto& [id, client] : m_impl->clients) {
    ids.push_back(id);
  }
  for (const CallbackId& id : ids) {
    m_impl->stopPrompting(id, true);
  }

  if (m_impl->nameAcquired) {
    try {
      m_impl->bus.connection().releaseName(kBusName);
    } catch (const sdbus::Error& e) {
      kLog.debug("prompter bus name release failed: {}", e.what());
    }
    m_impl->nameAcquired = false;
  }

  if (m_impl->object != nullptr) {
    try {
      m_impl->object->unregister();
    } catch (const sdbus::Error& e) {
      kLog.debug("prompter object unregister failed: {}", e.what());
    }
  }
}

void SecretPrompter::setStateCallback(StateCallback callback) {
  if (m_impl != nullptr) {
    m_impl->stateCallback = std::move(callback);
  }
}

bool SecretPrompter::hasPendingPrompt() const noexcept {
  return m_impl != nullptr && m_impl->active.has_value() && m_impl->active->prompting;
}

SecretPromptRequest SecretPrompter::pendingPrompt() const {
  if (m_impl == nullptr || !m_impl->active.has_value()) {
    return {};
  }
  return m_impl->active->request;
}

void SecretPrompter::submitPassword(const std::string& password, bool choiceChosen) {
  if (m_impl == nullptr || !hasPendingPrompt() || m_impl->active->request.type != SecretPromptRequest::Type::Password) {
    return;
  }
  Active& active = *m_impl->active;
  active.choiceChanged = active.request.choiceChosen != choiceChosen;
  active.request.choiceChosen = choiceChosen;
  active.prompting = false;
  m_impl->sendReady(kReplyYes, password.c_str());
  m_impl->notifyState();
}

void SecretPrompter::confirm(bool choiceChosen) {
  if (m_impl == nullptr || !hasPendingPrompt() || m_impl->active->request.type != SecretPromptRequest::Type::Confirm) {
    return;
  }
  Active& active = *m_impl->active;
  active.choiceChanged = active.request.choiceChosen != choiceChosen;
  active.request.choiceChosen = choiceChosen;
  active.prompting = false;
  m_impl->sendReady(kReplyYes, nullptr);
  m_impl->notifyState();
}

void SecretPrompter::cancel() {
  if (m_impl == nullptr || !hasPendingPrompt()) {
    return;
  }
  m_impl->active->prompting = false;
  m_impl->sendReady(kReplyNo, nullptr);
  m_impl->notifyState();
}
