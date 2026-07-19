#include "shell/control_center/tabs/network_tab.h"

#include "core/ui_phase.h"
#include "dbus/network/external_ip_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_glyphs.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

using namespace control_center;

namespace {

  constexpr float kRowMinHeight = Style::controlHeightLg;

  std::string currentTitle(const NetworkState& s) {
    if (s.kind == NetworkConnectivity::Wireless && s.connected && !s.ssid.empty()) {
      return s.ssid;
    }
    if (s.kind == NetworkConnectivity::Wired && s.connected) {
      return s.interfaceName.empty() ? i18n::tr("control-center.network.wired-connection") : s.interfaceName;
    }
    return i18n::tr("control-center.network.not-connected");
  }

  std::string currentDetail(const NetworkState& s, const std::string& externalIp) {
    if (!s.connected) {
      return s.wirelessEnabled ? i18n::tr("control-center.network.wifi-on")
                               : i18n::tr("control-center.network.wifi-off");
    }
    std::string out;
    if (!s.ipv4.empty()) {
      out = s.ipv4;
    }
    if (s.kind == NetworkConnectivity::Wireless && s.signalStrength > 0) {
      if (!out.empty()) {
        out += "  •  ";
      }
      out += std::to_string(static_cast<int>(s.signalStrength)) + "%";
    }
    if (!externalIp.empty()) {
      if (!out.empty()) {
        out += "  •  ";
      }
      out += i18n::tr("control-center.network.external-ip", "ip", externalIp);
    }
    return out;
  }

  std::string percentText(std::uint8_t percent) { return std::to_string(static_cast<int>(percent)) + "%"; }

  // Active first, then by signal band, then by name. Ordering on the raw strength —
  // as the services do — reshuffles rows on every scan update, moving the row you are
  // aiming at out from under the pointer.
  std::vector<AccessPointInfo> sortedAccessPoints(std::vector<AccessPointInfo> aps) {
    std::ranges::sort(aps, [](const AccessPointInfo& a, const AccessPointInfo& b) {
      if (a.active != b.active) {
        return a.active;
      }
      const int bandA = network_glyphs::wifiSignalBand(a.strength);
      const int bandB = network_glyphs::wifiSignalBand(b.strength);
      if (bandA != bandB) {
        return bandA > bandB;
      }
      return a.ssid < b.ssid;
    });
    return aps;
  }

} // namespace

