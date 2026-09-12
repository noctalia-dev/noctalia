#pragma once

#include <functional>
#include <string>

class PolkitAgent;
class SystemdPasswordAgent;

enum class AuthIconKind : unsigned char {
  InternalLogo = 0,
  GlyphIcon = 1,
  ThemeIcon = 2,
  FilePath = 3,
};

struct AuthView {
  std::string title;
  std::string message;
  std::string prompt;
  std::string supplementary;
  std::string placeholder;
  std::string submitLabel;
  std::string sessionId;
  std::string iconValue;
  AuthIconKind iconKind = AuthIconKind::GlyphIcon;
  bool needsInput = false;
  bool promptIsError = false;
  bool passwordMode = true;
  bool hasContent = false;
};

class AuthSource {
public:
  virtual ~AuthSource() = default;
  [[nodiscard]] virtual bool hasRequest() const = 0;
  [[nodiscard]] virtual bool hasDisplayableContent() const = 0;
  [[nodiscard]] virtual AuthView view() const = 0;
  [[nodiscard]] virtual const std::string sessionId() const = 0;
  virtual void submit(const std::string& response) = 0;
  virtual void cancel() = 0;
};

class PolkitAuthSource final : public AuthSource {
public:
  explicit PolkitAuthSource(std::function<PolkitAgent*()> provider);
  [[nodiscard]] bool hasRequest() const override;
  [[nodiscard]] bool hasDisplayableContent() const override;
  [[nodiscard]] const std::string sessionId() const override;
  [[nodiscard]] AuthView view() const override;
  void submit(const std::string& response) override;
  void cancel() override;

private:
  [[nodiscard]] PolkitAgent* agentOrNull() const;
  std::function<PolkitAgent*()> m_provider;
};

class SystemdAuthSource final : public AuthSource {
public:
  explicit SystemdAuthSource(std::function<SystemdPasswordAgent*()> provider);
  [[nodiscard]] bool hasRequest() const override;
  [[nodiscard]] bool hasDisplayableContent() const override;
  [[nodiscard]] const std::string sessionId() const override;
  [[nodiscard]] AuthView view() const override;
  void submit(const std::string& response) override;
  void cancel() override;

private:
  [[nodiscard]] SystemdPasswordAgent* agentOrNull() const;
  std::function<SystemdPasswordAgent*()> m_provider;
};

// Priority: polkit > systemd. Returns nullptr when neither has displayable content.
[[nodiscard]] AuthSource* selectActiveAuthSource(AuthSource* polkit, AuthSource* systemd) noexcept;
