#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct LauncherCategory {
  std::string label;
  std::string glyphName;
};

struct LauncherResult {
  std::string id;
  std::string providerId; // Set by LauncherPanel after query; used for activation dispatch and usage tracking
  std::string title;
  std::string subtitle;
  std::string glyphName;
  std::string iconName;
  std::string iconPath;
  // A short string drawn in the leading icon slot in place of an icon (e.g. an
  // emoji or a single symbol). When set, it replaces glyphName/iconName/iconPath.
  std::string badge;
  // When launching an application via AppProvider, matches DesktopAction::id (primary Exec leaves this empty).
  std::string desktopActionId;
  std::string category;
  std::string presentation;
  std::optional<std::string> query;
  double score = 0.0;
  int recentlyUsedIndex = 0; // Higher is more recent. <=0 means no record or too old.
};

class LauncherProvider {
public:
  virtual ~LauncherProvider() = default;

  [[nodiscard]] virtual std::string_view prefix() const = 0;
  // Stable opaque identity. Keys usage-tracking persistence and activation dispatch,
  // so it must never change or be translated.
  [[nodiscard]] virtual std::string_view id() const = 0;
  // Localizable title shown to the user (e.g. the prefix overview). Defaults to the
  // stable id(); override to return a translated string.
  [[nodiscard]] virtual std::string displayName() const { return std::string(id()); }
  [[nodiscard]] virtual std::string_view defaultGlyphName() const { return "search"; }

  // Return true to opt in to usage-based score boosting. The panel will
  // record each activation and surface frequently used entries higher.
  [[nodiscard]] virtual bool trackUsage() const { return false; }

  // Prefixed providers (non-empty prefix()) normally only respond when their prefix is typed.
  // Return true to also contribute results to the general (non-prefixed) search.
  [[nodiscard]] virtual bool includeInGlobalSearch() const { return false; }

  [[nodiscard]] virtual std::vector<LauncherCategory> categories() const { return {}; }

  // True for providers registered dynamically (plugin-backed). The panel can drop
  // and re-add just these when the enabled plugin set changes.
  [[nodiscard]] virtual bool isDynamic() const { return false; }

  // Async providers (plugin-backed) deliver results after query() returns; the
  // panel installs this callback so the provider can ask for the current query to
  // be re-gathered when fresh results land. Synchronous providers ignore it.
  virtual void setResultsChangedCallback(std::function<void()> /*callback*/) {}

  // Plugin-backed providers can request that the open launcher input be replaced,
  // e.g. to implement autocomplete.
  virtual void setQueryRequestedCallback(std::function<void(std::string)> /*callback*/) {}

  // Async (plugin-backed) providers defer the launcher close until their activation
  // handler resolves: if it rewrote the query the panel stays open, otherwise the
  // provider invokes this to close it (and record usage). The argument is the
  // activated result id. Synchronous providers close directly via activate().
  virtual void setActivationDoneCallback(std::function<void(std::string)> /*callback*/) {}

  virtual void initialize() {}

  // Called when the launcher panel closes. Async providers should drop any cached
  // result set so a later session doesn't briefly show the previous query's results.
  virtual void reset() {}

  [[nodiscard]] virtual std::vector<LauncherResult> query(std::string_view text) const = 0;

  virtual bool activate(const LauncherResult& result) = 0;
};