class AccessPointRow : public Flex {
public:
  AccessPointRow(
      float scale, AccessPointInfo ap, bool saved, std::function<void(const AccessPointInfo&)> onActivate,
      std::function<void(const AccessPointInfo&)> onForget
  )
      : m_ap(std::move(ap)), m_onActivate(std::move(onActivate)), m_onForget(std::move(onForget)) {
    setDirection(FlexDirection::Horizontal);
    setAlign(FlexAlign::Center);
    setGap(Style::spaceSm * scale);
    setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
    setMinHeight(kRowMinHeight * scale);
    setRadius(Style::scaledRadiusMd(scale));
    setFill(colorSpecFromRole(ColorRole::Surface));
    clearBorder();

    addChild(
        ui::glyph({
            .out = &m_signalGlyph,
            .glyph = network_glyphs::wifiGlyphForSignal(m_ap.strength),
            .glyphSize = Style::baseGlyphSize * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );

    addChild(
        ui::label({
            .out = &m_title,
            .text = m_ap.ssid,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = m_ap.active ? FontWeight::Bold : FontWeight::Normal,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0f,
        })
    );

    if (m_ap.secured) {
      addChild(
          ui::glyph({
              .glyph = "lock",
              .glyphSize = Style::baseGlyphSize * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    }

    addChild(
        ui::label({
            .out = &m_signalValue,
            .text = percentText(m_ap.strength),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );

    const float actionOpacity = (m_ap.active || saved) ? 1.0f : 0.0f;
    auto action = ui::button({
        .out = &m_actionButton,
        .glyphSize = Style::baseGlyphSize * scale,
        .variant = ButtonVariant::Ghost,
        .padding = Style::spaceXs * scale,
        .radius = Style::scaledRadiusSm(scale),
        .opacity = actionOpacity,
    });
    if (m_ap.active) {
      action->setGlyph("check");
    } else if (saved) {
      action->setGlyph("trash");
      action->setOnClick([this]() {
        if (m_onForget) {
          m_onForget(m_ap);
        }
      });
    }
    addChild(std::move(action));

    auto area = std::make_unique<InputArea>();
    area->setPropagateEvents(true);
    area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
    area->setOnLeave([this]() { applyState(); });
    area->setOnPress([this](const InputArea::PointerData& /*data*/) { applyState(); });
    area->setOnClick([this](const InputArea::PointerData& /*data*/) {
      if (m_onActivate) {
        m_onActivate(m_ap);
      }
    });
    m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));

    applyState();
    m_paletteConn = paletteChanged().connect([this] { applyState(); });
  }

  void doLayout(Renderer& renderer) override {
    if (m_inputArea == nullptr) {
      return;
    }
    m_inputArea->setVisible(false);
    Flex::doLayout(renderer);
    m_inputArea->setVisible(true);
    m_inputArea->setPosition(0.0f, 0.0f);
    m_inputArea->setSize(width(), height());
    if (m_actionButton != nullptr) {
      const float areaWidth = std::max(0.0f, m_actionButton->x() - gap());
      m_inputArea->setSize(areaWidth, height());
    }
    applyState();
  }

  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
    return measureByLayout(renderer, constraints);
  }

  void doArrange(Renderer& renderer, const LayoutRect& rect) override { arrangeByLayout(renderer, rect); }

  [[nodiscard]] const std::string& ssid() const noexcept { return m_ap.ssid; }

  // Refresh the values that move while a scan runs — the signal glyph and percent.
  // Everything else about the row is structural and belongs to the list key.
  // Returns true when a value actually changed, i.e. the row needs relayout.
  bool syncLiveMetrics(const AccessPointInfo& ap) {
    bool changed = false;
    if (m_signalGlyph != nullptr && m_signalGlyph->setGlyph(network_glyphs::wifiGlyphForSignal(ap.strength))) {
      changed = true;
    }
    if (m_signalValue != nullptr && m_signalValue->setText(percentText(ap.strength))) {
      changed = true;
    }
    m_ap.strength = ap.strength;
    return changed;
  }

private:
  void applyState() {
    const bool hov = m_inputArea != nullptr && m_inputArea->hovered();
    const bool pressed = m_inputArea != nullptr && m_inputArea->pressed();
    if (pressed) {
      setFill(colorSpecFromRole(ColorRole::Primary));
      setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
      if (m_title != nullptr) {
        m_title->setColor(colorSpecFromRole(ColorRole::OnPrimary));
      }
    } else {
      setFill(colorSpecFromRole(ColorRole::Surface));
      if (hov) {
        setBorder(colorSpecFromRole(ColorRole::Hover), Style::borderWidth);
      } else {
        clearBorder();
      }
      if (m_title != nullptr) {
        m_title->setColor(colorSpecFromRole(ColorRole::OnSurface));
      }
    }
  }

  AccessPointInfo m_ap;
  std::function<void(const AccessPointInfo&)> m_onActivate;
  std::function<void(const AccessPointInfo&)> m_onForget;
  Label* m_title = nullptr;
  Button* m_actionButton = nullptr;
  InputArea* m_inputArea = nullptr;
  Glyph* m_signalGlyph = nullptr;
  Label* m_signalValue = nullptr;
  Signal<>::ScopedConnection m_paletteConn;
};

namespace {

  class VpnConnectionRow : public Flex {
  public:
    VpnConnectionRow(
        float scale, VpnConnectionInfo vpn, std::function<void(const VpnConnectionInfo&)> onActivate,
        std::function<void(const VpnConnectionInfo&)> onDeactivate
    )
        : m_vpn(std::move(vpn)), m_onActivate(std::move(onActivate)), m_onDeactivate(std::move(onDeactivate)) {
      setDirection(FlexDirection::Horizontal);
      setAlign(FlexAlign::Center);
      setGap(Style::spaceSm * scale);
      setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
      setMinHeight(kRowMinHeight * scale);
      setRadius(Style::scaledRadiusMd(scale));
      setFill(colorSpecFromRole(ColorRole::Surface));
      clearBorder();

      addChild(
          ui::label({
              .out = &m_title,
              .text = m_vpn.name,
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = m_vpn.active ? FontWeight::Bold : FontWeight::Normal,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .flexGrow = 1.0f,
          })
      );

      addChild(
          ui::button({
              .out = &m_checkButton,
              .glyph = "check",
              .glyphSize = Style::baseGlyphSize * scale,
              .variant = ButtonVariant::Ghost,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .opacity = m_vpn.active ? 1.0f : 0.0f,
          })
      );

      addChild(
          ui::button({
              .out = &m_actionButton,
              .glyph = m_vpn.active ? "plug-off" : "plug",
              .glyphSize = Style::baseGlyphSize * scale,
              .variant = m_vpn.active ? ButtonVariant::Destructive : ButtonVariant::Default,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this]() { triggerAction(); },
          })
      );

      auto area = std::make_unique<InputArea>();
      area->setPropagateEvents(true);
      area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
      area->setOnLeave([this]() { applyState(); });
      area->setOnPress([this](const InputArea::PointerData& /*data*/) { applyState(); });
      area->setOnClick([this](const InputArea::PointerData& /*data*/) { triggerAction(); });
      m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));

      applyState();
      m_paletteConn = paletteChanged().connect([this] { applyState(); });
    }

    void doLayout(Renderer& renderer) override {
      if (m_inputArea == nullptr) {
        return;
      }
      m_inputArea->setVisible(false);
      Flex::doLayout(renderer);
      m_inputArea->setVisible(true);
      m_inputArea->setPosition(0.0f, 0.0f);
      m_inputArea->setSize(width(), height());
      if (m_actionButton != nullptr) {
        const float areaWidth = std::max(0.0f, m_actionButton->x() - gap());
        m_inputArea->setSize(areaWidth, height());
      }
      applyState();
    }

    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
      return measureByLayout(renderer, constraints);
    }

    void doArrange(Renderer& renderer, const LayoutRect& rect) override { arrangeByLayout(renderer, rect); }

  private:
    void triggerAction() {
      if (m_vpn.active) {
        if (m_onDeactivate) {
          m_onDeactivate(m_vpn);
        }
      } else {
        if (m_onActivate) {
          m_onActivate(m_vpn);
        }
      }
    }

    void applyState() {
      const bool hov = m_inputArea != nullptr && m_inputArea->hovered();
      const bool pressed = m_inputArea != nullptr && m_inputArea->pressed();
      if (pressed) {
        setFill(colorSpecFromRole(ColorRole::Primary));
        setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
        if (m_title != nullptr) {
          m_title->setColor(colorSpecFromRole(ColorRole::OnPrimary));
        }
        return;
      }
      setFill(colorSpecFromRole(ColorRole::Surface));
      if (hov) {
        setBorder(colorSpecFromRole(ColorRole::Hover), Style::borderWidth);
      } else {
        clearBorder();
      }
      if (m_title != nullptr) {
        m_title->setColor(colorSpecFromRole(ColorRole::OnSurface));
      }
    }

    VpnConnectionInfo m_vpn;
    std::function<void(const VpnConnectionInfo&)> m_onActivate;
    std::function<void(const VpnConnectionInfo&)> m_onDeactivate;
    Label* m_title = nullptr;
    Button* m_checkButton = nullptr;
    Button* m_actionButton = nullptr;
    InputArea* m_inputArea = nullptr;
    Signal<>::ScopedConnection m_paletteConn;
  };

} // namespace

