#pragma once

#include "wayland/wayland_connection.h"
#include "wayland/wayland_extension.h"

#include <unordered_set>
#include <vector>

struct zwlr_output_head_v1;
struct zwlr_output_manager_v1;
struct zwlr_output_mode_v1;

// Read-only wlr-output-management client. Publishes each head's make, model, serial, scale and transform as output
// metadata on the connection, applied to the wl_output with the same connector name.
class OutputManagementGlobals final : public WaylandExtension {
public:
  explicit OutputManagementGlobals(WaylandConnection& connection);

  void onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) override;
  void releaseGlobals() override;

  bool available() const noexcept { return m_manager != nullptr; }

  // Listener entrypoints
  void onHead(zwlr_output_head_v1* head);
  void onDone();
  void onFinished();
  void onHeadName(zwlr_output_head_v1* head, const char* name);
  void onHeadMake(zwlr_output_head_v1* head, const char* make);
  void onHeadModel(zwlr_output_head_v1* head, const char* model);
  void onHeadSerialNumber(zwlr_output_head_v1* head, const char* serialNumber);
  void onHeadMode(zwlr_output_mode_v1* mode);
  void onHeadScale(zwlr_output_head_v1* head, double scaleFactor);
  void onHeadTransform(zwlr_output_head_v1* head, std::int32_t transform);
  void onHeadFinished(zwlr_output_head_v1* head);
  void onModeFinished(zwlr_output_mode_v1* mode);

private:
  WaylandOutputMetadata* metadataFor(zwlr_output_head_v1* head);
  void releaseHeadsAndModes();

  WaylandConnection& m_connection;
  zwlr_output_manager_v1* m_manager = nullptr;
  // Parallel vectors: m_metadata is handed to the connection as one span.
  std::vector<zwlr_output_head_v1*> m_heads;
  std::vector<WaylandOutputMetadata> m_metadata;
  std::unordered_set<zwlr_output_mode_v1*> m_modes;
};
