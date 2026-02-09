#ifndef TPF_EL_FUTURE_HPP
#define TPF_EL_FUTURE_HPP

#include <functional>
#include <optional>

#include "el/intrusive_list.hpp"
#include "util.hpp"



/* Should we put completion_notifies_ on the promise or on the future as we have it now?  It's
a question of performance optimization.  (Other potential optimizations: Maybe it
could be a singular pointer or a singly linked intrusive list or single-pointer
root node, instead of the basic cyclic intrusive_list we have now.) */

namespace el {

template <class T>
class future;

template <class T>
class promise;

class future_notify : public intrusive_list_node {
public:
    virtual void future_completed() = 0;
};

template <class T>
class future {
    NONCOPYABLE(future);
    promise<T> *matching_promise_ = nullptr;

    // Only non-empty when matching_promise_ is non-null.  So, we couldn't wait for
    // completion of a future which is then attached to with .then.
    intrusive_list<future_notify> completion_notifies_;

    std::optional<T> value_;

    friend class promise<T>;

    explicit future(promise<T> *prom) : matching_promise_(prom), value_{} {
        tpf_assert(prom->matching_future_ == nullptr);
        prom->matching_future_ = this;
    }

    void notify_and_detach_completions() {
        intrusive_list_node *self = completion_notifies_.self();
        intrusive_list_node *node = self->next();
        while (node != self) {
            intrusive_list_node *next = node->next();
            node->detach_self();
            static_assert(std::is_same_v<decltype(completion_notifies_), intrusive_list<future_notify>>);
            static_cast<future_notify *>(node)->future_completed();
            node = next;
        }
    }

public:
    future() : matching_promise_{nullptr}, value_{} {}

    future(future&& other);
    // Asserts we are in a default-constructed state.
    future& operator=(future&& other);

    template <class U>
    future<U> then(std::move_only_function<future<U> (T&)>&& fn) &&;

    bool has_value() const {
        return value_.has_value();
    }

    bool is_default_constructed() const {
        return matching_promise_ == nullptr && !value_.has_value();
    }
};


template <class T>
class promise {
    friend class future<T>;
    NONCOPYABLE(promise);
    // has backpointer that needs to be maintained
    future<T> *matching_future_ = nullptr;

    // has no backpointer
    using attached_callback_type = std::move_only_function<void (T&)>;
    attached_callback_type attached_callback_;

    promise(promise&& other) : matching_future_(other.matching_future_), attached_callback_(std::move(other.attached_callback_)) {
        if (matching_future_ != nullptr) {
            matching_future_->matching_promise_ = this;
        }
        other.matching_future_ = nullptr;
    }

    // Asserts we are in a default-constructed state
    promise& operator=(promise&& other) {
        tpf_assert(matching_future_ == nullptr && !attached_callback_);
        matching_future_ = other.matching_future_;
        other.matching_future_ = nullptr;
        attached_callback_.swap(other.attached_callback_);
        if (matching_future_ != nullptr) {
            matching_future_->matching_promise_ = this;
        }
        return *this;
    }

    ~promise() {
        tpf_assert(matching_future_ == nullptr);
    }

    // Maybe this should even be marked &&
    void supply_value_and_detach(T&& value) {
        if (matching_future_ != nullptr) {
            future<T> *fut = matching_future_;
            fut->value_ = std::move(value);
            fut->matching_promise_ = nullptr;
            matching_future_ = nullptr;

            fut->notify_and_detach_completions();
        } else {
            tpf_assertf(attached_callback_, "No attached callback -- a promise was leaked");
            attached_callback_type cb;
            cb.swap(attached_callback_);
            cb(value);
        }
    }
};

template <class T>
future<T>::future(future&& other) : matching_promise_(other.matching_promise_), value_{} {
    value_.swap(other.value_);
    other.matching_promise_ = nullptr;
    if (matching_promise_ != nullptr) {
        matching_promise_->matching_future_ = this;
    }
}

template <class T>
future<T>& future<T>::operator=(future&& other) {
    tpf_assert(is_default_constructed());

    value_.swap(other.value_);
    matching_promise_ = other.matching_promise_;
    other.matching_promise_ = nullptr;
    if (matching_promise_ != nullptr) {
        matching_promise_->matching_future_ = this;
    }
    return *this;
}

template <class T>
template <class U>
future<U> future<T>::then(std::move_only_function<future<U> (T&)>&& fn) && {
    tpf_assert(!is_default_constructed());
    if (value_.has_value()) {
        std::move_only_function<future<U> (T&)> tmp;
        tmp.swap(fn);
        return tmp(*value_);
    }
    tpf_assert(matching_promise_ != nullptr);
    promise<U> prom;
    future<U> fut(&prom);

    promise<T> *our_prom = matching_promise_;

    tpf_assert(!our_prom->attached_callback_);
    our_prom->attached_callback_ = [prom = std::move(prom), fn = std::move(fn)](T& value) mutable {
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
            future<U> *prom_fut = prom.matching_future_;
            tpf_assert(prom_fut != nullptr);
            tpf_assert(prom_fut->matching_promise_ == &prom);
            prom_fut->matching_promise_ = nullptr;
            prom.matching_future_ = nullptr;

            promise<U> *res_promise = res.matching_promise_;
            prom_fut->matching_promise_ = res_promise;
            res.matching_promise_ = nullptr;
            res_promise->matching_future_ = prom_fut;
        }
    };
    our_prom->matching_future_ = nullptr;
    matching_promise_ = nullptr;
    tpf_assert(is_default_constructed());
    return fut;
}

template <class T>
void check_evaled(size_t *i, future<T> *fut, bool *seen_ready) {
    // TODO: We pay with an extra conditional check to return the _first_ ready future
    // instead of the last ready future.  It's not a guaranteed semantic (yet?).
    if (*seen_ready) {
        return;
    }
    tpf_assert(!fut->attached_callback_);
    if (fut->ready()) {
        *seen_ready = true;
        return;
    }
    // Element at index i (and all before) are not ready.
    ++*i;
}

template <class... Ts>
size_t wait_any(future<Ts>&... futs) {
    // So we have a bunch of futures.
    size_t i = 0;
    bool seen_ready = false;
    char arr[] = { (check_evaled(&i, &futs, &seen_ready))..., '\0' };
    if (seen_ready) {
        return i;
    }

    // So we have a bunch of futures, and none of them are ready.




    // We need some change to futures so we can attach ready notification instead of
    // simply a callback that receives and consumes the value.
    throw "TODO: Implement.";
}


}  // namespace el

#endif  // TPF_EL_FUTURE_HPP