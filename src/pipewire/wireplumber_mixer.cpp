#include "pipewire/wireplumber_mixer.h"

#include "core/log.h"

#include <functional>
#include <glib.h>
#include <optional>
#include <pipewire/keys.h>
#include <unordered_map>
#include <vector>
#include <wp/wp.h>

namespace {
  constexpr Logger kLog("wireplumber");

  // wp_mixer_api_volume_scale_enum: SCALE_LINEAR = 0, SCALE_CUBIC = 1. Cubic makes the
  // "volume" value match what pavucontrol displays, so we pass our perceptual value directly.
  constexpr int kScaleCubic = 1;
} // namespace

struct WirePlumberMixer::Impl {
  GMainContext* context = nullptr;
  GCancellable* cancellable = nullptr;
  WpCore* core = nullptr;
  WpObjectManager* nodesOm = nullptr; // mirrors the daemon's nodes, for id -> media.class/node.name
  WpPlugin* mixer = nullptr;          // mixer-api: volume/mute
  WpPlugin* defaultNodes = nullptr;   // default-nodes-api: default sink/source selection
  bool ready = false;                 // mixer-api active (gates volume/mute)
  bool defaultNodesReady = false;     // default-nodes-api active (gates default-device changes)

  // Writes requested before the mixer-api finished activating (~1s at startup). Keyed by node id,
  // latest value wins; flushed once ready so early volume/mute changes are not lost.
  struct PendingWrite {
    std::optional<float> volume;
    std::optional<bool> mute;
  };
  std::unordered_map<std::uint32_t, PendingWrite> pendingBeforeReady;
  // Default-device change requested before default-nodes-api activated, by node id.
  std::vector<std::uint32_t> pendingDefaultIds;

  // Pushes device volume/mute changes back to the owner (source of truth for device nodes).
  std::function<void(std::uint32_t, float, bool)> changeCb;

  // GLib poll-loop bridge state (see reference_glib_mainloop_bridge pattern).
  mutable std::vector<GPollFD> glibPollFds;
  mutable gint glibMaxPriority = G_PRIORITY_DEFAULT;
  mutable int glibPollTimeoutMs = -1;

  Impl() {
    static gboolean s_wpInit = [] {
      wp_init(static_cast<WpInitFlags>(WP_INIT_PIPEWIRE | WP_INIT_SPA_TYPES));
      return TRUE;
    }();
    (void)s_wpInit;

    context = g_main_context_new();
    cancellable = g_cancellable_new();

    // Run all WirePlumber setup with our private context pushed as the thread-default. The GTasks that
    // wp_core_load_component / wp_object_activate create capture whatever context is thread-default at
    // call time; without this they capture the global default context, which the poll bridge never
    // pumps, so their completion callbacks never fire and the mixer silently never activates.
    g_main_context_push_thread_default(context);

    core = wp_core_new(context, nullptr, nullptr);

    if (wp_core_connect(core) == FALSE) {
      kLog.warn("could not connect to PipeWire; device volume control unavailable");
      g_main_context_pop_thread_default(context);
      return;
    }

    // Track nodes so set-default can resolve an id to its media.class / node.name from the daemon's
    // own view (as wpctl does), rather than a possibly-divergent copy.
    nodesOm = wp_object_manager_new();
    wp_object_manager_add_interest(nodesOm, WP_TYPE_NODE, nullptr);
    wp_object_manager_request_object_features(nodesOm, WP_TYPE_NODE, WP_PIPEWIRE_OBJECT_FEATURES_MINIMAL);
    wp_core_install_object_manager(core, nodesOm);

    kLog.info("connected; loading mixer-api / default-nodes-api modules");

    wp_core_load_component(
        core, "libwireplumber-module-mixer-api", "module", nullptr, nullptr, cancellable, &Impl::onMixerLoaded, this
    );
    wp_core_load_component(
        core, "libwireplumber-module-default-nodes-api", "module", nullptr, nullptr, cancellable,
        &Impl::onDefaultNodesLoaded, this
    );

    // If neither module reports ready within a few seconds, device volume/mute is silently dead
    // (every write no-ops until the mixer activates). Surface it so users have something to report.
    GSource* watchdog = g_timeout_source_new_seconds(5);
    g_source_set_callback(watchdog, &Impl::onReadyWatchdog, this, nullptr);
    g_source_attach(watchdog, context);
    g_source_unref(watchdog);

    g_main_context_pop_thread_default(context);
  }

