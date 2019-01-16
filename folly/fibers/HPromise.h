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
#pragma once

#include <folly/Try.h>
#include <folly/fibers/traits.h>
#include <folly/MicroSpinLock.h>

namespace folly {
namespace fibers {

class Baton;

template <typename T>
class HPromise {
 public:
  HPromise();
  ~HPromise();

  // not copyable
  HPromise(const HPromise&) = delete;
  HPromise& operator=(const HPromise&) = delete;

  // movable
  HPromise(HPromise&&) noexcept;
  HPromise& operator=(HPromise&&);

  /** Fulfill this promise (only for HPromise<void>) */
  void setValue();

  /** Set the value (use perfect forwarding for both move and copy) */
  template <class M>
  void setValue(M&& value);

  /** Get the value */
  T getValue();

  /** Get Baton     */
  folly::fibers::Baton *getBaton();

  /** Fulfill the promise with a given try */
  void setTry(folly::Try<T>&& t);

  /** Fulfill the HPromise with an exception_wrapper */
  void setException(folly::exception_wrapper);

  template <typename F>
  void setCallback(F&& func) {
    lock_.lock();
    callback_ = std::forward<F>(func);
    callbackReferences_ = true;
    if (value_->hasValue() || value_->hasException()) 
        callback_(std::move(*value_));
    lock_.unlock();
    }

 private:
  folly::Try<T>* value_;
  folly::fibers::Baton* baton_;
  folly::Function<void(Try<T>&&)> callback_;
  std::atomic<bool> callbackReferences_{false};
  folly::MicroSpinLock lock_ {0};
};

template <typename T>
std::shared_ptr<HPromise<
  std::vector<
  Try<T>>>>
HCollectAll(std::vector<std::shared_ptr<HPromise<T>>> &c); 

} // namespace fibers
} // namespace folly

#include <folly/fibers/HPromise-inl.h>