NetworkTab::NetworkTab(INetworkService* network, NetworkSecretAgent* secrets, ExternalIpService* externalIp)
    : m_network(network), m_secrets(secrets), m_externalIpService(externalIp) {
  if (m_secrets != nullptr) {
    m_secrets->setRequestCallback([this](const NetworkSecretAgent::SecretRequest& request) {
      showPasswordPrompt(request);
      PanelManager::instance().refresh();
    });
  }
}

NetworkTab::~NetworkTab() {
  if (m_secrets != nullptr) {
    m_secrets->setRequestCallback(nullptr);
    m_secrets->cancelSecret();
  }
}

std::unique_ptr<Flex> NetworkTab::create() {
  const float scale = contentScale();

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto currentCard = ui::column({
      .out = &m_currentCard,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applySectionCardStyle(card, scale, opacity, borders);
      },
  });
  addTitle(*currentCard, i18n::tr("control-center.network.current-connection"), scale);

  auto connRow = ui::row(
      {.out = &m_currentRow, .align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::label({
          .out = &m_currentTitle,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      }),
      ui::label({
          .out = &m_currentDetail,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .flexGrow = 1.0f,
      }),
      ui::button({
          .out = &m_disconnectButton,
          .glyph = "plug-off",
          .glyphSize = Style::baseGlyphSize * scale,
          .variant = ButtonVariant::Destructive,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusSm(scale),
          .onClick = [this]() {
            if (m_network == nullptr || m_actionPending) {
              return;
            }
            const bool wasConnected = m_network->state().connected;
            if (wasConnected) {
              m_network->disconnect();
            } else if (!m_network->activateWiredConnection()) {
              return;
            }
            beginPendingAction(wasConnected);
            PanelManager::instance().refresh();
          },
      })
  );
  currentCard->addChild(std::move(connRow));

  tab->addChild(std::move(currentCard));

  auto passwordCard = ui::column({
      .out = &m_passwordCard,
      .visible = false,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applySectionCardStyle(card, scale, opacity, borders);
      },
  });

  passwordCard->addChild(
      ui::label({
          .out = &m_passwordTitle,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );

  auto inputRow = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::input({
          .out = &m_passwordInput,
          .placeholder = i18n::tr("control-center.network.password"),
          .passwordMode = true,
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0f,
          .onSubmit = [this](const std::string& value) { submitPasswordPrompt(value); },
      }),
      ui::button({
          .out = &m_passwordRevealButton,
          .glyph = "eye",
          .glyphSize = Style::baseGlyphSize * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick =
              [this]() {
                if (m_passwordInput == nullptr) {
                  return;
                }
                m_passwordRevealed = !m_passwordRevealed;
                m_passwordInput->setPasswordMode(!m_passwordRevealed);
                if (m_passwordRevealButton != nullptr) {
                  m_passwordRevealButton->setGlyph(m_passwordRevealed ? "eye-off" : "eye");
                }
              },
      }),
      ui::button({
          .text = i18n::tr("control-center.network.connect"),
          .variant = ButtonVariant::Default,
          .onClick =
              [this]() { submitPasswordPrompt(m_passwordInput != nullptr ? m_passwordInput->value() : std::string{}); },
      }),
      ui::button({
          .text = i18n::tr("common.actions.cancel"),
          .variant = ButtonVariant::Ghost,
          .onClick = [this]() { cancelPasswordPrompt(); },
      })
  );

  passwordCard->addChild(std::move(inputRow));
  tab->addChild(std::move(passwordCard));

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

std::unique_ptr<Flex> NetworkTab::createHeaderActions() { return nullptr; }

void NetworkTab::setActive(bool active) {
  if (m_active == active) {
    return;
  }
  m_active = active;
  if (m_active && m_network != nullptr) {
    m_network->requestScan();
  }
}

void NetworkTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  syncPasswordCard();
  rebuildApList(renderer);
  syncApRows();
  syncCurrentCard();
  m_rootLayout->layout(renderer);
}

