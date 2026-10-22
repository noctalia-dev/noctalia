// End-to-end test for the org.gnome.keyring.SystemPrompter service.
//
// The client half is driven with the same GcrSecretExchange implementation that
// gnome-keyring, oo7 and seahorse use, and follows gcr-system-prompt.c exactly:
// receive() our opening exchange, send(NULL, 0) back inside PerformPrompt, then
// receive() the reply and read the decrypted secret. A password that survives
// that round trip is a password that survives a real libsecret caller.
//
// Everything runs single-threaded on a private bus: both connections are pumped
// by hand, so every call is dispatched asynchronously to avoid self-deadlock.

#include "dbus/secrets/secret_prompter.h"
#include "dbus/session_bus.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define GCR_API_SUBJECT_TO_CHANGE
#include <gcr/gcr.h>

namespace {

  using VariantMap = std::map<std::string, sdbus::Variant>;
  using namespace std::chrono_literals;

  int g_failures = 0;

  bool expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "secret_prompter_test: {}", message);
      ++g_failures;
    }
    return condition;
  }

  const sdbus::ServiceName kPrompterName{"org.gnome.keyring.SystemPrompter"};
  const sdbus::ObjectPath kPrompterPath{"/org/gnome/keyring/Prompter"};
  constexpr auto kPrompterInterface = "org.gnome.keyring.internal.Prompter";
  constexpr auto kCallbackInterface = "org.gnome.keyring.internal.Prompter.Callback";

  // Mirrors gcr's GcrSystemPrompt: exports the Callback object, keeps a
  // GcrSecretExchange, and records what the prompter sent it.
  class FakeClient {
  public:
    FakeClient(SessionBus& bus, std::string path)
        : m_bus(bus), m_path(std::move(path)), m_exchange(gcr_secret_exchange_new(nullptr)) {
      m_object = sdbus::createObject(bus.connection(), sdbus::ObjectPath{m_path});
      m_object
          ->addVTable(
              sdbus::registerMethod("PromptReady")
                  .withInputParamNames("reply", "properties", "exchange")
                  .implementedAs(
                      [this](const std::string& reply, const VariantMap& /*properties*/, const std::string& exchange) {
                        onPromptReady(reply, exchange);
                      }
                  ),
              sdbus::registerMethod("PromptDone").implementedAs([this]() { m_done = true; })
          )
          .forInterface(kCallbackInterface);

      m_prompter = sdbus::createProxy(bus.connection(), kPrompterName, kPrompterPath);
    }

    ~FakeClient() {
      if (m_exchange != nullptr) {
        g_object_unref(m_exchange);
      }
    }

    FakeClient(const FakeClient&) = delete;
    FakeClient& operator=(const FakeClient&) = delete;

    void beginPrompting() { callAsync("BeginPrompting", sdbus::ObjectPath{m_path}); }

    void stopPrompting() { callAsync("StopPrompting", sdbus::ObjectPath{m_path}); }

    // gcr-system-prompt.c:1185 -- once the prompter's opening exchange has been
    // received, the client answers with send(NULL, 0), never begin().
    void performPrompt(const std::string& type, const VariantMap& properties) {
      gchar* sent =
          m_received ? gcr_secret_exchange_send(m_exchange, nullptr, 0) : gcr_secret_exchange_begin(m_exchange);
      const std::string payload = sent != nullptr ? std::string(sent) : std::string();
      g_free(sent);
      callAsync("PerformPrompt", sdbus::ObjectPath{m_path}, type, properties, payload);
    }

    [[nodiscard]] int readyCount() const noexcept { return m_readyCount; }
    [[nodiscard]] const std::string& lastReply() const noexcept { return m_lastReply; }
    [[nodiscard]] const std::string& lastExchange() const noexcept { return m_lastExchange; }
    [[nodiscard]] bool done() const noexcept { return m_done; }
    [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return m_errors; }

    // The decrypted secret from the most recent PromptReady, or empty.
    [[nodiscard]] std::string secret() const {
      gsize length = 0;
      const gchar* value = gcr_secret_exchange_get_secret(m_exchange, &length);
      if (value == nullptr) {
        return {};
      }
      return std::string(value, length);
    }

  private:
    template <typename... Args> void callAsync(const char* method, Args&&... args) {
      m_prompter->callMethodAsync(method)
          .onInterface(kPrompterInterface)
          .withArguments(std::forward<Args>(args)...)
          .uponReplyInvoke([this, method](std::optional<sdbus::Error> err) {
            if (err.has_value()) {
              m_errors.emplace_back(std::string(method) + ": " + err->getName());
            }
          });
    }

    void onPromptReady(const std::string& reply, const std::string& exchange) {
      ++m_readyCount;
      m_lastReply = reply;
      m_lastExchange = exchange;
      if (!exchange.empty() && gcr_secret_exchange_receive(m_exchange, exchange.c_str()) != FALSE) {
        m_received = true;
      }
    }

    SessionBus& m_bus;
    std::string m_path;
    GcrSecretExchange* m_exchange = nullptr;
    std::unique_ptr<sdbus::IObject> m_object;
    std::unique_ptr<sdbus::IProxy> m_prompter;
    std::vector<std::string> m_errors;
    std::string m_lastReply;
    std::string m_lastExchange;
    int m_readyCount = 0;
    bool m_received = false;
    bool m_done = false;
  };

  bool pumpBuses(
      const std::vector<SessionBus*>& buses, const std::function<bool()>& done, std::chrono::milliseconds timeout
  ) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      for (SessionBus* bus : buses) {
        bus->processPendingEvents();
      }
      if (done()) {
        return true;
      }
      std::this_thread::sleep_for(1ms);
    }
    return done();
  }

  bool pumpUntil(
      SessionBus& server, SessionBus& client, const std::function<bool()>& done, std::chrono::milliseconds timeout = 5s
  ) {
    return pumpBuses({&server, &client}, done, timeout);
  }

  VariantMap passwordProperties() {
    return VariantMap{
        {"title", sdbus::Variant{std::string("Unlock Keyring")}},
        {"message", sdbus::Variant{std::string("An application wants access to the keyring 'login'")}},
        {"description", sdbus::Variant{std::string("The password you use to log in")}},
        {"choice-label", sdbus::Variant{std::string("Automatically unlock this keyring")}},
        {"warning", sdbus::Variant{std::string("")}},
        {"password-new", sdbus::Variant{false}},
        {"continue-label", sdbus::Variant{std::string("Unlock")}},
    };
  }

  // A password typed by the user must arrive at the caller byte-for-byte, through
  // the encrypted exchange.
  void testPasswordRoundTrip(SessionBus& server, SessionBus& client, SecretPrompter& prompter) {
    FakeClient fake(client, "/org/gnome/keyring/Prompt/p0");

    fake.beginPrompting();
    expect(pumpUntil(server, client, [&] { return fake.readyCount() >= 1; }), "password: no opening PromptReady");
    expect(fake.lastReply().empty(), "password: opening reply should be the empty 'none' reply");
    expect(fake.lastExchange().starts_with("[sx-aes-1]"), "password: opening exchange is not an sx-aes-1 handshake");
    expect(!prompter.hasPendingPrompt(), "password: nothing should be on screen before PerformPrompt");

    fake.performPrompt("password", passwordProperties());
    expect(pumpUntil(server, client, [&] { return prompter.hasPendingPrompt(); }), "password: prompt never surfaced");

    const SecretPromptRequest request = prompter.pendingPrompt();
    expect(request.type == SecretPromptRequest::Type::Password, "password: wrong prompt type");
    expect(request.title == "Unlock Keyring", "password: title not propagated");
    expect(request.message == "An application wants access to the keyring 'login'", "password: message not propagated");
    expect(request.description == "The password you use to log in", "password: description not propagated");
    expect(request.choiceLabel == "Automatically unlock this keyring", "password: choice label not propagated");
    expect(request.continueLabel == "Unlock", "password: continue label not propagated");
    expect(!request.passwordNew, "password: password-new should be false");

    const std::string typed = "correct horse battery staple";
    prompter.submitPassword(typed, true);
    expect(
        pumpUntil(server, client, [&] { return fake.readyCount() >= 2; }), "password: no PromptReady with the secret"
    );
    expect(fake.lastReply() == "yes", "password: reply should be 'yes' after submit");
    expect(fake.secret() == typed, "password: secret did not survive the exchange");
    expect(!prompter.hasPendingPrompt(), "password: prompt should be dismissed after submit");

    fake.stopPrompting();
    expect(pumpUntil(server, client, [&] { return fake.done(); }), "password: PromptDone never arrived");
    expect(fake.errors().empty(), "password: prompter returned an error to a well-formed client");
  }

  // Cancelling must answer "no" and carry no secret, so the caller fails cleanly
  // instead of hanging.
  void testCancel(SessionBus& server, SessionBus& client, SecretPrompter& prompter) {
    FakeClient fake(client, "/org/gnome/keyring/Prompt/p1");

    fake.beginPrompting();
    expect(pumpUntil(server, client, [&] { return fake.readyCount() >= 1; }), "cancel: no opening PromptReady");

    fake.performPrompt("password", passwordProperties());
    expect(pumpUntil(server, client, [&] { return prompter.hasPendingPrompt(); }), "cancel: prompt never surfaced");

    prompter.cancel();
    expect(pumpUntil(server, client, [&] { return fake.readyCount() >= 2; }), "cancel: no PromptReady after cancel");
    expect(fake.lastReply() == "no", "cancel: reply should be 'no'");
    expect(fake.secret().empty(), "cancel: a cancelled prompt must not carry a secret");

    fake.stopPrompting();
    expect(pumpUntil(server, client, [&] { return fake.done(); }), "cancel: PromptDone never arrived");
  }

  // Confirm prompts take yes/no and never a secret.
  void testConfirm(SessionBus& server, SessionBus& client, SecretPrompter& prompter) {
    FakeClient fake(client, "/org/gnome/keyring/Prompt/p2");

    fake.beginPrompting();
    expect(pumpUntil(server, client, [&] { return fake.readyCount() >= 1; }), "confirm: no opening PromptReady");

    fake.performPrompt("confirm", VariantMap{{"message", sdbus::Variant{std::string("Allow access?")}}});
    expect(pumpUntil(server, client, [&] { return prompter.hasPendingPrompt(); }), "confirm: prompt never surfaced");
    expect(prompter.pendingPrompt().type == SecretPromptRequest::Type::Confirm, "confirm: wrong prompt type");

    // A password submission must not satisfy a confirm prompt.
    prompter.submitPassword("ignored", false);
    expect(prompter.hasPendingPrompt(), "confirm: submitPassword must not answer a confirm prompt");

    prompter.confirm(false);
    expect(pumpUntil(server, client, [&] { return fake.readyCount() >= 2; }), "confirm: no PromptReady after confirm");
    expect(fake.lastReply() == "yes", "confirm: reply should be 'yes'");
    expect(fake.secret().empty(), "confirm: a confirm reply must not carry a secret");

    fake.stopPrompting();
    expect(pumpUntil(server, client, [&] { return fake.done(); }), "confirm: PromptDone never arrived");
  }

  // PerformPrompt without a preceding BeginPrompting is a protocol violation and
  // must be rejected rather than silently opening a prompt.
  void testPerformWithoutBegin(SessionBus& server, SessionBus& client, SecretPrompter& prompter) {
    FakeClient fake(client, "/org/gnome/keyring/Prompt/p3");

    fake.performPrompt("password", passwordProperties());
    expect(
        pumpUntil(server, client, [&] { return !fake.errors().empty(); }),
        "unbegun: PerformPrompt without BeginPrompting was not rejected"
    );
    expect(!prompter.hasPendingPrompt(), "unbegun: no prompt should have been raised");
  }

  // Only one prompt owns the screen at a time; a second client waits its turn
  // rather than racing the first one's exchange.
  void testSingleSlotQueueing(SessionBus& server, SessionBus& client, SecretPrompter& prompter) {
    FakeClient first(client, "/org/gnome/keyring/Prompt/p4");
    FakeClient second(client, "/org/gnome/keyring/Prompt/p5");

    first.beginPrompting();
    expect(pumpUntil(server, client, [&] { return first.readyCount() >= 1; }), "queue: first client never got a turn");

    second.beginPrompting();
    // Give the prompter a chance to (wrongly) start the second one.
    pumpUntil(server, client, [] { return false; }, 200ms);
    expect(second.readyCount() == 0, "queue: second client was started while the first was active");

    first.stopPrompting();
    expect(
        pumpUntil(server, client, [&] { return second.readyCount() >= 1; }),
        "queue: second client never got a turn after the first stopped"
    );

    second.stopPrompting();
    expect(pumpUntil(server, client, [&] { return second.done(); }), "queue: second client never got PromptDone");
  }

  // A caller that crashes mid-prompt must not wedge the queue behind a prompt
  // nobody is waiting for. This is also the path where the prompter tears a client
  // down from inside that client's own reply handler.
  void testClientDisconnect(SessionBus& server, SessionBus& client, SecretPrompter& prompter) {
    FakeClient queued(client, "/org/gnome/keyring/Prompt/p7");

    {
      SessionBus transient;
      FakeClient leaver(transient, "/org/gnome/keyring/Prompt/p6");
      const std::vector<SessionBus*> all{&server, &client, &transient};

      leaver.beginPrompting();
      expect(pumpBuses(all, [&] { return leaver.readyCount() >= 1; }, 5s), "disconnect: leaver never got a turn");
      leaver.performPrompt("password", passwordProperties());
      expect(pumpBuses(all, [&] { return prompter.hasPendingPrompt(); }, 5s), "disconnect: prompt never surfaced");

      queued.beginPrompting();
      pumpBuses(all, [] { return false; }, 200ms);
      expect(queued.readyCount() == 0, "disconnect: queued client jumped ahead");
    } // transient connection closes here

    expect(
        pumpUntil(server, client, [&] { return !prompter.hasPendingPrompt(); }),
        "disconnect: prompt survived the caller"
    );
    expect(
        pumpUntil(server, client, [&] { return queued.readyCount() >= 1; }),
        "disconnect: queue did not advance after the caller vanished"
    );

    queued.stopPrompting();
    expect(pumpUntil(server, client, [&] { return queued.done(); }), "disconnect: PromptDone never arrived");
  }

  // The decisive test: drive the prompter with gcr's own client state machine --
  // GcrSystemPrompt is exactly what gnome-keyring, seahorse and oo7 use. It runs
  // in a forked child because its sync API spins its own GMainLoop, while the
  // parent keeps pumping the prompter's sdbus connection.
  [[noreturn]] void runGcrClientChild(const std::string& expected) {
    ::alarm(20); // never wedge the suite if the prompter stops answering
    GError* error = nullptr;
    GcrPrompt* prompt = gcr_system_prompt_open(10, nullptr, &error);
    if (prompt == nullptr) {
      std::println(stderr, "gcr client: open failed: {}", error != nullptr ? error->message : "unknown");
      ::_exit(2);
    }
    gcr_prompt_set_message(prompt, "interop");

    int status = 0;
    const gchar* password = gcr_prompt_password(prompt, nullptr, &error);
    if (password == nullptr) {
      std::println(stderr, "gcr client: no password: {}", error != nullptr ? error->message : "cancelled");
      status = 3;
    } else if (expected != password) {
      std::println(stderr, "gcr client: password did not match what the prompter sent");
      status = 4;
    }
    ::_exit(status);
  }

  void testGcrClientInterop(SessionBus& server, SessionBus& client, SecretPrompter& prompter) {
    const std::string expected = "interop-secret-42";

    ::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
      runGcrClientChild(expected);
    }
    if (!expect(pid > 0, "gcr interop: fork failed")) {
      return;
    }

    bool submitted = false;
    bool reaped = false;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + 25s;
    while (std::chrono::steady_clock::now() < deadline) {
      server.processPendingEvents();
      client.processPendingEvents();
      if (!submitted && prompter.hasPendingPrompt()) {
        expect(prompter.pendingPrompt().message == "interop", "gcr interop: message property not propagated");
        prompter.submitPassword(expected, false);
        submitted = true;
      }
      if (::waitpid(pid, &status, WNOHANG) == pid) {
        reaped = true;
        break;
      }
      std::this_thread::sleep_for(1ms);
    }

    if (!reaped) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, &status, 0);
      expect(false, "gcr interop: gcr client never completed");
      return;
    }

    expect(submitted, "gcr interop: gcr client never raised a prompt");
    expect(WIFEXITED(status) != 0 && WEXITSTATUS(status) == 0, "gcr interop: gcr client rejected the exchange");
  }
} // namespace

