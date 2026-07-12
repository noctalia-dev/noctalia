#include "shell/control_center/tabs/bluetooth_tab.h"

#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/collapsible.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

using namespace control_center;

namespace {

  constexpr float kRowMinHeight = Style::controlHeightLg;

  const char* glyphFor(BluetoothDeviceKind kind) {
    switch (kind) {
    case BluetoothDeviceKind::Headset:
      return "bluetooth-device-headset";
    case BluetoothDeviceKind::Headphones:
      return "bluetooth-device-headphones";
    case BluetoothDeviceKind::Earbuds:
      return "bluetooth-device-earbuds";
    case BluetoothDeviceKind::Speaker:
      return "bluetooth-device-speaker";
    case BluetoothDeviceKind::Microphone:
      return "bluetooth-device-microphone";
    case BluetoothDeviceKind::Mouse:
      return "bluetooth-device-mouse";
    case BluetoothDeviceKind::Keyboard:
      return "bluetooth-device-keyboard";
    case BluetoothDeviceKind::Phone:
      return "bluetooth-device-phone";
    case BluetoothDeviceKind::Computer:
      return "device-laptop";
    case BluetoothDeviceKind::Gamepad:
      return "bluetooth-device-gamepad";
    case BluetoothDeviceKind::Watch:
      return "bluetooth-device-watch";
    case BluetoothDeviceKind::Tv:
      return "bluetooth-device-tv";
    case BluetoothDeviceKind::Unknown:
    default:
      return "bluetooth-device-generic";
    }
  }

  enum class DeviceBucket : std::uint8_t {
    Connected,
    Paired,
    Available,
  };

  DeviceBucket bucketFor(const BluetoothDeviceInfo& d) {
    if (d.connected) {
      return DeviceBucket::Connected;
    }
    if (d.paired) {
      return DeviceBucket::Paired;
    }
    return DeviceBucket::Available;
  }

  int signalPercentFromRssi(std::int16_t rssi) {
    constexpr int kWeakRssi = -100;
    constexpr int kStrongRssi = -40;
    constexpr int kRange = kStrongRssi - kWeakRssi;
    const int clamped = std::clamp(static_cast<int>(rssi), kWeakRssi, kStrongRssi);
    return ((clamped - kWeakRssi) * 100 + kRange / 2) / kRange;
  }

  // Coarse signal band (0 weakest .. 4 strongest). Available devices order by band,
  // not by raw RSSI: every advertisement moves the RSSI a little, and ordering on it
  // reshuffles rows under the pointer mid-scan.
  int signalBandFromRssi(std::int16_t rssi) { return std::min(signalPercentFromRssi(rssi) / 20, 4); }

  std::string percentText(int percent) { return std::to_string(percent) + "%"; }

  // Bucket first, then strongest band, then a stable alias/path tiebreak so equally
  // named devices keep their order across rebuilds.
  std::vector<BluetoothDeviceInfo> sortedDevices(std::vector<BluetoothDeviceInfo> devices) {
    std::ranges::sort(devices, [](const BluetoothDeviceInfo& a, const BluetoothDeviceInfo& b) {
      const auto ba = bucketFor(a);
      const auto bb = bucketFor(b);
      if (ba != bb) {
        return static_cast<int>(ba) < static_cast<int>(bb);
      }
      if (ba == DeviceBucket::Available) {
        if (a.hasRssi != b.hasRssi) {
          return a.hasRssi;
        }
        if (a.hasRssi) {
          const int bandA = signalBandFromRssi(a.rssi);
          const int bandB = signalBandFromRssi(b.rssi);
          if (bandA != bandB) {
            return bandA > bandB;
          }
        }
      }
      if (a.alias != b.alias) {
        return a.alias < b.alias;
      }
      return a.path < b.path;
    });
    return devices;
  }