void NetworkTab::doUpdate(Renderer& renderer) {
  syncPasswordCard();
  rebuildApList(renderer);
  // A signal percent's text changes its width, so the list has to be laid out again.
  if (syncApRows() && m_list != nullptr) {
    m_list->layout(renderer);
  }
  syncCurrentCard();
}

void NetworkTab::onClose() {
  m_rootLayout = nullptr;
  m_currentCard = nullptr;
  m_currentTitle = nullptr;
  m_currentDetail = nullptr;
  m_passwordCard = nullptr;
  m_passwordTitle = nullptr;
  m_passwordInput = nullptr;
  m_passwordRevealButton = nullptr;
  m_passwordRevealed = false;
  m_listScroll = nullptr;
  m_list = nullptr;
  m_rescanButton = nullptr;
  m_wifiToggle = nullptr;
  m_scanSpinner = nullptr;
  m_currentRow = nullptr;
  m_disconnectButton = nullptr;
  m_apRows.clear();
  m_lastStructureKey.clear();
  m_lastListWidth = -1.0f;
  m_pendingAccessPoint.reset();
  m_active = false;
  m_actionPending = false;
}

void NetworkTab::syncPasswordCard() {
  if (m_passwordCard == nullptr) {
    return;
  }
  m_passwordCard->setVisible(m_hasPendingSecret);
  if (m_hasPendingSecret && m_passwordTitle != nullptr) {
    m_passwordTitle->setText(
        m_pendingSsid.empty() ? i18n::tr("control-center.network.password-prompt")
                              : i18n::tr("control-center.network.password-prompt-for", "ssid", m_pendingSsid)
    );
  }
}