int main(int /*argc*/, char** argv) {
  // Always run against a throwaway bus: owning org.gnome.keyring.SystemPrompter on
  // a developer's real session bus would hijack their keyring prompts.
  if (std::getenv("NOCTALIA_TEST_PRIVATE_BUS") == nullptr) {
    ::setenv("NOCTALIA_TEST_PRIVATE_BUS", "1", 1);
    ::execlp("dbus-run-session", "dbus-run-session", "--", argv[0], nullptr);
    std::println(stderr, "secret_prompter_test: dbus-run-session unavailable; skipping");
    return 77;
  }

  try {
    SessionBus server;
    SessionBus client;
    SecretPrompter prompter(server);

    testPasswordRoundTrip(server, client, prompter);
    testCancel(server, client, prompter);
    testConfirm(server, client, prompter);
    testPerformWithoutBegin(server, client, prompter);
    testSingleSlotQueueing(server, client, prompter);
    testClientDisconnect(server, client, prompter);
    testGcrClientInterop(server, client, prompter);
  } catch (const std::exception& e) {
    std::println(stderr, "secret_prompter_test: unexpected exception: {}", e.what());
    return 1;
  }

  if (g_failures > 0) {
    std::println(stderr, "secret_prompter_test: {} failure(s)", g_failures);
    return 1;
  }
  return 0;
}