  std::unique_ptr<Flex> makeMetricPill(const char* glyphName, std::string text, float scale, Label** valueOut) {
    return ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceXs * 0.5f * scale},
        ui::glyph({
            .glyph = glyphName,
            .glyphSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        }),
        ui::label({
            .out = valueOut,
            .text = std::move(text),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  }

} // namespace

class BluetoothDeviceRow : public Collapsible {
public:
  BluetoothDeviceRow(BluetoothDeviceInfo device, BluetoothService* service, float scale)
      : m_device(std::move(device)), m_service(service) {
    setScale(scale);
    setRadius(Style::scaledRadiusMd(scale));
    setFill(colorSpecFromRole(ColorRole::Surface));
    clearBorder();

    auto header = ui::row(
        {.align = FlexAlign::Center,
         .gap = Style::spaceSm * scale,
         .padding = Style::spaceSm * scale,
         .minHeight = kRowMinHeight * scale},
        ui::glyph({
            .glyph = glyphFor(m_device.kind),
            .glyphSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        }),
        ui::label({
            .text = m_device.alias,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = m_device.connected ? FontWeight::Bold : FontWeight::Normal,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0f,
        })
    );
    header->setPadding(Style::spaceSm * scale, Style::spaceMd * scale);

    auto metrics = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
    });

    if (m_device.hasBattery) {
      metrics->addChild(
          makeMetricPill("battery", percentText(static_cast<int>(m_device.batteryPercent)), scale, &m_batteryValue)
      );
    }
    if (m_device.hasRssi && bucketFor(m_device) == DeviceBucket::Available) {
      metrics->addChild(
          makeMetricPill("antenna-bars-5", percentText(signalPercentFromRssi(m_device.rssi)), scale, &m_signalValue)
      );
    }
    if (!metrics->children().empty()) {
      header->addChild(std::move(metrics));
    }

    const auto bucket = bucketFor(m_device);

    if (m_device.connecting) {
      header->addChild(
          ui::spinner({
              .out = &m_connectingSpinner,
              .color = colorSpecFromRole(ColorRole::Primary),
              .spinnerSize = Style::baseGlyphSize * scale,
          })
      );
    } else {
      ButtonVariant primaryVariant = ButtonVariant::Default;
      std::string primaryGlyph;
      switch (bucket) {
      case DeviceBucket::Connected:
        primaryVariant = ButtonVariant::Destructive;
        primaryGlyph = "plug-off";
        break;
      case DeviceBucket::Paired:
        primaryGlyph = "plug";
        break;
      case DeviceBucket::Available:
        primaryGlyph = "bluetooth";
        break;
      }
      auto primary = ui::button({
          .glyph = std::move(primaryGlyph),
          .glyphSize = Style::fontSizeBody * scale,
          .variant = primaryVariant,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusSm(scale),
          .onClick = [this]() {
            if (m_service == nullptr) {
              return;
            }
            switch (bucketFor(m_device)) {
            case DeviceBucket::Connected:
              m_service->disconnectDevice(m_device.path);
              break;
            case DeviceBucket::Paired:
              m_service->connect(m_device.path);
              break;
            case DeviceBucket::Available:
              m_service->pair(m_device.path);
              break;
            }
            PanelManager::instance().refresh();
          },
      });
      header->addChild(std::move(primary));
    }

    if (m_device.paired) {
      header->addChild(
          ui::button({
              .glyph = "trash",
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this]() {
                if (m_service != nullptr) {
                  m_service->forget(m_device.path);
                }
                PanelManager::instance().refresh();
              },
          })
      );
    }

    setHeader(std::move(header));

    if (m_device.paired) {
      setBody(
          ui::row(
              {.align = FlexAlign::Center,
               .gap = Style::spaceSm * scale,
               .configure =
                   [scale](Flex& body) {
                     body.setPadding(
                         Style::spaceXs * scale, Style::spaceMd * scale, Style::spaceSm * scale, Style::spaceMd * scale
                     );
                   }},
              ui::label({
                  .text = i18n::tr("control-center.bluetooth.auto-reconnect"),
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .flexGrow = 1.0f,
              }),
              ui::toggle({
                  .checkedImmediate = m_device.trusted,
                  .toggleSize = ToggleSize::Medium,
                  .scale = scale,
                  .onChange = [this](bool checked) {
                    if (m_service != nullptr) {
                      m_service->setTrusted(m_device.path, checked);
                    }
                  },
              })
          )
      );
    }
  }

  void startConnectingSpinner() {
    if (m_connectingSpinner != nullptr) {
      m_connectingSpinner->start();
    }
  }

  [[nodiscard]] const std::string& devicePath() const noexcept { return m_device.path; }

  // Refresh the values that move while a scan runs. Which pills exist is fixed at
  // construction (it belongs to the list structure key); only their text is live.
  // Returns true when a value actually changed, i.e. the row needs relayout.
  bool syncLiveMetrics(const BluetoothDeviceInfo& device) {
    bool changed = false;
    if (m_batteryValue != nullptr && m_batteryValue->setText(percentText(static_cast<int>(device.batteryPercent)))) {
      changed = true;
    }
    if (m_signalValue != nullptr && m_signalValue->setText(percentText(signalPercentFromRssi(device.rssi)))) {
      changed = true;
    }
    m_device.batteryPercent = device.batteryPercent;
    m_device.rssi = device.rssi;
    return changed;
  }

