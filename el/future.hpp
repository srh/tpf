#ifndef TPF_EL_FUTURE_HPP
#define TPF_EL_FUTURE_HPP

#include <functional>
#include <optional>

#include "el/intrusive_list.hpp"
#include "util.hpp"



/* Should we put completion_notifies_ on the promise or on the future as we have it now?
 * Should we make completion_notifies_ a single pointer? */

namespace el {

template <class T>
class future;

template <class T>
class promise;

class future_notify : public intrusive_list_node {
public:
    virtual void future_completed() = 0;
    void unregister_self() {
        // Should this be an assertion?
        if (!intrusive_list_node::is_detached()) {
            intrusive_list_node::detach();
        }
    }
protected:
    ~future_notify() = default;
};

template <class T>
class future {
private:
    NONCOPYABLE(future);
    promise<T> *matching_promise_ = nullptr;

    // Only non-empty when matching_promise_ is non-null.  So, we couldn't wait for
    // completion of a future which is then attached to with .then.
    intrusive_list<future_notify> completion_notifies_;

    std::optional<T> value_;

    friend class promise<T>;


    void notify_and_detach_completions() {
        intrusive_list<future_notify> notifies = std::move(completion_notifies_);
        intrusive_list_node *self = notifies.self();
        intrusive_list_node *node = self->next();
        while (node != self) {
            node->detach();
            static_cast<future_notify *>(node)->future_completed();
            // Use self->next() over and over again to tolerate future_completed callbacks
            // causing other notifies to self-detach.  (Which would be weird; instead
            // maybe we should assert that nothing "weird" is going on.)
            node = self->next();
        }
    }

public:
    explicit future(promise<T> *prom) : matching_promise_(prom), value_{} {
        tpf_assert(prom->matching_future_ == nullptr);
        prom->matching_future_ = this;
    }

    future() : matching_promise_{nullptr}, completion_notifies_{}, value_{} {}

    future(T&& value) : matching_promise_{nullptr}, completion_notifies_{}, value_{std::move(value)} {}

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

public:
    promise() : matching_future_(nullptr), attached_callback_{} {}

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

// TODO: Maybe, move wait_any to a separate file.
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

template <size_t N>
struct wait_any_state;

template <size_t N>
struct wait_any_notify : public future_notify {
    NONCOPYABLE(wait_any_notify);
    wait_any_notify() = default;
    wait_any_state<N> *state = nullptr;
    void future_completed() override;
};

template <size_t N>
struct wait_any_state {
    NONCOPYABLE(wait_any_state);
    wait_any_state() : value_supplied(false), prom{} {
        for (size_t i = 0; i < N; ++i) {
            notifies[i].state = this;
        }
    }
    bool value_supplied = false;
    promise<size_t> prom;
    wait_any_notify<N> notifies[N];
    void notify_complete(wait_any_notify<N> *notify) {
        tpf_assert(notify >= notifies + 0 && notify < notifies + N);
        size_t index = notify - notifies;
        tpf_assert(index < N);  // No funny offsets

        for (size_t i = 0; i < index; ++i) {
            notifies[i].unregister_self();
        }
        for (size_t i = index + 1; i < N; ++i) {
            notifies[i].unregister_self();
        }

        if (!value_supplied) {
            value_supplied = true;
            prom.supply_value_and_detach(std::move(index));
        }
    }
};

template <size_t N>
void wait_any_notify<N>::future_completed() {
    state->notify_complete(this);
}

template <class T0>
future<size_t> await_notifications(future<T0>& fut0) {
    auto state = make_unique<wait_any_state<2>>();
    future<size_t> fut(&state->prom);

    fut0.register_notify(&state->notifies[0]);

    return fut;
}

template <class T0, class T1>
future<size_t> await_notifications(future<T0>& fut0, future<T1>& fut1) {
    auto state = make_unique<wait_any_state<2>>();
    future<size_t> fut(&state->prom);

    fut0.register_notify(&state->notifies[0]);
    fut1.register_notify(&state->notifies[1]);

    return fut;
}

template <class T0, class T1, class T2>
future<size_t> await_notifications(future<T0>& fut0, future<T1>& fut1, future<T2>& fut2) {
    auto state = make_unique<wait_any_state<2>>();
    future<size_t> fut(&state->prom);

    // TODO: Use variadic template (once this is used, tested, stabilized).
    fut0.register_notify(&state->notifies[0]);
    fut1.register_notify(&state->notifies[1]);
    fut2.register_notify(&state->notifies[2]);

    return fut;
}

template <class... Ts>
future<size_t> wait_any(future<Ts>&... futs) {
    static_assert(sizeof...(Ts) > 0);
    // So we have a bunch of futures.
    size_t i = 0;
    bool seen_ready = false;
    char arr[] = { (check_evaled(&i, &futs, &seen_ready))..., '\0' };
    if (seen_ready) {
        return future<size_t>(std::move(i));
    }

    // So we have a bunch of futures, and none of them are ready.
    return await_notifications(futs...);
}


}  // namespace el

#endif  // TPF_EL_FUTURE_HPP