// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "flutter/shell/platform/windows/keyboard_key_handler.h"

#include <rapidjson/document.h>
#include <memory>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {

namespace {

static constexpr int kHandledScanCode = 20;
static constexpr int kHandledScanCode2 = 22;
static constexpr int kUnhandledScanCode = 21;

constexpr uint64_t kScanCodeShiftRight = 0x36;
constexpr uint64_t kScanCodeControl = 0x1D;
constexpr uint64_t kScanCodeAltLeft = 0x38;

typedef std::function<void(bool)> Callback;
typedef std::function<void(Callback&)> CallbackHandler;
void dont_respond(Callback& callback) {}
void respond_true(Callback& callback) {
  callback(true);
}
void respond_false(Callback& callback) {
  callback(false);
}

// A testing |KeyHandlerDelegate| that records all calls
// to |KeyboardHook| and can be customized with whether
// the hook is handled in async.
class MockKeyHandlerDelegate
    : public KeyboardKeyHandler::KeyboardKeyHandlerDelegate {
 public:
  class KeyboardHookCall {
   public:
    int delegate_id;
    int key;
    int scancode;
    int action;
    char32_t character;
    bool extended;
    bool was_down;
    std::function<void(bool)> callback;
  };

  // Create a |MockKeyHandlerDelegate|.
  //
  // The |delegate_id| is an arbitrary ID to tell between delegates
  // that will be recorded in |KeyboardHookCall|.
  //
  // The |hook_history| will store every call to |KeyboardHookCall| that are
  // handled asynchronously.
  //
  // The |is_async| is a function that the class calls upon every
  // |KeyboardHookCall| to decide whether the call is handled asynchronously.
  // Defaults to always returning true (async).
  MockKeyHandlerDelegate(int delegate_id,
                         std::list<KeyboardHookCall>* hook_history)
      : delegate_id(delegate_id),
        hook_history(hook_history),
        callback_handler(dont_respond) {}
  virtual ~MockKeyHandlerDelegate() = default;

  virtual void KeyboardHook(int key,
                            int scancode,
                            int action,
                            char32_t character,
                            bool extended,
                            bool was_down,
                            std::function<void(bool)> callback) {
    hook_history->push_back(KeyboardHookCall{
        .delegate_id = delegate_id,
        .key = key,
        .scancode = scancode,
        .character = character,
        .extended = extended,
        .was_down = was_down,
        .callback = std::move(callback),
    });
    callback_handler(hook_history->back().callback);
  }

  CallbackHandler callback_handler;
  int delegate_id;
  std::list<KeyboardHookCall>* hook_history;
};

class TestKeyboardKeyHandler : public KeyboardKeyHandler {
 public:
  explicit TestKeyboardKeyHandler(EventDispatcher redispatch_event)
      : KeyboardKeyHandler(redispatch_event) {}

  bool HasRedispatched() { return RedispatchedCount() > 0; }
};

}  // namespace

