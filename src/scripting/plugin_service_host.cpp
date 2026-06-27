#include "scripting/plugin_service_host.h"

#include "core/log.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "scripting/plugin_ipc.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_registry.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace scripting {

  namespace {
    constexpr Logger kLog("plugin-service");

    std::string readFile(const std::filesystem::path& path) {
      std::ifstream file(path);
      if (!file) {
        return {};
      }
      std::stringstream ss;
      ss << file.rdbuf();
      return ss.str();
    }
  } // namespace

  PluginServiceHost::PluginServiceHost(
      ScriptApiContext& scriptApi, HttpClient* httpClient, ClipboardService* clipboard, FileWatcher* fileWatcher
  )
      : m_scriptApi(scriptApi), m_httpClient(httpClient), m_clipboard(clipboard), m_fileWatcher(fileWatcher) {}

  PluginServiceHost::~PluginServiceHost() {
    for (auto& service : m_services) {
      stopService(*service);
    }
  }

  PluginServiceHost::Service::DispatchResult PluginServiceHost::Service::dispatchIpc(
      std::string_view event, std::string_view payload, const ScriptSnapshot& snapshot
  ) {
    if (runtime == nullptr) {
      return DispatchResult::MissingHost;
    }
    if (!runtime->enqueueCallStrings("onIpc", std::string(event), std::string(payload), snapshot)) {
      return DispatchResult::Failed;
    }
    return DispatchResult::Handled;
  }

  std::optional<ScriptSettings>
  PluginServiceHost::seedFor(const std::string& entryId, const PluginSettingsMap& pluginSettings) const {
    auto entry = PluginRegistry::instance().resolve(entryId);
    if (!entry.has_value()) {
      return std::nullopt;
    }
    auto seeded = seedEntrySettings(*entry->entry, {});
    static const ScriptSettings kEmpty;
    const auto it = pluginSettings.find(entry->manifest->id);
    mergePluginSettings(*entry->manifest, it != pluginSettings.end() ? it->second : kEmpty, seeded);
    return seeded;
  }

  void PluginServiceHost::subscribeAndArm(Service& service) {
    Service* svc = &service;
    std::weak_ptr<bool> alive = service.alive;
    service.subscription = service.runtime->subscribe([this, svc, alive](const ScriptResult& result) {
      auto token = alive.lock();
      if (token == nullptr || !*token) {
        return;
      }
      if (result.patch.updateIntervalMs.has_value()) {
        const int next = std::max(16, *result.patch.updateIntervalMs);
        if (next != svc->updateIntervalMs) {
          svc->updateIntervalMs = next;
          armTimer(*svc);
        }
      }
    });
    armTimer(service);
  }

  std::unique_ptr<PluginServiceHost::Service> PluginServiceHost::makeService(
      const std::string& entryId, const std::filesystem::path& source, ScriptSettings seeded
  ) {
    std::string code = readFile(source);
    if (code.empty()) {
      kLog.warn("service '{}': empty or unreadable source {}", entryId, source.string());
      return nullptr;
    }
    auto service = std::make_unique<Service>();
    service->entryId = entryId;
    service->sourcePath = source;
    service->lastSeededSettings = std::move(seeded);
    service->runtime = std::make_shared<ScriptRuntime>(
        entryId, service->lastSeededSettings, m_scriptApi, source.parent_path(), m_httpClient, m_clipboard
    );
    subscribeAndArm(*service);
    service->runtime->start(source.string(), std::move(code), {});
    setupScriptWatch(*service);
    PluginIpcRouter::instance().registerEndpoint(service.get());
    return service;
  }

  void PluginServiceHost::stopService(Service& service) {
    PluginIpcRouter::instance().unregisterEndpoint(&service);
    teardownScriptWatch(service);
    service.updateTimer.stop();
    if (service.alive) {
      *service.alive = false;
    }
    if (service.runtime != nullptr) {
      if (service.subscription != 0) {
        service.runtime->unsubscribe(service.subscription);
        service.subscription = 0;
      }
      service.runtime->stop();
    }
  }

  void PluginServiceHost::start(const PluginSettingsMap& pluginSettings) {
    PluginRegistry::instance().ensureScanned();
    for (const auto& entry : PluginRegistry::instance().entriesOfKind(PluginEntryKind::Service)) {
      auto seeded = seedFor(entry.fullId(), pluginSettings);
      if (auto service = makeService(entry.fullId(), entry.sourcePath, seeded.value_or(ScriptSettings{}))) {
        kLog.info("started service '{}'", entry.fullId());
        m_services.push_back(std::move(service));
      }
    }
  }

  void PluginServiceHost::refresh(const PluginSettingsMap& pluginSettings) {
    PluginRegistry::instance().ensureScanned();
    const auto desired = PluginRegistry::instance().entriesOfKind(PluginEntryKind::Service);

    // Stop + drop services whose entry no longer exists (plugin disabled / removed /
    // an update dropped the [[service]]).
    std::unordered_set<std::string> desiredIds;
    for (const auto& entry : desired) {
      desiredIds.insert(entry.fullId());
    }
    for (auto it = m_services.begin(); it != m_services.end();) {
      if (!desiredIds.contains((*it)->entryId)) {
        kLog.info("stopping service '{}' (no longer active)", (*it)->entryId);
        stopService(**it);
        it = m_services.erase(it);
      } else {
        ++it;
      }
    }

    // Start newly-appeared services (plugin enabled / updated to add one); re-seed the
    // rest only when their effective settings changed.
    for (const auto& entry : desired) {
      const std::string id = entry.fullId();
      auto seeded = seedFor(id, pluginSettings);
      if (!seeded.has_value()) {
        continue;
      }
      const auto existing = std::ranges::find_if(m_services, [&](const auto& s) { return s->entryId == id; });
      if (existing == m_services.end()) {
        if (auto service = makeService(id, entry.sourcePath, *seeded)) {
          kLog.info("started service '{}'", id);
          m_services.push_back(std::move(service));
        }
        continue;
      }

      Service& service = **existing;
      const bool sourceChanged = service.sourcePath != entry.sourcePath;
      const bool settingsChanged = !settingsEqual(*seeded, service.lastSeededSettings);
      if (!sourceChanged && !settingsChanged) {
        continue;
      }
      std::string code = readFile(entry.sourcePath);
      if (code.empty()) {
        kLog.warn("service '{}': empty or unreadable source on refresh {}", id, entry.sourcePath.string());
        continue;
      }
      if (sourceChanged) {
        teardownScriptWatch(service);
        service.sourcePath = entry.sourcePath;
      }
      service.updateTimer.stop();
      if (service.subscription != 0) {
        service.runtime->unsubscribe(service.subscription);
        service.subscription = 0;
      }
      service.runtime->stop();
      service.lastSeededSettings = *seeded;
      service.runtime = std::make_shared<ScriptRuntime>(
          id, service.lastSeededSettings, m_scriptApi, service.sourcePath.parent_path(), m_httpClient, m_clipboard
      );
      subscribeAndArm(service);
      service.runtime->start(service.sourcePath.string(), std::move(code), {});
      if (sourceChanged) {
        setupScriptWatch(service);
      }
      kLog.info("restarted service '{}' after {} change", id, sourceChanged ? "source" : "settings");
    }
  }

  void PluginServiceHost::onOutputChange() {
    for (auto& service : m_services) {
      if (service->runtime != nullptr) {
        (void)service->runtime->enqueueCall("onOutputsChanged", {});
      }
    }
  }

  void PluginServiceHost::setupScriptWatch(Service& service) {
    if (service.watchId != 0 || service.sourcePath.empty() || m_fileWatcher == nullptr) {
      return;
    }
    Service* svc = &service;
    service.watchId = m_fileWatcher->watch(
        service.sourcePath, [this, svc] { reloadService(*svc); }, FileWatcher::WatchTrigger::WriteCompleted
    );
  }

  void PluginServiceHost::teardownScriptWatch(Service& service) {
    if (service.watchId == 0 || m_fileWatcher == nullptr) {
      return;
    }
    m_fileWatcher->unwatch(service.watchId);
    service.watchId = 0;
  }

  void PluginServiceHost::reloadService(Service& service) {
    std::string code = readFile(service.sourcePath);
    auto name = service.sourcePath.filename().string();
    if (code.empty()) {
      kLog.warn("service '{}': failed to reload '{}'", service.entryId, service.sourcePath.string());
      notify::error("Noctalia", i18n::tr("bar.widgets.scripted.reload-failed"), name);
      return;
    }
    if (service.runtime == nullptr) {
      kLog.warn("service '{}': runtime unavailable for reload", service.entryId);
      notify::error("Noctalia", i18n::tr("bar.widgets.scripted.reload-failed"), name);
      return;
    }

    service.updateTimer.stop();
    service.updateIntervalMs = 1000;
    service.runtime->reload(service.sourcePath.string(), std::move(code), {});
    armTimer(service);
    kLog.info("hot reload: reloaded service '{}'", service.entryId);
    notify::info("Noctalia", i18n::tr("bar.widgets.scripted.reloaded"), name);
  }

  void PluginServiceHost::armTimer(Service& service) {
    service.updateTimer.stop();
    Service* svc = &service;
    std::weak_ptr<bool> alive = service.alive;
    service.updateTimer.startRepeating(std::chrono::milliseconds(service.updateIntervalMs), [svc, alive] {
      auto token = alive.lock();
      if (token != nullptr && *token && svc->runtime != nullptr) {
        (void)svc->runtime->enqueueUpdate({});
      }
    });
  }

} // namespace scripting