private:
  BluetoothDeviceInfo m_device;
  BluetoothService* m_service = nullptr;
  Spinner* m_connectingSpinner = nullptr;
  Label* m_batteryValue = nullptr;
  Label* m_signalValue = nullptr;
};

BluetoothTab::BluetoothTab(BluetoothService* service, BluetoothAgent* agent) : m_service(service), m_agent(agent) {}

BluetoothTab::~BluetoothTab() = default;

std::unique_ptr<Flex> BluetoothTab::create() {
  const float scale = contentScale();

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto pairingCard = ui::column({
      .out = &m_pairingCard,
      .visible = false,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applySectionCardStyle(card, scale, opacity, borders);
      },
  });

  pairingCard->addChild(
      ui::label({
          .out = &m_pairingTitle,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );

  pairingCard->addChild(
      ui::label({
          .out = &m_pairingDetail,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );

  pairingCard->addChild(
      ui::label({
          .out = &m_pairingCode,
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Primary),
      })
  );

  auto pairingInputRow = ui::row(
      {.out = &m_pairingInputRow, .align = FlexAlign::Center, .gap = Style::spaceSm * scale, .visible = false},
      ui::input({
          .out = &m_pairingInput,
          .placeholder = i18n::tr("control-center.bluetooth.enter-code"),
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0f,
          .onSubmit = [this](const std::string& value) {
            if (m_agent == nullptr) {
              return;
            }
            const auto req = m_agent->pendingRequest();
            if (req.kind == BluetoothPairingKind::PinCode) {
              m_agent->submitPin(value);
            } else if (req.kind == BluetoothPairingKind::Passkey) {
              try {
                m_agent->submitPasskey(static_cast<std::uint32_t>(std::stoul(value)));
              } catch (...) {
                m_agent->cancelPending();
              }
            }
            PanelManager::instance().refresh();
          },
      })
  );
  pairingCard->addChild(std::move(pairingInputRow));

  auto pairingButtonRow = ui::row(
      {.out = &m_pairingButtonRow, .align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .out = &m_pairingAccept,
          .text = i18n::tr("control-center.bluetooth.accept"),
          .variant = ButtonVariant::Default,
          .onClick =
              [this]() {
                if (m_agent == nullptr) {
                  return;
                }
                const auto req = m_agent->pendingRequest();
                switch (req.kind) {
                case BluetoothPairingKind::Confirm:
                case BluetoothPairingKind::Authorize:
                case BluetoothPairingKind::AuthorizeService:
                case BluetoothPairingKind::DisplayPinCode:
                  m_agent->acceptConfirm();
                  break;
                case BluetoothPairingKind::PinCode:
                  if (m_pairingInput != nullptr) {
                    m_agent->submitPin(m_pairingInput->value());
                  }
                  break;
                case BluetoothPairingKind::Passkey:
                  if (m_pairingInput != nullptr) {
                    try {
                      m_agent->submitPasskey(static_cast<std::uint32_t>(std::stoul(m_pairingInput->value())));
                    } catch (...) {
                      m_agent->cancelPending();
                    }
                  }
                  break;
                default:
                  m_agent->cancelPending();
                  break;
                }
                PanelManager::instance().refresh();
              },
      }),
      ui::button({
          .out = &m_pairingReject,
          .text = i18n::tr("control-center.bluetooth.reject"),
          .variant = ButtonVariant::Ghost,
          .onClick = [this]() {
            if (m_agent != nullptr) {
              m_agent->rejectConfirm();
            }
            PanelManager::instance().refresh();
          },
      })
  );
  pairingCard->addChild(std::move(pairingButtonRow));

  tab->addChild(std::move(pairingCard));

  auto listScroll = ui::scrollView({
      .out = &m_listScroll,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0f,
      .viewportPaddingV = 0.0f,
      .flexGrow = 1.0f,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });
  m_list = listScroll->content();
  m_list->setDirection(FlexDirection::Vertical);
  m_list->setAlign(FlexAlign::Stretch);
  m_list->setGap(Style::spaceMd * scale);

  tab->addChild(std::move(listScroll));
  return tab;
}

std::unique_ptr<Flex> BluetoothTab::createHeaderActions() { return nullptr; }

void BluetoothTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  syncPairingCard();
  rebuildDeviceList(renderer);
  syncDeviceRows();
  syncHeader();
  m_rootLayout->layout(renderer);
}

