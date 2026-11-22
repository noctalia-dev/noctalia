#include "filemanager/dialogs/help_dialog.h"

#include "core/build_info.h"
#include "core/files/resource_paths.h"
#include "cursor-shape-v1-client-protocol.h"
#include "net/url_open.h"
#include "render/core/renderer.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/separator.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace filemanager {

  namespace {

    constexpr int kCloseDialog = 0;

    constexpr std::array kWindowAndNavigationShortcuts{
        HelpDialogController::ShortcutEntry{"New Window", "Ctrl+N"},
        HelpDialogController::ShortcutEntry{"Open", "Enter"},
        HelpDialogController::ShortcutEntry{"Go to Parent Folder", "Backspace"},
        HelpDialogController::ShortcutEntry{"Edit Location", "Ctrl+L"},
        HelpDialogController::ShortcutEntry{"Move Selection Up", "Up"},
        HelpDialogController::ShortcutEntry{"Move Selection Down", "Down"},
        HelpDialogController::ShortcutEntry{"Quit", "Ctrl+Q"},
    };

    constexpr std::array kEditAndSelectionShortcuts{
        HelpDialogController::ShortcutEntry{"Undo", "Ctrl+Z"},
        HelpDialogController::ShortcutEntry{"Redo", "Ctrl+Shift+Z"},
        HelpDialogController::ShortcutEntry{"Cut", "Ctrl+X"},
        HelpDialogController::ShortcutEntry{"Copy", "Ctrl+C"},
        HelpDialogController::ShortcutEntry{"Paste", "Ctrl+V"},
        HelpDialogController::ShortcutEntry{"Select All", "Ctrl+A"},
        HelpDialogController::ShortcutEntry{"Cancel or Clear Selection", "Escape"},
    };

    constexpr std::array kFileActionShortcuts{
        HelpDialogController::ShortcutEntry{"New Folder", "Ctrl+Shift+N"},
        HelpDialogController::ShortcutEntry{"Rename", "F2"},
        HelpDialogController::ShortcutEntry{"Move to Wastebin", "Delete"},
        HelpDialogController::ShortcutEntry{"Delete Permanently", "Shift+Delete"},
        HelpDialogController::ShortcutEntry{"Properties", "Alt+Enter"},
    };

    constexpr std::array kViewShortcuts{
        HelpDialogController::ShortcutEntry{"Zoom In", "Ctrl++"},
        HelpDialogController::ShortcutEntry{"Zoom Out", "Ctrl+-"},
        HelpDialogController::ShortcutEntry{"Reset Zoom", "Ctrl+0"},
        HelpDialogController::ShortcutEntry{"Refresh", "Ctrl+R"},
    };

    constexpr std::array kTabShortcuts{
        HelpDialogController::ShortcutEntry{"New Tab", "Ctrl+T"},
        HelpDialogController::ShortcutEntry{"Close Tab", "Ctrl+W"},
        HelpDialogController::ShortcutEntry{"Reopen Closed Tab", "Ctrl+Shift+T"},
        HelpDialogController::ShortcutEntry{"Next Tab", "Ctrl+Tab"},
        HelpDialogController::ShortcutEntry{"Previous Tab", "Ctrl+Shift+Tab"},
    };

    constexpr std::array kShortcutGroups{
        HelpDialogController::ShortcutGroup{"Window & Navigation", kWindowAndNavigationShortcuts},
        HelpDialogController::ShortcutGroup{"Tabs", kTabShortcuts},
        HelpDialogController::ShortcutGroup{"Edit & Selection", kEditAndSelectionShortcuts},
        HelpDialogController::ShortcutGroup{"Files", kFileActionShortcuts},
        HelpDialogController::ShortcutGroup{"View", kViewShortcuts},
    };

    void applyNytheraBevel(Flex& panel, float radius) {
      panel.setRadius(radius);
      panel.setCornerShapes(
          CornerShapes{
              .tl = CornerShape::Chamfered,
              .tr = CornerShape::Chamfered,
              .br = CornerShape::Chamfered,
              .bl = CornerShape::Chamfered,
          }
      );
    }

    std::unique_ptr<Flex> makeDialogCard(ModalDialogHost& dialogHost, float gap) {
      auto card = std::make_unique<Flex>();
      dialogHost.registerCard(card.get());
      card->setDirection(FlexDirection::Vertical);
      card->setAlign(FlexAlign::Stretch);
      card->setGap(gap);
      card->setPadding(10.0F, 12.0F);
      card->setFill(colorSpecFromRole(ColorRole::SurfaceContainer));
      card->setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
      applyNytheraBevel(*card, Style::scaledRadiusSm(1.0F));
      return card;
    }

  } // namespace

  HelpDialogController::HelpDialogController(
      ModalDialogHost& dialogHost, WaylandConnection& wayland, Callbacks callbacks
  )
      : m_dialogHost(dialogHost), m_wayland(wayland), m_callbacks(std::move(callbacks)) {}

  HelpDialogController::~HelpDialogController() {
    *m_alive = false;
    dismiss();
  }

  std::vector<ContextMenuControlEntry> HelpDialogController::menuEntries() {
    return {
        {.id = kKeyboardShortcutsMenuId, .label = "Keyboard Shortcuts", .actionGlyph = "keyboard"},
        {.separator = true},
        {.id = kReportBugMenuId, .label = "Report a Bug…", .actionGlyph = "bug"},
        {.id = kGitHubMenuId, .label = "Nythera Files on GitHub", .actionGlyph = "brand-github"},
        {.separator = true},
        {.id = kAboutMenuId, .label = "About Nythera Files", .actionGlyph = "info-circle"},
    };
  }

  std::span<const HelpDialogController::ShortcutGroup> HelpDialogController::shortcutGroups() noexcept {
    return kShortcutGroups;
  }

  HelpDialogController::AboutInfo HelpDialogController::aboutInfo() {
    return AboutInfo{
        .appName = "Nythera Files",
        .version = nythera::build_info::displayVersion(),
        .description = "A native C++ Wayland file manager built with Nythera's UI and rendering infrastructure.",
        .licence = "MIT License",
        .copyright = "Copyright © 2026 Kangy",
        .credits = "Incorporates Noctalia-derived rendering and UI infrastructure.",
        .creditsCopyright = "Copyright © 2026 noctalia-dev",
        .repositoryUrl = std::string(kRepositoryUrl),
        .iconPath = paths::assetPath(kAppIconAssetPath),
        .licencePath = paths::assetPath(kLicenceAssetPath),
    };
  }

  bool HelpDialogController::handleMenuAction(int menuId) {
    switch (menuId) {
    case kKeyboardShortcutsMenuId:
      showShortcuts();
      return true;
    case kReportBugMenuId:
      (void)openReportBug();
      return true;
    case kGitHubMenuId:
      (void)openRepository();
      return true;
    case kAboutMenuId:
      showAbout();
      return true;
    default:
      return false;
    }
  }

  void HelpDialogController::showShortcuts() {
    if (m_dialogHost.isOpen())
      return;
    clearState();
    m_open = true;
    const std::weak_ptr<bool> alive = m_alive;
    m_dialogHost.showDialog(
        "Keyboard Shortcuts", "", {{"Close", kCloseDialog}},
        [this, alive](int) {
          if (const auto locked = alive.lock(); locked && *locked)
            clearState();
        },
        nullptr, false, false, makeShortcutsCustomContent()
    );
    if (!m_dialogHost.isOpen())
      clearState();
  }

  void HelpDialogController::showAbout() {
    if (m_dialogHost.isOpen())
      return;
    clearState();
    m_open = true;
    const std::weak_ptr<bool> alive = m_alive;
    m_dialogHost.showDialog(
        "About Nythera Files", "", {{"Close", kCloseDialog}},
        [this, alive](int) {
          if (const auto locked = alive.lock(); locked && *locked)
            clearState();
        },
        nullptr, false, false, makeAboutCustomContent()
    );
    if (!m_dialogHost.isOpen())
      clearState();
  }

  bool HelpDialogController::openReportBug() { return openExternalUrl(kReportBugUrl); }

  bool HelpDialogController::openRepository() { return openExternalUrl(kRepositoryUrl); }

  void HelpDialogController::dismiss() {
    const bool wasOpen = m_open;
    clearState();
    if (wasOpen && m_dialogHost.isOpen())
      m_dialogHost.forceCloseImmediate();
  }

  void HelpDialogController::clearState() noexcept {
    m_open = false;
    m_shortcutsScroll = nullptr;
    m_shortcutsClose = nullptr;
    m_aboutIcon = nullptr;
    m_repositoryButton = nullptr;
    m_aboutClose = nullptr;
  }

  bool HelpDialogController::openExternalUrl(std::string_view url) {
    bool launched = false;
    if (m_callbacks.openUrl) {
      launched = m_callbacks.openUrl(std::string(url));
    } else {
      wl_surface* source = m_dialogHost.isOpen()
          ? m_dialogHost.wlSurface()
          : (m_callbacks.mainSurface && m_callbacks.mainSurface() ? m_callbacks.mainSurface()->wlSurface() : nullptr);
      const std::string token = m_wayland.requestActivationToken(source);
      launched = net::openInBrowser(std::string(url), token);
    }
    if (!launched && m_callbacks.onStatus)
      m_callbacks.onStatus("Could not open link in default browser");
    return launched;
  }

  ModalDialogHost::CustomContent HelpDialogController::makeShortcutsCustomContent() {
    const std::weak_ptr<bool> alive = m_alive;
    return ModalDialogHost::CustomContent{
        .buildFields =
            [this, alive](Flex& root, float width) {
              if (const auto locked = alive.lock(); locked && *locked)
                buildShortcutsFields(root, width);
            },
        .bindChoiceButton =
            [this, alive](int result, Button* button) {
              if (const auto locked = alive.lock(); locked && *locked && result == kCloseDialog)
                m_shortcutsClose = button;
            },
        .initialFocus = [this, alive]() -> InputArea* {
          if (const auto locked = alive.lock(); locked && *locked && m_shortcutsClose != nullptr)
            return m_shortcutsClose->inputArea();
          return nullptr;
        },
        .onKeyEvent =
            [this, alive](const KeyboardEvent& event) {
              if (const auto locked = alive.lock(); locked && *locked)
                return handleShortcutsKey(event);
              return false;
            },
        .onDismiss =
            [this, alive] {
              if (const auto locked = alive.lock(); locked && *locked)
                clearState();
            },
        .fillContent = true,
        .preferredWidth = 540,
        .preferredHeight = 560,
        .minWidth = 420,
        .minHeight = 320,
    };
  }

  ModalDialogHost::CustomContent HelpDialogController::makeAboutCustomContent() {
    const std::weak_ptr<bool> alive = m_alive;
    return ModalDialogHost::CustomContent{
        .buildFields =
            [this, alive](Flex& root, float width) {
              if (const auto locked = alive.lock(); locked && *locked)
                buildAboutFields(root, width);
            },
        .bindChoiceButton =
            [this, alive](int result, Button* button) {
              if (const auto locked = alive.lock(); locked && *locked && result == kCloseDialog)
                m_aboutClose = button;
            },
        .initialFocus = [this, alive]() -> InputArea* {
          if (const auto locked = alive.lock(); locked && *locked && m_aboutClose != nullptr)
            return m_aboutClose->inputArea();
          return nullptr;
        },
        .onPrepareLayout =
            [this, alive](Renderer& renderer) {
              if (const auto locked = alive.lock(); locked && *locked)
                prepareAboutLayout(renderer);
            },
        .onDismiss =
            [this, alive] {
              if (const auto locked = alive.lock(); locked && *locked)
                clearState();
            },
        .useMeasuredMinHeight = true,
        .preferredWidth = 500,
        .minWidth = 420,
    };
  }

  void HelpDialogController::buildShortcutsFields(Flex& root, float width) {
    (void)width;
    auto scroll = std::make_unique<ScrollView>();
    scroll->setFlexGrow(1.0F);
    scroll->setScrollbarVisible(true);
    scroll->setReserveScrollbarGutter(true);
    scroll->setViewportPaddingH(0.0F);
    scroll->setViewportPaddingV(2.0F);
    auto* content = scroll->content();
    content->setDirection(FlexDirection::Vertical);
    content->setAlign(FlexAlign::Stretch);
    content->setGap(Style::spaceSm);

    for (const auto& group : shortcutGroups()) {
      auto card = makeDialogCard(m_dialogHost, 4.0F);

      auto heading = std::make_unique<Label>();
      heading->setText(group.title);
      heading->setFontSize(Style::fontSizeCaption);
      heading->setFontWeight(FontWeight::SemiBold);
      heading->setColor(colorSpecFromRole(ColorRole::Primary));
      card->addChild(std::move(heading));

      auto rule = std::make_unique<Separator>();
      rule->setOrientation(SeparatorOrientation::HorizontalRule);
      rule->setColor(colorSpecFromRole(ColorRole::OutlineVariant));
      card->addChild(std::move(rule));

      for (const auto& entry : group.entries) {
        auto row = std::make_unique<Flex>();
        row->setDirection(FlexDirection::Horizontal);
        row->setAlign(FlexAlign::Center);
        row->setGap(Style::spaceMd);
        row->setPadding(2.0F, 0.0F);

        auto action = std::make_unique<Label>();
        action->setText(entry.action);
        action->setFontSize(Style::fontSizeCaption);
        action->setColor(colorSpecFromRole(ColorRole::OnSurface));
        action->setFlexGrow(1.0F);
        action->setMaxLines(1);
        action->setEllipsize(TextEllipsize::End);
        row->addChild(std::move(action));

        auto keycap = std::make_unique<Flex>();
        keycap->setAlign(FlexAlign::Center);
        keycap->setJustify(FlexJustify::Center);
        keycap->setMinWidth(36.0F);
        keycap->setPadding(2.0F, 8.0F);
        keycap->setFill(colorSpecFromRole(ColorRole::SurfaceContainerHigh));
        keycap->setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
        keycap->setRadius(Style::scaledRadiusSm(1.0F));

        auto keyLabel = std::make_unique<Label>();
        keyLabel->setText(entry.shortcut);
        keyLabel->setFontSize(Style::fontSizeMini);
        keyLabel->setFontWeight(FontWeight::SemiBold);
        keyLabel->setColor(colorSpecFromRole(ColorRole::OnSurface));
        keycap->addChild(std::move(keyLabel));

        row->addChild(std::move(keycap));
        card->addChild(std::move(row));
      }

      content->addChild(std::move(card));
    }

    m_shortcutsScroll = scroll.get();
    root.addChild(std::move(scroll));
  }

  bool HelpDialogController::handleShortcutsKey(const KeyboardEvent& event) {
    if (!event.pressed || event.preedit || m_shortcutsScroll == nullptr)
      return false;
    const float pageStep = std::max(Style::scrollWheelStep, m_shortcutsScroll->contentViewportHeight() * 0.85F);
    switch (event.sym) {
    case XKB_KEY_Down:
      m_shortcutsScroll->setScrollOffset(m_shortcutsScroll->scrollOffset() + Style::scrollWheelStep);
      break;
    case XKB_KEY_Up:
      m_shortcutsScroll->setScrollOffset(m_shortcutsScroll->scrollOffset() - Style::scrollWheelStep);
      break;
    case XKB_KEY_Page_Down:
      m_shortcutsScroll->setScrollOffset(m_shortcutsScroll->scrollOffset() + pageStep);
      break;
    case XKB_KEY_Page_Up:
      m_shortcutsScroll->setScrollOffset(m_shortcutsScroll->scrollOffset() - pageStep);
      break;
    case XKB_KEY_Home:
      m_shortcutsScroll->setScrollOffset(0.0F);
      break;
    case XKB_KEY_End:
      m_shortcutsScroll->setScrollOffset(m_shortcutsScroll->maxScrollOffset());
      break;
    default:
      return false;
    }
    m_dialogHost.requestLayout();
    return true;
  }

  void HelpDialogController::buildAboutFields(Flex& root, float width) {
    const AboutInfo info = aboutInfo();

    auto hero = std::make_unique<Flex>();
    hero->setDirection(FlexDirection::Horizontal);
    hero->setAlign(FlexAlign::Center);
    hero->setGap(16.0F);
    hero->setPadding(4.0F, 0.0F);

    auto icon = std::make_unique<Image>();
    icon->setHitTestVisible(false);
    icon->setFit(ImageFit::Contain);
    icon->setFrameSize(84.0F, 63.0F);
    m_aboutIcon = icon.get();
    hero->addChild(std::move(icon));

    auto summary = std::make_unique<Flex>();
    summary->setDirection(FlexDirection::Vertical);
    summary->setAlign(FlexAlign::Start);
    summary->setGap(4.0F);
    summary->setFlexGrow(1.0F);

    auto brand = std::make_unique<Flex>();
    brand->setDirection(FlexDirection::Horizontal);
    brand->setAlign(FlexAlign::Center);
    brand->setGap(8.5F);

    auto brandNythera = std::make_unique<Label>();
    brandNythera->setText("NYTHERA");
    brandNythera->setFontSize(24.0F);
    brandNythera->setDirectDisplayFont(true);
    brandNythera->setColor(colorSpecFromRole(ColorRole::OnSurface));
    brand->addChild(std::move(brandNythera));

    auto brandFiles = std::make_unique<Label>();
    brandFiles->setText("FILES");
    brandFiles->setFontSize(24.0F);
    brandFiles->setDirectDisplayFont(true);
    brandFiles->setColor(colorSpecFromRole(ColorRole::Primary));
    brand->addChild(std::move(brandFiles));
    summary->addChild(std::move(brand));

    auto description = std::make_unique<Label>();
    description->setText(info.description);
    description->setFontSize(Style::fontSizeCaption);
    description->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    description->setMaxWidth(std::max(1.0F, width - 136.0F));
    description->setMaxLines(4);
    summary->addChild(std::move(description));

    hero->addChild(std::move(summary));
    root.addChild(std::move(hero));

    auto card = makeDialogCard(m_dialogHost, 8.0F);
    const float valueMaxWidth = std::max(1.0F, width - 168.0F);

    const auto addRow = [&card](std::string_view key, std::unique_ptr<Node> value) {
      auto row = std::make_unique<Flex>();
      row->setDirection(FlexDirection::Horizontal);
      row->setAlign(FlexAlign::Center);
      row->setGap(12.0F);

      auto keyLabel = std::make_unique<Label>();
      keyLabel->setText(key);
      keyLabel->setMinWidth(84.0F);
      keyLabel->setFontSize(Style::fontSizeCaption);
      keyLabel->setFontWeight(FontWeight::SemiBold);
      keyLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      row->addChild(std::move(keyLabel));

      row->addChild(std::move(value));
      card->addChild(std::move(row));
    };

    const auto addDivider = [&card] {
      auto divider = std::make_unique<Separator>();
      divider->setOrientation(SeparatorOrientation::HorizontalRule);
      divider->setColor(colorSpecFromRole(ColorRole::OutlineVariant));
      card->addChild(std::move(divider));
    };

    auto versionLabel = std::make_unique<Label>();
    versionLabel->setText(info.version);
    versionLabel->setFontSize(Style::fontSizeCaption);
    versionLabel->setColor(colorSpecFromRole(ColorRole::OnSurface));
    versionLabel->setMaxWidth(valueMaxWidth);
    versionLabel->setMaxLines(2);
    addRow("Version", std::move(versionLabel));

    addDivider();

    auto licenceBox = std::make_unique<Flex>();
    licenceBox->setDirection(FlexDirection::Vertical);
    licenceBox->setAlign(FlexAlign::Start);
    licenceBox->setGap(2.0F);
    licenceBox->setFlexGrow(1.0F);

    auto licenceLabel = std::make_unique<Label>();
    licenceLabel->setText(info.licence);
    licenceLabel->setFontSize(Style::fontSizeCaption);
    licenceLabel->setColor(colorSpecFromRole(ColorRole::OnSurface));
    licenceLabel->setMaxWidth(valueMaxWidth);
    licenceBox->addChild(std::move(licenceLabel));

    auto copyrightLabel = std::make_unique<Label>();
    copyrightLabel->setText(info.copyright);
    copyrightLabel->setFontSize(Style::fontSizeMini);
    copyrightLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    copyrightLabel->setMaxWidth(valueMaxWidth);
    licenceBox->addChild(std::move(copyrightLabel));

    addRow("Licence", std::move(licenceBox));

    addDivider();

    auto creditsBox = std::make_unique<Flex>();
    creditsBox->setDirection(FlexDirection::Vertical);
    creditsBox->setAlign(FlexAlign::Start);
    creditsBox->setGap(2.0F);
    creditsBox->setFlexGrow(1.0F);

    auto creditsLabel = std::make_unique<Label>();
    creditsLabel->setText(info.credits);
    creditsLabel->setFontSize(Style::fontSizeCaption);
    creditsLabel->setColor(colorSpecFromRole(ColorRole::OnSurface));
    creditsLabel->setMaxWidth(valueMaxWidth);
    creditsLabel->setMaxLines(3);
    creditsBox->addChild(std::move(creditsLabel));

    auto creditsCopyrightLabel = std::make_unique<Label>();
    creditsCopyrightLabel->setText(info.creditsCopyright);
    creditsCopyrightLabel->setFontSize(Style::fontSizeMini);
    creditsCopyrightLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    creditsCopyrightLabel->setMaxWidth(valueMaxWidth);
    creditsBox->addChild(std::move(creditsCopyrightLabel));

    addRow("Credits", std::move(creditsBox));

    addDivider();

    auto repoButton = std::make_unique<Button>();
    repoButton->setText(info.repositoryUrl);
    repoButton->setGlyph("brand-github");
    repoButton->setFontSize(Style::fontSizeCaption);
    repoButton->setGlyphSize(14.0F);
    repoButton->setVariant(ButtonVariant::Ghost);
    repoButton->setContentAlign(ButtonContentAlign::Start);
    repoButton->setControlHeight(28.0F);
    repoButton->setPadding(3.0F, 6.0F);
    repoButton->setMaxWidth(valueMaxWidth);
    repoButton->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
    repoButton->setTooltip("Open repository in default browser");
    auto repoPalette = Button::defaultPalette(ButtonVariant::Ghost);
    repoPalette.normal.label = colorSpecFromRole(ColorRole::Primary);
    repoPalette.hover.bg = colorSpecFromRole(ColorRole::Hover);
    repoPalette.hover.label = colorSpecFromRole(ColorRole::OnHover);
    repoPalette.pressed.bg = colorSpecFromRole(ColorRole::SurfaceContainerHigh);
    repoPalette.pressed.label = colorSpecFromRole(ColorRole::Primary);
    repoButton->setCustomPalette(std::move(repoPalette));
    const std::weak_ptr<bool> alive = m_alive;
    repoButton->setOnClick([this, alive] {
      if (const auto locked = alive.lock(); locked && *locked)
        (void)openRepository();
    });
    m_repositoryButton = repoButton.get();
    addRow("Repository", std::move(repoButton));

    root.addChild(std::move(card));
  }

  void HelpDialogController::prepareAboutLayout(Renderer& renderer) {
    if (m_aboutIcon != nullptr)
      (void)m_aboutIcon->setSourceFile(renderer, aboutInfo().iconPath.string(), 96, true);
  }

} // namespace filemanager
