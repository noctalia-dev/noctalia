#include "dbus/polkit/polkit_agent.h"

#include "core/log.h"
#include "dbus/polkit/polkit_session_support.h"
#include "i18n/i18n.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>
#include <memory>
#include <optional>
#include <poll.h>
#include <pwd.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
#include <polkit/polkit.h>
#include <polkitagent/polkitagent.h>

namespace {

  constexpr Logger kLog("polkit");
  constexpr auto kAgentObjectPath = "/org/noctalia/PolkitAuthenticationAgent";

  // GObject dispatches these signal/vfunc callbacks through libffi (g_cclosure_marshal_generic),
  // so a C++ exception escaping one would unwind across the C ABI and call std::terminate. Run the
  // body inside this guard so nothing can cross the boundary; these are UI/state notifications, so
  // logging and swallowing is the correct degradation.
  template <typename F> void guardPolkitCallback(const char* name, F&& body) noexcept {
    try {
      std::forward<F>(body)();
    } catch (const std::exception& e) {
      kLog.warn("exception in polkit callback {} ({}); ignoring", name, e.what());
    } catch (...) {
      kLog.warn("unknown exception in polkit callback {}; ignoring", name);
    }
  }

  std::optional<std::string> usernameFromUid(uid_t uid) {
    passwd pwd{};
    passwd* result = nullptr;
    std::array<char, 4096> buffer{};
    const int rc = getpwuid_r(uid, &pwd, buffer.data(), buffer.size(), &result);
    if (rc != 0 || result == nullptr || result->pw_name == nullptr || result->pw_name[0] == '\0') {
      return std::nullopt;
    }
    return std::string(result->pw_name);
  }

  PolkitRequestIdentity toRequestIdentity(PolkitIdentity* identity) {
    PolkitRequestIdentity out;
    if (POLKIT_IS_UNIX_USER(identity)) {
      const auto uid = static_cast<uid_t>(polkit_unix_user_get_uid(POLKIT_UNIX_USER(identity)));
      out.kind = "unix-user";
      out.uid = static_cast<std::uint32_t>(uid);
      out.userName = usernameFromUid(uid).value_or(std::to_string(uid));
    } else if (POLKIT_IS_UNIX_GROUP(identity)) {
      out.kind = "unix-group";
      out.uid = static_cast<std::uint32_t>(polkit_unix_group_get_gid(POLKIT_UNIX_GROUP(identity)));
    }
    return out;
  }

  std::string identityDisplayName(PolkitIdentity* identity) {
    if (POLKIT_IS_UNIX_USER(identity)) {
      const auto uid = static_cast<uid_t>(polkit_unix_user_get_uid(POLKIT_UNIX_USER(identity)));
      return usernameFromUid(uid).value_or(std::to_string(uid));
    }
    if (POLKIT_IS_UNIX_GROUP(identity)) {
      return "group " + std::to_string(polkit_unix_group_get_gid(POLKIT_UNIX_GROUP(identity)));
    }
    return "unknown";
  }

  class IdentityRef {
  public:
    explicit IdentityRef(PolkitIdentity* identity = nullptr) : m_identity(identity) {
      if (m_identity != nullptr) {
        g_object_ref(m_identity);
      }
    }

    ~IdentityRef() {
      if (m_identity != nullptr) {
        g_object_unref(m_identity);
      }
    }

    IdentityRef(const IdentityRef&) = delete;
    IdentityRef& operator=(const IdentityRef&) = delete;

    IdentityRef(IdentityRef&& other) noexcept : m_identity(std::exchange(other.m_identity, nullptr)) {}

    IdentityRef& operator=(IdentityRef&& other) noexcept {
      if (this == &other) {
        return *this;
      }
      if (m_identity != nullptr) {
        g_object_unref(m_identity);
      }
      m_identity = std::exchange(other.m_identity, nullptr);
      return *this;
    }

    [[nodiscard]] PolkitIdentity* get() const noexcept { return m_identity; }

  private:
    PolkitIdentity* m_identity = nullptr;
  };