void NetworkTab::showPasswordPrompt(const NetworkSecretAgent::SecretRequest& request) {
  m_hasPendingSecret = true;
  m_pendingSsid = request.ssid;
  m_pendingAccessPoint.reset();
  m_passwordRevealed = false;
  if (m_passwordInput != nullptr) {
    m_passwordInput->setValue("");
    m_passwordInput->setPasswordMode(true);
  }
  if (m_passwordRevealButton != nullptr) {
    m_passwordRevealButton->setGlyph("eye");
  }
}

void NetworkTab::showPasswordPrompt(const AccessPointInfo& ap) {
  m_hasPendingSecret = true;
  m_pendingSsid = ap.ssid;
  m_pendingAccessPoint = ap;
  m_passwordRevealed = false;
  if (m_passwordInput != nullptr) {
    m_passwordInput->setValue("");
    m_passwordInput->setPasswordMode(true);
  }
  if (m_passwordRevealButton != nullptr) {
    m_passwordRevealButton->setGlyph("eye");
  }
}

void NetworkTab::submitPasswordPrompt(const std::string& value) {
  if (m_pendingAccessPoint.has_value()) {
    if (value.empty()) {
      return;
    }
    if (m_network != nullptr) {
      m_network->activateAccessPoint(*m_pendingAccessPoint, value);
    }
  } else if (m_secrets != nullptr) {
    m_secrets->submitSecret(value);
  }
  clearPasswordPrompt();
  PanelManager::instance().refresh();
}

void NetworkTab::cancelPasswordPrompt() {
  if (!m_pendingAccessPoint.has_value() && m_secrets != nullptr) {
    m_secrets->cancelSecret();
  }
  clearPasswordPrompt();
  PanelManager::instance().refresh();
}

void NetworkTab::clearPasswordPrompt() {
  m_hasPendingSecret = false;
  m_pendingSsid.clear();
  m_pendingAccessPoint.reset();
  m_passwordRevealed = false;
  if (m_passwordInput != nullptr) {
    m_passwordInput->setValue("");
    m_passwordInput->setPasswordMode(true);
  }
  if (m_passwordRevealButton != nullptr) {
    m_passwordRevealButton->setGlyph("eye");
  }
}

void NetworkTab::syncCurrentCard() {
  if (m_currentTitle == nullptr || m_currentDetail == nullptr) {
    return;
  }
  if (m_network == nullptr) {
    m_currentTitle->setText(i18n::tr("control-center.network.unavailable-title"));
    m_currentDetail->setText(i18n::tr("control-center.network.unavailable-detail"));
    if (m_currentRow != nullptr) {
      m_currentRow->setVisible(false);
    }
    return;
  }
  const NetworkState& s = m_network->state();
  if (m_actionPending) {
    const bool flipped = s.connected != m_actionPendingConnected;
    const bool timedOut = std::chrono::steady_clock::now() - m_actionPendingSince > std::chrono::seconds(6);
    if (flipped || timedOut) {
      m_actionPending = false;
    }
  }
  static const std::string kNoExternalIp;
  const std::string& externalIp = m_externalIpService != nullptr ? m_externalIpService->externalIp() : kNoExternalIp;
  m_currentTitle->setText(currentTitle(s));
  m_currentDetail->setText(currentDetail(s, externalIp));
  if (m_disconnectButton != nullptr) {
    const bool canReconnectWired = !s.connected && m_network->canActivateWiredConnection();
    m_disconnectButton->setVisible(s.connected || canReconnectWired || m_actionPending);
    m_disconnectButton->setGlyph(s.connected ? "plug-off" : "plug");
    m_disconnectButton->setVariant(s.connected ? ButtonVariant::Destructive : ButtonVariant::Default);
    m_disconnectButton->setEnabled(!m_actionPending);
  }
  if (m_wifiToggle != nullptr) {
    m_wifiToggle->setChecked(s.wirelessEnabled);
  }
  if (m_scanSpinner != nullptr) {
    m_scanSpinner->setVisible(s.scanning);
    if (s.scanning && !m_scanSpinner->spinning()) {
      m_scanSpinner->start();
    } else if (!s.scanning && m_scanSpinner->spinning()) {
      m_scanSpinner->stop();
    }
  }
}

