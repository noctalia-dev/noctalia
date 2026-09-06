#include "shell/auth/auth_source.h"

#include "dbus/polkit/polkit_agent.h"
#include "i18n/i18n.h"
#include "password/systemd_password_agent.h"

#include <fstream>

namespace {
  std::string processName(pid_t pid) {
    if (pid <= 0) {
      return {};
    }
    std::ifstream file("/proc/" + std::to_string(pid) + "/comm");
    std::string name;
    if (!std::getline(file, name)) {
      return {};
    }
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) {
      name.pop_back();
    }
    return name;
  }

} // namespace

PolkitAuthSource::PolkitAuthSource(std::function<PolkitAgent*()> provider) : m_provider(std::move(provider)) {}

PolkitAgent* PolkitAuthSource::agentOrNull() const { return m_provider != nullptr ? m_provider() : nullptr; }

bool PolkitAuthSource::hasRequest() const {
  PolkitAgent* agent = agentOrNull();
  return agent != nullptr && agent->hasPendingRequest();
}

bool PolkitAuthSource::hasDisplayableContent() const {
  PolkitAgent* agent = agentOrNull();
  if (agent == nullptr || !agent->hasPendingRequest()) {
    return false;
  }
  return agent->isResponseRequired() || !agent->supplementaryMessage().empty();
}

const std::string PolkitAuthSource::sessionId() const {
  PolkitAgent* agent = agentOrNull();
  if (agent == nullptr || !agent->hasPendingRequest()) {
    return "";
  }
  const auto request = agent->pendingRequest();
  return "polkit:" + request.cookie;
}

AuthView PolkitAuthSource::view() const {
  AuthView out;
  out.title = i18n::tr("auth.polkit.title");
  out.placeholder = i18n::tr("auth.polkit.password-placeholder");
  out.submitLabel = i18n::tr("auth.polkit.authenticate");
  out.passwordMode = true;
  PolkitAgent* agent = agentOrNull();
  if (agent == nullptr || !agent->hasPendingRequest()) {
    return out;
  }
  const PolkitRequest request = agent->pendingRequest();
  out.message = request.message.empty() ? request.actionId : request.message;
  out.sessionId = "polkit:" + request.cookie;

  const bool needsInput = agent->isResponseRequired();
  const bool supplementaryError = agent->supplementaryIsError();
  std::string prompt = agent->inputPrompt();
  std::string supplementary = agent->supplementaryMessage();
  if (!needsInput && !supplementary.empty() && !supplementaryError) {
    prompt = supplementary;
    supplementary.clear();
  } else if (
      !supplementary.empty() && (supplementaryError || supplementary == i18n::tr("auth.polkit.authenticating"))
  ) {
    prompt = supplementary;
    supplementary.clear();
  }
  out.prompt = std::move(prompt);
  out.supplementary = std::move(supplementary);
  out.needsInput = needsInput;
  out.promptIsError = supplementaryError && agent->supplementaryMessage() == i18n::tr("auth.polkit.invalid-password");
  out.hasContent = needsInput || !agent->supplementaryMessage().empty();

  if (request.isInternal) {
    out.iconKind = AuthIconKind::InternalLogo;
  } else if (!request.iconName.empty()) {
    out.iconKind = AuthIconKind::ThemeIcon;
    out.iconValue = request.iconName;
  } else {
    out.iconKind = AuthIconKind::GlyphIcon;
    out.iconValue = "shield-lock";
  }
  return out;
}

void PolkitAuthSource::submit(const std::string& response) {
  PolkitAgent* agent = agentOrNull();
  if (agent == nullptr || response.empty()) {
    return;
  }
  agent->submitResponse(response);
}

void PolkitAuthSource::cancel() {
  PolkitAgent* agent = agentOrNull();
  if (agent != nullptr && agent->hasPendingRequest()) {
    agent->cancelRequest();
  }
}

SystemdAuthSource::SystemdAuthSource(std::function<SystemdPasswordAgent*()> provider)
    : m_provider(std::move(provider)) {}

SystemdPasswordAgent* SystemdAuthSource::agentOrNull() const { return m_provider != nullptr ? m_provider() : nullptr; }

bool SystemdAuthSource::hasRequest() const {
  SystemdPasswordAgent* agent = agentOrNull();
  return agent != nullptr && agent->hasPendingRequest();
}

bool SystemdAuthSource::hasDisplayableContent() const { return hasRequest(); }

const std::string SystemdAuthSource::sessionId() const {
  SystemdPasswordAgent* agent = agentOrNull();
  if (agent == nullptr || !agent->hasPendingRequest()) {
    return "";
  }
  const auto request = agent->pendingRequest();
  if (request.ask_file.empty()) {
    return "";
  }
  return "systemd:" + request.ask_file.string();
}

AuthView SystemdAuthSource::view() const {
  AuthView out;
  out.title = i18n::tr("auth.systemd.title");
  out.placeholder = i18n::tr("auth.systemd.password-placeholder");
  out.submitLabel = i18n::tr("auth.systemd.authenticate");
  SystemdPasswordAgent* agent = agentOrNull();
  if (agent == nullptr || !agent->hasPendingRequest()) {
    return out;
  }
  const SystemdPasswordQuery request = agent->pendingRequest();
  out.prompt = request.message.empty() ? i18n::tr("auth.systemd.default-message") : request.message;
  if (request.pid > 0) {
    const std::string name = processName(request.pid);
    if (name.empty()) {
      out.supplementary = i18n::tr("auth.systemd.request-from-unknown", "pid", request.pid);
    } else {
      out.supplementary = i18n::tr("auth.systemd.request-from", "process", name, "pid", request.pid);
    }
  }
  out.needsInput = true;
  out.passwordMode = !request.echo;
  out.hasContent = true;
  if (!request.ask_file.empty()) {
    out.sessionId = "systemd:" + request.ask_file.string();
  }
  if (!request.icon.empty()) {
    out.iconKind = AuthIconKind::FilePath;
    out.iconValue = request.icon.string();
  } else {
    out.iconKind = AuthIconKind::GlyphIcon;
    out.iconValue = "password";
  }
  return out;
}

void SystemdAuthSource::submit(const std::string& response) {
  SystemdPasswordAgent* agent = agentOrNull();
  if (agent == nullptr || response.empty()) {
    return;
  }
  agent->submitResponse(response);
}

void SystemdAuthSource::cancel() {
  SystemdPasswordAgent* agent = agentOrNull();
  if (agent != nullptr && agent->hasPendingRequest()) {
    agent->cancelRequest();
  }
}

AuthSource* selectActiveAuthSource(AuthSource* polkit, AuthSource* systemd) noexcept {
  if (polkit != nullptr && polkit->hasDisplayableContent()) {
    return polkit;
  }
  if (systemd != nullptr && systemd->hasDisplayableContent()) {
    return systemd;
  }
  return nullptr;
}