  struct InternalAuthRequest {
    std::string actionId;
    std::string message;
    std::string iconName;
    std::string cookie;
    bool isInternal = false;
    std::vector<IdentityRef> identities;
    GTask* task = nullptr;
    GCancellable* cancellable = nullptr;
    gulong cancelHandlerId = 0;
    bool finished = false;

    ~InternalAuthRequest() {
      if (cancellable != nullptr && cancelHandlerId != 0) {
        g_cancellable_disconnect(cancellable, cancelHandlerId);
      }
      if (cancellable != nullptr) {
        g_object_unref(cancellable);
      }
      if (!finished && task != nullptr) {
        g_task_return_new_error(
            task, POLKIT_ERROR, POLKIT_ERROR_CANCELLED, "%s", "Authentication request was destroyed"
        );
      }
      if (task != nullptr) {
        g_object_unref(task);
      }
    }

    void complete() {
      if (finished || task == nullptr) {
        return;
      }
      finished = true;
      g_task_return_boolean(task, TRUE);
    }

    void cancel(const char* reason) {
      if (finished || task == nullptr) {
        return;
      }
      finished = true;
      g_task_return_new_error(task, POLKIT_ERROR, POLKIT_ERROR_CANCELLED, "%s", reason);
    }
  };

  using InitiateCallback = void (*)(void*, std::unique_ptr<InternalAuthRequest>);
  using CancelCallback = void (*)(void*, InternalAuthRequest*);

} // namespace

// GObject code follows GLib naming conventions because the virtual method
// signatures are defined by libpolkit-agent.
// NOLINTBEGIN(readability-identifier-naming)

using NoctaliaPolkitListener = struct _NoctaliaPolkitListener {
  PolkitAgentListener parent_instance;
  void* owner = nullptr;
  InitiateCallback initiate = nullptr;
  CancelCallback cancel = nullptr;
  gpointer registration_handle = nullptr;
};

using NoctaliaPolkitListenerClass = struct _NoctaliaPolkitListenerClass {
  PolkitAgentListenerClass parent_class;
};

static void noctalia_polkit_listener_initiate_authentication(
    PolkitAgentListener* listener, const gchar* action_id, const gchar* message, const gchar* icon_name,
    PolkitDetails* /*details*/, const gchar* cookie, GList* identities, GCancellable* cancellable,
    GAsyncReadyCallback callback, gpointer user_data
) noexcept;
static gboolean noctalia_polkit_listener_initiate_authentication_finish(
    PolkitAgentListener* listener, GAsyncResult* result, GError** error
);
static void noctalia_polkit_request_cancelled(GCancellable* cancellable, gpointer user_data) noexcept;

G_DEFINE_TYPE(NoctaliaPolkitListener, noctalia_polkit_listener, POLKIT_AGENT_TYPE_LISTENER)

static void noctalia_polkit_listener_init(NoctaliaPolkitListener* self) {
  self->owner = nullptr;
  self->initiate = nullptr;
  self->cancel = nullptr;
  self->registration_handle = nullptr;
}

static void noctalia_polkit_listener_class_init(NoctaliaPolkitListenerClass* klass) {
  auto* listenerClass = POLKIT_AGENT_LISTENER_CLASS(klass);
  listenerClass->initiate_authentication = noctalia_polkit_listener_initiate_authentication;
  listenerClass->initiate_authentication_finish = noctalia_polkit_listener_initiate_authentication_finish;
}

