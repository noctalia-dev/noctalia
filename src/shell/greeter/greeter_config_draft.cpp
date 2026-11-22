#include "shell/greeter/greeter_config_draft.h"

#include "shell/greeter/greeter_profile_store.h"

#include <utility>

GreeterConfigDraft::GreeterConfigDraft(std::filesystem::path path) : m_path(std::move(path)) {
  if (m_path.empty()) {
    const auto resolved = greeter::localProfilePath();
    if (!resolved) {
      m_readError = resolved.error();
      return;
    }
    m_path = *resolved;
  }
  refresh();
  m_base = m_backing;
  m_profile = m_base;
}

void GreeterConfigDraft::refresh() {
  if (m_path.empty()) {
    return;
  }
  const auto saved = greeter::readLocalProfile(m_path);
  if (!saved) {
    m_readError = saved.error();
    return;
  }
  m_readError.clear();
  m_backing = *saved;
}

void GreeterConfigDraft::keepDraft() {
  refresh();
  if (!m_readError.empty()) {
    return;
  }
  m_base = m_backing;
  m_error.clear();
}

void GreeterConfigDraft::discard() {
  keepDraft();
  if (!m_readError.empty()) {
    return;
  }
  m_profile = m_base;
}

bool GreeterConfigDraft::save() {
  refresh();
  if (!m_readError.empty()) {
    return false;
  }
  if (backingChanged()) {
    m_error = "The saved greeter profile changed. Reload it or explicitly keep the draft before saving.";
    return false;
  }
  if (!dirty()) {
    m_error.clear();
    return true;
  }
  const auto saved = greeter::writeLocalProfile(m_path, m_profile);
  if (!saved) {
    m_error = saved.error();
    return false;
  }
  discard();
  return true;
}
