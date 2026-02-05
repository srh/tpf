#ifndef TPF_EL_FUTURE_HPP
#define TPF_EL_FUTURE_HPP

#include <functional>
#include <optional>

#include "util.hpp"

namespace el {

template <class T>
class future;

template <class T>
class promise;

template <class T>
class future {
    NONCOPYABLE(future);
    promise<T> *matching_promise_ = nullptr;

    // TODO: Be fancier about attached callback type.

    std::optional<T> value_;
    using attached_callback_type = std::move_only_function<void (T&)>;
    attached_callback_type attached_callback_;

    friend class promise<T>;

    explicit future(promise<T> *prom) : matching_promise_(prom), value_{}, attached_callback_{} {
        tpf_assert(prom->our_future_ == nullptr);
        prom->our_future_ = this;
    }

public:
    future() : matching_promise_{nullptr}, value_{}, attached_callback_{} {}

    template <class U>
    future<U> then(std::move_only_function<future<U> (T&)> fn);
};


template <class T>
class promise {
    NONCOPYABLE(promise);
    future<T> *our_future_ = nullptr;

    promise(promise&& other) : our_future_(other.our_future_) {
        if (our_future_ != nullptr) {
            our_future_->matching_promise_ = this;
        }
        other.our_future_ = nullptr;
    }

    promise& operator=(promise&& other) {
        if (&other == this) {
            return *this;
        }
        tpf_assert(our_future_ == nullptr);
        our_future_ = other.our_future_;
        if (our_future_ != nullptr) {
            our_future_->matching_promise_ = this;
        }
        return *this;
    }

    ~promise() {
        tpf_assert(our_future_ == nullptr);
    }

    void supply_value_and_detach(T&& value) {
        tpf_assert(our_future_ != nullptr);
        future<T> *fut = our_future_;
        fut->value_ = std::move(value);
        fut->matching_promise_ = nullptr;
        our_future_ = nullptr;
        if (fut->attached_callback_) {
            typename future<T>::attached_callback_type cb = std::move(fut->attached_callback_);
            cb(fut->value_);
        }
    }
};


template <class T>
template <class U>
future<U> future<T>::then(std::move_only_function<future<U> (T&)> fn) {
    tpf_assert(!attached_callback_);
    if (value_.has_value()) {
        std::move_only_function<future<U> (T&)> tmp;
        tmp.swap(fn);
        return tmp(*value_);
    }
    promise<U> prom;
    future<U> fut(&prom);

    attached_callback_ = [prom = std::move(prom), fn = std::move(fn)](T& value) mutable {
        std::move_only_function<future<U> (T&)> tmp;
        tmp.swap(fn);
        future<U> res = tmp(value);
        tpf_assert(!res.attached_callback_);

        if (res.value_.has_value()) {
            // res should be detached from its promise because it has a value.
            tpf_assert(res.matching_promise_ == nullptr);
            // If we have a value just supply it, which detaches prom from prom_fut, perhaps invokes callback
            // TODO: Should we move the whole optional for some performance or other reason like calling *res.value_'s destructor first?
            // TODO: We're in a callback.  Are we really calling recursively?  We could blow the stack.
            prom.supply_value_and_detach(std::move(*res.value_));
        } else {
            // res must have a promise.
            tpf_assert(res.matching_promise_ != nullptr);

            // Splice fut's promise pointer (where "fut" is wherever the local
            // variable 'fut' above got moved to) to point to the new promise (and
            // vice versa).
            future<U> *prom_fut = prom.our_future_;
            tpf_assert(prom_fut != nullptr);
            tpf_assert(prom_fut->matching_promise_ == &prom);
            prom_fut->matching_promise_ = nullptr;
            prom.our_future_ = nullptr;

            promise<U> *res_promise = res.matching_promise_;
            prom_fut->matching_promise_ = res_promise;
            res.matching_promise_ = nullptr;
            res_promise->our_future_ = prom_fut;
        }
    };
    return fut;
}


}  // namespace el

#endif  // TPF_EL_FUTURE_HPP