static void noctalia_polkit_listener_initiate_authentication(
    PolkitAgentListener* listener, const gchar* action_id, const gchar* message, const gchar* icon_name,
    PolkitDetails* /*details*/, const gchar* cookie, GList* identities, GCancellable* cancellable,
    GAsyncReadyCallback callback, gpointer user_data
) noexcept {
  guardPolkitCallback("initiate_authentication", [&]() {
    auto* self = reinterpret_cast<NoctaliaPolkitListener*>(listener);
    auto request = std::make_unique<InternalAuthRequest>();
    request->actionId = action_id != nullptr ? action_id : "";
    request->message = message != nullptr ? message : "";
    request->iconName = icon_name != nullptr ? icon_name : "";
    request->cookie = cookie != nullptr ? cookie : "";
    request->task = g_task_new(G_OBJECT(listener), nullptr, callback, user_data);
    request->cancellable = cancellable != nullptr ? static_cast<GCancellable*>(g_object_ref(cancellable)) : nullptr;

    for (GList* item = g_list_first(identities); item != nullptr; item = g_list_next(item)) {
      auto* identity = static_cast<PolkitIdentity*>(item->data);
      if (identity == nullptr) {
        continue;
      }
      const auto duplicate = std::ranges::find_if(request->identities, [identity](const IdentityRef& existing) {
        return polkit_identity_equal(existing.get(), identity);
      });
      if (duplicate == request->identities.end()) {
        request->identities.emplace_back(identity);
      }
    }

    if (cancellable != nullptr) {
      request->cancelHandlerId =
          g_cancellable_connect(cancellable, G_CALLBACK(noctalia_polkit_request_cancelled), request.get(), nullptr);
    }

    if (self->initiate == nullptr || self->owner == nullptr) {
      request->cancel("Polkit listener is not attached");
      return;
    }
    self->initiate(self->owner, std::move(request));
  });
}

static gboolean noctalia_polkit_listener_initiate_authentication_finish(
    PolkitAgentListener* /*listener*/, GAsyncResult* result, GError** error
) {
  return g_task_propagate_boolean(G_TASK(result), error);
}

static void noctalia_polkit_request_cancelled(GCancellable* /*cancellable*/, gpointer user_data) noexcept {
  guardPolkitCallback("request_cancelled", [&]() {
    auto* request = static_cast<InternalAuthRequest*>(user_data);
    request->cancelHandlerId = 0;
    auto* source = G_IS_TASK(request->task) ? g_task_get_source_object(request->task) : nullptr;
    auto* listener = source != nullptr ? reinterpret_cast<NoctaliaPolkitListener*>(source) : nullptr;
    if (listener != nullptr && listener->cancel != nullptr && listener->owner != nullptr) {
      listener->cancel(listener->owner, request);
    }
  });
}

// NOLINTEND(readability-identifier-naming)

struct PolkitAgent::Impl {
  StateCallback stateCallback;
  ReadyCallback readyCallback;
  NoctaliaPolkitListener* listener = nullptr;
  PolkitAgentSession* session = nullptr;
  GMainContext* context = nullptr;
  GCancellable* registerCancellable = nullptr;
  bool m_nextInternal = false;
  bool starting = false;
  bool registered = false;
  std::unique_ptr<InternalAuthRequest> pending;
  PolkitIdentity* activeIdentity = nullptr;
  bool cancelling = false;

  bool responseRequired = false;
  bool responseVisible = false;
  std::string inputPrompt;
  std::string supplementaryMessage;
  bool supplementaryError = false;

  mutable std::vector<GPollFD> glibPollFds;
  mutable gint glibMaxPriority = G_PRIORITY_DEFAULT;
  mutable int glibPollTimeoutMs = -1;

  Impl() : context(g_main_context_default()) {
    listener = static_cast<NoctaliaPolkitListener*>(g_object_new(noctalia_polkit_listener_get_type(), nullptr));
    listener->owner = this;
    listener->initiate = &Impl::initiateBridge;
    listener->cancel = &Impl::cancelBridge;
  }

  ~Impl() {
    clearPending("PolkitAgent is being destroyed", true);
    if (registerCancellable != nullptr) {
      g_cancellable_cancel(registerCancellable);
      g_object_unref(registerCancellable);
      registerCancellable = nullptr;
    }
    if (listener != nullptr) {
      listener->owner = nullptr;
      listener->initiate = nullptr;
      listener->cancel = nullptr;
      if (listener->registration_handle != nullptr) {
        polkit_agent_listener_unregister(listener->registration_handle);
        listener->registration_handle = nullptr;
      }
      g_object_unref(listener);
    }
  }

