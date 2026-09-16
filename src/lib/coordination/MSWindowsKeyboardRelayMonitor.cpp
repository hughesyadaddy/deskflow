/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/KeyboardRelayMonitor.h"

#include "coordination/KeyboardRelayHookPolicy.h"
#include "coordination/KeyboardRelayMap.h"

#include "base/Log.h"

#include <windows.h>

#include <atomic>
#include <thread>

namespace deskflow::coordination {

namespace {

class MSWindowsKeyboardRelayMonitor;

MSWindowsKeyboardRelayMonitor *g_relayInstance = nullptr;

class MSWindowsKeyboardRelayMonitor : public IKeyboardRelayMonitor
{
public:
  ~MSWindowsKeyboardRelayMonitor() override
  {
    stop();
  }

  bool start(RelayPassThroughQuery passThrough, KeyForwardSend send) override
  {
    if (m_thread.joinable()) {
      if (m_active) {
        return true;
      }
      // Thread finished without a live hook: reap and retry fresh.
      stop();
    }
    m_passThrough = std::move(passThrough);
    m_send = std::move(send);
    // The hook missed whatever was released while it was down; start from
    // the OS toggle and no held modifiers (the OS view is ORed in per key).
    m_shadow.reset();
    m_shadow.seedCapsLock((GetKeyState(VK_CAPITAL) & 1) != 0);
    m_running = true;
    m_thread = std::thread([this] { runLoop(); });
    return true;
  }

  void stop() override
  {
    m_running = false;
    if (m_threadId != 0) {
      PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
    }
    if (m_thread.joinable()) {
      m_thread.join();
    }
    m_threadId = 0;
    flushLedger();
  }

  bool running() const override
  {
    return m_active;
  }

  void setForwardedReleaseSink(ForwardedReleaseSink sink) override
  {
    m_releaseSink = std::move(sink);
  }

  void releaseForwardedLocally() override
  {
    m_ledgerResyncPending = true;
  }

private:
  //! Boundary (hook thread joined): report every forwarded hold so the
  //! coordinator posts its Up, then forget everything. The ledger outlives
  //! epoch flips (the monitor object does), so without this a key held
  //! across a stop stayed pressed on the peer with nobody left to release it.
  void flushLedger()
  {
    const auto held = m_ledger.forwardedButtons();
    if (!held.empty() && m_releaseSink) {
      std::vector<KeyButton> buttons;
      buttons.reserve(held.size());
      for (const int button : held) {
        buttons.push_back(static_cast<KeyButton>(button));
      }
      LOG_DEBUG("coordination: relay stop releasing %zu forwarded key(s) on the peer", buttons.size());
      m_releaseSink(buttons);
    }
    m_ledger.clear();
    m_ledgerResyncPending = false;
  }

  //! Hook thread: apply a pending cross-thread resync before touching the ledger.
  void applyPendingResync()
  {
    if (m_ledgerResyncPending.exchange(false)) {
      m_ledger.releaseAllForwardedLocally();
    }
  }

  KeyForwardResult send(Message::KeyPhase phase, KeyID id, KeyModifierMask mask, KeyButton button)
  {
    return m_send ? m_send(phase, id, mask, button, {}) : KeyForwardResult::Local;
  }

