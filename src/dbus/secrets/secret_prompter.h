#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class SessionBus;

// One prompt as requested by a Secret Service client (gnome-keyring, oo7, gcr,
// seahorse, pinentry-gnome3, ...). Field names mirror the GcrPrompt properties
// carried over the wire, minus the ones only the legacy GTK dialog consumed.
struct SecretPromptRequest {
  enum class Type : std::uint8_t { Password, Confirm };

  Type type = Type::Password;
  std::string title;
  std::string message;
  std::string description;
  std::string warning;
  // Optional checkbox, e.g. "Automatically unlock this keyring whenever I'm logged in".
  // Empty when the client offers no choice.
  std::string choiceLabel;
  bool choiceChosen = false;
  // The prompt asks the user to pick a *new* password, so it must be confirmed twice.
  bool passwordNew = false;
  std::string continueLabel;
  std::string cancelLabel;
  std::string callerWindow;
};

// org.gnome.keyring.SystemPrompter: answers Secret Service password and confirm
// prompts (keyring unlock, CreateCollection, ChangePassword) in the shell.
//
// Single-slot, matching GCR_SYSTEM_PROMPTER_SINGLE: one prompt on screen at a
// time, further clients queue in arrival order.
//
// Lifecycle per client:
//   1. client -> BeginPrompting(callback); queued, then PromptReady("") on its turn
//   2. client -> PerformPrompt(callback, type, properties, exchange)
//   3. UI     -> submitPassword()/confirm()/cancel() -> PromptReady("yes"|"no", secret)
//   4. client -> StopPrompting(callback) -> PromptDone()
//
// The secret itself never crosses D-Bus in the clear: it is encrypted with a
// GcrSecretExchange ("sx-aes-1") session key negotiated in steps 1-2.
class SecretPrompter {
public:
  using StateCallback = std::function<void()>;

  // Throws when the bus name is already owned -- another prompter (gnome-shell,
  // a running gcr-prompter) is serving this session and must be left alone.
  explicit SecretPrompter(SessionBus& bus);
  ~SecretPrompter();

  SecretPrompter(const SecretPrompter&) = delete;
  SecretPrompter& operator=(const SecretPrompter&) = delete;

  // Fires whenever the prompt shown to the user should appear, change, or go away.
  void setStateCallback(StateCallback callback);

  [[nodiscard]] bool hasPendingPrompt() const noexcept;
  [[nodiscard]] SecretPromptRequest pendingPrompt() const;

  // Reply paths for the prompt on screen. Safe no-ops when nothing is pending.
  void submitPassword(const std::string& password, bool choiceChosen);
  void confirm(bool choiceChosen);
  void cancel();

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
