#pragma once

#include "ui/text_input_client.h"

#include <cstdint>
#include <string>

struct wl_seat;
struct wl_surface;
struct zwp_text_input_manager_v3;
struct zwp_text_input_v3;

class TextInputService {
public:
  TextInputService();
  ~TextInputService();

  TextInputService(const TextInputService&) = delete;
  TextInputService& operator=(const TextInputService&) = delete;

  bool bind(zwp_text_input_manager_v3* manager, wl_seat* seat);
  void cleanup();

  [[nodiscard]] bool isAvailable() const noexcept;

  void setFocusedClient(
      wl_surface* surface, TextInputClient* client, bool acceptKeyboardFocusActivation = false,
      wl_surface* keyboardFocusParentSurface = nullptr
  );
  void clearFocusedClient(TextInputClient* client);
  void notifyClientStateChanged(TextInputClient* client, TextInputChangeCause cause);
  void onKeyboardFocusSurface(wl_surface* surface, bool entered);

  void handleEnter(wl_surface* surface);
  void handleLeave(wl_surface* surface);
  void handlePreeditString(const char* text, std::int32_t cursorBegin, std::int32_t cursorEnd);
  void handleCommitString(const char* text);
  void handleDeleteSurroundingText(std::uint32_t beforeLength, std::uint32_t afterLength);
  void handleDone(std::uint32_t serial);
  void handleAction(std::uint32_t action);

private:
  // Protocol state last committed during the current text-input activation.
  struct StateSignature {
    bool sendSurrounding = false;
    std::string surroundingText;
    std::int32_t cursor = 0;
    std::int32_t anchor = 0;
    std::uint32_t contentHint = 0;
    std::uint32_t contentPurpose = 0;
    std::int32_t rectX = 0;
    std::int32_t rectY = 0;
    std::int32_t rectWidth = 1;
    std::int32_t rectHeight = 1;

    [[nodiscard]] bool operator==(const StateSignature& other) const noexcept = default;
  };

  [[nodiscard]] bool activeSurfaceAcceptsTextInput() const noexcept;
  void enableActive(TextInputChangeCause cause);
  void disableActive();
  void commitActiveState(TextInputChangeCause cause);
  void commitProtocolState();
  void deactivateClient(TextInputClient* client);
  void resetCommitSignature();

  zwp_text_input_manager_v3* m_manager = nullptr;
  wl_seat* m_seat = nullptr;
  zwp_text_input_v3* m_textInput = nullptr;
  wl_surface* m_enteredSurface = nullptr;
  wl_surface* m_keyboardFocusSurface = nullptr;
  wl_surface* m_keyboardFocusParentSurface = nullptr;
  wl_surface* m_activeSurface = nullptr;
  TextInputClient* m_activeClient = nullptr;
  bool m_activeAcceptsKeyboardFocusActivation = false;
  TextInputEdit m_pendingEdit;
  std::uint32_t m_commitSerial = 0;
  bool m_lastCommitValid = false;
  StateSignature m_lastCommitSignature;
  std::uint32_t m_textInputVersion = 0;
  bool m_enabled = false;
};
