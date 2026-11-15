#include "filemanager/mount_backends.h"

#include <algorithm>
#include <gio/gunixmounts.h>
#include <memory>
#include <string_view>

namespace filemanager {
  namespace {
    constexpr const char* systemd = "org.freedesktop.systemd1";
    constexpr const char* manager = "/org/freedesktop/systemd1";
    constexpr const char* udisks = "org.freedesktop.UDisks2";
    using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
    Variant owned(GVariant* value) { return {value, g_variant_unref}; }
    std::string deviceSource(std::string source) {
      if (!source.starts_with("/dev/"))
        return source;
      std::error_code error;
      const auto resolved = std::filesystem::canonical(source, error);
      return error ? source : resolved.string();
    }
    bool within(const std::filesystem::path& path, const std::filesystem::path& parent) {
      const auto relative = path.lexically_relative(parent);
      return !relative.empty() && *relative.begin() != "..";
    }
    std::string stringProperty(GVariant* props, const char* name) {
      const char* value = nullptr;
      g_variant_lookup(props, name, "&s", &value);
      return value == nullptr ? "" : value;
    }
    Variant call(
        GDBusConnection* bus, const char* service, const char* path, const char* interface, const char* method,
        GVariant* args, GError** error = nullptr, GCancellable* cancellable = nullptr, bool interactive = false
    ) {
      return owned(g_dbus_connection_call_sync(
          bus, service, path, interface, method, args, nullptr,
          GDBusCallFlags(
              G_DBUS_CALL_FLAGS_NO_AUTO_START
              | (interactive ? G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION : G_DBUS_CALL_FLAGS_NONE)
          ),
          interactive ? 120000 : 3000, cancellable, error
      ));
    }
    Variant properties(GDBusConnection* bus, const char* service, const char* path, const char* interface) {
      auto reply =
          call(bus, service, path, "org.freedesktop.DBus.Properties", "GetAll", g_variant_new("(s)", interface));
      return owned(reply ? g_variant_get_child_value(reply.get(), 0) : nullptr);
    }
    std::vector<ManagedMount> discover(GDBusConnection* bus) {
      std::vector<ManagedMount> result;
      auto objects = call(
          bus, udisks, "/org/freedesktop/UDisks2", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr
      );
      if (objects) {
        auto dict = owned(g_variant_get_child_value(objects.get(), 0));
        GVariantIter iter;
        g_variant_iter_init(&iter, dict.get());
        const char* path;
        GVariant* raw;
        while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &raw)) {
          auto interfaces = owned(raw);
          auto fs = owned(g_variant_lookup_value(raw, "org.freedesktop.UDisks2.Filesystem", G_VARIANT_TYPE_VARDICT));
          auto block = owned(g_variant_lookup_value(raw, "org.freedesktop.UDisks2.Block", G_VARIANT_TYPE_VARDICT));
          if (!fs || !block)
            continue;
          auto device = owned(g_variant_lookup_value(block.get(), "Device", G_VARIANT_TYPE_BYTESTRING));
          auto points = owned(g_variant_lookup_value(fs.get(), "MountPoints", G_VARIANT_TYPE("aay")));
          if (!device || !points)
            continue;
          const std::string source = deviceSource(g_variant_get_bytestring(device.get()));
          for (gsize i = 0; i < g_variant_n_children(points.get()); ++i) {
            auto point = owned(g_variant_get_child_value(points.get(), i));
            result.push_back(
                {MountBackend::UDisks,
                 {g_variant_get_bytestring(point.get()), source, stringProperty(block.get(), "IdType"), "/"},
                 path,
                 {}}
            );
          }
        }
      }
      auto units = call(bus, systemd, manager, "org.freedesktop.systemd1.Manager", "ListUnits", nullptr);
      if (units) {
        auto list = owned(g_variant_get_child_value(units.get(), 0));
        for (gsize i = 0; i < g_variant_n_children(list.get()); ++i) {
          auto row = owned(g_variant_get_child_value(list.get(), i));
          auto nameValue = owned(g_variant_get_child_value(row.get(), 0));
          const std::string name = g_variant_get_string(nameValue.get(), nullptr);
          if (!name.ends_with(".mount"))
            continue;
          auto state = owned(g_variant_get_child_value(row.get(), 3));
          if (std::string_view(g_variant_get_string(state.get(), nullptr)) != "active")
            continue;
          auto objectValue = owned(g_variant_get_child_value(row.get(), 6));
          const std::string object = g_variant_get_string(objectValue.get(), nullptr);
          auto unit = properties(bus, systemd, object.c_str(), "org.freedesktop.systemd1.Unit");
          auto mount = properties(bus, systemd, object.c_str(), "org.freedesktop.systemd1.Mount");
          if (unit && mount && safeMountUnit(unit.get(), mount.get()))
            result.push_back(
                {MountBackend::Systemd,
                 {stringProperty(mount.get(), "Where"),
                  deviceSource(stringProperty(mount.get(), "What")),
                  stringProperty(mount.get(), "Type"),
                  {}},
                 object,
                 name}
            );
        }
      }
      return result;
    }

    struct Job {
      std::string path;
      std::vector<std::pair<std::string, std::string>> results;
      bool timedOut = false;
      bool unavailable = false;
    };
    std::string stopUnit(GDBusConnection* bus, const ManagedMount& mount) {
      // A private context queues JobRemoved even if it arrives before StopUnit replies.
      auto* context = g_main_context_new();
      g_main_context_push_thread_default(context);
      Job job;
      const auto ownerSubscription = g_dbus_connection_signal_subscribe(
          bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", systemd,
          G_DBUS_SIGNAL_FLAGS_NONE,
          +[](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* args, gpointer data) {
            const char *name, *oldOwner, *newOwner;
            g_variant_get(args, "(&s&s&s)", &name, &oldOwner, &newOwner);
            if (*oldOwner != '\0')
              static_cast<Job*>(data)->unavailable = true;
          },
          &job, nullptr
      );
      const auto subscription = g_dbus_connection_signal_subscribe(
          bus, systemd, "org.freedesktop.systemd1.Manager", "JobRemoved", manager, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
          +[](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* args, gpointer data) {
            guint id;
            const char *path, *unit, *result;
            g_variant_get(args, "(u&o&s&s)", &id, &path, &unit, &result);
            static_cast<Job*>(data)->results.emplace_back(path, result);
          },
          &job, nullptr
      );
      GError* error = nullptr;
      auto subscribed = call(bus, systemd, manager, "org.freedesktop.systemd1.Manager", "Subscribe", nullptr, &error);
      std::string message;
      if (!subscribed)
        message = mountBackendError(error);
      g_clear_error(&error);
      if (subscribed) {
        auto reply = call(
            bus, systemd, manager, "org.freedesktop.systemd1.Manager", "StopUnit",
            g_variant_new("(ss)", mount.unit.c_str(), "fail"), &error, nullptr, true
        );
        if (!reply)
          message = mountBackendError(error);
        else {
          const char* path;
          g_variant_get(reply.get(), "(&o)", &path);
          job.path = path;
          auto* deadline = g_timeout_source_new_seconds(120);
          g_source_set_callback(
              deadline,
              +[](gpointer data) -> gboolean {
                static_cast<Job*>(data)->timedOut = true;
                return G_SOURCE_REMOVE;
              },
              &job, nullptr
          );
          g_source_attach(deadline, context);
          // Do not release the operation guard on caller cancellation once queued.
          while (!job.timedOut && !job.unavailable) {
            const auto found = std::ranges::find(job.results, job.path, &decltype(job.results)::value_type::first);
            if (found != job.results.end()) {
              if (found->second != "done")
                message = found->second == "canceled"
                    ? "Device operation cancelled."
                    : "Unmount failed. The mount may be busy or a dependent unit prevents removal.";
              break;
            }
            g_main_context_iteration(context, true);
          }
          if (job.timedOut)
            message = "Unmount did not finish in time. Check the device state before retrying.";
          if (job.unavailable)
            message = "The mount service disconnected before reporting completion. Check the device state.";
          g_source_destroy(deadline);
          g_source_unref(deadline);
        }
        g_clear_error(&error);
        call(bus, systemd, manager, "org.freedesktop.systemd1.Manager", "Unsubscribe", nullptr);
      }
      g_dbus_connection_signal_unsubscribe(bus, subscription);
      g_dbus_connection_signal_unsubscribe(bus, ownerSubscription);
      g_main_context_pop_thread_default(context);
      g_main_context_unref(context);
      return message;
    }
  } // namespace

  bool protectedMount(const std::filesystem::path& path) {
    if (!path.is_absolute())
      return true;
    const auto normalized = path.lexically_normal();
    if (normalized == "/")
      return true;
    for (const auto* critical :
         {"/home", "/boot", "/efi", "/usr", "/var", "/etc", "/dev", "/proc", "/sys", "/run", "/root", "/opt", "/tmp"}) {
      if (within(std::filesystem::path(critical), normalized))
        return true;
      if (std::string_view(critical) == "/home")
        continue;
      if (within(normalized, critical) && !within(normalized, "/run/media"))
        return true;
    }
    return false;
  }

  bool isolatedFilesystem(const MountRecord& mount, const std::vector<MountRecord>& mounts) {
    if (mount.systemCritical || protectedMount(mount.path) || mount.root != "/" || mount.type == "btrfs")
      return false;
    return std::ranges::count(mounts, mount.source, &MountRecord::source) == 1;
  }

  bool safeMountUnit(GVariant* unit, GVariant* mount) {
    gboolean canStop = false, refused = true, force = true, lazy = true;
    g_variant_lookup(unit, "CanStop", "b", &canStop);
    g_variant_lookup(unit, "RefuseManualStop", "b", &refused);
    g_variant_lookup(mount, "ForceUnmount", "b", &force);
    g_variant_lookup(mount, "LazyUnmount", "b", &lazy);
    if (!canStop
        || refused
        || force
        || lazy
        || stringProperty(unit, "ActiveState") != "active"
        || protectedMount(stringProperty(mount, "Where")))
      return false;
    // Stopping a mount must not stop required/bound services or other mounts.
    for (const auto* key : {"RequiredBy", "RequisiteOf", "BoundBy", "ConsistsOf", "PropagatesStopTo"}) {
      auto deps = owned(g_variant_lookup_value(unit, key, G_VARIANT_TYPE_STRING_ARRAY));
      if (!deps || g_variant_n_children(deps.get()) != 0)
        return false;
    }
    return true;
  }

  std::vector<MountRecord> mountRecords() {
    std::vector<MountRecord> result;
    GList* mounts = g_unix_mount_entries_get(nullptr);
    for (auto* it = mounts; it; it = it->next) {
      auto* entry = static_cast<GUnixMountEntry*>(it->data);
      const auto* root = g_unix_mount_entry_get_root_path(entry);
      result.push_back(
          {g_unix_mount_entry_get_mount_path(entry), deviceSource(g_unix_mount_entry_get_device_path(entry)),
           g_unix_mount_entry_get_fs_type(entry), root ? root : ""}
      );
    }
    g_list_free_full(mounts, reinterpret_cast<GDestroyNotify>(g_unix_mount_entry_free));
    // Account for system paths relocated through symlinks, including Home.
    for (const auto* critical :
         {"/home", "/boot", "/efi", "/usr", "/var", "/etc", "/dev", "/proc", "/sys", "/run", "/root", "/opt", "/tmp",
          g_get_home_dir()}) {
      std::error_code error;
      const auto resolved = std::filesystem::canonical(critical, error);
      if (!error)
        for (auto& record : result)
          if (within(resolved, record.path))
            record.systemCritical = true;
    }
    return result;
  }

  std::string mountBackendError(const GError* error) {
    if (!error)
      return "The mount service is unavailable.";
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      return "Device operation cancelled.";
    char* raw = g_dbus_error_get_remote_error(error);
    const std::string name = raw ? raw : "";
    g_free(raw);
    if (name.ends_with("NotAuthorizedDismissed"))
      return "Authorisation cancelled.";
    if (name.find("NotAuthorized") != std::string::npos || name.ends_with("AccessDenied"))
      return "Authorisation was denied. Check that a desktop authentication agent is running.";
    if (name.ends_with("DeviceBusy"))
      return "This device is busy. Close open files and try again.";
    if (name.ends_with("UnknownObject") || name.ends_with("NoSuchUnit"))
      return "The device or mount is no longer available.";
    if (name.ends_with("ServiceUnknown")
        || name.ends_with("NameHasNoOwner")
        || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CLOSED))
      return "The mount service is unavailable.";
    return "Device operation failed. The mount may be busy, disconnected or restricted by system policy.";
  }

  void discoverManagedMounts(std::function<void(std::vector<ManagedMount>)> completion) {
    auto* callback = new decltype(completion)(std::move(completion));
    auto* asyncTask = g_task_new(
        nullptr, nullptr,
        +[](GObject*, GAsyncResult* result, gpointer data) {
          std::unique_ptr<std::function<void(std::vector<ManagedMount>)>> done(
              static_cast<std::function<void(std::vector<ManagedMount>)>*>(data)
          );
          std::unique_ptr<std::vector<ManagedMount>> mounts(
              static_cast<std::vector<ManagedMount>*>(g_task_propagate_pointer(G_TASK(result), nullptr))
          );
          (*done)(mounts ? std::move(*mounts) : std::vector<ManagedMount>{});
        },
        callback
    );
    g_task_run_in_thread(
        asyncTask, +[](GTask* task, gpointer, gpointer, GCancellable*) {
          auto* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
          auto* result = new std::vector<ManagedMount>(bus ? discover(bus) : std::vector<ManagedMount>{});
          if (bus)
            g_object_unref(bus);
          g_task_return_pointer(
              task, result, +[](gpointer data) { delete static_cast<std::vector<ManagedMount>*>(data); }
          );
        }
    );
    g_object_unref(asyncTask);
  }

  void operateManagedMount(
      const ManagedMount& mount, GCancellable* cancellable, std::function<void(std::string)> completion,
      std::function<std::vector<MountRecord>()> snapshot
  ) {
    struct Request {
      ManagedMount mount;
      std::function<void(std::string)> done;
      std::function<std::vector<MountRecord>()> snapshot;
    };
    auto* asyncTask = g_task_new(
        nullptr, cancellable,
        +[](GObject*, GAsyncResult* result, gpointer) {
          auto* request = static_cast<Request*>(g_task_get_task_data(G_TASK(result)));
          std::unique_ptr<std::string> message(
              static_cast<std::string*>(g_task_propagate_pointer(G_TASK(result), nullptr))
          );
          request->done(message ? *message : "Device operation cancelled.");
        },
        nullptr
    );
    g_task_set_check_cancellable(asyncTask, false);
    g_task_set_task_data(
        asyncTask, new Request{mount, std::move(completion), std::move(snapshot)},
        +[](gpointer data) { delete static_cast<Request*>(data); }
    );
    g_task_run_in_thread(
        asyncTask, +[](GTask* task, gpointer, gpointer data, GCancellable* cancellation) {
          const auto& requested = static_cast<Request*>(data)->mount;
          std::string message;
          GError* error = nullptr;
          auto* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, cancellation, &error);
          if (!bus)
            message = mountBackendError(error);
          else {
            const auto current = discover(bus);
            const auto records = static_cast<Request*>(data)->snapshot();
            const auto actual = std::ranges::find_if(records, [&](const auto& record) {
              return record.path == requested.mount.path
                  && record.source == requested.mount.source
                  && record.type == requested.mount.type
                  && record.root == requested.mount.root;
            });
            const auto backend = std::ranges::find_if(current, [&](const auto& entry) {
              return entry.backend == requested.backend
                  && entry.object == requested.object
                  && entry.unit == requested.unit
                  && entry.mount.path == requested.mount.path
                  && entry.mount.source == requested.mount.source;
            });
            if (backend == current.end())
              message = "The mount service is unavailable or this mount is no longer safely manageable.";
            else if (
                actual == records.end()
                || protectedMount(requested.mount.path)
                || actual->systemCritical
                || std::ranges::count(records, requested.mount.path, &MountRecord::path) != 1
                || (requested.backend == MountBackend::UDisks && !isolatedFilesystem(*actual, records))
            )
              message = "The mount changed or can no longer be safely managed. Open its menu again.";
            else if (cancellation && g_cancellable_is_cancelled(cancellation))
              message = "Device operation cancelled.";
            else if (requested.backend == MountBackend::Systemd)
              message = stopUnit(bus, requested);
            else {
              GVariantBuilder options;
              g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
              auto reply = call(
                  bus, udisks, requested.object.c_str(), "org.freedesktop.UDisks2.Filesystem", "Unmount",
                  g_variant_new("(a{sv})", &options), &error, nullptr, true
              );
              if (!reply)
                message = mountBackendError(error);
            }
            g_object_unref(bus);
          }
          g_clear_error(&error);
          g_task_return_pointer(
              task, new std::string(std::move(message)),
              +[](gpointer value) { delete static_cast<std::string*>(value); }
          );
        }
    );
    g_object_unref(asyncTask);
  }
} // namespace filemanager