  static void sessionReadyTrampoline(GObject* source, GAsyncResult* result, gpointer userData) noexcept {
    guardPolkitCallback("sessionReady", [&]() {
      auto* self = static_cast<Impl*>(userData);
      self->onSessionReady(source, result);
    });
  }

  void start() {
    if (starting || registered) {
      return;
    }
    starting = true;

    // Prefer the session id exported by the user's login/session manager. The
    // process lookup path below is asynchronous in API shape, but it still can
    // enter GLib's global worker pool before returning.
    const char* sessionId = std::getenv("XDG_SESSION_ID");
    registerCancellable = g_cancellable_new();
    if (sessionId != nullptr && sessionId[0] != '\0') {
      PolkitSubject* subject = polkit_unix_session_new(sessionId);
      beginRegisterSubject(subject, nullptr);
      return;
    }

    polkit_unix_session_new_for_process(::getpid(), registerCancellable, &Impl::sessionReadyTrampoline, this);
  }

  void onSessionReady(GObject* /*source*/, GAsyncResult* result) {
    GError* error = nullptr;

    PolkitSubject* pidSubject = polkit_unix_session_new_for_process_finish(result, &error);

    // If we were cancelled (destruction), bail out without touching members
    // beyond the cancellable cleanup that the destructor already handled.
    if (error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_clear_error(&error);
      if (pidSubject != nullptr) {
        g_object_unref(pidSubject);
      }
      return;
    }

    if (pidSubject == nullptr || error != nullptr) {
      const bool noSession = error != nullptr && polkit_session::isNoSessionForPidError(error->message);
      if (pidSubject != nullptr) {
        g_object_unref(pidSubject);
        pidSubject = nullptr;
      }
      if (noSession) {
        g_clear_error(&error);
        kLog.info("polkit: no logind session for pid; trying unix-user authentication agent");
        PolkitSubject* userSubject = POLKIT_SUBJECT(polkit_unix_user_new(static_cast<gint>(::getuid())));
        beginRegisterSubject(userSubject, nullptr);
        return;
      }
    }

    beginRegisterSubject(pidSubject, error);
  }

  void beginRegisterSubject(PolkitSubject* subject, GError* error) {
    starting = false;
    if (registerCancellable != nullptr) {
      g_object_unref(registerCancellable);
      registerCancellable = nullptr;
    }

    if (subject == nullptr || error != nullptr) {
      std::string message = error != nullptr ? error->message : "failed to create polkit session subject";
      g_clear_error(&error);
      if (subject != nullptr) {
        g_object_unref(subject);
      }
      if (readyCallback) {
        readyCallback(false, message);
      }
      return;
    }

    // Register on this thread (the shell poll / GLib default context). A worker
    // thread would attach the agent D-Bus filters to a context that never runs,
    // so password responses never reach polkit-agent-helper (PAM "conversation failed").
    GError* registerError = nullptr;
    gpointer handle = polkit_agent_listener_register(
        POLKIT_AGENT_LISTENER(listener), POLKIT_AGENT_REGISTER_FLAGS_NONE, subject, kAgentObjectPath, nullptr,
        &registerError
    );
    g_object_unref(subject);

    if (registerError != nullptr) {
      const std::string message = registerError->message;
      g_clear_error(&registerError);
      if (handle != nullptr) {
        polkit_agent_listener_unregister(handle);
      }
      if (readyCallback) {
        readyCallback(false, message);
      }
      return;
    }
    if (handle == nullptr) {
      if (readyCallback) {
        readyCallback(false, "polkit listener registration returned no handle");
      }
      return;
    }

    if (listener->registration_handle != nullptr) {
      polkit_agent_listener_unregister(listener->registration_handle);
    }
    listener->registration_handle = handle;
    registered = true;
    kLog.info("registered Polkit authentication agent at {}", kAgentObjectPath);
    if (readyCallback) {
      readyCallback(true, std::string{});
    }
  }

