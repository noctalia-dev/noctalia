#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// A minimal signal/slot primitive for reactive UI updates.
//
// emit() dispatches over a snapshot of the slots connected when it starts, so
// callbacks may connect or disconnect freely. A slot disconnected during the
// emit (including one whose subscriber a previous callback destroyed) is not
// called; a slot connected during the emit waits for the next one.
//
// ScopedConnection is move-only RAII: it disconnects on destruction. Store
// one as a member of whatever object subscribes, and the subscription dies
// with the object.

template <class... Args> class Signal {
public:
  using Callback = std::function<void(Args...)>;

private:
  struct Slot {
    std::uint64_t id;
    // Shared so the snapshot is cheap and a callback stays alive while it
    // disconnects itself.
    std::shared_ptr<const Callback> callback;
  };
  struct State {
    // Sorted by id: ids only increase and disconnect preserves order.
    std::vector<Slot> slots;
    std::uint64_t nextId = 0;
  };

  static bool isConnected(const State& state, std::uint64_t id) {
    return std::ranges::binary_search(state.slots, id, {}, &Slot::id);
  }

public:
  class ScopedConnection {
  public:
    ScopedConnection() = default;
    ScopedConnection(std::weak_ptr<State> state, std::uint64_t id) : m_state(std::move(state)), m_id(id) {}
    ~ScopedConnection() { disconnect(); }

    ScopedConnection(const ScopedConnection&) = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;

    ScopedConnection(ScopedConnection&& other) noexcept : m_state(std::move(other.m_state)), m_id(other.m_id) {
      other.m_id = 0;
    }
    ScopedConnection& operator=(ScopedConnection&& other) noexcept {
      if (this != &other) {
        disconnect();
        m_state = std::move(other.m_state);
        m_id = other.m_id;
        other.m_id = 0;
      }
      return *this;
    }

    void disconnect() {
      if (m_id == 0) {
        return;
      }
      if (auto state = m_state.lock()) {
        auto& slots = state->slots;
        slots.erase(
            std::remove_if(slots.begin(), slots.end(), [id = m_id](const Slot& slot) { return slot.id == id; }),
            slots.end()
        );
      }
      m_id = 0;
    }

  private:
    std::weak_ptr<State> m_state;
    std::uint64_t m_id = 0;
  };

  Signal() : m_state(std::make_shared<State>()) {}
  // An emit still in progress holds the state; clearing it stops dispatch to the remaining slots.
  ~Signal() { m_state->slots.clear(); }

  Signal(const Signal&) = delete;
  Signal& operator=(const Signal&) = delete;
  Signal(Signal&&) = delete;
  Signal& operator=(Signal&&) = delete;

  [[nodiscard]] ScopedConnection connect(Callback callback) {
    const auto id = ++m_state->nextId;
    m_state->slots.push_back({id, std::make_shared<const Callback>(std::move(callback))});
    return ScopedConnection{std::weak_ptr<State>(m_state), id};
  }

  void emit(Args... args) {
    // Hold the state so a callback that destroys this Signal cannot free it mid-dispatch.
    const std::shared_ptr<State> state = m_state;
    const std::vector<Slot> snapshot = state->slots;
    for (const auto& slot : snapshot) {
      if (*slot.callback && isConnected(*state, slot.id)) {
        (*slot.callback)(args...);
      }
    }
  }

private:
  std::shared_ptr<State> m_state;
};
