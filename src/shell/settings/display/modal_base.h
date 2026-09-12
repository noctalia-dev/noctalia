#pragma once

#include "shell/settings/settings_modal_host.h"

#include <memory>
#include <optional>

class Flex;
class Renderer;

namespace settings {

  // Shared push/pop plumbing for stacked settings modals; subclasses implement
  // the content hooks and own their state/request structs.
  class ModalBase {
  public:
    virtual ~ModalBase();

    ModalBase(const ModalBase&) = delete;
    ModalBase& operator=(const ModalBase&) = delete;

    [[nodiscard]] bool isOpen() const noexcept { return m_open; }
    void close();
    void requestLayout();
    void rebuild();

  protected:
    ModalBase() = default;

    void pushModal(SettingsModalHost& host, float contentPadding, float windowMargin);
    [[nodiscard]] bool isTop() const;
    [[nodiscard]] std::weak_ptr<void> aliveToken() const { return m_aliveGuard; }
    [[nodiscard]] LayoutSize
    measureFixedWidth(Renderer& renderer, const SettingsModalLayoutSpace& space, float width) const;
    void arrangeRoot(Renderer& renderer, float width, float height);
    void resetRoot() { m_root = nullptr; }

    [[nodiscard]] virtual std::unique_ptr<Node> buildContent() = 0;
    [[nodiscard]] virtual LayoutSize measureContent(Renderer& renderer, const SettingsModalLayoutSpace& space) = 0;
    virtual void arrangeContent(Renderer& renderer, float width, float height) = 0;
    virtual void updateContent(Renderer& renderer);
    virtual void onClosed();

    SettingsModalHost* m_host = nullptr;
    std::optional<SettingsModalHost::ModalId> m_modalId;
    bool m_open = false;
    Flex* m_root = nullptr;

  private:
    std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
  };

} // namespace settings