  static LRESULT CALLBACK hookProc(int code, WPARAM wParam, LPARAM lParam)
  {
    if (code < 0) {
      return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    auto *self = g_relayInstance;
    if (self == nullptr) {
      return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const auto *info = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
    const bool keyUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
    const bool isRepeat = !keyUp && (info->flags & LLKHF_UP) == 0 && (GetAsyncKeyState(info->vkCode) & 0x8000);
    const auto vk = static_cast<int>(info->vkCode);
    const bool capsPress = vk == VK_CAPITAL && !keyUp && !isRepeat;

    const bool isInjected = (info->flags & LLKHF_INJECTED) != 0;
    if (isInjected) {
      // The OS applies injected presses, so an injected CapsLock toggles the
      // real state: keep the shadow in step. Held injected modifiers are
      // visible through GetAsyncKeyState and need no shadow.
      if (capsPress) {
        self->m_shadow.note(vk, true, false);
      }
      return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    self->applyPendingResync();

    // Modifier state for THIS key: the hook's own shadow (which knows about
    // swallowed modifier Downs the OS never saw) ORed with the OS view. A
    // CapsLock press flips the shadow first so its own mask (and the glyph
    // translation of every key after it) carries the post-toggle state.
    if (capsPress) {
      self->m_shadow.note(vk, true, false);
    }
    const KeyModifierMask modifiers = self->m_shadow.mask() | relayOsHeldModifiers();
    if (!capsPress) {
      self->m_shadow.note(vk, !keyUp, isRepeat);
    }

    Message::KeyPhase phase = Message::KeyPhase::Down;
    KeyID id = 0;
    KeyModifierMask mask = 0;
    KeyButton button = 0;
    const bool mapped = mapRelayKeyFromHook(
        vk, static_cast<int>(info->scanCode), (info->flags & LLKHF_EXTENDED) != 0, keyUp, isRepeat, modifiers, id,
        mask, button, phase
    );

    // Repeat/Up of a held key follow the DOWN's destination (ledger), not the
    // cursor's current screen: a mid-hold screen switch must not strand the
    // key held on one side with its release delivered to the other.
    if (keyUp) {
      const auto destination = self->m_ledger.destination(button);
      if (destination == KeyboardRelayForwardLedger::Destination::Local) {
        // Its Down was delivered to the local OS: the Up MUST be too, or the
        // physical key never releases here (stuck Shift = everything typed
        // afterwards is uppercase, password boxes included).
        self->m_ledger.release(button);
        return CallNextHookEx(nullptr, code, wParam, lParam);
      }
      if (destination == KeyboardRelayForwardLedger::Destination::Unknown) {
        // Genuinely unseen Down (ledger lost to a monitor restart mid-hold).
        // If the cursor is remote, forward-and-swallow so a remote target
        // never keeps the key held; otherwise let it pass locally.
        const bool passLocalNow = self->m_passThrough ? self->m_passThrough() : true;
        if (!passLocalNow && mapped &&
            self->send(Message::KeyPhase::Up, id, mask, button) != KeyForwardResult::Local) {
          return 1;
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
      }
      // Forwarded: the Up follows its Down to the peer, whatever the cursor
      // does now (consulting passLocal first here is the stuck-modifier bug).
      self->m_ledger.release(button);
      const bool forwarded = self->send(Message::KeyPhase::Up, id, mask, button) != KeyForwardResult::Local;
      return forwarded ? 1 : CallNextHookEx(nullptr, code, wParam, lParam);
    }
    if (isRepeat) {
      if (!self->m_ledger.follow(button)) {
        return CallNextHookEx(nullptr, code, wParam, lParam); // Down was local/unseen
      }
      const bool forwarded = self->send(Message::KeyPhase::Repeat, id, mask, button) != KeyForwardResult::Local;
      return forwarded ? 1 : CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // Fresh Down: destination is decided by where the cursor is NOW.
    const bool passLocal = self->m_passThrough ? self->m_passThrough() : true;
    // When keys stay local, still deliver Downs to sendKeyForward so 5x Esc
    // restart can observe taps (Swallowed = eat this key).
    if (passLocal) {
      self->m_ledger.downLocal(button);
      if (mapped && self->send(phase, id, mask, button) == KeyForwardResult::Swallowed) {
        return 1;
      }
      return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    KeyForwardResult result = KeyForwardResult::Local;
    if (mapped) {
      result = self->send(phase, id, mask, button);
    }
    switch (result) {
    case KeyForwardResult::Forwarded:
      self->m_ledger.downForwarded(button);
      break;
    case KeyForwardResult::Swallowed:
      // Consumed by the rescue gesture: neither OS saw it, so its Up must
      // not chase a hold on the peer. Local = the Up passes through here as
      // a harmless no-op.
      self->m_ledger.downLocal(button);
      return 1;
    default:
      self->m_ledger.downLocal(button);
      break;
    }

    const KeyboardRelayHookContext ctx{passLocal, isInjected, mapped, result == KeyForwardResult::Forwarded};
    if (keyboardRelayHookShouldPassThrough(ctx)) {
      return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    return 1;
  }

  void runLoop()
  {
    m_threadId = GetCurrentThreadId();
    g_relayInstance = this;

    m_hook = SetWindowsHookExW(WH_KEYBOARD_LL, hookProc, GetModuleHandleW(nullptr), 0);
    if (m_hook == nullptr) {
      LOG_WARN("coordination: keyboard relay hook unavailable");
      g_relayInstance = nullptr;
      return;
    }
    m_active = true;
    LOG_DEBUG("coordination: keyboard relay monitor started");

    MSG message;
    while (m_running && GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }

    m_active = false;
    UnhookWindowsHookEx(m_hook);
    m_hook = nullptr;
    g_relayInstance = nullptr;
    LOG_DEBUG("coordination: keyboard relay monitor stopped");
  }

  RelayPassThroughQuery m_passThrough;
  KeyForwardSend m_send;
  ForwardedReleaseSink m_releaseSink;
  KeyboardRelayForwardLedger m_ledger;            //!< hook thread only (stop() after join)
  KeyboardRelayModifierShadow m_shadow;           //!< hook thread only
  std::atomic<bool> m_ledgerResyncPending{false}; //!< releaseForwardedLocally() -> hook thread
  std::thread m_thread;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_active{false}; //!< hook installed and pumping
  DWORD m_threadId = 0;
  HHOOK m_hook = nullptr;
};

} // namespace

std::unique_ptr<IKeyboardRelayMonitor> createKeyboardRelayMonitor()
{
  return std::make_unique<MSWindowsKeyboardRelayMonitor>();
}

} // namespace deskflow::coordination
