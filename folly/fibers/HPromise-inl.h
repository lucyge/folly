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
#include <folly/fibers/Baton.h>

namespace folly {
namespace fibers {

template <typename T>
HPromise<T>::HPromise() {
    value_ = new folly::Try<T>();
    baton_ = new Baton();
}

template <typename T>
HPromise<T>::~HPromise() {
   delete value_;
   delete baton_;
}

template <typename T>
void HPromise<T>::setException(folly::exception_wrapper e) {
  setTry(folly::Try<T>(e));
}

template <typename T>
void HPromise<T>::setTry(folly::Try<T>&& t) {
  lock_.lock();
  if (callbackReferences_) { 
    callback_(std::move(t));
  } else {
    *value_ = std::move(t);
    baton_->post();
  }
  lock_.unlock();
}

template <typename T>
template <typename M>
void HPromise<T>::setValue(M&& v) {
  static_assert(!std::is_same<T, void>::value, "Use setValue() instead");

  setTry(folly::Try<T>(std::forward<M>(v)));
}

template <typename T>
void HPromise<T>::setValue() {
  static_assert(std::is_same<T, void>::value, "Use setValue(value) instead");

  setTry(folly::Try<void>());
}

template <typename T>
T HPromise<T>::getValue() {
    return value_->value();
}

template <typename T>
Baton *HPromise<T>::getBaton() {
    return baton_;
}

template <typename T, class F>
void hmapSetCallback(std::vector<std::shared_ptr<HPromise<T>>> &c, F func) {
  for (size_t i=0; i < c.size(); i++) {
    c[i]->setCallback([func, i](Try<T>&& t) {
      func(i, std::move(t));
    });
  }
}

template <typename T>
std::shared_ptr<HPromise<
  std::vector<
  Try<T>>>>
HCollectAll(std::vector<std::shared_ptr<HPromise<T>>> &c) {

  struct HCollectAllContext {
    HCollectAllContext(size_t n) : results(n) {
        p = std::make_shared<folly::fibers::HPromise<std::vector<Try<T>>>>();
        results.resize(n);
        callbackReferences = n;
    }
    ~HCollectAllContext() {
      //p->setValue(std::move(results));
    }
    std::shared_ptr<HPromise<std::vector<Try<T>>>> p;
    std::vector<Try<T>> results;
    std::atomic<size_t> callbackReferences;
  }; 

  typedef typename folly::Function<void(size_t i, Try<T>&&)> fn_t;

  auto ctx =
      std::make_shared<HCollectAllContext>(c.size());
  //hmapSetCallback<T>(c, [ctx](size_t i, Try<T>&& t) {
  hmapSetCallback<T>(c, [ctx](size_t i, Try<T>&& t) {
    ctx->results[i] = std::move(t);
    if (--ctx->callbackReferences == 0)
        ctx->p->setValue(std::move(ctx->results));
  });

  return ctx->p;
}

} // namespace fibers
} // namespace folly