  ~Impl() {
    if (cancellable != nullptr) {
      g_cancellable_cancel(cancellable);
      g_object_unref(cancellable);
    }
    if (defaultNodes != nullptr) {
      g_object_unref(defaultNodes);
    }
    if (mixer != nullptr) {
      g_object_unref(mixer);
    }
    if (nodesOm != nullptr) {
      g_object_unref(nodesOm);
    }
    if (core != nullptr) {
      wp_core_disconnect(core);
      g_object_unref(core);
    }
    if (context != nullptr) {
      g_main_context_unref(context);
    }
  }

  static gboolean onReadyWatchdog(gpointer data) noexcept {
    auto* self = static_cast<Impl*>(data);
    if (!self->ready || !self->defaultNodesReady) {
      kLog.warn(
          "device volume control unavailable after 5s (mixer-api {}, default-nodes-api {}); volume/mute "
          "changes are being ignored — WirePlumber modules did not load or activate",
          self->ready ? "ready" : "not ready", self->defaultNodesReady ? "ready" : "not ready"
      );
    }
    return G_SOURCE_REMOVE;
  }

  static void onMixerLoaded(GObject* /*source*/, GAsyncResult* res, gpointer data) noexcept {
    auto* self = static_cast<Impl*>(data);
    GError* err = nullptr;
    if (wp_core_load_component_finish(self->core, res, &err) == FALSE) {
      kLog.warn("mixer-api load failed: {}", err != nullptr ? err->message : "unknown");
      g_clear_error(&err);
      return;
    }

    self->mixer = wp_plugin_find(self->core, "mixer-api");
    if (self->mixer == nullptr) {
      kLog.warn("mixer-api plugin not found after load");
      return;
    }

    wp_object_activate(WP_OBJECT(self->mixer), WP_PLUGIN_FEATURE_ENABLED, nullptr, &Impl::onMixerActivated, self);
  }

  static void onMixerActivated(GObject* /*source*/, GAsyncResult* res, gpointer data) noexcept {
    auto* self = static_cast<Impl*>(data);
    GError* err = nullptr;
    if (wp_object_activate_finish(WP_OBJECT(self->mixer), res, &err) == FALSE) {
      kLog.warn("mixer-api activation failed: {}", err != nullptr ? err->message : "unknown");
      g_clear_error(&err);
      return;
    }

    g_object_set(self->mixer, "scale", kScaleCubic, nullptr);
    self->ready = true;
    kLog.info("mixer-api ready");

    g_signal_connect(self->mixer, "changed", G_CALLBACK(&Impl::onMixerChanged), self);

    for (const auto& [id, write] : self->pendingBeforeReady) {
      if (write.volume.has_value()) {
        self->applyVolume(id, *write.volume);
      }
      if (write.mute.has_value()) {
        self->applyMute(id, *write.mute);
      }
    }
    self->pendingBeforeReady.clear();

    self->sweepDeviceVolumes();
  }

  // mixer-api "changed" fires with the node's global id. Runs on the poll thread during dispatch().
  static void onMixerChanged(GObject* /*mixer*/, guint id, gpointer data) noexcept {
    static_cast<Impl*>(data)->pushVolume(static_cast<std::uint32_t>(id));
  }

  bool readVolume(std::uint32_t id, float& volume, bool& muted) {
    if (mixer == nullptr) {
      return false;
    }
    GVariant* variant = nullptr;
    g_signal_emit_by_name(mixer, "get-volume", static_cast<guint>(id), &variant);
    if (variant == nullptr) {
      return false;
    }
    gdouble vol = 0.0;
    gboolean mute = FALSE;
    const gboolean hasVolume = g_variant_lookup(variant, "volume", "d", &vol);
    g_variant_lookup(variant, "mute", "b", &mute);
    g_variant_unref(variant);
    if (hasVolume == FALSE) {
      return false;
    }
    volume = static_cast<float>(vol);
    muted = mute != FALSE;
    return true;
  }

  void pushVolume(std::uint32_t id) {
    if (!changeCb) {
      return;
    }
    float volume = 0.0F;
    bool muted = false;
    if (readVolume(id, volume, muted)) {
      changeCb(id, volume, muted);
    }
  }