void BluetoothTab::doUpdate(Renderer& renderer) {
  syncPairingCard();
  rebuildDeviceList(renderer);
  // A metric pill's text changes its width, so the list has to be laid out again.
  if (syncDeviceRows() && m_list != nullptr) {
    m_list->layout(renderer);
  }
  syncHeader();
}

void BluetoothTab::setActive(bool active) {
  if (!active && m_service != nullptr && m_service->state().discovering) {
    m_service->stopDiscovery();
  }
}

void BluetoothTab::onClose() {
  m_rootLayout = nullptr;
  m_pairingCard = nullptr;
  m_pairingTitle = nullptr;
  m_pairingDetail = nullptr;
  m_pairingCode = nullptr;
  m_pairingInputRow = nullptr;
  m_pairingInput = nullptr;
  m_pairingButtonRow = nullptr;
  m_pairingAccept = nullptr;
  m_pairingReject = nullptr;
  m_listScroll = nullptr;
  m_list = nullptr;
  m_powerToggle = nullptr;
  m_discoverableToggle = nullptr;
  m_rescanButton = nullptr;
  m_scanSpinner = nullptr;
  m_deviceRows.clear();
  m_lastStructureKey.clear();
  m_lastListWidth = -1.0f;
}

void BluetoothTab::syncHeader() {
  if (m_service == nullptr) {
    return;
  }
  const BluetoothState& s = m_service->state();
  if (m_powerToggle != nullptr) {
    m_powerToggle->setChecked(s.powered);
    m_powerToggle->setEnabled(s.adapterPresent);
  }
  if (m_discoverableToggle != nullptr) {
    m_discoverableToggle->setChecked(s.discoverable);
    m_discoverableToggle->setEnabled(s.adapterPresent && s.powered);
  }
  if (m_scanSpinner != nullptr) {
    const bool spinnerVisible = s.discovering && s.powered && s.adapterPresent;
    m_scanSpinner->setVisible(spinnerVisible);
    if (spinnerVisible && !m_scanSpinner->spinning()) {
      m_scanSpinner->start();
    } else if (!spinnerVisible && m_scanSpinner->spinning()) {
      m_scanSpinner->stop();
    }
  }
  if (m_rescanButton != nullptr) {
    m_rescanButton->setEnabled(s.adapterPresent && s.powered);
  }
}

void BluetoothTab::syncPairingCard() {
  if (m_pairingCard == nullptr) {
    return;
  }
  const bool hasPending = m_agent != nullptr && m_agent->hasPendingRequest();
  m_pairingCard->setVisible(hasPending);
  if (!hasPending) {
    return;
  }
  const auto req = m_agent->pendingRequest();
  std::string alias = req.devicePath;
  if (m_service != nullptr) {
    for (const auto& d : m_service->devices()) {
      if (d.path == req.devicePath && !d.alias.empty()) {
        alias = d.alias;
        break;
      }
    }
  }

  if (m_pairingTitle != nullptr) {
    m_pairingTitle->setText(i18n::tr("control-center.bluetooth.pair-title", "device", alias));
  }
  const bool needsInput = req.kind == BluetoothPairingKind::PinCode || req.kind == BluetoothPairingKind::Passkey;
  const bool showsCode = req.kind == BluetoothPairingKind::Confirm
      || req.kind == BluetoothPairingKind::DisplayPasskey
      || req.kind == BluetoothPairingKind::DisplayPinCode;

  if (m_pairingDetail != nullptr) {
    switch (req.kind) {
    case BluetoothPairingKind::Confirm:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.confirm"));
      break;
    case BluetoothPairingKind::Authorize:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.authorize"));
      break;
    case BluetoothPairingKind::AuthorizeService:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.authorize-service", "uuid", req.uuid));
      break;
    case BluetoothPairingKind::DisplayPinCode:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.display-pin"));
      break;
    case BluetoothPairingKind::DisplayPasskey:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.display-passkey"));
      break;
    case BluetoothPairingKind::PinCode:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.pin-code"));
      break;
    case BluetoothPairingKind::Passkey:
      m_pairingDetail->setText(i18n::tr("control-center.bluetooth.pairing-detail.passkey"));
      break;
    case BluetoothPairingKind::None:
      break;
    }
  }
  if (m_pairingCode != nullptr) {
    m_pairingCode->setVisible(showsCode);
    if (showsCode) {
      if (req.kind == BluetoothPairingKind::DisplayPinCode) {
        m_pairingCode->setText(req.pin);
      } else {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%06u", req.passkey);
        m_pairingCode->setText(buf);
      }
    }
  }
  if (m_pairingInputRow != nullptr) {
    m_pairingInputRow->setVisible(needsInput);
  }
}