  static void initiateBridge(void* owner, std::unique_ptr<InternalAuthRequest> request) {
    static_cast<Impl*>(owner)->beginAuthentication(std::move(request));
  }

  static void cancelBridge(void* owner, InternalAuthRequest* request) {
    static_cast<Impl*>(owner)->cancelFromAuthority(request);
  }

  static void
  completedCallback(PolkitAgentSession* /*session*/, gboolean gainedAuthorization, gpointer userData) noexcept {
    guardPolkitCallback("completed", [&]() {
      static_cast<Impl*>(userData)->handleCompleted(gainedAuthorization != FALSE);
    });
  }

  static void
  requestCallback(PolkitAgentSession* /*session*/, gchar* request, gboolean echoOn, gpointer userData) noexcept {
    guardPolkitCallback("request", [&]() {
      static_cast<Impl*>(userData)->handleRequest(request != nullptr ? request : "", echoOn != FALSE);
    });
  }

  static void showErrorCallback(PolkitAgentSession* /*session*/, gchar* text, gpointer userData) noexcept {
    guardPolkitCallback("showError", [&]() {
      static_cast<Impl*>(userData)->setSupplementary(text != nullptr ? text : "", true);
    });
  }

  static void showInfoCallback(PolkitAgentSession* /*session*/, gchar* text, gpointer userData) noexcept {
    guardPolkitCallback("showInfo", [&]() {
      static_cast<Impl*>(userData)->setSupplementary(text != nullptr ? text : "", false);
    });
  }

  void emitStateChanged() {
    if (stateCallback) {
      stateCallback();
    }
  }

  void clearConversationState() {
    responseRequired = false;
    responseVisible = false;
    inputPrompt.clear();
    supplementaryMessage.clear();
    supplementaryError = false;
  }

  void stopSession() {
    activeIdentity = nullptr;
    if (session != nullptr) {
      g_signal_handlers_disconnect_by_data(session, this);
      g_object_unref(session);
      session = nullptr;
    }
  }

  void clearPending(const char* cancelReason = "Authentication request cancelled", bool silent = false) {
    stopSession();
    if (pending != nullptr && !pending->finished) {
      pending->cancel(cancelReason);
    }
    pending.reset();
    cancelling = false;
    clearConversationState();
    if (!silent) {
      emitStateChanged();
    }
  }

  void beginAuthentication(std::unique_ptr<InternalAuthRequest> request) {
    if (pending != nullptr) {
      clearPending("Replaced by a newer authentication request", true);
    }

    request->isInternal = m_nextInternal;
    m_nextInternal = false;

    if (request->identities.empty()) {
      kLog.warn("polkit request \"{}\" has no identities", request->actionId);
      request->cancel("Authentication request has no identities");
      return;
    }

    pending = std::move(request);
    clearConversationState();
    if (!startSession()) {
      kLog.warn("polkit session startup failed for action \"{}\"", pending->actionId);
      clearPending("Failed to start authentication session");
      return;
    }
    emitStateChanged();
  }

  bool startSession() {
    if (pending == nullptr) {
      return false;
    }

    stopSession();

    // Prefer the current user when several wheel members are offered.
    PolkitIdentity* chosen = nullptr;
    const auto selfUid = static_cast<gint>(::geteuid());
    for (const IdentityRef& identity : pending->identities) {
      if (!POLKIT_IS_UNIX_USER(identity.get())) {
        continue;
      }
      if (polkit_unix_user_get_uid(POLKIT_UNIX_USER(identity.get())) == selfUid) {
        chosen = identity.get();
        break;
      }
    }
    if (chosen == nullptr) {
      for (const IdentityRef& identity : pending->identities) {
        if (POLKIT_IS_UNIX_USER(identity.get())) {
          chosen = identity.get();
          break;
        }
      }
    }
    if (chosen == nullptr) {
      kLog.warn("polkit action \"{}\" has no unix-user identity (unix-group alone is unsupported)", pending->actionId);
      return false;
    }

    activeIdentity = chosen;
    session = polkit_agent_session_new(activeIdentity, pending->cookie.c_str());
    if (session == nullptr) {
      return false;
    }

    g_signal_connect(G_OBJECT(session), "completed", G_CALLBACK(completedCallback), this);
    g_signal_connect(G_OBJECT(session), "request", G_CALLBACK(requestCallback), this);
    g_signal_connect(G_OBJECT(session), "show-error", G_CALLBACK(showErrorCallback), this);
    g_signal_connect(G_OBJECT(session), "show-info", G_CALLBACK(showInfoCallback), this);

    polkit_agent_session_initiate(session);
    return true;
  }

