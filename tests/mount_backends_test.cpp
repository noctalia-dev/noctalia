#include "filemanager/mount_backends.h"
#include "filemanager/places_model.h"
#include "test_isolation.h"

#include <algorithm>
#include <atomic>
#include <future>
#include <thread>

using namespace filemanager;

namespace {
  GVariant* unitProperties(bool dependent = false, bool force = false) {
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&props, "{sv}", "CanStop", g_variant_new_boolean(true));
    g_variant_builder_add(&props, "{sv}", "RefuseManualStop", g_variant_new_boolean(false));
    g_variant_builder_add(&props, "{sv}", "ActiveState", g_variant_new_string("active"));
    const char* dependencies[] = {"important.service", nullptr};
    for (const auto* key : {"RequiredBy", "RequisiteOf", "BoundBy", "ConsistsOf", "PropagatesStopTo"})
      g_variant_builder_add(
          &props, "{sv}", key, g_variant_new_strv(dependent ? dependencies : nullptr, dependent ? -1 : 0)
      );
    g_variant_builder_add(&props, "{sv}", "Where", g_variant_new_string("/mnt/fixture"));
    g_variant_builder_add(&props, "{sv}", "What", g_variant_new_string("server:/share"));
    g_variant_builder_add(&props, "{sv}", "Type", g_variant_new_string("nfs4"));
    g_variant_builder_add(&props, "{sv}", "ForceUnmount", g_variant_new_boolean(force));
    g_variant_builder_add(&props, "{sv}", "LazyUnmount", g_variant_new_boolean(false));
    return g_variant_ref_sink(g_variant_builder_end(&props));
  }

  struct Service {
    std::atomic<int> operations = 0;
    std::atomic<bool> deny = false, busy = false, dismissed = false, failJob = false, disconnectJob = false;
    std::atomic<bool> releaseOk = true;
    bool setupOk = false;
    GMainContext* context = nullptr;
    GMainLoop* loop = nullptr;
    std::thread thread;
    Service(const char* address) {
      std::promise<bool> ready;
      auto future = ready.get_future();
      thread = std::thread([&, address] {
        context = g_main_context_new();
        g_main_context_push_thread_default(context);
        loop = g_main_loop_new(context, false);
        GError* error = nullptr;
        auto* bus = g_dbus_connection_new_for_address_sync(
            address,
            GDBusConnectionFlags(
                G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
            ),
            nullptr, nullptr, &error
        );
        if (bus == nullptr || error != nullptr) {
          g_clear_error(&error);
          if (bus != nullptr)
            g_object_unref(bus);
          g_main_context_pop_thread_default(context);
          ready.set_value(false);
          return;
        }
        const char* xml = R"(<node>
          <interface name='org.freedesktop.DBus.ObjectManager'>
            <method name='GetManagedObjects'><arg type='a{oa{sa{sv}}}' direction='out'/></method>
          </interface>
          <interface name='org.freedesktop.DBus.Properties'>
            <method name='GetAll'><arg type='s' direction='in'/><arg type='a{sv}' direction='out'/></method>
          </interface>
          <interface name='org.freedesktop.UDisks2.Filesystem'>
            <method name='Unmount'><arg type='a{sv}' direction='in'/></method>
          </interface>
          <interface name='org.freedesktop.systemd1.Manager'>
            <method name='ListUnits'><arg type='a(ssssssouso)' direction='out'/></method>
            <method name='Subscribe'/><method name='Unsubscribe'/>
            <method name='StopUnit'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='o' direction='out'/></method>
            <signal name='JobRemoved'><arg type='u'/><arg type='o'/><arg type='s'/><arg type='s'/></signal>
          </interface>
        </node>)";
        auto* info = g_dbus_node_info_new_for_xml(xml, &error);
        if (info == nullptr || error != nullptr) {
          g_clear_error(&error);
          if (info != nullptr)
            g_dbus_node_info_unref(info);
          g_object_unref(bus);
          g_main_context_pop_thread_default(context);
          ready.set_value(false);
          return;
        }
        static const GDBusInterfaceVTable vtable = {
            +[](GDBusConnection* bus, const char*, const char*, const char*, const char* method, GVariant*,
                GDBusMethodInvocation* invocation, gpointer data) {
              auto& self = *static_cast<Service*>(data);
              if (std::string_view(method) == "GetManagedObjects") {
                GVariantBuilder objects, interfaces, fs, block, points;
                g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
                g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
                g_variant_builder_init(&fs, G_VARIANT_TYPE_VARDICT);
                g_variant_builder_init(&block, G_VARIANT_TYPE_VARDICT);
                g_variant_builder_init(&points, G_VARIANT_TYPE("aay"));
                g_variant_builder_add_value(&points, g_variant_new_bytestring("/mnt/disk"));
                g_variant_builder_add(&fs, "{sv}", "MountPoints", g_variant_builder_end(&points));
                g_variant_builder_add(&block, "{sv}", "Device", g_variant_new_bytestring("/dev/mock"));
                g_variant_builder_add(&block, "{sv}", "IdType", g_variant_new_string("ext4"));
                g_variant_builder_add(
                    &interfaces, "{s@a{sv}}", "org.freedesktop.UDisks2.Filesystem", g_variant_builder_end(&fs)
                );
                g_variant_builder_add(
                    &interfaces, "{s@a{sv}}", "org.freedesktop.UDisks2.Block", g_variant_builder_end(&block)
                );
                g_variant_builder_add(&objects, "{o@a{sa{sv}}}", "/mock/block", g_variant_builder_end(&interfaces));
                g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{oa{sa{sv}}})", &objects));
              } else if (std::string_view(method) == "ListUnits") {
                GVariantBuilder rows;
                g_variant_builder_init(&rows, G_VARIANT_TYPE("a(ssssssouso)"));
                g_variant_builder_add(
                    &rows, "(ssssssouso)", "mnt-fixture.mount", "fixture", "loaded", "active", "mounted", "",
                    "/mock/unit", 0U, "", "/"
                );
                g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(ssssssouso))", &rows));
              } else if (std::string_view(method) == "GetAll") {
                auto* props = unitProperties();
                g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a{sv})", props));
              } else if (std::string_view(method) == "Subscribe" || std::string_view(method) == "Unsubscribe") {
                g_dbus_method_invocation_return_value(invocation, nullptr);
              } else {
                ++self.operations;
                if (self.deny || self.dismissed || self.busy) {
                  g_dbus_method_invocation_return_dbus_error(
                      invocation,
                      self.deny            ? "org.freedesktop.UDisks2.Error.NotAuthorized"
                          : self.dismissed ? "org.freedesktop.UDisks2.Error.NotAuthorizedDismissed"
                                           : "org.freedesktop.UDisks2.Error.DeviceBusy",
                      "fixture diagnostic"
                  );
                } else if (std::string_view(method) == "StopUnit") {
                  if (self.disconnectJob) {
                    g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", "/mock/job"));
                    auto* released = g_dbus_connection_call_sync(
                        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ReleaseName",
                        g_variant_new("(s)", "org.freedesktop.systemd1"), nullptr, G_DBUS_CALL_FLAGS_NONE, 1000,
                        nullptr, nullptr
                    );
                    if (released == nullptr)
                      self.releaseOk.store(false, std::memory_order_release);
                    else
                      g_variant_unref(released);
                    return;
                  }
                  // Deliberately emit before the method reply to exercise the job race.
                  g_dbus_connection_emit_signal(
                      bus, nullptr, "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "JobRemoved",
                      g_variant_new("(uoss)", 1U, "/mock/job", "mnt-fixture.mount", self.failJob ? "failed" : "done"),
                      nullptr
                  );
                  g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", "/mock/job"));
                } else
                  g_dbus_method_invocation_return_value(invocation, nullptr);
              }
            },
            nullptr,
            nullptr,
            {nullptr}
        };
        bool regOk = true;
        for (auto** iface = info->interfaces; *iface; ++iface) {
          const std::string_view name((*iface)->name);
          const char* path = name == "org.freedesktop.DBus.ObjectManager" ? "/org/freedesktop/UDisks2"
              : name == "org.freedesktop.DBus.Properties"                 ? "/mock/unit"
              : name == "org.freedesktop.UDisks2.Filesystem"              ? "/mock/block"
                                                                          : "/org/freedesktop/systemd1";
          if (!g_dbus_connection_register_object(bus, path, *iface, &vtable, this, nullptr, &error)
              || error != nullptr) {
            g_clear_error(&error);
            regOk = false;
            break;
          }
        }
        for (const auto* name : {"org.freedesktop.UDisks2", "org.freedesktop.systemd1"}) {
          if (!regOk)
            break;
          auto* reply = g_dbus_connection_call_sync(
              bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
              g_variant_new("(su)", name, 0U), nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error
          );
          if (reply == nullptr || error != nullptr) {
            g_clear_error(&error);
            if (reply != nullptr)
              g_variant_unref(reply);
            regOk = false;
            break;
          }
          g_variant_unref(reply);
        }
        if (!regOk) {
          g_dbus_node_info_unref(info);
          g_object_unref(bus);
          g_main_context_pop_thread_default(context);
          ready.set_value(false);
          return;
        }
        ready.set_value(true);
        g_main_loop_run(loop);
        g_dbus_node_info_unref(info);
        g_dbus_connection_close_sync(bus, nullptr, nullptr);
        g_object_unref(bus);
        g_main_context_pop_thread_default(context);
      });
      setupOk = future.get();
    }
    ~Service() {
      if (setupOk) {
        g_main_context_invoke(
            context,
            +[](gpointer data) -> gboolean {
              g_main_loop_quit(static_cast<Service*>(data)->loop);
              return G_SOURCE_REMOVE;
            },
            this
        );
      }
      if (thread.joinable())
        thread.join();
      if (loop != nullptr)
        g_main_loop_unref(loop);
      if (context != nullptr)
        g_main_context_unref(context);
    }
  };
} // namespace

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-mount-backends"});
    // Pure fixtures never inspect or change a real device.
    for (const auto* path :
         {"/", "/home", "/boot", "/boot/efi", "/efi", "/var", "/usr/local", "/run", "/root", "relative"})
      TEST_CHECK(protectedMount(path));
    TEST_CHECK(!protectedMount("/mnt/data") && !protectedMount("/run/media/user/disk"));
    TEST_CHECK(!protectedMount("/home/user/cloud"));
    MountRecord disk{"/mnt/disk", "/dev/mock", "ext4", "/"};
    TEST_CHECK(isolatedFilesystem(disk, {disk}));
    auto other = disk;
    other.path = "/home";
    TEST_CHECK(!isolatedFilesystem(disk, {disk, other}));
    other = disk;
    other.type = "btrfs";
    TEST_CHECK(!isolatedFilesystem(other, {other}));
    other = disk;
    other.root = "/subdirectory";
    TEST_CHECK(!isolatedFilesystem(other, {other}));
    Place place{.path = disk.path, .canUnmount = true};
    applyMountSafety(place, {disk}, {});
    TEST_CHECK(place.canUnmount); // Advertised GIO capability is retained when safe.
    place.path = "/home";
    applyMountSafety(place, {other}, {});
    TEST_CHECK(!place.canUnmount);
    ManagedMount udisks{MountBackend::UDisks, disk, "/mock/block", {}};
    place = {.path = disk.path};
    applyMountSafety(place, {disk}, {udisks});
    TEST_CHECK(place.canUnmount && place.managedMount.backend == MountBackend::UDisks);
    place = {.path = disk.path};
    applyMountSafety(place, {disk, other}, {udisks});
    TEST_CHECK(!place.canUnmount);
    MountRecord nfs{"/mnt/fixture", "server:/share", "nfs4", "/"};
    ManagedMount systemd{MountBackend::Systemd, nfs, "/mock/unit", "mnt-fixture.mount"};
    place = {.path = nfs.path, .network = true};
    applyMountSafety(place, {nfs}, {systemd});
    TEST_CHECK(place.canUnmount && !place.canEject && place.network);
    place = {.path = nfs.path};
    applyMountSafety(place, {nfs}, {});
    TEST_CHECK(!place.canUnmount); // Unix-only NFS without a safe service stays unsupported.
    auto btrfs = disk;
    btrfs.type = "btrfs";
    btrfs.root = "/@data";
    ManagedMount exact{MountBackend::Systemd, btrfs, "/mock/subvolume", "mnt-disk.mount"};
    place = {.path = btrfs.path, .canUnmount = true, .canEject = true};
    applyMountSafety(place, {btrfs}, {udisks});
    TEST_CHECK(!place.canUnmount && !place.canEject);
    applyMountSafety(place, {btrfs}, {exact});
    TEST_CHECK(place.canUnmount && place.managedMount.backend == MountBackend::Systemd);
    btrfs.systemCritical = true; // A relocated system path takes precedence over all backends.
    applyMountSafety(place, {btrfs}, {exact});
    TEST_CHECK(!place.canUnmount);
    std::vector<Place> duplicates{{.identity = "same", .path = disk.path}, {.identity = "same", .path = disk.path}};
    orderPlaces(duplicates, {});
    TEST_CHECK(duplicates.size() == 1);
    for (const auto& flags : {std::pair{false, false}, {true, false}, {false, true}}) {
      auto* props = unitProperties(flags.first, flags.second);
      TEST_CHECK(safeMountUnit(props, props) == (!flags.first && !flags.second));
      g_variant_unref(props);
    }

    auto* testBus = g_test_dbus_new(G_TEST_DBUS_NONE);
    TEST_CHECK(testBus != nullptr);
    const auto busCleanup = nythera::test::makeScopeExit([&] {
      g_test_dbus_down(testBus);
      g_object_unref(testBus);
    });
    g_test_dbus_up(testBus);
    const auto* address = g_test_dbus_get_bus_address(testBus);
    TEST_CHECK(address != nullptr && *address != '\0');
    nythera::test::ScopedEnvVar systemBusEnv("DBUS_SYSTEM_BUS_ADDRESS", address);
    {
      Service service(address);
      TEST_CHECK(service.setupOk);
      bool discovered = false;
      std::size_t discoveredCount = 0;
      discoverManagedMounts([&](auto mounts) {
        discoveredCount = mounts.size();
        discovered = true;
      });
      while (!discovered)
        g_main_context_iteration(nullptr, true);
      TEST_CHECK(discoveredCount == 2);
      const auto run = [&](const ManagedMount& mount, const std::vector<MountRecord>& records,
                           GCancellable* cancel = nullptr) {
        bool done = false;
        std::string result;
        operateManagedMount(
            mount, cancel,
            [&](std::string message) {
              result = std::move(message);
              done = true;
            },
            [records] { return records; }
        );
        TEST_CHECK(!done); // Always returns to the UI before service work completes.
        while (!done)
          g_main_context_iteration(nullptr, true);
        return result;
      };
      TEST_CHECK(run(udisks, {disk}).empty());
      TEST_CHECK(run(systemd, {nfs}).empty());
      service.failJob = true;
      TEST_CHECK(run(systemd, {nfs}).find("Unmount failed") != std::string::npos);
      service.failJob = false;
      service.deny = true;
      TEST_CHECK(run(udisks, {disk}).find("denied") != std::string::npos);
      service.deny = false;
      service.dismissed = true;
      TEST_CHECK(run(udisks, {disk}).find("cancelled") != std::string::npos);
      service.dismissed = false;
      service.busy = true;
      TEST_CHECK(run(udisks, {disk}).find("busy") != std::string::npos);
      service.busy = false;
      const int before = service.operations;
      TEST_CHECK(!run(udisks, {}).empty());           // Disappeared mount never reaches Unmount.
      TEST_CHECK(!run(udisks, {disk, disk}).empty()); // Stacked mount is ambiguous.
      auto* cancel = g_cancellable_new();
      g_cancellable_cancel(cancel);
      TEST_CHECK(run(udisks, {disk}, cancel).find("cancelled") != std::string::npos);
      g_object_unref(cancel);
      TEST_CHECK(service.operations == before);
      service.disconnectJob = true;
      TEST_CHECK(run(systemd, {nfs}).find("disconnected") != std::string::npos);
      TEST_CHECK(service.releaseOk.load(std::memory_order_acquire));
    }
    bool unavailable = false;
    bool discoveredEmpty = false;
    discoverManagedMounts([&](auto mounts) {
      discoveredEmpty = mounts.empty();
      unavailable = true;
    });
    while (!unavailable)
      g_main_context_iteration(nullptr, true);
    TEST_CHECK(discoveredEmpty);
    unavailable = false;
    std::string unavailableMessage;
    operateManagedMount(
        udisks, nullptr,
        [&](std::string message) {
          unavailableMessage = std::move(message);
          unavailable = true;
        },
        [disk] { return std::vector{disk}; }
    );
    while (!unavailable)
      g_main_context_iteration(nullptr, true);
    TEST_CHECK(unavailableMessage.find("unavailable") != std::string::npos);
    auto* error = g_dbus_error_new_for_dbus_error("org.freedesktop.DBus.Error.ServiceUnknown", "private diagnostic");
    TEST_CHECK(mountBackendError(error) == "The mount service is unavailable.");
    g_error_free(error);
  });
}
