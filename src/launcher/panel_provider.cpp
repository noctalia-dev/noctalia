#include "launcher/panel_provider.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "launcher/panel_catalog.h"
#include "shell/control_center/control_center_panel.h"
#include "shell/panel/panel_manager.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

  constexpr std::size_t kMaxResults = 50;
  // Synthetic id prefix for a Control Center tab row ("cc-tab:media"). Never
  // collides with a real panel id: built-ins have no colon and plugin ids are
  // "author/plugin:entry" (a slash always precedes the colon).
  constexpr std::string_view kControlCenterTabPrefix = "cc-tab:";

} // namespace

PanelProvider::PanelProvider(PanelManager* panelManager, ControlCenterPanel* controlCenterPanel, ConfigService* config)
    : m_panelManager(panelManager), m_controlCenterPanel(controlCenterPanel), m_config(config) {}

std::string PanelProvider::displayName() const { return i18n::tr("launcher.providers.panel.title"); }

std::vector<LauncherResult> PanelProvider::query(std::string_view text) const {
  if (m_panelManager == nullptr) {
    return {};
  }

  static const std::vector<std::string> kNoIgnored;
  const std::vector<std::string>& ignored =
      m_config != nullptr ? m_config->config().shell.launcher.panels.ignored : kNoIgnored;

  std::vector<std::pair<std::string, panel_catalog::Description>> entries;
  const std::vector<std::string> ids = m_panelManager->availablePanelIds();
  entries.reserve(ids.size());
  for (const auto& panelId : ids) {
    if (std::ranges::contains(ignored, panelId)) {
      continue;
    }
    entries.emplace_back(panelId, panel_catalog::describe(panelId));
  }

  if (m_controlCenterPanel != nullptr && !std::ranges::contains(ignored, "control-center")) {
    const std::string controlCenterTitle = i18n::tr("launcher.providers.panel.builtin.control-center");
    for (const auto& tab : m_controlCenterPanel->visibleTabsForLauncher()) {
      entries.emplace_back(
          std::string(kControlCenterTabPrefix) + std::string(tab.key),
          panel_catalog::Description{
              .title = i18n::tr(tab.titleKey), .subtitle = controlCenterTitle, .glyph = std::string(tab.glyph)
          }
      );
    }
  }

  if (entries.empty()) {
    return {};
  }

  struct ScoredPanel {
    std::string id;
    panel_catalog::Description description;
    double score = 0.0;
  };

  const std::string query = StringUtils::toLower(StringUtils::trim(text));
  std::vector<ScoredPanel> scored;
  scored.reserve(entries.size());
  for (auto& [panelId, description] : entries) {
    double score = 0.0;
    if (!query.empty()) {
      const std::string searchable = StringUtils::toLower(description.title + " " + panelId);
      score = FuzzyMatch::score(query, searchable);
      if (!FuzzyMatch::isMatch(score)) {
        continue;
      }
    }
    scored.push_back(ScoredPanel{.id = std::move(panelId), .description = std::move(description), .score = score});
  }

  if (query.empty()) {
    std::ranges::sort(scored, [](const auto& a, const auto& b) { return a.description.title < b.description.title; });
  } else {
    std::ranges::sort(scored, [](const auto& a, const auto& b) { return a.score > b.score; });
  }
  if (scored.size() > kMaxResults) {
    scored.resize(kMaxResults);
  }

  std::vector<LauncherResult> results;
  results.reserve(scored.size());
  for (auto& entry : scored) {
    LauncherResult result;
    result.id = std::move(entry.id);
    result.title = std::move(entry.description.title);
    result.subtitle = std::move(entry.description.subtitle);
    result.glyphName = std::move(entry.description.glyph);
    result.score = entry.score;
    results.push_back(std::move(result));
  }
  return results;
}

bool PanelProvider::activate(const LauncherResult& result) {
  if (m_panelManager == nullptr) {
    return false;
  }
  if (!result.providerId.empty() && result.providerId != id()) {
    return false;
  }

  PanelManager* panelManager = m_panelManager;
  wl_output* output = nullptr;
  std::string sourceBarName;
  if (panelManager->isOpenPanel("launcher")) {
    output = panelManager->attachedPanelOutput();
    sourceBarName = panelManager->attachedSourceBarName();
  }

  std::string panelId = result.id;
  std::string context;
  if (panelId.starts_with(kControlCenterTabPrefix)) {
    context = panelId.substr(kControlCenterTabPrefix.size());
    panelId = "control-center";
  }

  // Preserve the launcher's output and source bar across its close, then defer
  // the toggle so that close cannot immediately undo the selected panel's open.
  DeferredCall::callLater([panelManager, panelId = std::move(panelId), context = std::move(context), output,
                           sourceBarName = std::move(sourceBarName)]() {
    panelManager->togglePanel(
        panelId, PanelOpenRequest{.output = output, .context = context, .sourceBarName = sourceBarName}
    );
  });
  return true;
}
