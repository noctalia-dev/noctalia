#include "shell/greeter/greeter_config_draft.h"

#include "config/config_service.h"

GreeterConfigDraft::GreeterConfigDraft(ConfigService& config)
    : m_config(config), m_base(config.config().shell.greeter), m_backing(m_base), m_profile(m_base) {}

void GreeterConfigDraft::refresh() { m_backing = m_config.config().shell.greeter; }

void GreeterConfigDraft::keepDraft() {
  refresh();
  m_base = m_backing;
  m_error.clear();
}

void GreeterConfigDraft::discard() {
  refresh();
  m_base = m_backing;
  m_profile = m_base;
  m_error.clear();
}

bool GreeterConfigDraft::save() {
  refresh();
  if (backingChanged()) {
    m_error = "The saved greeter profile changed. Reload it or explicitly keep the draft before saving.";
    return false;
  }
  if (!dirty()) {
    m_error.clear();
    return true;
  }
  if (!m_config.setGreeterConfig(m_profile)) {
    m_error = m_config.lastMutationError();
    return false;
  }
  discard();
  return true;
}