  void handleRequest(const std::string& prompt, bool echoOn) {
    inputPrompt = prompt.empty() ? i18n::tr("auth.polkit.default-message") : prompt;
    responseVisible = echoOn;
    responseRequired = true;
    emitStateChanged();
  }

  void setSupplementary(const std::string& text, bool isError) {
    supplementaryMessage = text;
    supplementaryError = isError;
    emitStateChanged();
  }

  void handleCompleted(bool gainedAuthorization) {
    if (pending == nullptr) {
      return;
    }

    if (gainedAuthorization) {
      kLog.info(
          "polkit action \"{}\" authorized as {}", pending->actionId,
          activeIdentity != nullptr ? identityDisplayName(activeIdentity) : std::string("unknown")
      );
      pending->complete();
      pending.reset();
      stopSession();
      clearConversationState();
      emitStateChanged();
      return;
    }

    if (cancelling) {
      clearPending("Authentication request cancelled");
      return;
    }

    responseRequired = false;
    responseVisible = false;
    inputPrompt.clear();
    supplementaryMessage = i18n::tr("auth.polkit.invalid-password");
    supplementaryError = true;
    emitStateChanged();

    if (!startSession()) {
      kLog.warn("polkit session restart failed for action \"{}\"", pending->actionId);
      clearPending("Failed to restart authentication session");
    }
  }

  void cancelFromAuthority(InternalAuthRequest* request) {
    if (pending == nullptr || pending.get() != request) {
      return;
    }
    cancelling = true;
    clearPending("Authentication request cancelled by polkit");
  }

  void submitResponse(const std::string& response) {
    if (pending == nullptr || session == nullptr || !responseRequired) {
      return;
    }
    // Empty responses make pam_unix report "conversation failed" / "auth could not identify password".
    if (response.empty()) {
      return;
    }
    polkit_agent_session_response(session, response.c_str());
    responseRequired = false;
    inputPrompt.clear();
    supplementaryMessage = i18n::tr("auth.polkit.authenticating");
    supplementaryError = false;
    emitStateChanged();

    while (g_main_context_pending(context)) {
      g_main_context_iteration(context, FALSE);
    }
  }

  void cancelRequest() {
    if (pending == nullptr) {
      return;
    }
    cancelling = true;
    if (session != nullptr) {
      polkit_agent_session_cancel(session);
    }
    clearPending("Authentication request cancelled by user");
  }

  void addPollFds(std::vector<pollfd>& fds) const {
    glibPollFds.clear();
    glibMaxPriority = G_PRIORITY_DEFAULT;
    glibPollTimeoutMs = -1;

    if (!g_main_context_acquire(context)) {
      return;
    }

    const gboolean ready = g_main_context_prepare(context, &glibMaxPriority);
    gint timeout = -1;
    const gint count = g_main_context_query(context, glibMaxPriority, &timeout, nullptr, 0);
    glibPollTimeoutMs = ready ? 0 : timeout;
    if (count > 0) {
      glibPollFds.resize(static_cast<std::size_t>(count));
      g_main_context_query(context, glibMaxPriority, &timeout, glibPollFds.data(), count);
      glibPollTimeoutMs = ready ? 0 : timeout;
      for (const GPollFD& glibFd : glibPollFds) {
        fds.push_back({.fd = glibFd.fd, .events = static_cast<short>(glibFd.events), .revents = 0});
      }
    }
    g_main_context_release(context);
  }

