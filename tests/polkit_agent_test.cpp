#include "core/files/resource_paths.h"
#include "dbus/polkit/polkit_agent.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <functional>
#include <string>
#include <unistd.h>

#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
#include <polkitagent/polkitagent.h>

namespace {

  // Use real Polkit session objects and signals, but never register with the
  // host authority or launch a privileged helper. The linker wraps only those
  // side effects so the production agent handles the entire conversation.
  PolkitAgentListener* gListener = nullptr;
  PolkitAgentSession* gSession = nullptr;
  int gStarts = 0;
  int gResponses = 0;
  std::function<void(PolkitAgentSession*)> gOnInitiate;
  std::function<void(PolkitAgentSession*)> gOnResponse;

  constexpr auto kHelperError = "Incorrect permissions on polkit-agent-helper-1 (needs to be setuid root)";

  void pump() {
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
  }

  void complete(bool authorized) { g_signal_emit_by_name(gSession, "completed", authorized ? TRUE : FALSE); }

  void prompt() { g_signal_emit_by_name(gSession, "request", "Password:", FALSE); }

  struct RequestResult {
    bool finished = false;
    bool authorized = false;
    bool cancelled = false;
  };

  void requestFinished(GObject* source, GAsyncResult* result, gpointer data) {
    auto& outcome = *static_cast<RequestResult*>(data);
    GError* error = nullptr;
    outcome.authorized =
        polkit_agent_listener_initiate_authentication_finish(POLKIT_AGENT_LISTENER(source), result, &error);
    outcome.cancelled = error != nullptr && g_error_matches(error, POLKIT_ERROR, POLKIT_ERROR_CANCELLED);
    outcome.finished = true;
    g_clear_error(&error);
  }

  void beginRequest(RequestResult& outcome, GCancellable* cancellable = nullptr) {
    auto* identity = polkit_unix_user_new(static_cast<gint>(getuid()));
    GList* identities = g_list_append(nullptr, identity);
    polkit_agent_listener_initiate_authentication(
        gListener, "org.noctalia.test", "Authenticate", "", nullptr, "test-cookie", identities, cancellable,
        requestFinished, &outcome
    );
    g_list_free(identities);
    g_object_unref(identity);
  }

  void checkFailure(const PolkitAgent& agent, const std::string& message) {
    TEST_CHECK(agent.hasPendingRequest());
    TEST_CHECK(!agent.isResponseRequired());
    TEST_CHECK(!agent.responseVisible());
    TEST_CHECK(agent.inputPrompt().empty());
    TEST_CHECK(agent.supplementaryIsError());
    TEST_CHECK(agent.supplementaryMessage() == message);
  }

  void dismiss(PolkitAgent& agent, RequestResult& outcome) {
    agent.cancelRequest();
    pump();
    TEST_CHECK(outcome.finished && outcome.cancelled && !outcome.authorized);
    TEST_CHECK(!agent.hasPendingRequest());
    TEST_CHECK(agent.supplementaryMessage().empty());
  }

  void helperFailureDoesNotLoop(PolkitAgent& agent) {
    gStarts = 0;
    gOnInitiate = [](PolkitAgentSession* session) {
      g_signal_emit_by_name(session, "show-error", kHelperError);
      // An informational message must not erase the useful failure diagnostic.
      g_signal_emit_by_name(session, "show-info", "Please wait");
      g_signal_emit_by_name(session, "completed", FALSE);
    };
    RequestResult outcome;
    beginRequest(outcome);
    pump();
    checkFailure(agent, kHelperError);
    TEST_CHECK(gStarts == 1 && !outcome.finished);
    // No input is accepted after a terminal failure.
    const int responses = gResponses;
    agent.submitResponse("ignored");
    TEST_CHECK(gResponses == responses);
    dismiss(agent, outcome);
    gOnInitiate = {};
  }

  void failureWithoutDiagnosticIsGeneric(PolkitAgent& agent) {
    gStarts = 0;
    RequestResult outcome;
    beginRequest(outcome);
    g_signal_emit_by_name(gSession, "show-info", "Waiting for fingerprint");
    complete(false);
    checkFailure(agent, "Authentication failed");
    TEST_CHECK(gStarts == 1);
    dismiss(agent, outcome);
  }

  void emptySubmissionDoesNotEnableRetry(PolkitAgent& agent) {
    gStarts = 0;
    RequestResult outcome;
    beginRequest(outcome);
    prompt();
    const int responses = gResponses;
    agent.submitResponse("");
    TEST_CHECK(gResponses == responses);
    complete(false);
    checkFailure(agent, "Authentication failed");
    TEST_CHECK(gStarts == 1);
    dismiss(agent, outcome);
  }