// Identity of the built list: which rows exist, in which order, and which controls
// each carries. Live metric values (battery percent, RSSI) are absent by design —
// they refresh in place through syncDeviceRows(), so an advertisement no longer tears
// down the list. Devices arrive sorted, so a change in row order changes the key.
std::string BluetoothTab::structureKey(const std::vector<BluetoothDeviceInfo>& devices) const {
  if (m_service == nullptr) {
    return "empty";
  }
  const auto& s = m_service->state();
  std::string key;
  key += s.adapterPresent ? '1' : '0';
  key += s.powered ? '1' : '0';
  key += s.rfkillSoftBlocked ? '1' : '0';
  key += s.discovering ? '1' : '0';
  key.push_back('|');
  for (const auto& d : devices) {
    key += d.path;
    key.push_back(':');
    key += d.alias;
    key.push_back(':');
    key += std::to_string(static_cast<int>(d.kind));
    key.push_back(':');
    key += d.paired ? '1' : '0';
    key += d.trusted ? '1' : '0';
    key += d.connected ? '1' : '0';
    key += d.connecting ? '1' : '0';
    key += d.hasBattery ? '1' : '0';
    key += d.hasRssi ? '1' : '0';
    key.push_back('\n');
  }
  return key;
}

bool BluetoothTab::syncDeviceRows() {
  if (m_service == nullptr || m_deviceRows.empty()) {
    return false;
  }
  bool changed = false;
  for (const auto& d : m_service->devices()) {
    const auto it = m_deviceRows.find(d.path);
    if (it != m_deviceRows.end() && it->second->syncLiveMetrics(d)) {
      changed = true;
    }
  }
  return changed;
}