TEST(KeyboardKeyHandlerTest, SingleDelegateWithAsyncResponds) {
  std::list<MockKeyHandlerDelegate::KeyboardHookCall> hook_history;

  // Capture the scancode of the last redispatched event
  int redispatch_scancode = 0;
  bool delegate_handled = false;
  TestKeyboardKeyHandler handler([&redispatch_scancode](UINT cInputs,
                                                        LPINPUT pInputs,
                                                        int cbSize) -> UINT {
    EXPECT_TRUE(cbSize > 0);
    redispatch_scancode = pInputs->ki.wScan;
    return 1;
  });
  // Add one delegate
  auto delegate = std::make_unique<MockKeyHandlerDelegate>(1, &hook_history);
  handler.AddDelegate(std::move(delegate));

  /// Test 1: One event that is handled by the framework

  // Dispatch a key event
  delegate_handled =
      handler.KeyboardHook(64, kHandledScanCode, WM_KEYDOWN, L'a', false, true);
  EXPECT_EQ(delegate_handled, true);
  EXPECT_EQ(redispatch_scancode, 0);
  EXPECT_EQ(hook_history.size(), 1);
  EXPECT_EQ(hook_history.back().delegate_id, 1);
  EXPECT_EQ(hook_history.back().scancode, kHandledScanCode);
  EXPECT_EQ(hook_history.back().was_down, true);

  EXPECT_EQ(handler.HasRedispatched(), false);
  hook_history.back().callback(true);
  EXPECT_EQ(redispatch_scancode, 0);

  EXPECT_EQ(handler.HasRedispatched(), false);
  hook_history.clear();

  /// Test 2: Two events that are unhandled by the framework

  delegate_handled = handler.KeyboardHook(64, kHandledScanCode, WM_KEYDOWN,
                                          L'a', false, false);
  EXPECT_EQ(delegate_handled, true);
  EXPECT_EQ(redispatch_scancode, 0);
  EXPECT_EQ(hook_history.size(), 1);
  EXPECT_EQ(hook_history.back().delegate_id, 1);
  EXPECT_EQ(hook_history.back().scancode, kHandledScanCode);
  EXPECT_EQ(hook_history.back().was_down, false);

  // Dispatch another key event
  delegate_handled =
      handler.KeyboardHook(65, kHandledScanCode2, WM_KEYUP, L'b', false, true);
  EXPECT_EQ(delegate_handled, true);
  EXPECT_EQ(redispatch_scancode, 0);
  EXPECT_EQ(hook_history.size(), 2);
  EXPECT_EQ(hook_history.back().delegate_id, 1);
  EXPECT_EQ(hook_history.back().scancode, kHandledScanCode2);
  EXPECT_EQ(hook_history.back().was_down, true);

  // Resolve the second event first to test out-of-order response
  hook_history.back().callback(false);
  EXPECT_EQ(redispatch_scancode, kHandledScanCode2);

  // Resolve the first event then
  hook_history.front().callback(false);
  EXPECT_EQ(redispatch_scancode, kHandledScanCode);

  EXPECT_EQ(handler.KeyboardHook(64, kHandledScanCode, WM_KEYDOWN, L'a', false,
                                 false),
            false);
  EXPECT_EQ(
      handler.KeyboardHook(65, kHandledScanCode2, WM_KEYUP, L'b', false, false),
      false);

  EXPECT_EQ(handler.HasRedispatched(), false);
  hook_history.clear();
  redispatch_scancode = 0;
}

TEST(KeyboardKeyHandlerTest, SingleDelegateWithSyncResponds) {
  std::list<MockKeyHandlerDelegate::KeyboardHookCall> hook_history;

  // Capture the scancode of the last redispatched event
  int redispatch_scancode = 0;
  bool delegate_handled = false;
  TestKeyboardKeyHandler handler([&redispatch_scancode](UINT cInputs,
                                                        LPINPUT pInputs,
                                                        int cbSize) -> UINT {
    EXPECT_TRUE(cbSize > 0);
    redispatch_scancode = pInputs->ki.wScan;
    return 1;
  });
  // Add one delegate
  auto delegate = std::make_unique<MockKeyHandlerDelegate>(1, &hook_history);
  CallbackHandler& delegate_handler = delegate->callback_handler;
  handler.AddDelegate(std::move(delegate));

  /// Test 1: One event that is handled by the framework

  // Dispatch a key event
  delegate_handler = respond_true;
  delegate_handled = handler.KeyboardHook(64, kHandledScanCode, WM_KEYDOWN,
                                          L'a', false, false);
  EXPECT_EQ(delegate_handled, true);
  EXPECT_EQ(redispatch_scancode, 0);
  EXPECT_EQ(hook_history.size(), 1);
  EXPECT_EQ(hook_history.back().delegate_id, 1);
  EXPECT_EQ(hook_history.back().scancode, kHandledScanCode);
  EXPECT_EQ(hook_history.back().was_down, false);

  EXPECT_EQ(handler.HasRedispatched(), false);
  hook_history.clear();

  /// Test 2: An event unhandled by the framework

  delegate_handler = respond_false;
  delegate_handled = handler.KeyboardHook(64, kHandledScanCode, WM_KEYDOWN,
                                          L'a', false, false);
  EXPECT_EQ(delegate_handled, true);
  EXPECT_EQ(redispatch_scancode, kHandledScanCode);
  EXPECT_EQ(hook_history.size(), 1);
  EXPECT_EQ(hook_history.back().delegate_id, 1);
  EXPECT_EQ(hook_history.back().scancode, kHandledScanCode);
  EXPECT_EQ(hook_history.back().was_down, false);

  EXPECT_EQ(handler.HasRedispatched(), true);

  // Resolve the event
  EXPECT_EQ(handler.KeyboardHook(64, kHandledScanCode, WM_KEYDOWN, L'a', false,
                                 false),
            false);

  EXPECT_EQ(handler.HasRedispatched(), false);
  hook_history.clear();
  redispatch_scancode = 0;
}

}  // namespace testing
}  // namespace flutter