void NetworkTab::beginPendingAction(bool wasConnected) {
  m_actionPending = true;
  m_actionPendingConnected = wasConnected;
  m_actionPendingSince = std::chrono::steady_clock::now();
  if (m_disconnectButton != nullptr) {
    m_disconnectButton->setEnabled(false);
  }
}

// Identity of the built list: which rows exist, in which order, and which controls
// each carries. The signal strength is absent by design — it refreshes in place
// through syncApRows(), so a scan update no longer tears the list down. Access points
// arrive sorted, so a change in row order changes the key.
std::string
NetworkTab::structureKey(const std::vector<AccessPointInfo>& aps, const std::vector<VpnConnectionInfo>& vpns) const {
  std::string key;
  for (const auto& ap : aps) {
    key += ap.ssid;
    key.push_back(':');
    key += ap.secured ? '1' : '0';
    key.push_back(':');
    key += ap.active ? '1' : '0';
    key.push_back(':');
    key += (m_network != nullptr && m_network->hasSavedConnection(ap.ssid)) ? '1' : '0';
    key.push_back('\n');
  }
  key += "---\n";
  for (const auto& vpn : vpns) {
    key += vpn.path;
    key.push_back(':');
    key += vpn.name;
    key.push_back(':');
    key += vpn.active ? '1' : '0';
    key.push_back('\n');
  }
  const bool wirelessEnabled = m_network != nullptr && m_network->state().wirelessEnabled;
  const bool scanning = m_network != nullptr && m_network->state().scanning;
  key += "vis:";
  key += m_vpnVisible ? '1' : '0';
  key += "\nwifi:";
  key += wirelessEnabled ? '1' : '0';
  key += "\nscan:";
  key += scanning ? '1' : '0';
  return key;
}