  // Emit an initial value for every device node the daemon already knows, so volume is correct
  // without waiting for the first user/external change. "changed" backstops nodes mixer-api has not
  // finished tracking yet.
  void sweepDeviceVolumes() {
    if (nodesOm == nullptr || !changeCb) {
      return;
    }
    g_autoptr(WpIterator) it = wp_object_manager_new_iterator(nodesOm);
    if (it == nullptr) {
      return;
    }
    GValue val = G_VALUE_INIT;
    while (wp_iterator_next(it, &val) != FALSE) {
      auto* obj = static_cast<WpPipewireObject*>(g_value_get_object(&val));
      if (obj != nullptr) {
        const gchar* mediaClass = wp_pipewire_object_get_property(obj, PW_KEY_MEDIA_CLASS);
        if (mediaClass != nullptr
            && (g_str_has_prefix(mediaClass, "Audio/Sink") || g_str_has_prefix(mediaClass, "Audio/Source"))) {
          pushVolume(wp_proxy_get_bound_id(WP_PROXY(obj)));
        }
      }
      g_value_unset(&val);
    }
  }

  static void onDefaultNodesLoaded(GObject* /*source*/, GAsyncResult* res, gpointer data) noexcept {
    auto* self = static_cast<Impl*>(data);
    GError* err = nullptr;
    if (wp_core_load_component_finish(self->core, res, &err) == FALSE) {
      kLog.warn("default-nodes-api load failed: {}", err != nullptr ? err->message : "unknown");
      g_clear_error(&err);
      return;
    }

    self->defaultNodes = wp_plugin_find(self->core, "default-nodes-api");
    if (self->defaultNodes == nullptr) {
      kLog.warn("default-nodes-api plugin not found after load");
      return;
    }

    wp_object_activate(
        WP_OBJECT(self->defaultNodes), WP_PLUGIN_FEATURE_ENABLED, nullptr, &Impl::onDefaultNodesActivated, self
    );
  }

  static void onDefaultNodesActivated(GObject* /*source*/, GAsyncResult* res, gpointer data) noexcept {
    auto* self = static_cast<Impl*>(data);
    GError* err = nullptr;
    if (wp_object_activate_finish(WP_OBJECT(self->defaultNodes), res, &err) == FALSE) {
      kLog.warn("default-nodes-api activation failed: {}", err != nullptr ? err->message : "unknown");
      g_clear_error(&err);
      return;
    }

    self->defaultNodesReady = true;
    kLog.info("default-nodes-api ready");

    for (const std::uint32_t id : self->pendingDefaultIds) {
      self->applyDefault(id);
    }
    self->pendingDefaultIds.clear();
  }

  void requestVolume(std::uint32_t id, float volume) {
    if (ready) {
      applyVolume(id, volume);
    } else {
      pendingBeforeReady[id].volume = volume;
    }
  }

  void requestMute(std::uint32_t id, bool muted) {
    if (ready) {
      applyMute(id, muted);
    } else {
      pendingBeforeReady[id].mute = muted;
    }
  }

  void applyVolume(std::uint32_t id, float volume) { emit(id, "volume", g_variant_new_double(volume)); }
  void applyMute(std::uint32_t id, bool muted) { emit(id, "mute", g_variant_new_boolean(muted ? TRUE : FALSE)); }

  void requestDefault(std::uint32_t id) {
    if (defaultNodesReady) {
      applyDefault(id);
    } else {
      pendingDefaultIds.push_back(id);
    }
  }

  // Mirrors wpctl set-default: resolve the node in our own WpCore, read its media.class + node.name,
  // and drive default-nodes-api, then sync so the write reaches the daemon.
  void applyDefault(std::uint32_t id) {
    if (defaultNodes == nullptr || nodesOm == nullptr) {
      kLog.warn("set-default: WirePlumber not ready");
      return;
    }

    auto* node = static_cast<WpPipewireObject*>(wp_object_manager_lookup(
        nodesOm, WP_TYPE_NODE, WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY, "object.id", "=u", static_cast<guint32>(id),
        nullptr
    ));
    if (node == nullptr) {
      kLog.warn("set-default: node {} not found", id);
      return;
    }

    const gchar* mediaClass = wp_pipewire_object_get_property(node, PW_KEY_MEDIA_CLASS);
    const gchar* name = wp_pipewire_object_get_property(node, PW_KEY_NODE_NAME);
    if (mediaClass != nullptr && name != nullptr) {
      for (const char* cls : {"Audio/Sink", "Audio/Source", "Video/Source"}) {
        if (g_str_has_prefix(mediaClass, cls) && !g_str_has_suffix(mediaClass, "/Internal")) {
          gboolean res = FALSE;
          g_signal_emit_by_name(defaultNodes, "set-default-configured-node-name", cls, name, &res);
          if (res == FALSE) {
            kLog.warn("set-default rejected for node {} ({})", id, name);
          }
          break;
        }
      }
    } else {
      kLog.warn("set-default: node {} missing media.class/node.name", id);
    }

    g_object_unref(node);
    // wpctl calls wp_core_sync here only to flush before it exits; our persistent loop flushes the
    // write on the next dispatch after the wakeup, so no sync is needed.
    g_main_context_wakeup(context);
  }

