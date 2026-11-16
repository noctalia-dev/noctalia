#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <string>
#include <vector>

namespace filemanager {
  struct SystemIdentity {
    std::string name;
    std::uint32_t id;
    bool eligible = false;
  };
  struct PropertyAccounts {
    std::vector<SystemIdentity> users;
    std::vector<SystemIdentity> groups;
  };
  PropertyAccounts propertyAccounts();
  bool savePropertyDefaultApplication(GAppInfo* app, const std::string& mime, GError** error);
  bool resolvePropertyIdentity(const std::string& name, bool group, std::uint32_t& id);
  bool propertyGroupEligible(std::uint32_t id);

  struct PropertyRequest {
    std::filesystem::path path;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint32_t original = 0;
    std::uint32_t edited = 0;
    std::string owner;
    std::string group;
    bool recursive = false;
  };
  struct PropertyResult {
    std::uint64_t succeeded = 0;
    std::uint64_t failed = 0;
    std::uint64_t skipped = 0;
    bool cancelled = false;
    bool ownershipEffects = false;
    bool authorizationRequired = false;
    std::uint64_t inspected = 0;
    std::string error;
    [[nodiscard]] bool ok() const { return !cancelled && failed == 0 && error.empty(); }
    [[nodiscard]] std::string summary() const;
  };
  using PropertyProgress = std::function<void(const PropertyResult&)>;
  using PropertyCancelled = std::function<bool()>;
  int openPropertyDescendant(int root, const char* relative);
  struct PropertyLimits {
    std::size_t depth = 128;
    std::uint64_t entries = 1000000;
    bool inspectOnly = false;
  };
  // Both the unprivileged client and installed helper use this validator and engine.
  std::string validatePropertyRequest(const PropertyRequest& request);
  std::vector<std::string> propertyArguments(const PropertyRequest& request);
  bool parsePropertyArguments(const std::vector<std::string>& args, PropertyRequest& request);
  PropertyResult executePropertyRequest(
      const PropertyRequest& request, PropertyCancelled cancelled = {}, PropertyProgress progress = {},
      PropertyLimits limits = {}
  );
  bool propertyNeedsAuthorization(const PropertyRequest& request, std::uint32_t uid);
  using PropertyAuthorizer = std::function<PropertyResult(const PropertyRequest&, GCancellable*, PropertyProgress)>;
  PropertyResult propertyAuthorizationFailure(int exitCode, bool cancelled);
  PropertyResult runPropertyRequest(
      const PropertyRequest& request, std::uint32_t uid, GCancellable* cancellable, PropertyProgress progress = {},
      PropertyAuthorizer authorizer = {}
  );
} // namespace filemanager
