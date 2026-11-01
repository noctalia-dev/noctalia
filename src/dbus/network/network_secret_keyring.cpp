#include "dbus/network/network_secret_keyring.h"

#include "core/deferred_call.h"
#include "core/log.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <libsecret/secret.h>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

namespace nm_keyring {

  namespace {

    constexpr Logger kLog("network");

    using StringMap = std::map<std::string, std::string>;

    constexpr std::uint32_t kSecretFlagAgentOwned = 0x1;
    constexpr std::uint32_t kSecretFlagNotSaved = 0x2;
    constexpr std::string_view kFlagsSuffix = "-flags";
    constexpr std::string_view kVpnSetting = "vpn";

    // Settings with secret properties. Outside them a "-flags" companion means
    // something else (e.g. ipv4.dhcp-hostname-flags).
    constexpr std::array<std::string_view, 9> kSecretSettings = {
        "802-11-wireless-security", "802-1x", "vpn", "gsm", "cdma", "pppoe", "adsl", "wireguard", "macsec",
    };

    const SecretSchema& schema() {
      // DONT_MATCH_NAME: other agents have not always tagged their items with
      // the schema name, but they all use these attributes.
      static const SecretSchema value = {
          "org.freedesktop.NetworkManager.Connection",
          SECRET_SCHEMA_DONT_MATCH_NAME,
          {
              {"connection-uuid", SECRET_SCHEMA_ATTRIBUTE_STRING},
              {"setting-name", SECRET_SCHEMA_ATTRIBUTE_STRING},
              {"setting-key", SECRET_SCHEMA_ATTRIBUTE_STRING},
              {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
          },
      };
      return value;
    }

    bool keepsSecret(std::uint32_t flags) {
      return (flags & kSecretFlagAgentOwned) != 0U && (flags & kSecretFlagNotSaved) == 0U;
    }

    template <typename T> std::optional<T> variantValue(const sdbus::Variant& variant) {
      try {
        return variant.get<T>();
      } catch (const sdbus::Error&) {
        return std::nullopt;
      }
    }

    std::optional<std::uint32_t> parseFlags(std::string_view text) {
      std::uint32_t flags = 0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), flags);
      if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
      }
      return flags;
    }

    void addChange(
        SecretChanges& changes, const std::string& setting, std::string_view flagsKey, std::uint32_t flags,
        const std::optional<std::string>& value
    ) {
      SecretKey id{.setting = setting, .key = std::string(flagsKey.substr(0, flagsKey.size() - kFlagsSuffix.size()))};
      if (!keepsSecret(flags)) {
        changes.forget.push_back(std::move(id));
      } else if (value.has_value() && !value->empty()) {
        changes.save.push_back({.id = std::move(id), .value = *value});
      }
    }

    void vpnChanges(
        SecretChanges& changes, const SecretsDict& connection, const std::map<std::string, sdbus::Variant>& props
    ) {
      const auto settingIt = connection.find(std::string(kVpnSetting));
      if (settingIt == connection.end()) {
        return;
      }
      const auto dataIt = settingIt->second.find("data");
      if (dataIt == settingIt->second.end()) {
        return;
      }
      const auto data = variantValue<StringMap>(dataIt->second).value_or(StringMap{});
      const auto secretsIt = props.find("secrets");
      const auto values =
          secretsIt == props.end() ? StringMap{} : variantValue<StringMap>(secretsIt->second).value_or(StringMap{});
      // VPN secret flags live in vpn.data as strings. Secrets without one, like
      // the dynamic challenge answer, are never stored.
      for (const auto& [key, text] : data) {
        if (!key.ends_with(kFlagsSuffix)) {
          continue;
        }
        const auto flags = parseFlags(text);
        if (!flags.has_value()) {
          continue;
        }
        const auto valueIt = values.find(key.substr(0, key.size() - kFlagsSuffix.size()));
        addChange(
            changes, std::string(kVpnSetting), key, *flags,
            valueIt == values.end() ? std::nullopt : std::optional<std::string>(valueIt->second)
        );
      }
    }

    // Owns a GHashTable of borrowed strings, for libsecret's attribute arguments.
    struct Attributes {
      GHashTable* table = g_hash_table_new(g_str_hash, g_str_equal);
      Attributes() = default;
      Attributes(const Attributes&) = delete;
      Attributes& operator=(const Attributes&) = delete;
      ~Attributes() { g_hash_table_unref(table); }
      void set(const char* key, const std::string& value) {
        g_hash_table_insert(table, const_cast<char*>(key), const_cast<char*>(value.c_str()));
      }
    };

    void logError(const char* operation, GError* error) {
      if (error != nullptr) {
        kLog.warn("keyring {} failed: {}", operation, error->message);
        g_error_free(error);
      }
    }

    StringMap lookupSync(const std::string& uuid, const std::string& setting) {
      Attributes attributes;
      attributes.set("connection-uuid", uuid);
      attributes.set("setting-name", setting);
      GError* error = nullptr;
      const auto flags =
          static_cast<SecretSearchFlags>(SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK | SECRET_SEARCH_LOAD_SECRETS);
      GList* items = secret_service_search_sync(nullptr, &schema(), attributes.table, flags, nullptr, &error);
      logError("lookup", error);

      StringMap values;
      for (GList* it = items; it != nullptr; it = it->next) {
        auto* item = static_cast<SecretItem*>(it->data);
        SecretValue* secret = secret_item_get_secret(item);
        GHashTable* itemAttributes = secret_item_get_attributes(item);
        const auto* key = static_cast<const char*>(g_hash_table_lookup(itemAttributes, "setting-key"));
        const char* text = secret != nullptr ? secret_value_get_text(secret) : nullptr;
        if (key != nullptr && text != nullptr && *text != '\0') {
          values.insert_or_assign(key, text);
        }
        g_hash_table_unref(itemAttributes);
        if (secret != nullptr) {
          secret_value_unref(secret);
        }
      }
      g_list_free_full(items, g_object_unref);
      return values;
    }

    void storeSync(const std::string& uuid, const std::string& connectionId, const Secret& secret) {
      // The label nm-applet gives these items, so they read the same in a keyring browser.
      const std::string label = "Network secret for " + connectionId + "/" + secret.id.setting + "/" + secret.id.key;
      GError* error = nullptr;
      secret_password_store_sync(
          &schema(), SECRET_COLLECTION_DEFAULT, label.c_str(), secret.value.c_str(), nullptr, &error, "connection-uuid",
          uuid.c_str(), "setting-name", secret.id.setting.c_str(), "setting-key", secret.id.key.c_str(), nullptr
      );
      logError("store", error);
    }

    void forgetSync(const std::string& uuid, const SecretKey& id) {
      GError* error = nullptr;
      secret_password_clear_sync(
          &schema(), nullptr, &error, "connection-uuid", uuid.c_str(), "setting-name", id.setting.c_str(),
          "setting-key", id.key.c_str(), nullptr
      );
      logError("clear", error);
    }

    void eraseAllSync(const std::string& uuid) {
      GError* error = nullptr;
      secret_password_clear_sync(&schema(), nullptr, &error, "connection-uuid", uuid.c_str(), nullptr);
      logError("clear", error);
    }

  } // namespace

  SecretChanges secretChanges(const SecretsDict& connection, const SecretsDict& secrets) {
    SecretChanges changes;
    for (const auto& [setting, props] : secrets) {
      if (std::ranges::find(kSecretSettings, setting) == kSecretSettings.end()) {
        continue;
      }
      if (setting == kVpnSetting) {
        vpnChanges(changes, connection, props);
        continue;
      }
      const auto settingIt = connection.find(setting);
      if (settingIt == connection.end()) {
        continue;
      }
      for (const auto& [key, variant] : settingIt->second) {
        if (!key.ends_with(kFlagsSuffix)) {
          continue;
        }
        const auto flags = variantValue<std::uint32_t>(variant);
        if (!flags.has_value()) {
          continue;
        }
        const auto valueIt = props.find(key.substr(0, key.size() - kFlagsSuffix.size()));
        addChange(
            changes, setting, key, *flags,
            valueIt == props.end() ? std::nullopt : variantValue<std::string>(valueIt->second)
        );
      }
    }
    return changes;
  }

  SecretsDict secretsReply(const std::string& setting, const std::map<std::string, std::string>& values) {
    SecretsDict reply;
    if (setting == kVpnSetting) {
      reply[setting]["secrets"] = sdbus::Variant{values};
      return reply;
    }
    auto& props = reply[setting];
    for (const auto& [key, value] : values) {
      props[key] = sdbus::Variant{value};
    }
    return reply;
  }

  struct Keyring::State {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    bool workerRunning = false;
    bool stopping = false;
    // Expires with the Keyring; checked on the main thread before a lookup's callback.
    std::shared_ptr<int> alive = std::make_shared<int>(0);

    void enqueue(const std::shared_ptr<State>& self, std::function<void()> job) {
      std::scoped_lock lock(mutex);
      jobs.push_back(std::move(job));
      if (!workerRunning) {
        // Detached rather than joined: a job can sit on an unlock prompt, and
        // shutdown must not wait for the user to answer it.
        workerRunning = true;
        std::thread([self]() { self->run(); }).detach();
      }
      wake.notify_one();
    }

    void run() {
      for (;;) {
        std::function<void()> job;
        {
          std::unique_lock lock(mutex);
          wake.wait(lock, [this]() { return stopping || !jobs.empty(); });
          if (jobs.empty()) {
            workerRunning = false;
            return;
          }
          job = std::move(jobs.front());
          jobs.pop_front();
        }
        job();
      }
    }
  };

  Keyring::Keyring() : m_state(std::make_shared<State>()) {}

  Keyring::~Keyring() {
    // Queued saves still run; only callbacks into the destroyed owner stop.
    std::scoped_lock lock(m_state->mutex);
    m_state->alive.reset();
    m_state->stopping = true;
    m_state->wake.notify_one();
  }

  void Keyring::lookup(std::string uuid, std::string setting, LookupCallback callback) {
    const std::weak_ptr<int> alive = m_state->alive;
    m_state->enqueue(
        m_state,
        [uuid = std::move(uuid), setting = std::move(setting), callback = std::move(callback), alive]() mutable {
          auto values = uuid.empty() ? StringMap{} : lookupSync(uuid, setting);
          DeferredCall::callLater([alive, callback = std::move(callback), values = std::move(values)]() mutable {
            if (!alive.expired()) {
              callback(std::move(values));
            }
          });
        }
    );
  }

  void Keyring::apply(std::string uuid, std::string connectionId, SecretChanges changes) {
    if (uuid.empty() || (changes.save.empty() && changes.forget.empty())) {
      return;
    }
    m_state->enqueue(
        m_state, [uuid = std::move(uuid), connectionId = std::move(connectionId), changes = std::move(changes)]() {
          for (const auto& id : changes.forget) {
            forgetSync(uuid, id);
          }
          for (const auto& secret : changes.save) {
            storeSync(uuid, connectionId, secret);
          }
          kLog.debug("keyring saved {} and cleared {} secrets of {}", changes.save.size(), changes.forget.size(), uuid);
        }
    );
  }

  void Keyring::eraseAll(std::string uuid) {
    if (uuid.empty()) {
      return;
    }
    m_state->enqueue(m_state, [uuid = std::move(uuid)]() { eraseAllSync(uuid); });
  }

} // namespace nm_keyring
