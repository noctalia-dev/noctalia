#include "shell/settings/settings_security_hardware.h"

#include "i18n/i18n.h"
#include "security/security_key_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/select.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace settings {

  namespace {

    using Op = SecurityKeyManager::Op;
    using Area = SecurityKeyManager::Area;

    // Connection reuse choices for a git host, in minutes (0: authenticate every connection).
    constexpr std::array<int, 9> kPersistChoices{0, 5, 15, 30, 60, 120, 240, 480, 1440};

    std::unique_ptr<Label> bodyLabel(std::string text, float scale, ColorRole role = ColorRole::OnSurface) {
      return ui::label({
          .text = std::move(text),
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(role),
          .maxLines = 0,
      });
    }

    std::unique_ptr<Label> captionLabel(std::string text, float scale, ColorRole role = ColorRole::OnSurfaceVariant) {
      return ui::label({
          .text = std::move(text),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(role),
          .maxLines = 0,
      });
    }

    std::unique_ptr<Label> monoLabel(std::string text, float scale) {
      return ui::label({
          .text = std::move(text),
          .fontSize = Style::fontSizeCaption * scale,
          .fontFamily = "monospace",
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 0,
          .flexGrow = 1.0F,
      });
    }

    std::unique_ptr<Label> sectionLabel(std::string text, float scale) {
      return ui::label({
          .text = std::move(text),
          .fontSize = Style::fontSizeCaption * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      });
    }

    void addStatus(Flex& parent, const std::string& message, bool error, float scale) {
      if (message.empty()) {
        return;
      }
      auto row = ui::row({.align = FlexAlign::Start, .gap = Style::spaceSm * scale});
      row->addChild(
          ui::glyph({
              .glyph = error ? "alert-circle" : "circle-check",
              .glyphSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(error ? ColorRole::Error : ColorRole::Primary),
          })
      );
      auto text = bodyLabel(message, scale, error ? ColorRole::Error : ColorRole::OnSurface);
      text->setFlexGrow(1.0F);
      row->addChild(std::move(text));
      parent.addChild(std::move(row));
    }

    void addStatus(Flex& parent, const SecurityKeyManager::Status& status, float scale) {
      addStatus(parent, status.message, status.error, scale);
    }

    // A warning that the page can't do something here, and why.
    void addNotice(Flex& parent, const std::string& message, float scale) {
      auto row = ui::row({
          .align = FlexAlign::Start,
          .gap = Style::spaceSm * scale,
          .padding = Style::spaceSm * scale,
          .fill = colorSpecFromRole(ColorRole::ErrorContainer),
          .radius = Style::scaledRadiusMd(scale),
          .fillWidth = true,
      });
      row->addChild(
          ui::glyph({
              .glyph = "alert-circle",
              .glyphSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::Error),
          })
      );
      auto text = bodyLabel(message, scale, ColorRole::OnSurface);
      text->setFlexGrow(1.0F);
      row->addChild(std::move(text));
      parent.addChild(std::move(row));
    }

    // "Label: value" line. <ellipsize> keeps a long value on one line, cut in the middle.
    void
    addInfoRow(Flex& parent, const std::string& label, const std::string& value, float scale, bool ellipsize = false) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
      row->addChild(
          ui::label({
              .text = label,
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
      row->addChild(
          ui::label({
              .text = value,
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = ellipsize ? 1 : 0,
              .ellipsize = ellipsize ? std::optional{TextEllipsize::Middle} : std::nullopt,
              .flexGrow = 1.0F,
          })
      );
      parent.addChild(std::move(row));
    }

    std::unique_ptr<Button> actionButton(
        std::string text, std::string glyph, bool enabled, ButtonVariant variant, float scale,
        std::function<void()> onClick
    ) {
      return ui::button({
          .text = std::move(text),
          .glyph = glyph.empty() ? std::nullopt : std::optional<std::string>{std::move(glyph)},
          .fontSize = Style::fontSizeBody * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .enabled = enabled,
          .variant = variant,
          .minHeight = Style::controlHeight * scale,
          .paddingV = Style::spaceSm * scale,
          .paddingH = Style::spaceMd * scale,
          .gap = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = std::move(onClick),
      });
    }

    std::unique_ptr<Button>
    iconButton(std::string glyph, std::string tooltip, bool enabled, float scale, std::function<void()> onClick) {
      return ui::button({
          .glyph = std::move(glyph),
          .glyphSize = Style::fontSizeBody * scale,
          .enabled = enabled,
          .variant = ButtonVariant::Ghost,
          .tooltip = std::move(tooltip),
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = std::move(onClick),
      });
    }

    std::unique_ptr<Flex> buttonRow(float scale) {
      return ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceSm * scale});
    }

    std::unique_ptr<Flex> card(float scale, bool danger = false) {
      return ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .padding = Style::spaceSm * scale,
          .fill = colorSpecFromRole(
              danger ? ColorRole::ErrorContainer : ColorRole::SurfaceContainerHigh, danger ? 0.5F : 1.0F
          ),
          .radius = Style::scaledRadiusMd(scale),
          .fillWidth = true,
      });
    }

    // One row of a list: leading glyph, text, then trailing controls.
    std::unique_ptr<Flex>
    listRow(const std::string& glyph, std::string text, float scale, ColorRole role = ColorRole::OnSurface) {
      auto row = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
          .paddingH = Style::spaceSm * scale,
          .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
          .radius = Style::scaledRadiusMd(scale),
          .minHeight = Style::controlHeight * scale,
          .fillWidth = true,
      });
      row->addChild(
          ui::glyph({
              .glyph = glyph,
              .glyphSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(role == ColorRole::OnSurface ? ColorRole::Primary : role),
          })
      );
      row->addChild(
          ui::label({
              .text = std::move(text),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(role),
              .maxLines = 2,
              .flexGrow = 1.0F,
          })
      );
      return row;
    }

    // Trailing delete control with an inline confirmation step.
    void addDeleteControls(
        Flex& row, bool pending, bool enabled, float scale, std::function<void()> ask, std::function<void()> confirm,
        std::function<void()> dismiss, const std::string& confirmText = {}
    ) {
      if (!pending) {
        row.addChild(
            iconButton("trash", i18n::tr("settings.security-hardware.delete"), enabled, scale, std::move(ask))
        );
        return;
      }
      row.addChild(actionButton(
          confirmText.empty() ? i18n::tr("settings.security-hardware.confirm-delete") : confirmText, "trash", enabled,
          ButtonVariant::Destructive, scale, std::move(confirm)
      ));
      row.addChild(iconButton("close", i18n::tr("settings.security-hardware.cancel"), true, scale, std::move(dismiss)));
    }

    // "Touch the key when it blinks…" with a way out, while an operation waits for the key.
    void addBusyRow(Flex& parent, SecurityKeyManager& manager, Area area, float scale) {
      if (!manager.busy() || manager.pinPrompt().has_value()) {
        return;
      }
      const Op op = manager.op();
      const bool mine = (area == Area::Fido && (op == Op::FidoRegister || op == Op::FidoRemove || op == Op::FidoPin))
          || (area == Area::Gpg
              && (op == Op::GpgImport || op == Op::GpgImportFile || op == Op::GpgTrust || op == Op::GpgRemove))
          || (area == Area::Ssh && op >= Op::SshImport);
      if (!mine) {
        return;
      }
      const std::string& stage = manager.stage();
      std::string text = i18n::tr("settings.security-keys.working");
      std::string glyph = "refresh";
      if (stage == "touch") {
        text = i18n::tr("settings.security-keys.touch");
        glyph = "key";
      } else if (stage == "fetch") {
        text = i18n::tr("settings.security-keys.gpg.fetching");
        glyph = "cloud-download";
      }
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      row->addChild(
          ui::glyph({
              .glyph = glyph,
              .glyphSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
          })
      );
      auto label = bodyLabel(std::move(text), scale);
      label->setFlexGrow(1.0F);
      row->addChild(std::move(label));
      row->addChild(actionButton(
          i18n::tr("settings.security-hardware.cancel"), {}, true, ButtonVariant::Default, scale,
          [&manager] { manager.cancel(); }
      ));
      parent.addChild(std::move(row));
    }

    std::string pinOpTitle(Op op) {
      switch (op) {
      case Op::FidoRegister:
        return i18n::tr("settings.security-keys.fido.register");
      case Op::SshImport:
        return i18n::tr("settings.security-keys.ssh.import");
      case Op::SshCreate:
        return i18n::tr("settings.security-keys.ssh.create-key");
      case Op::SshDelete:
        return i18n::tr("settings.security-keys.ssh.delete");
      default:
        return i18n::tr("settings.security-keys.pin-title");
      }
    }

    std::string pinOpDetail(Op op) {
      switch (op) {
      case Op::FidoRegister:
        return i18n::tr("settings.security-keys.fido.register-detail");
      case Op::SshImport:
        return i18n::tr("settings.security-keys.ssh.import-detail");
      case Op::SshCreate:
        return i18n::tr("settings.security-keys.ssh.create-detail");
      case Op::SshDelete:
        return i18n::tr("settings.security-keys.ssh.delete-warning");
      default:
        return {};
      }
    }

  } // namespace

  // -- FIDO2/U2F ----------------------------------------------------------------------------------

  void buildFido2Block(
      Flex& block, SecurityKeyManager& manager, SecurityHardwareUiState& state, const SecurityHardwareActions& actions,
      float scale
  ) {
    if (!manager.loaded()) {
      manager.refresh();
    }
    const bool busy = manager.busy();
    const auto* device = manager.device();
    const auto& target = manager.target();

    switch (target.kind) {
    case security_hw::FidoTargetKind::NoPam:
      addNotice(block, i18n::tr("settings.security-keys.fido.target.no-pam"), scale);
      break;
    case security_hw::FidoTargetKind::Mixed:
      addNotice(block, i18n::tr("settings.security-keys.fido.target.mixed"), scale);
      break;
    case security_hw::FidoTargetKind::OtherFile:
      addNotice(
          block,
          i18n::tr(
              "settings.security-keys.fido.target.other-file", "path", target.path, "count",
              std::to_string(manager.otherCount())
          ),
          scale
      );
      break;
    case security_hw::FidoTargetKind::Own:
      if (device != nullptr && manager.pinRequired() && !device->clientPin) {
        addNotice(block, i18n::tr("settings.security-keys.error.no-pin"), scale);
      }
      break;
    }

    if (device != nullptr) {
      std::string details = device->name;
      if (device->clientPin && device->pinRetries >= 0) {
        details += " · " + i18n::tr("settings.security-keys.pin-retries", "count", std::to_string(device->pinRetries));
      }
      block.addChild(listRow("usb", std::move(details), scale));
    } else {
      block.addChild(captionLabel(i18n::tr("settings.security-keys.connect"), scale));
    }

    block.addChild(sectionLabel(i18n::tr("settings.security-keys.fido.registered"), scale));
    const auto& credentials = manager.credentials();
    if (credentials.empty()) {
      block.addChild(captionLabel(i18n::tr("settings.security-keys.fido.none"), scale));
    }
    for (std::size_t i = 0; i < credentials.size(); ++i) {
      const auto& credential = credentials[i];
      auto row = listRow(
          "key",
          i18n::tr(
              credential.unused ? "settings.security-keys.fido.credential-unused"
                                : "settings.security-keys.fido.credential",
              "index", std::to_string(i + 1), "label", credential.label
          ),
          scale, credential.unused ? ColorRole::OnSurfaceVariant : ColorRole::OnSurface
      );
      addDeleteControls(
          *row, state.pendingCredentialRemove == credential.handle, !busy, scale,
          [&state, handle = credential.handle, rebuild = actions.requestRebuild] {
            state.pendingCredentialRemove = handle;
            rebuild();
          },
          [&state, &manager, handle = credential.handle] {
            state.pendingCredentialRemove.clear();
            manager.removeCredential(handle);
          },
          [&state, rebuild = actions.requestRebuild] {
            state.pendingCredentialRemove.clear();
            rebuild();
          },
          i18n::tr("settings.security-hardware.confirm-remove")
      );
      block.addChild(std::move(row));
    }
    if (target.kind == security_hw::FidoTargetKind::Own) {
      block.addChild(captionLabel(i18n::tr("settings.security-keys.fido.authfile", "path", target.path), scale));

      // The PIN with the touch: +pin on every registered key, which pam_u2f honors unless the
      // PAM line itself sets pinverification.
      const bool forced = manager.pinRequired();
      const bool overridden = manager.pinForSignInOverridden();
      auto pinRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      auto pinLabel = bodyLabel(i18n::tr("settings.security-keys.fido.pin.label"), scale);
      pinLabel->setFlexGrow(1.0F);
      pinRow->addChild(std::move(pinLabel));
      pinRow->addChild(
          ui::toggle({
              .checked = forced || (manager.pinForSignIn() && !overridden),
              .enabled = !busy && !forced && !overridden && !credentials.empty(),
              .scale = scale,
              .onChange = [&manager](bool checked) { manager.setPinForSignIn(checked); },
          })
      );
      block.addChild(std::move(pinRow));
      std::string pinCaption = "settings.security-keys.fido.pin.description";
      if (forced) {
        pinCaption = "settings.security-keys.fido.pin.forced";
      } else if (overridden) {
        pinCaption = "settings.security-keys.fido.pin.overridden";
      }
      block.addChild(captionLabel(i18n::tr(pinCaption), scale));
    }

    addBusyRow(block, manager, Area::Fido, scale);
    addStatus(block, manager.status(Area::Fido), scale);

    auto buttons = buttonRow(scale);
    buttons->addChild(actionButton(
        i18n::tr(
            credentials.empty() ? "settings.security-keys.fido.register"
                                : "settings.security-keys.fido.register-another"
        ),
        "add", !busy && manager.canRegister(), ButtonVariant::Primary, scale, [&manager] { manager.requestRegister(); }
    ));
    buttons->addChild(iconButton("refresh", i18n::tr("settings.security-hardware.refresh"), !busy, scale, [&manager] {
      manager.refresh();
    }));
    block.addChild(std::move(buttons));
  }

  // -- OpenPGP card -------------------------------------------------------------------------------

  void buildGpgCardBlock(
      Flex& block, SecurityKeyManager& manager, SecurityHardwareUiState& state, const SecurityHardwareActions& actions,
      float scale
  ) {
    if (!manager.loaded()) {
      manager.refresh();
    }
    const auto& card = manager.card();
    const bool busy = manager.busy();

    if (!card.has_value()) {
      block.addChild(bodyLabel(i18n::tr("settings.security-keys.gpg.no-card"), scale, ColorRole::OnSurfaceVariant));
      addStatus(block, manager.status(Area::Gpg), scale);
      return;
    }

    std::string cardName = card->vendor.empty() ? "OpenPGP" : card->vendor;
    if (!card->serial.empty()) {
      cardName += " · " + i18n::tr("settings.security-keys.gpg.serial", "serial", card->serial);
    }
    addInfoRow(block, i18n::tr("settings.security-keys.gpg.card"), cardName, scale);
    if (!card->holder.empty()) {
      addInfoRow(block, i18n::tr("settings.security-keys.gpg.holder"), card->holder, scale);
    }
    const bool hasKeyOnCard = !card->fingerprints.empty();
    if (hasKeyOnCard) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      row->addChild(
          ui::label({
              .text = i18n::tr("settings.security-keys.gpg.fingerprint"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
      row->addChild(monoLabel(security_hw::formatFingerprint(card->fingerprints.front()), scale));
      row->addChild(iconButton(
          "copy", i18n::tr("settings.security-hardware.copy"), true, scale,
          [&manager, fpr = card->fingerprints.front(), copy = actions.copyText] {
            if (copy && copy(fpr)) {
              manager.setStatus(Area::Gpg, i18n::tr("settings.security-hardware.copied"), false);
            }
          }
      ));
      block.addChild(std::move(row));
    } else {
      block.addChild(bodyLabel(i18n::tr("settings.security-keys.gpg.state.none"), scale, ColorRole::OnSurfaceVariant));
    }
    if (!card->url.empty()) {
      addInfoRow(block, i18n::tr("settings.security-keys.gpg.url"), card->url, scale);
    }

    using KeyState = SecurityKeyManager::KeyState;
    const KeyState keyState = manager.keyState();
    if (hasKeyOnCard) {
      std::string stateKey = "settings.security-keys.gpg.state.missing";
      if (keyState == KeyState::Ready) {
        stateKey = "settings.security-keys.gpg.state.ready";
      } else if (keyState == KeyState::PublicOnly) {
        stateKey = "settings.security-keys.gpg.state.public";
      }
      block.addChild(listRow(
          keyState == KeyState::Ready ? "circle-check" : "info-circle", i18n::tr(stateKey), scale,
          keyState == KeyState::Ready ? ColorRole::OnSurface : ColorRole::OnSurfaceVariant
      ));
      if (manager.ultimatelyTrusted()) {
        block.addChild(listRow("shield-check", i18n::tr("settings.security-keys.gpg.trusted"), scale));
      }
    }

    addBusyRow(block, manager, Area::Gpg, scale);
    addStatus(block, manager.status(Area::Gpg), scale);

    if (!hasKeyOnCard) {
      return;
    }
    const bool inKeyring = keyState == KeyState::Ready || keyState == KeyState::PublicOnly;
    auto buttons = buttonRow(scale);
    std::string importKey = "settings.security-keys.gpg.import";
    if (keyState == KeyState::Ready) {
      importKey = "settings.security-keys.gpg.update";
    } else if (keyState == KeyState::PublicOnly) {
      importKey = "settings.security-keys.gpg.link";
    }
    buttons->addChild(actionButton(
        i18n::tr(importKey), "cloud-download", !busy, ButtonVariant::Primary, scale,
        [&manager, offline = actions.offline] { manager.importPublicKey(offline); }
    ));
    if (keyState != KeyState::Ready) {
      buttons->addChild(actionButton(
          i18n::tr("settings.security-keys.gpg.import-file"), "file-import", !busy, ButtonVariant::Default, scale,
          actions.browseGpgPublicKey
      ));
    }
    if (inKeyring && !manager.ultimatelyTrusted()) {
      buttons->addChild(actionButton(
          i18n::tr("settings.security-keys.gpg.trust"), "user-check", !busy, ButtonVariant::Default, scale,
          [&manager] { manager.trustKey(); }
      ));
    }
    if (inKeyring) {
      if (state.pendingGpgRemove) {
        buttons->addChild(actionButton(
            i18n::tr("settings.security-keys.gpg.confirm-remove"), "trash", !busy, ButtonVariant::Destructive, scale,
            [&state, &manager] {
              state.pendingGpgRemove = false;
              manager.removeKey();
            }
        ));
        buttons->addChild(iconButton(
            "close", i18n::tr("settings.security-hardware.cancel"), true, scale,
            [&state, rebuild = actions.requestRebuild] {
              state.pendingGpgRemove = false;
              rebuild();
            }
        ));
      } else {
        buttons->addChild(actionButton(
            i18n::tr("settings.security-keys.gpg.remove"), "trash", !busy, ButtonVariant::Ghost, scale,
            [&state, rebuild = actions.requestRebuild] {
              state.pendingGpgRemove = true;
              rebuild();
            }
        ));
      }
    }
    buttons->addChild(iconButton("refresh", i18n::tr("settings.security-hardware.refresh"), !busy, scale, [&manager] {
      manager.refresh();
    }));
    block.addChild(std::move(buttons));
  }

  // -- Resident SSH keys --------------------------------------------------------------------------

  namespace {

    // How long ssh keeps an authenticated connection to <key>'s git host for later ones.
    void addPersistSelect(
        Flex& box, SecurityKeyManager& manager, const SecurityKeyManager::SshKey& key, bool enabled, float scale
    ) {
      std::vector<std::string> options;
      std::vector<int> values(kPersistChoices.begin(), kPersistChoices.end());
      if (!std::ranges::contains(values, key.persistMinutes)) {
        values.push_back(key.persistMinutes);
        std::ranges::sort(values);
      }
      std::size_t selected = 0;
      for (std::size_t i = 0; i < values.size(); ++i) {
        options.push_back(
            values[i] == 0 ? i18n::tr("settings.security-keys.ssh.persist-never")
                           : i18n::tr("settings.security-keys.ssh.minutes", "minutes", std::to_string(values[i]))
        );
        if (values[i] == key.persistMinutes) {
          selected = i;
        }
      }
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      auto label = bodyLabel(i18n::tr("settings.security-keys.ssh.persist-label", "host", key.host), scale);
      label->setFlexGrow(1.0F);
      row->addChild(std::move(label));
      row->addChild(
          ui::select({
              .options = std::move(options),
              .selectedIndex = selected,
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeightSm * scale,
              .enabled = enabled,
              .width = 160.0F * scale,
              .onSelectionChanged = [&manager, name = key.name, values,
                                     current = key.persistMinutes](std::size_t index, std::string_view) {
                if (index < values.size() && values[index] != current) {
                  manager.setSshPersist(name, values[index]);
                }
              },
          })
      );
      box.addChild(std::move(row));
      box.addChild(captionLabel(i18n::tr("settings.security-keys.ssh.persist-hint"), scale));
    }

    void addSshKey(
        Flex& block, SecurityKeyManager& manager, SecurityHardwareUiState& state,
        const SecurityHardwareActions& actions, const SecurityKeyManager::SshKey& key, float scale
    ) {
      using HostState = SecurityKeyManager::HostState;
      const bool busy = manager.busy();
      const bool deleting = state.pendingSshDelete == key.name;
      auto box = card(scale, deleting);

      auto title = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      title->addChild(
          ui::glyph({
              .glyph = "key",
              .glyphSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
          })
      );
      title->addChild(
          ui::label({
              .text = key.application.empty() ? std::string("ssh:") : key.application,
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 2,
              .flexGrow = 1.0F,
          })
      );
      title->addChild(captionLabel(key.type, scale));
      box->addChild(std::move(title));
      if (!key.user.empty()) {
        addInfoRow(*box, i18n::tr("settings.security-keys.ssh.user"), key.user, scale, /*ellipsize=*/true);
      }
      if (!key.fingerprint.empty()) {
        box->addChild(monoLabel(key.fingerprint, scale));
      }
      box->addChild(
          ui::label({
              .text = "~/.ssh/" + key.name,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .ellipsize = TextEllipsize::Middle,
          })
      );
      if (!key.host.empty()) {
        if (key.hostState == HostState::On) {
          box->addChild(captionLabel(i18n::tr("settings.security-keys.ssh.host-on", "host", key.host), scale));
        } else if (key.hostState == HostState::Edited || key.hostState == HostState::Foreign) {
          box->addChild(captionLabel(
              i18n::tr(
                  "settings.security-keys.error.ssh-config-edited", "detail", "~/.ssh/config.d/" + key.host + ".conf"
              ),
              scale
          ));
        }
      }
      if (deleting) {
        box->addChild(bodyLabel(i18n::tr("settings.security-keys.ssh.delete-warning"), scale, ColorRole::Error));
      }

      // Connection reuse for the git host, once ssh uses the key for it.
      if (key.hostState == HostState::On) {
        addPersistSelect(*box, manager, key, !busy, scale);
      }

      auto buttons =
          ui::row({.align = FlexAlign::Center, .wrap = true, .gap = Style::spaceXs * scale, .fillWidth = true});
      if (!key.publicKey.empty()) {
        buttons->addChild(iconButton(
            "copy", i18n::tr("settings.security-keys.ssh.copy"), true, scale,
            [&manager, text = key.publicKey, copy = actions.copyText] {
              if (copy && copy(text)) {
                manager.setStatus(Area::Ssh, i18n::tr("settings.security-keys.ssh.copied"), false);
              }
            }
        ));
      }
      if (!key.host.empty() && (key.hostState == HostState::On || key.hostState == HostState::Off)) {
        const bool on = key.hostState == HostState::On;
        buttons->addChild(actionButton(
            i18n::tr(
                on ? "settings.security-keys.ssh.host-disable" : "settings.security-keys.ssh.host-enable", "host",
                key.host
            ),
            on ? "unlink" : "link", !busy, on ? ButtonVariant::Ghost : ButtonVariant::Secondary, scale,
            [&manager, name = key.name, on] { manager.setSshHost(name, !on); }
        ));
      }
      auto spacer = ui::row({});
      spacer->setFlexGrow(1.0F);
      buttons->addChild(std::move(spacer));

      const auto rebuild = actions.requestRebuild;
      if (state.pendingSshRemove == key.name) {
        buttons->addChild(actionButton(
            i18n::tr("settings.security-hardware.confirm-remove"), "x", !busy, ButtonVariant::Destructive, scale,
            [&state, &manager, name = key.name] {
              state.pendingSshRemove.clear();
              manager.removeSshCopy(name);
            }
        ));
        buttons->addChild(
            iconButton("close", i18n::tr("settings.security-hardware.cancel"), true, scale, [&state, rebuild] {
              state.pendingSshRemove.clear();
              rebuild();
            })
        );
      } else if (!deleting) {
        buttons->addChild(actionButton(
            i18n::tr("settings.security-keys.ssh.remove-copy"), "x", !busy, ButtonVariant::Ghost, scale,
            [&state, rebuild, name = key.name] {
              state.pendingSshRemove = name;
              state.pendingSshDelete.clear();
              rebuild();
            }
        ));
      }
      if (manager.device() != nullptr && !key.encrypted && state.pendingSshRemove != key.name) {
        if (deleting) {
          buttons->addChild(actionButton(
              i18n::tr("settings.security-keys.ssh.confirm-delete"), "trash", !busy, ButtonVariant::Destructive, scale,
              [&state, &manager, name = key.name] {
                state.pendingSshDelete.clear();
                manager.requestSshDelete(name);
              }
          ));
          buttons->addChild(
              iconButton("close", i18n::tr("settings.security-hardware.cancel"), true, scale, [&state, rebuild] {
                state.pendingSshDelete.clear();
                rebuild();
              })
          );
        } else {
          buttons->addChild(actionButton(
              i18n::tr("settings.security-keys.ssh.delete"), "trash", !busy, ButtonVariant::Ghost, scale,
              [&state, rebuild, name = key.name] {
                state.pendingSshDelete = name;
                state.pendingSshRemove.clear();
                rebuild();
              }
          ));
        }
      }
      box->addChild(std::move(buttons));
      block.addChild(std::move(box));
    }

  } // namespace

  void buildSshBlock(
      Flex& block, SecurityKeyManager& manager, SecurityHardwareUiState& state, const SecurityHardwareActions& actions,
      float scale
  ) {
    if (!manager.loaded()) {
      manager.refresh();
    }
    const bool busy = manager.busy();
    const auto* device = manager.device();
    const bool residentCapable = std::ranges::any_of(manager.devices(), [](const auto& d) { return d.residentKeys; });

    if (manager.sshConfigIsForeignLink()) {
      addNotice(block, i18n::tr("settings.security-keys.error.ssh-config-link", "detail", "~/.ssh/config"), scale);
    }
    if (device == nullptr) {
      block.addChild(captionLabel(i18n::tr("settings.security-keys.connect"), scale));
    } else if (!residentCapable) {
      block.addChild(captionLabel(i18n::tr("settings.security-keys.ssh.no-resident"), scale));
    }

    addBusyRow(block, manager, Area::Ssh, scale);
    addStatus(block, manager.status(Area::Ssh), scale);

    if (const auto& replace = manager.sshReplaceCandidate(); replace.has_value() && !busy) {
      auto buttons = buttonRow(scale);
      buttons->addChild(actionButton(
          i18n::tr("settings.security-hardware.cancel"), {}, true, ButtonVariant::Default, scale,
          [&manager] { manager.clearSshReplaceCandidate(); }
      ));
      buttons->addChild(actionButton(
          i18n::tr("settings.security-keys.ssh.replace"), "refresh", true, ButtonVariant::Destructive, scale,
          [&manager, options = *replace] mutable {
            options.replace = true;
            manager.requestSshCreate(options);
          }
      ));
      block.addChild(std::move(buttons));
    }

    if (const auto& created = manager.sshCreated(); created.has_value()) {
      auto box = card(scale);
      box->addChild(
          bodyLabel(i18n::tr("settings.security-keys.ssh.created", "name", "~/.ssh/" + created->name), scale)
      );
      box->addChild(monoLabel(created->publicKey, scale));
      if (!created->host.empty()) {
        box->addChild(captionLabel(i18n::tr("settings.security-keys.ssh.host-suggest", "host", created->host), scale));
      }
      auto buttons = buttonRow(scale);
      buttons->addChild(actionButton(
          i18n::tr("settings.security-keys.ssh.copy"), "copy", true, ButtonVariant::Primary, scale,
          [&manager, text = created->publicKey, copy = actions.copyText] {
            if (copy && copy(text)) {
              manager.setStatus(Area::Ssh, i18n::tr("settings.security-keys.ssh.copied"), false);
            }
          }
      ));
      buttons->addChild(actionButton(
          i18n::tr("settings.security-hardware.close"), {}, true, ButtonVariant::Default, scale,
          [&manager] { manager.clearSshCreated(); }
      ));
      box->addChild(std::move(buttons));
      block.addChild(std::move(box));
    }

    const auto& keys = manager.sshKeys();
    if (keys.empty()) {
      block.addChild(captionLabel(i18n::tr("settings.security-keys.ssh.none-copied"), scale));
    }
    for (const auto& key : keys) {
      addSshKey(block, manager, state, actions, key, scale);
    }
    if (!keys.empty()) {
      block.addChild(captionLabel(i18n::tr("settings.security-keys.ssh.touch-note"), scale));
    }

    auto buttons = buttonRow(scale);
    buttons->addChild(actionButton(
        i18n::tr("settings.security-keys.ssh.import"), "download", !busy && residentCapable, ButtonVariant::Primary,
        scale, [&manager] { manager.requestSshImport(); }
    ));
    buttons->addChild(actionButton(
        i18n::tr("settings.security-keys.ssh.create"), "add", !busy && residentCapable, ButtonVariant::Default, scale,
        actions.openSshCreateSheet
    ));
    buttons->addChild(iconButton("refresh", i18n::tr("settings.security-hardware.refresh"), !busy, scale, [&manager] {
      manager.refresh();
    }));
    block.addChild(std::move(buttons));
  }

  // -- Sheets -------------------------------------------------------------------------------------

  void buildSecurityKeyPinSheet(
      Flex& body, SecurityKeyManager& manager, std::string& pin, Input** pinInput, const std::function<void()>& close,
      float scale
  ) {
    *pinInput = nullptr;
    auto content = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceMd * scale, .fillWidth = true});
    const auto& prompt = manager.pinPrompt();

    if (!prompt.has_value()) {
      // The PIN went in: the operation waits for a touch.
      content->addChild(
          ui::glyph({
              .glyph = "key",
              .glyphSize = Style::fontSizeHeader * 2.0F * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
          })
      );
      content->addChild(bodyLabel(
          i18n::tr(manager.stage() == "touch" ? "settings.security-keys.touch" : "settings.security-keys.working"),
          scale
      ));
      auto buttons = buttonRow(scale);
      buttons->addChild(actionButton(
          i18n::tr("settings.security-hardware.cancel"), {}, true, ButtonVariant::Default, scale,
          [&manager] { manager.cancel(); }
      ));
      content->addChild(std::move(buttons));
      body.addChild(std::move(content));
      return;
    }

    const Op op = prompt->op;
    const bool destructive = op == Op::SshDelete;
    if (const std::string detail = pinOpDetail(op); !detail.empty()) {
      content->addChild(bodyLabel(detail, scale, destructive ? ColorRole::Error : ColorRole::OnSurface));
    }
    if (const auto* device = manager.device(); device != nullptr && device->pinRetries >= 0) {
      content->addChild(captionLabel(
          i18n::tr("settings.security-keys.pin-retries", "count", std::to_string(device->pinRetries)), scale
      ));
    }
    const auto submit = [&manager, &pin] {
      std::string value = std::move(pin);
      pin.clear();
      manager.submitPin(std::move(value));
    };
    content->addChild(
        ui::input({
            .out = pinInput,
            .value = pin,
            .placeholder = i18n::tr("settings.security-keys.pin-placeholder"),
            .fontSize = Style::fontSizeBody * scale,
            .controlHeight = Style::controlHeight * scale,
            .passwordMode = true,
            .invalid = !prompt->error.empty(),
            .onChange = [&pin](const std::string& value) { pin = value; },
            .onSubmit =
                [&pin, submit](const std::string& value) {
                  pin = value;
                  submit();
                },
        })
    );
    if (!prompt->error.empty()) {
      addStatus(*content, prompt->error, true, scale);
    }
    content->addChild(captionLabel(i18n::tr("settings.security-keys.pin-touch"), scale));
    auto buttons = buttonRow(scale);
    buttons->addChild(
        actionButton(i18n::tr("settings.security-hardware.cancel"), {}, true, ButtonVariant::Default, scale, close)
    );
    buttons->addChild(actionButton(
        pinOpTitle(op), destructive ? "trash" : "key", true,
        destructive ? ButtonVariant::Destructive : ButtonVariant::Primary, scale, submit
    ));
    content->addChild(std::move(buttons));
    body.addChild(std::move(content));
  }

  void buildSshCreateSheet(
      Flex& body, SshCreateForm& form, const std::function<void()>& submit, const std::function<void()>& rebuild,
      const std::function<void()>& close, float scale
  ) {
    auto content = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .fillWidth = true});
    const float labelWidth = 140.0F * scale;
    const auto fieldRow = [&](const std::string& label, std::unique_ptr<Node> control) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      row->addChild(
          ui::label({
              .text = label,
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .width = labelWidth,
          })
      );
      row->addChild(std::move(control));
      content->addChild(std::move(row));
    };

    auto& options = form.options;
    const std::vector<std::string> algorithms{"ed25519-sk", "ecdsa-sk"};
    fieldRow(
        i18n::tr("settings.security-keys.ssh.algorithm"),
        ui::select({
            .options = algorithms,
            .selectedIndex = options.algorithm == "ecdsa-sk" ? 1U : 0U,
            .fontSize = Style::fontSizeBody * scale,
            .controlHeight = Style::controlHeight * scale,
            .flexGrow = 1.0F,
            .onSelectionChanged = [&options](std::size_t, std::string_view label) {
              options.algorithm = std::string(label);
            },
        })
    );
    content->addChild(captionLabel(i18n::tr("settings.security-keys.ssh.algorithm-hint"), scale));

    const auto textField = [&](const std::string& label, std::string& value, std::string placeholder) {
      fieldRow(
          label,
          ui::input({
              .value = value,
              .placeholder = std::move(placeholder),
              .fontSize = Style::fontSizeBody * scale,
              .controlHeight = Style::controlHeight * scale,
              .flexGrow = 1.0F,
              .onChange = [&value](const std::string& text) { value = text; },
              .onSubmit =
                  [&value, submit](const std::string& text) {
                    value = text;
                    submit();
                  },
          })
      );
    };
    textField(i18n::tr("settings.security-keys.ssh.application"), options.application, "github.com");
    content->addChild(captionLabel(i18n::tr("settings.security-keys.ssh.application-hint"), scale));
    textField(i18n::tr("settings.security-keys.ssh.user"), options.user, {});
    content->addChild(captionLabel(i18n::tr("settings.security-keys.ssh.user-hint"), scale));
    textField(i18n::tr("settings.security-keys.ssh.comment"), options.comment, "user@host");

    const auto toggleRow = [&](const std::string& label, bool& value) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      auto text = bodyLabel(label, scale);
      text->setFlexGrow(1.0F);
      row->addChild(std::move(text));
      row->addChild(
          ui::toggle({
              .checked = value,
              .scale = scale,
              .onChange = [&value, rebuild](bool checked) {
                value = checked;
                rebuild();
              },
          })
      );
      content->addChild(std::move(row));
    };
    toggleRow(i18n::tr("settings.security-keys.ssh.verify"), options.verify);
    toggleRow(i18n::tr("settings.security-keys.ssh.touch"), options.touch);
    if (!options.touch) {
      content->addChild(captionLabel(i18n::tr("settings.security-keys.ssh.touch-off"), scale));
    }
    if (!form.problem.empty()) {
      addStatus(*content, i18n::tr(form.problem), true, scale);
    }

    auto buttons = buttonRow(scale);
    buttons->addChild(
        actionButton(i18n::tr("settings.security-hardware.cancel"), {}, true, ButtonVariant::Default, scale, close)
    );
    buttons->addChild(actionButton(
        i18n::tr("settings.security-keys.ssh.create-key"), "add", true, ButtonVariant::Primary, scale, submit
    ));
    content->addChild(std::move(buttons));
    body.addChild(std::move(content));
  }

} // namespace settings
