#pragma once

#include "render/scene/node.h"

#include <cstddef>
#include <functional>
#include <memory>

class InputArea;
class InputDispatcher;
class Renderer;
struct KeyboardEvent;

namespace settings {

  struct SettingsModalLayoutSpace {
    float windowWidth = 0.0F;
    float maxContentWidth = 0.0F;
    float maxContentHeight = 0.0F;
  };

  struct SettingsModalRequest {
    std::function<std::unique_ptr<Node>()> build;
    std::function<LayoutSize(Renderer&, const SettingsModalLayoutSpace&)> measure;
    std::function<void(Renderer&, float width, float height)> arrange;
    std::function<void(Renderer&)> update;
    std::function<InputArea*()> initialFocusArea;
    std::function<bool(const KeyboardEvent&)> preDispatchKeyboard;
    // The callback owns close policy and calls pop() when the modal should close.
    std::function<void()> requestClose;
    std::function<void()> onClosed;
    float contentPadding = 12.0F;
    float windowMargin = 24.0F;
  };

  // Owns an in-scene stack of modal dialogs above the Settings content. The stack uses the
  // Settings window's InputDispatcher, so it remains part of the toplevel when focus changes.
  class SettingsModalHost {
  public:
    SettingsModalHost();
    ~SettingsModalHost();

    SettingsModalHost(const SettingsModalHost&) = delete;
    SettingsModalHost& operator=(const SettingsModalHost&) = delete;

    void initialize(
        InputDispatcher& inputDispatcher, std::function<void()> requestLayout, std::function<void()> requestUpdateOnly
    );
    void attach(Node& sceneRoot, Node* settingsContent, Renderer& renderer, float width, float height);
    void detach();
    void resize(float width, float height);

    [[nodiscard]] bool push(SettingsModalRequest request);
    void rebuildTop();
    void pop();
    void closeAll();
    void requestCloseTop();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] Node* topRoot() const noexcept;
    [[nodiscard]] InputArea* focusedArea() const noexcept;
    void requestLayout();
    void requestUpdateOnly();
    [[nodiscard]] bool onKeyboardEvent(const KeyboardEvent& event);
    void update(Renderer& renderer);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace settings
