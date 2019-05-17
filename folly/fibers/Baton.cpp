/*
 * Copyright 2017 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "Baton.h"

#include <chrono>

#include <folly/detail/MemoryIdler.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <folly/portability/Asm.h>

namespace folly {
namespace fibers {

void Baton::wait() {
  wait([]() {});
}

#if 2
void Baton::lwait(folly::Function<void(int)> f) {
  lwait(std::move(f), []() {});
}
#endif

void Baton::wait(TimeoutHandler& timeoutHandler) {
  auto timeoutFunc = [this, &timeoutHandler] {
    if (!try_wait()) {
      postHelper(TIMEOUT);
    }
    timeoutHandler.timeoutPtr_ = 0;
  };
  timeoutHandler.timeoutFunc_ = std::ref(timeoutFunc);
  timeoutHandler.fiberManager_ = FiberManager::getFiberManagerUnsafe();
  wait();
  timeoutHandler.cancelTimeout();
}

bool Baton::timed_wait(TimeoutController::Duration timeout) {
  return timed_wait(timeout, []() {});
}

#if 2
bool Baton::timed_lwait(folly::Function<void(int)> f, TimeoutController::Duration timeout) {
  return timed_lwait(std::move(f), timeout, []() {});
}
#endif

void Baton::waitThread() {
  if (spinWaitForEarlyPost()) {
    assert(waitingFiber_.load(std::memory_order_acquire) == POSTED);
    return;
  }

  auto fiber = waitingFiber_.load();

  if (LIKELY(
          fiber == NO_WAITER &&
          waitingFiber_.compare_exchange_strong(fiber, THREAD_WAITING))) {
    do {
      folly::detail::MemoryIdler::futexWait(
          futex_.futex, uint32_t(THREAD_WAITING));
      fiber = waitingFiber_.load(std::memory_order_acquire);
    } while (fiber == THREAD_WAITING);
  }

  if (LIKELY(fiber == POSTED)) {
    return;
  }

  // Handle errors
  if (fiber == TIMEOUT) {
    throw std::logic_error("Thread baton can't have timeout status");
  }
  if (fiber == THREAD_WAITING) {
    throw std::logic_error("Other thread is already waiting on this baton");
  }
  throw std::logic_error("Other fiber is already waiting on this baton");
}

#if 2
void Baton::lwaitThread(folly::Function<void(int)> f) {
    f(0);
    waitThread();
    f(1);
}
#endif

bool Baton::spinWaitForEarlyPost() {
  static_assert(
      PreBlockAttempts > 0,
      "isn't this assert clearer than an uninitialized variable warning?");
  for (int i = 0; i < PreBlockAttempts; ++i) {
    if (try_wait()) {
      // hooray!
      return true;
    }
    // The pause instruction is the polite way to spin, but it doesn't
    // actually affect correctness to omit it if we don't have it.
    // Pausing donates the full capabilities of the current core to
    // its other hyperthreads for a dozen cycles or so
    asm_volatile_pause();
  }

  return false;
}

bool Baton::timedWaitThread(TimeoutController::Duration timeout) {
  if (spinWaitForEarlyPost()) {
    assert(waitingFiber_.load(std::memory_order_acquire) == POSTED);
    return true;
  }

  auto fiber = waitingFiber_.load();

  if (LIKELY(
          fiber == NO_WAITER &&
          waitingFiber_.compare_exchange_strong(fiber, THREAD_WAITING))) {
    auto deadline = TimeoutController::Clock::now() + timeout;
    do {
      const auto wait_rv =
          futex_.futex.futexWaitUntil(uint32_t(THREAD_WAITING), deadline);
      if (wait_rv == folly::detail::FutexResult::TIMEDOUT) {
        return false;
      }
      fiber = waitingFiber_.load(std::memory_order_relaxed);
    } while (fiber == THREAD_WAITING);
  }

  if (LIKELY(fiber == POSTED)) {
    return true;
  }

  // Handle errors
  if (fiber == TIMEOUT) {
    throw std::logic_error("Thread baton can't have timeout status");
  }
  if (fiber == THREAD_WAITING) {
    throw std::logic_error("Other thread is already waiting on this baton");
  }
  throw std::logic_error("Other fiber is already waiting on this baton");
}

#if 2
bool Baton::timedLWaitThread(folly::Function<void(int)> f, TimeoutController::Duration timeout) {
    f(0);
    bool res = timedWaitThread(timeout);
    f(1);
    return res;
}
#endif

void Baton::post() {
  LOG(WARNING) << "Baton::post() called";
  postHelper(POSTED);
}

void Baton::postHelper(intptr_t new_value) {
  auto fiber = waitingFiber_.load();

  LOG(WARNING) << "postHelper: waitingFiber_ is:" << fiber;
  do {
    if (fiber == THREAD_WAITING) {
      LOG(WARNING) << "waitingFiber_ is THREAD_WAITING";
      assert(new_value == POSTED);

      return postThread();
    }

    if (fiber == POSTED || fiber == TIMEOUT) {
      LOG(WARNING) << "waitingFiber_ is POSTED / TIMEOUT";
      return;
    }
  } while (!waitingFiber_.compare_exchange_weak(fiber, new_value));

  if (fiber != NO_WAITER) {
	LOG(WARNING) << "waitingFiber_ is not NO_WAITER, resuming fiber";
    reinterpret_cast<Fiber*>(fiber)->resume();
  }
}

bool Baton::try_wait() {
  auto state = waitingFiber_.load();
  return state == POSTED;
}

void Baton::postThread() {
  auto expected = THREAD_WAITING;

  if (!waitingFiber_.compare_exchange_strong(expected, POSTED)) {
    return;
  }

  futex_.futex.futexWake(1);
}

void Baton::reset() {
  waitingFiber_.store(NO_WAITER, std::memory_order_relaxed);
  ;
}

void Baton::TimeoutHandler::scheduleTimeout(
    TimeoutController::Duration timeout) {
  assert(fiberManager_ != nullptr);
  assert(timeoutFunc_ != nullptr);
  assert(timeoutPtr_ == 0);

  if (timeout.count() > 0) {
    timeoutPtr_ =
        fiberManager_->timeoutManager_->registerTimeout(timeoutFunc_, timeout);
  }
}

void Baton::TimeoutHandler::cancelTimeout() {
  if (timeoutPtr_) {
    fiberManager_->timeoutManager_->cancel(timeoutPtr_);
  }
}
} // namespace fibers
} // namespace folly