void BluetoothTab::rebuildDeviceList(Renderer& renderer) {
  uiAssertNotRendering("BluetoothTab::rebuildDeviceList");
  if (m_list == nullptr || m_listScroll == nullptr) {
    return;
  }
  const float listWidth = m_listScroll->contentViewportWidth();
  if (listWidth <= 0.0f) {
    return;
  }
  std::vector<BluetoothDeviceInfo> devices;
  if (m_service != nullptr) {
    devices = sortedDevices(m_service->devices());
  }
  const std::string nextKey = structureKey(devices);
  if (listWidth == m_lastListWidth && nextKey == m_lastStructureKey) {
    return;
  }
  m_lastListWidth = listWidth;
  m_lastStructureKey = nextKey;
  const float scale = contentScale();

  m_powerToggle = nullptr;
  m_discoverableToggle = nullptr;
  m_scanSpinner = nullptr;
  m_rescanButton = nullptr;
  m_deviceRows.clear();

  while (!m_list->children().empty()) {
    m_list->removeChild(m_list->children().front().get());
  }

  if (m_service == nullptr) {
    m_list->addChild(
        ui::label({
            .text = i18n::tr("control-center.bluetooth.unavailable"),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    m_list->layout(renderer);
    return;
  }

  const auto& s = m_service->state();

  // Adapter card: power + discoverable toggles
  {
    auto adapterCard = ui::column({
        .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
          applySectionCardStyle(card, scale, opacity, borders);
        },
    });

    auto powerRow = ui::row(
        {.align = FlexAlign::Center,
         .gap = Style::spaceSm * scale,
         .minHeight = Style::controlHeightSm * scale,
         .maxHeight = Style::controlHeightSm * scale},
        ui::label({
            .text = i18n::tr("control-center.bluetooth.bluetooth"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0f,
        })
    );

    powerRow->addChild(
        ui::spinner({
            .out = &m_scanSpinner,
            .color = colorSpecFromRole(ColorRole::Primary),
            .spinnerSize = Style::baseGlyphSize * scale,
            .visible = false,
        })
    );

    powerRow->addChild(
        ui::button({
            .out = &m_rescanButton,
            .glyph = "refresh",
            .glyphSize = Style::baseGlyphSize * scale,
            .enabled = s.adapterPresent && s.powered,
            .variant = ButtonVariant::Ghost,
            .minHeight = Style::fontSizeBody * scale,
            .padding = Style::spaceXs * scale,
            .radius = Style::scaledRadiusSm(scale),
            .onClick = [this]() {
              if (m_service == nullptr) {
                return;
              }
              m_service->stopDiscovery();
              m_service->startDiscovery();
            },
        })
    );

    powerRow->addChild(
        ui::toggle({
            .out = &m_powerToggle,
            .checkedImmediate = s.powered,
            .enabled = s.adapterPresent,
            .toggleSize = ToggleSize::Medium,
            .scale = scale,
            .onChange = [this](bool checked) {
              if (m_service != nullptr) {
                m_service->setPowered(checked);
              }
            },
        })
    );

    adapterCard->addChild(std::move(powerRow));

    auto visibleRow = ui::row(
        {.align = FlexAlign::Center,
         .gap = Style::spaceSm * scale,
         .minHeight = Style::controlHeightSm * scale,
         .maxHeight = Style::controlHeightSm * scale},
        ui::label({
            .text = i18n::tr("control-center.bluetooth.visible"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0f,
        }),
        ui::toggle({
            .out = &m_discoverableToggle,
            .checkedImmediate = s.discoverable,
            .enabled = s.adapterPresent && s.powered,
            .toggleSize = ToggleSize::Medium,
            .scale = scale,
            .onChange = [this](bool checked) {
              if (m_service != nullptr) {
                m_service->setDiscoverable(checked);
              }
            },
        })
    );

    adapterCard->addChild(std::move(visibleRow));

    m_list->addChild(std::move(adapterCard));
  }

  if (!s.powered) {
    m_list->addChild(
        ui::label({
            .text = s.rfkillSoftBlocked ? i18n::tr("control-center.bluetooth.rfkill-blocked")
                                        : i18n::tr("control-center.bluetooth.off"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    m_list->layout(renderer);
    return;
  }

  if (devices.empty()) {
    m_list->addChild(
        ui::label({
            .text = i18n::tr("control-center.bluetooth.no-devices"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    m_list->layout(renderer);
    return;
  }

  Flex* bucketCard = nullptr;
  DeviceBucket currentBucket = DeviceBucket::Connected;
  bool first = true;
  for (const auto& device : devices) {
    const auto bucket = bucketFor(device);
    if (first || bucket != currentBucket) {
      std::string sectionText;
      switch (bucket) {
      case DeviceBucket::Connected:
        sectionText = i18n::tr("control-center.bluetooth.sections.connected");
        break;
      case DeviceBucket::Paired:
        sectionText = i18n::tr("control-center.bluetooth.sections.paired");
        break;
      case DeviceBucket::Available:
        sectionText = i18n::tr("control-center.bluetooth.sections.available");
        break;
      }
      auto card = ui::column({
          .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& c) {
            applySectionCardStyle(c, scale, opacity, borders);
          },
      });
      card->addChild(makeCardHeaderRow(sectionText, scale));
      bucketCard = card.get();
      m_list->addChild(std::move(card));
      currentBucket = bucket;
      first = false;
    }
    auto row = std::make_unique<BluetoothDeviceRow>(device, m_service, scale);
    auto* rowPtr = row.get();
    bucketCard->addChild(std::move(row));
    rowPtr->startConnectingSpinner();
    m_deviceRows.emplace(rowPtr->devicePath(), rowPtr);
  }
  m_list->layout(renderer);
}

void BluetoothTab::onPanelCardOpacityChanged(float opacity) {
  if (m_pairingInput != nullptr) {
    m_pairingInput->setSurfaceOpacity(opacity);
  }
}