void NetworkTab::rebuildApList(Renderer& renderer) {
  uiAssertNotRendering("NetworkTab::rebuildApList");
  if (m_list == nullptr || m_listScroll == nullptr) {
    return;
  }
  const float listWidth = m_listScroll->contentViewportWidth();
  if (listWidth <= 0.0f) {
    return;
  }

  std::vector<AccessPointInfo> aps;
  if (m_network != nullptr) {
    aps = sortedAccessPoints(m_network->accessPoints());
  }
  const auto& vpns = m_network != nullptr ? m_network->vpnConnections() : std::vector<VpnConnectionInfo>{};
  const std::string nextStructure = structureKey(aps, vpns);
  if (listWidth == m_lastListWidth && nextStructure == m_lastStructureKey) {
    return;
  }
  m_lastListWidth = listWidth;
  m_lastStructureKey = nextStructure;
  const float scale = contentScale();

  auto buildApRows = [&]() {
    auto container = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
    });
    if (aps.empty()) {
      container->addChild(
          ui::label({
              .text = i18n::tr("control-center.network.no-networks"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    } else {
      for (const auto& ap : aps) {
        const bool saved = m_network != nullptr && m_network->hasSavedConnection(ap.ssid);
        auto row = std::make_unique<AccessPointRow>(
            scale, ap, saved,
            [this](const AccessPointInfo& clicked) {
              if (clicked.active || m_network == nullptr) {
                return;
              }
              if (clicked.secured && !m_network->hasSavedConnection(clicked.ssid)) {
                showPasswordPrompt(clicked);
                PanelManager::instance().refresh();
                return;
              }
              m_network->activateAccessPoint(clicked);
            },
            [this](const AccessPointInfo& clicked) {
              if (m_network != nullptr) {
                m_network->forgetSsid(clicked.ssid);
              }
              PanelManager::instance().refresh();
            }
        );
        auto* rowPtr = row.get();
        container->addChild(std::move(row));
        m_apRows.emplace(rowPtr->ssid(), rowPtr);
      }
    }
    return container;
  };

  m_wifiToggle = nullptr;
  m_scanSpinner = nullptr;
  m_rescanButton = nullptr;
  m_apRows.clear();

  while (!m_list->children().empty()) {
    m_list->removeChild(m_list->children().front().get());
  }

  if (m_network == nullptr) {
    m_list->addChild(
        ui::label({
            .text = i18n::tr("control-center.network.unavailable-title"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  } else {
    const float opacity = panelCardOpacity();
    const bool borders = panelBordersEnabled();

    if (!vpns.empty()) {
      auto vpnCard = ui::column({
          .configure = [scale, opacity, borders](Flex& card) { applySectionCardStyle(card, scale, opacity, borders); },
      });

      auto vpnHeader = makeCardHeaderRow(i18n::tr("control-center.network.vpns"), scale);
      vpnHeader->addChild(
          ui::toggle({
              .checkedImmediate = m_vpnVisible,
              .toggleSize = ToggleSize::Medium,
              .scale = scale,
              .onChange = [this](bool checked) {
                m_vpnVisible = checked;
                PanelManager::instance().refresh();
              },
          })
      );
      vpnCard->addChild(std::move(vpnHeader));

      if (m_vpnVisible) {
        for (const auto& vpn : vpns) {
          auto row = std::make_unique<VpnConnectionRow>(
              scale, vpn,
              [this](const VpnConnectionInfo& clicked) {
                if (m_network != nullptr) {
                  m_network->activateVpnConnection(clicked);
                }
                PanelManager::instance().refresh();
              },
              [this](const VpnConnectionInfo& clicked) {
                if (m_network != nullptr) {
                  m_network->deactivateVpnConnection(clicked);
                }
                PanelManager::instance().refresh();
              }
          );
          vpnCard->addChild(std::move(row));
        }
      }

      m_list->addChild(std::move(vpnCard));
    }

    {
      auto wifiCard = ui::column({
          .configure = [scale, opacity, borders](Flex& card) { applySectionCardStyle(card, scale, opacity, borders); },
      });

      auto wifiHeader = makeCardHeaderRow(i18n::tr("control-center.network.wifi"), scale);
      wifiHeader->addChild(
          ui::spinner({
              .out = &m_scanSpinner,
              .color = colorSpecFromRole(ColorRole::Primary),
              .spinnerSize = Style::baseGlyphSize * scale,
              .visible = false,
          })
      );

      wifiHeader->addChild(
          ui::button({
              .out = &m_rescanButton,
              .glyph = "refresh",
              .glyphSize = Style::baseGlyphSize * scale,
              .variant = ButtonVariant::Ghost,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusSm(scale),
              .onClick = [this]() {
                if (m_network != nullptr) {
                  m_network->requestScan();
                }
              },
          })
      );

      wifiHeader->addChild(
          ui::toggle({
              .out = &m_wifiToggle,
              .checkedImmediate = m_network->state().wirelessEnabled,
              .toggleSize = ToggleSize::Medium,
              .scale = scale,
              .onChange = [this](bool checked) {
                if (m_network != nullptr) {
                  m_network->setWirelessEnabled(checked);
                }
              },
          })
      );
      wifiCard->addChild(std::move(wifiHeader));

      wifiCard->addChild(buildApRows());

      m_list->addChild(std::move(wifiCard));

      // Live state (spinner visibility/animation, toggle checked) is owned by
      // syncCurrentCard(), which runs every frame after the card is attached.
      // rebuildApList() builds structure only and must not drive animations.
    }
  }
  m_list->layout(renderer);
}

bool NetworkTab::syncApRows() {
  if (m_network == nullptr || m_apRows.empty()) {
    return false;
  }
  bool changed = false;
  for (const auto& ap : m_network->accessPoints()) {
    const auto it = m_apRows.find(ap.ssid);
    if (it != m_apRows.end() && it->second->syncLiveMetrics(ap)) {
      changed = true;
    }
  }
  return changed;
}

void NetworkTab::onPanelCardOpacityChanged(float opacity) {
  if (m_passwordInput != nullptr) {
    m_passwordInput->setSurfaceOpacity(opacity);
  }
}