  void submittedResponseAllowsRetryButNotFailureLoop(PolkitAgent& agent) {
    gStarts = 0;
    RequestResult outcome;
    beginRequest(outcome);
    prompt();
    // A response can complete synchronously. Its result must not be overwritten
    // by the submitting code, and a restarted session starts without input.
    gOnResponse = [](PolkitAgentSession* session) {
      g_signal_emit_by_name(session, "show-error", "Authentication backend rejected the response");
      g_signal_emit_by_name(session, "completed", FALSE);
    };
    agent.submitResponse("test-response");
    checkFailure(agent, "Authentication backend rejected the response");
    TEST_CHECK(gStarts == 2);
    gOnResponse = {};
    // A startup failure in the retry must not reuse its predecessor's error or
    // submission state, even though the old error was displayed during retry.
    complete(false);
    checkFailure(agent, "Authentication failed");
    TEST_CHECK(gStarts == 2);
    dismiss(agent, outcome);
  }

  void retryCanSucceed(PolkitAgent& agent) {
    gStarts = 0;
    RequestResult outcome;
    beginRequest(outcome);
    prompt();
    agent.submitResponse("first-response");
    complete(false);
    TEST_CHECK(gStarts == 2);
    TEST_CHECK(agent.supplementaryMessage() == "Authentication failed");
    prompt();
    TEST_CHECK(agent.isResponseRequired());
    agent.submitResponse("second-response");
    complete(true);
    pump();
    TEST_CHECK(outcome.finished && outcome.authorized && !outcome.cancelled);
    TEST_CHECK(!agent.hasPendingRequest());
    TEST_CHECK(agent.supplementaryMessage().empty());
  }

  void authorityCanCancelFailedRequest(PolkitAgent& agent) {
    RequestResult outcome;
    auto* cancellable = g_cancellable_new();
    beginRequest(outcome, cancellable);
    complete(false);
    g_cancellable_cancel(cancellable);
    pump();
    TEST_CHECK(outcome.finished && outcome.cancelled);
    TEST_CHECK(!agent.hasPendingRequest());
    g_object_unref(cancellable);
  }

} // namespace

// The ABI of these linker wrappers is defined by libpolkit-agent.
// NOLINTBEGIN(readability-identifier-naming)
extern "C" gpointer __wrap_polkit_agent_listener_register(
    PolkitAgentListener* listener, PolkitAgentRegisterFlags, PolkitSubject*, const gchar*, GCancellable*, GError**
) {
  gListener = listener;
  return listener;
}

extern "C" void __wrap_polkit_agent_listener_unregister(gpointer) { gListener = nullptr; }

extern "C" void __wrap_polkit_agent_session_initiate(PolkitAgentSession* session) {
  // Bound the old broken implementation's recursive retries so it fails fast.
  TEST_CHECK(++gStarts < 10);
  g_clear_object(&gSession);
  gSession = static_cast<PolkitAgentSession*>(g_object_ref(session));
  if (gOnInitiate) {
    gOnInitiate(session);
  }
}

extern "C" void __wrap_polkit_agent_session_response(PolkitAgentSession* session, const gchar*) {
  ++gResponses;
  if (gOnResponse) {
    gOnResponse(session);
  }
}

extern "C" void __wrap_polkit_agent_session_cancel(PolkitAgentSession* session) {
  g_signal_emit_by_name(session, "completed", FALSE);
}
// NOLINTEND(readability-identifier-naming)

int main(int argc, char* argv[]) {
  TEST_CHECK(argc == 3);
  paths::setAssetRoots({.application = argv[1], .library = argv[2]});
  i18n::Service::instance().init("en");
  TEST_CHECK(setenv("XDG_SESSION_ID", "polkit-test", 1) == 0);
  SystemBus bus;
  {
    PolkitAgent agent(bus);
    bool ready = false;
    agent.setReadyCallback([&](bool ok, const std::string&) { ready = ok; });
    agent.start();
    TEST_CHECK(ready && gListener != nullptr);
    helperFailureDoesNotLoop(agent);
    failureWithoutDiagnosticIsGeneric(agent);
    emptySubmissionDoesNotEnableRetry(agent);
    submittedResponseAllowsRetryButNotFailureLoop(agent);
    retryCanSucceed(agent);
    authorityCanCancelFailedRequest(agent);
  }
  g_clear_object(&gSession);
  pump();
  TEST_CHECK(gListener == nullptr);
}
