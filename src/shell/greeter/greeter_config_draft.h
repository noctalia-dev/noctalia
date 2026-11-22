#pragma once

#include "shell/greeter/greeter_profile.h"

#include <filesystem>
#include <string>

// Editor-owned transaction against the dedicated GUI profile file.
class GreeterConfigDraft {
public:
  explicit GreeterConfigDraft(std::filesystem::path path = {});
  GreeterProfile& profile() noexcept { return m_profile; }
  const GreeterProfile& profile() const noexcept { return m_profile; }
  const GreeterProfile& backingProfile() const noexcept { return m_backing; }
  const std::filesystem::path& path() const noexcept { return m_path; }
  bool dirty() const { return m_profile != m_base; }
  bool backingChanged() const { return !m_readError.empty() || m_backing != m_base; }
  const std::string& error() const noexcept { return m_readError.empty() ? m_error : m_readError; }
  void refresh();
  void keepDraft();
  void discard();
  bool save();

private:
  std::filesystem::path m_path;
  GreeterProfile m_base, m_backing, m_profile;
  std::string m_error, m_readError;
};