  void emit(std::uint32_t id, const char* key, GVariant* value) {
    if (mixer == nullptr) {
      return;
    }
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", key, value);
    GVariant* variant = g_variant_ref_sink(g_variant_builder_end(&builder));
    gboolean res = FALSE;
    g_signal_emit_by_name(mixer, "set-volume", static_cast<guint>(id), variant, &res);
    g_variant_unref(variant);

    // Wake the loop so the queued write flushes on the next dispatch. A synchronous drain here
    // chews through an ever-growing echo backlog under held media keys and lags the final value.
    g_main_context_wakeup(context);
  }

  mutable bool acquireWarned = false;

  void addPollFds(std::vector<pollfd>& fds) const {
    glibPollFds.clear();
    glibMaxPriority = G_PRIORITY_DEFAULT;
    glibPollTimeoutMs = -1;

    if (g_main_context_acquire(context) == FALSE) {
      if (!acquireWarned) {
        acquireWarned = true;
        kLog.warn("could not acquire mixer GLib context; module-load events will not be pumped");
      }
      return;
    }

    const gboolean ready_ = g_main_context_prepare(context, &glibMaxPriority);
    gint timeout = -1;
    const gint count = g_main_context_query(context, glibMaxPriority, &timeout, nullptr, 0);
    glibPollTimeoutMs = ready_ != FALSE ? 0 : timeout;
    if (count > 0) {
      glibPollFds.resize(static_cast<std::size_t>(count));
      g_main_context_query(context, glibMaxPriority, &timeout, glibPollFds.data(), count);
      glibPollTimeoutMs = ready_ != FALSE ? 0 : timeout;
      for (const GPollFD& glibFd : glibPollFds) {
        fds.push_back({.fd = glibFd.fd, .events = static_cast<short>(glibFd.events), .revents = 0});
      }
    }
    g_main_context_release(context);
  }

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
    if (g_main_context_acquire(context) == FALSE) {
      return;
    }
    // Keep our context thread-default while dispatching so async work started from callbacks (e.g.
    // wp_object_activate) captures it rather than the global default context (see the constructor).
    g_main_context_push_thread_default(context);
    for (std::size_t i = 0; i < glibPollFds.size(); ++i) {
      const std::size_t pollIndex = startIdx + i;
      glibPollFds[i].revents =
          pollIndex < fds.size() ? static_cast<gushort>(fds[pollIndex].revents) : static_cast<gushort>(0);
    }
    const gboolean ready_ =
        g_main_context_check(context, glibMaxPriority, glibPollFds.data(), static_cast<gint>(glibPollFds.size()));
    if (ready_ != FALSE) {
      g_main_context_dispatch(context);
    }
    g_main_context_pop_thread_default(context);
    g_main_context_release(context);
  }
};

WirePlumberMixer::WirePlumberMixer() : m_impl(std::make_unique<Impl>()) {}
WirePlumberMixer::~WirePlumberMixer() = default;

bool WirePlumberMixer::ready() const noexcept { return m_impl->ready; }

void WirePlumberMixer::setVolume(std::uint32_t id, float volume) { m_impl->requestVolume(id, volume); }

void WirePlumberMixer::setMuted(std::uint32_t id, bool muted) { m_impl->requestMute(id, muted); }

void WirePlumberMixer::setDefaultNode(std::uint32_t id) { m_impl->requestDefault(id); }

void WirePlumberMixer::setChangeCallback(ChangeCallback callback) { m_impl->changeCb = std::move(callback); }

int WirePlumberMixer::pollTimeoutMs() const {
  // WpCore's async connect/load/activate makes GLib vote 0 ("dispatch now") in a burst, which
  // hot-spins the shared loop until the mixer is ready. Socket wakeups still come through the
  // polled fds, so flooring a 0 vote to a few ms paces the spin without slowing real progress.
  const int t = m_impl->glibPollTimeoutMs;
  return t == 0 ? 4 : t;
}

void WirePlumberMixer::doAddPollFds(std::vector<pollfd>& fds) { m_impl->addPollFds(fds); }

void WirePlumberMixer::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  m_impl->dispatch(fds, startIdx);
}