  int pollTimeoutMs() const {
    if (starting) {
      return glibPollTimeoutMs < 0 ? 100 : std::min(glibPollTimeoutMs, 100);
    }
    return glibPollTimeoutMs;
  }

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
    if (!g_main_context_acquire(context)) {
      return;
    }

    for (std::size_t i = 0; i < glibPollFds.size(); ++i) {
      const std::size_t pollIndex = startIdx + i;
      glibPollFds[i].revents =
          pollIndex < fds.size() ? static_cast<gushort>(fds[pollIndex].revents) : static_cast<gushort>(0);
    }

    const gboolean ready =
        g_main_context_check(context, glibMaxPriority, glibPollFds.data(), static_cast<gint>(glibPollFds.size()));
    g_main_context_release(context);

    if (ready) {
      if (!g_main_context_acquire(context)) {
        return;
      }
      g_main_context_dispatch(context);
      g_main_context_release(context);
    }

    while (g_main_context_pending(context)) {
      g_main_context_iteration(context, FALSE);
    }
  }

  PolkitRequest pendingRequest() const {
    if (pending == nullptr) {
      return {};
    }
    PolkitRequest request;
    request.actionId = pending->actionId;
    request.message = pending->message;
    request.iconName = pending->iconName;
    request.cookie = pending->cookie;
    request.isInternal = pending->isInternal;
    request.identities.reserve(pending->identities.size());
    for (const IdentityRef& identity : pending->identities) {
      request.identities.push_back(toRequestIdentity(identity.get()));
    }
    return request;
  }
};

PolkitAgent::PolkitAgent(SystemBus& /*bus*/) : m_impl(std::make_unique<Impl>()) {}

PolkitAgent::~PolkitAgent() = default;

void PolkitAgent::start() {
  if (m_impl != nullptr) {
    m_impl->start();
  }
}

void PolkitAgent::setStateCallback(StateCallback callback) {
  if (m_impl != nullptr) {
    m_impl->stateCallback = std::move(callback);
  }
}

void PolkitAgent::setReadyCallback(ReadyCallback callback) {
  if (m_impl != nullptr) {
    m_impl->readyCallback = std::move(callback);
  }
}

void PolkitAgent::submitResponse(const std::string& response) {
  if (m_impl != nullptr) {
    m_impl->submitResponse(response);
  }
}

void PolkitAgent::cancelRequest() {
  if (m_impl != nullptr) {
    m_impl->cancelRequest();
  }
}

void PolkitAgent::markNextRequestInternal() {
  if (m_impl != nullptr) {
    m_impl->m_nextInternal = true;
  }
}

void PolkitAgent::addPollFds(std::vector<pollfd>& fds) const {
  if (m_impl != nullptr) {
    m_impl->addPollFds(fds);
  }
}

int PolkitAgent::pollTimeoutMs() const { return m_impl != nullptr ? m_impl->pollTimeoutMs() : -1; }

void PolkitAgent::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (m_impl != nullptr) {
    m_impl->dispatch(fds, startIdx);
  }
}

bool PolkitAgent::hasPendingRequest() const noexcept { return m_impl != nullptr && m_impl->pending != nullptr; }

PolkitRequest PolkitAgent::pendingRequest() const {
  return m_impl != nullptr ? m_impl->pendingRequest() : PolkitRequest{};
}

bool PolkitAgent::isResponseRequired() const noexcept { return m_impl != nullptr && m_impl->responseRequired; }

bool PolkitAgent::responseVisible() const noexcept { return m_impl != nullptr && m_impl->responseVisible; }

std::string PolkitAgent::inputPrompt() const { return m_impl != nullptr ? m_impl->inputPrompt : std::string{}; }

std::string PolkitAgent::supplementaryMessage() const {
  return m_impl != nullptr ? m_impl->supplementaryMessage : std::string{};
}

bool PolkitAgent::supplementaryIsError() const noexcept { return m_impl != nullptr && m_impl->supplementaryError; }
