#ifndef TPF_EL_FUTURE_HPP
#define TPF_EL_FUTURE_HPP

#include <functional>
#include <optional>

#include "el/loop.hpp"
#include "el/intrusive_list.hpp"
#include "util.hpp"



/* Should we put completion_notifies_ on the promise or on the future as we have it now?
 * Should we make completion_notifies_ a single pointer? */

namespace el {

// TODO: Support future<void>...?
struct Unit {};

struct CompletionResult {
    static CompletionResult completed() {
        return CompletionResult{};
    }
};

template <class T>
class future;

template <class T>
class cancellable_future;

template <class T>
class base_promise;

template <class T>
class promise;

template <class T, class U>
class cancellable_then_f_promise;

class future_notify : public intrusive_list_node {
public:
    virtual void future_completed() = 0;
    void unregister_self() {
        // Should this be an assertion?
        if (!is_detached()) {
            detach();
        }
    }
protected:
    ~future_notify() = default;
};

template <class T>
struct is_future {
    static constexpr bool value = false;
};
template <class T>
struct is_future<future<T>> {
    static constexpr bool value = true;
};
template <class T>
struct is_future<cancellable_future<T>> {
    static constexpr bool value = true;
};
template <class T>
using is_future_v = is_future<T>::value;

template <class F, class T>
struct is_future_returns {
    static constexpr bool value = false;
};
template <class T>
struct is_future_returns<future<T>, T> {
    static constexpr bool value = true;
};
template <class T>
struct is_future_returns<cancellable_future<T>, T> {
    static constexpr bool value = true;
};
template <class F, class T>
using is_future_returns_v = is_future_returns<F, T>::value;

template <class T>
struct is_plain_future {
    static constexpr bool value = false;
};
template <class T>
struct is_plain_future<future<T>> {
    static constexpr bool value = true;
};
template <class T>
using is_plain_future_v = is_plain_future<T>::value;

template <class T>
struct is_cancellable_future {
    static constexpr bool value = false;
};
template <class T>
struct is_cancellable_future<cancellable_future<T>> {
    static constexpr bool value = true;
};
template <class T>
using is_cancellable_future_v = is_cancellable_future<T>::value;

template <class T>
class [[nodiscard]] future {
protected:
    NONCOPYABLE(future);
    base_promise<T> *matching_promise_ = nullptr;

    std::optional<T> value_;

    friend class base_promise<T>;
    template <class U>
    friend class future;
    template <class U>
    friend class cancellable_future;

public:
    using value_type = T;

    explicit future(base_promise<T> *prom) : matching_promise_(prom), value_{} {
        tpf_assert(prom->matching_future_ == nullptr);
        prom->matching_future_ = this;
    }

    future() : matching_promise_{nullptr}, value_{} {}

    explicit future(T&& value) : matching_promise_{nullptr}, value_{std::move(value)} {}

    future(future&& other);
    // Asserts we are in a default-constructed state.
    future& operator=(future&& other);

    template <class U>
    future<U> then_f(std::move_only_function<future<U> (T&&)>&& fn) &&;

    template <class C>
    requires (is_cancellable_future<std::invoke_result_t<C&, T&&>>::value)
    future<typename std::invoke_result_t<C&, T&&>::value_type> then(C&& callable) && {
        return std::move(*this).then_f(std::move_only_function<future<typename std::invoke_result_t<C&, T&&>::value_type> (T&&)>(std::forward<C>(callable)));
    }

    template <class C>
    requires (is_plain_future<std::invoke_result_t<C&, T&&>>::value)
    future<typename std::invoke_result_t<C&, T&&>::value_type> then(C&& callable) && {
        return std::move(*this).then_f(std::move_only_function<future<typename std::invoke_result_t<C&, T&&>::value_type> (T&&)>(std::forward<C>(callable)));
    }

    // Right now we have no exception logic, so "finally" does nothing with regard to
    // error handling (or cancellation) -- and it's probably not really better than using
    // .then and returning the return value.
    template <class C>
    requires (is_future_returns<std::invoke_result_t<C&>, Unit>::value)
    future<T> finally(C&& callable) && {
        return std::move(*this).then([cb = std::forward<C>(callable)](T&& value) mutable {
            static_assert(std::is_same_v<decltype(cb), std::remove_cvref_t<C>>);
            return cb().then([MC(value)](Unit&&) mutable {
                return future<T>(std::move(value));
            });
        });
    }

    bool has_value() const {
        return value_.has_value();
    }
    T& value() {
        tpf_assert(value_.has_value());
        return *value_;
    }

    bool is_default_constructed() const {
        return matching_promise_ == nullptr && !value_.has_value();
    }

    bool is_active() const {
        return matching_promise_ != nullptr;
    }

    void wait_with_callback(std::move_only_function<void (T&&)>&& fn) &&;

    void wait_with_callback_schedule_if_immediate(Loop *loop, std::move_only_function<void (T&&)>&& fn) &&;

    void register_notify(future_notify *notify) {
        matching_promise_->register_notify(notify);
    }
};

template <class T>
class base_promise {
protected:
    template <class U>
    friend class future;
    template <class U>
    friend class cancellable_future;
    NONCOPYABLE(base_promise);
    // has backpointer that needs to be maintained
    future<T> *matching_future_ = nullptr;

    // Only non-empty when matching_future_ is non-null and a value is not supplied.  So,
    // we may not wait for completion of a future/promise which is then attached to with
    // .then.
    intrusive_list<future_notify> completion_notifies_;

    // has no backpointer
    using attached_callback_type = std::move_only_function<void (T&&)>;
    attached_callback_type attached_callback_;

private:
    void register_notify(future_notify *notify) {
        tpf_assert(notify->is_detached());
        tpf_assert(matching_future_ != nullptr);
        tpf_assert(!attached_callback_);  // TODO make a helper that returns if a callback is attached.
        completion_notifies_.push(notify);
    }

protected:
    base_promise() : matching_future_(nullptr), completion_notifies_{}, attached_callback_{} {}

    base_promise(base_promise&& other) : matching_future_(other.matching_future_), completion_notifies_{std::move(other.completion_notifies_)}, attached_callback_(swap_out(other.attached_callback_)) {
        if (matching_future_ != nullptr) {
            matching_future_->matching_promise_ = this;
        }
        other.matching_future_ = nullptr;
    }

    // Asserts we are in a default-constructed state
    base_promise& operator=(base_promise&& other) {
        tpf_assert(matching_future_ == nullptr && !attached_callback_);
        tpf_assert(completion_notifies_.empty());
        matching_future_ = other.matching_future_;
        other.matching_future_ = nullptr;
        attached_callback_.swap(other.attached_callback_);
        if (matching_future_ != nullptr) {
            matching_future_->matching_promise_ = this;
        }
        completion_notifies_.swap(other.completion_notifies_);
        return *this;
    }

    ~base_promise() {
        tpf_assert(matching_future_ == nullptr);
        tpf_assert(!attached_callback_);
    }

public:

    bool is_default_constructed_but_for_completions() const {
        return matching_future_ == nullptr && !attached_callback_;
    }
    bool completions_empty() const {
        return completion_notifies_.empty();
    }
    bool is_default_constructed() const {
        return is_default_constructed_but_for_completions() && completions_empty();
    }
    bool is_active() const {
        return !is_default_constructed_but_for_completions();
    }

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

    // This leaves promise in default-constructed state before invoking callbacks.
    void supply_value_and_detach(T&& value) && {
        if (matching_future_ != nullptr) {
            future<T> *fut = matching_future_;
            fut->value_ = std::move(value);
            fut->matching_promise_ = nullptr;
            matching_future_ = nullptr;
            tpf_assert(is_default_constructed_but_for_completions());
            notify_and_detach_completions();
        } else {
            tpf_assertf(attached_callback_, "No attached callback -- a promise was leaked");
            attached_callback_type cb;
            cb.swap(attached_callback_);
            tpf_assert(is_default_constructed());
            cb(std::move(value));
        }
    }
};

template <class T>
class promise : public base_promise<T> {
public:
    promise() = default;
    promise(promise&&) = default;
    promise& operator=(promise&&) = default;
};

template <class T>
class cancellable_promise;
template <class T>
class cancellable_then_f_promise_base {
protected:
    NONCOPYABLE(cancellable_then_f_promise_base);
    friend class cancellable_promise<T>;
    // Is in a pointer/backpointer pair with attached_promise_ptr_.
    cancellable_promise<T> *backwards_pointed_promise_ = nullptr;
    cancellable_then_f_promise_base() = default;
    cancellable_then_f_promise_base(cancellable_then_f_promise_base&& other) noexcept;

    cancellable_then_f_promise_base& operator=(cancellable_then_f_promise_base&& other) noexcept;
    void attach(cancellable_promise<T> *prom);
};

template <class T>
class cancellable_promise : public base_promise<T> {
public:
    NONCOPYABLE(cancellable_promise);
    cancellable_promise(cancellable_promise &&other) : base_promise<T>(std::move(other)), attached_promise_ptr_(other.attached_promise_ptr_) {
        if (attached_promise_ptr_) {
            tpf_assert(attached_promise_ptr_->backwards_pointed_promise_ == &other);
            attached_promise_ptr_->backwards_pointed_promise_ = this;
            other.attached_promise_ptr_ = nullptr;
        }
    }
    cancellable_promise& operator=(cancellable_promise&& other) {
        base_promise<T>::operator=(std::move(other));
        // base_promise<T>::operator= also asserts the base class is in a default-constructed state.
        tpf_assert(attached_promise_ptr_ == nullptr);
        attached_promise_ptr_ = other.attached_promise_ptr_;
        if (attached_promise_ptr_) {
            tpf_assert(other.attached_promise_ptr_->backwards_pointed_promise_ == &other);
            attached_promise_ptr_->backwards_pointed_promise_ = this;
            other.attached_promise_ptr_ = nullptr;
        }
        return *this;
    }
    cancellable_promise() = default;
    // It's worth noting the implicit decision that cancellation of a cancellable_future
    // can't be async.  We must support zero-wait cancellation.
    virtual void cancel() = 0;
protected:
    ~cancellable_promise() {}

    friend class cancellable_then_f_promise_base<T>;
    template <class U, class V>
    friend class cancellable_then_f_promise;
    // Is in a pointer/backpointer pair with backwards_pointed_promise_.
    cancellable_then_f_promise_base<T> *attached_promise_ptr_ = nullptr;
};

template <class T>
class default_cancellable_promise : public cancellable_promise<T> {
public:
    NONCOPYABLE_MOVABLE(default_cancellable_promise);
    default_cancellable_promise() = default;
    void cancel() final {
        // Do nothing. Should be default-constructed when we're done cancelling (caller
        // disconnects us first).
        tpf_assert(base_promise<T>::is_default_constructed());
    }
};

// Futures of type cancellable_future have no fields and may be object-sliced to future<T>
// -- you just lose the ability to cancel().
template <class T>
class cancellable_future : public future<T> {
public:
    // Public, but only for the sake of assertions.
    cancellable_promise<T> *matching_promise() {
        // The invariant is maintained that matching_promise_ is a cancellable_promise.
        return static_cast<cancellable_promise<T> *>(future<T>::matching_promise_);
    }
public:
    using future<T>::future;
    cancellable_future& operator=(cancellable_future&&) = default;
    cancellable_future(cancellable_future&&) = default;

    void cancel() {
        if (future<T>::value_.has_value()) {
            tpf_setupf("Cancelling future that has a value assigned.\n");
            // We are already detached... just get rid of the value.
            tpf_assert(future<T>::matching_promise_ == nullptr);
            future<T>::value_.reset();
            tpf_assert(future<T>::is_default_constructed());
            return;
        }

        tpf_setupf("Cancelling a future with a matching promise.\n");
        tpf_assertf(future<T>::matching_promise_ != nullptr, "canceling a consumed future");
        // TODO: Support canceling a future with attached notifies.
        tpf_assertf(future<T>::matching_promise_->completion_notifies_.empty(), "canceling a future with waiters (the lib could allow this though)");

        cancellable_promise<T> *prom = matching_promise();
        tpf_assert(prom->matching_future_ == this);
        prom->matching_future_ = nullptr;
        future<T>::matching_promise_ = nullptr;
        prom->cancel();
        // prom can be (and will be) destructed (and memory freed) by the ->cancel() call.
        // This actually happens if prom is a cancellable_then_f_promise.
        // The cancel() implementation may have this assertion.
        // tpf_assert(prom->is_default_constructed());
        tpf_assert(future<T>::is_default_constructed());
    }

    template <class U>
    cancellable_future<U> cancellable_then_f(std::move_only_function<cancellable_future<U> (T&&)>&& fn) &&;

    template <class C>
    requires (is_cancellable_future<std::invoke_result_t<C&, T&&>>::value)
    std::invoke_result_t<C&, T&&> cancellable_then(C&& callable) && {
        return std::move(*this).cancellable_then_f(std::move_only_function<std::invoke_result_t<C&, T&&> (T&&)>(std::forward<C>(callable)));
    }
};

template <class T>
cancellable_future<std::remove_cvref_t<T>> make_cancellable_future(T&& value) {
    return cancellable_future(std::forward<T>(value));
}

template <class T>
future<std::remove_cvref_t<T>> make_future(T&& value) {
    return future(std::forward<T>(value));
}

// TODO: Remove this?  It's unused.
template <class T>
class self_cancellable_future : public cancellable_future<T> {
public:
    using cancellable_future<T>::cancellable_future;
    self_cancellable_future& operator=(self_cancellable_future&&) = default;
    self_cancellable_future(self_cancellable_future&&) = default;
    self_cancellable_future(cancellable_future<T>&& cf) : cancellable_future<T>(std::move(cf)) {}
    ~self_cancellable_future() {
        if (!future<T>::is_default_constructed()) {
            cancellable_future<T>::cancel();
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
void future<T>::wait_with_callback(std::move_only_function<void (T&&)>&& fn) && {
    tpf_assert(!is_default_constructed());
    if (value_.has_value()) {
        swap_out(fn)(*swap_out(value_));
        return;
    }

    base_promise<T> *our_prom = matching_promise_;

    tpf_assert(!our_prom->attached_callback_);
    our_prom->attached_callback_.swap(fn);
    our_prom->matching_future_ = nullptr;
    matching_promise_ = nullptr;
    tpf_assert(is_default_constructed());
    return;
}

template <class T>
void future<T>::wait_with_callback_schedule_if_immediate(Loop *loop, std::move_only_function<void (T&&)>&& fn) && {
    tpf_assert(!is_default_constructed());
    if (value_.has_value()) {
        loop->schedule([fn = swap_out(fn), v = swap_out(value_)] mutable { fn(std::move(*v)); });
        return;
    }

    base_promise<T> *our_prom = matching_promise_;

    tpf_assert(!our_prom->attached_callback_);
    our_prom->attached_callback_.swap(fn);
    our_prom->matching_future_ = nullptr;
    matching_promise_ = nullptr;
    tpf_assert(is_default_constructed());
    return;
}

template<class T>
void cancellable_then_f_promise_base<T>::attach(cancellable_promise<T> *prom) {
    tpf_assert(prom->attached_promise_ptr_ == nullptr);
    backwards_pointed_promise_ = prom;
    prom->attached_promise_ptr_ = this;
}

template<class T>
cancellable_then_f_promise_base<T>::cancellable_then_f_promise_base(
    cancellable_then_f_promise_base &&other) noexcept : backwards_pointed_promise_(other.backwards_pointed_promise_) {
    if (backwards_pointed_promise_ != nullptr) {
        tpf_assert(backwards_pointed_promise_->attached_promise_ptr_ == &other);
        backwards_pointed_promise_->attached_promise_ptr_ = this;
        other.backwards_pointed_promise_ = nullptr;
    }
}

template<class T>
cancellable_then_f_promise_base<T> & cancellable_then_f_promise_base<T>::operator=(
    cancellable_then_f_promise_base &&other) noexcept {
    // TODO: Not a fan of this base class operator= nonsense.
    tpf_assert(backwards_pointed_promise_ == nullptr);
    backwards_pointed_promise_ = other.backwards_pointed_promise_;
    if (backwards_pointed_promise_) {
        tpf_assert(backwards_pointed_promise_->attached_promise_ptr_ == &other);
        backwards_pointed_promise_->attached_promise_ptr_ = this;
        other.backwards_pointed_promise_ = nullptr;
    }
    return *this;
}

template <class T>
template <class U>
future<U> future<T>::then_f(std::move_only_function<future<U> (T&&)>&& fn) && {
    tpf_assert(!is_default_constructed());
    if (value_.has_value()) {
        return swap_out(fn)(*swap_out(value_));
    }
    tpf_assert(matching_promise_ != nullptr);
    promise<U> prom;
    future<U> fut(&prom);

    base_promise<T> *our_prom = matching_promise_;
    tpf_assert(our_prom->matching_future_ == this);

    tpf_assert(!our_prom->attached_callback_);
    our_prom->attached_callback_ = [MC(prom), fn = swap_out(fn)](T&& value) mutable {
        future<U> res = swap_out(fn)(std::move(value));

        if (res.value_.has_value()) {
            // res should be detached from its promise because it has a value.
            tpf_assert(res.matching_promise_ == nullptr);
            // If we have a value just supply it, which detaches prom from prom_fut, perhaps invokes callback
            // TODO: Should we move the whole optional for some performance or other reason like calling *res.value_'s destructor first?
            // TODO: We're in a callback.  Are we really calling recursively?  We could blow the stack.
            std::move(prom).supply_value_and_detach(std::move(*res.value_));
            res.value_.reset();
        } else {
            // res must have a promise.
            tpf_assert(res.matching_promise_ != nullptr);
            base_promise<U> *res_promise = res.matching_promise_;
            tpf_assert(res_promise->matching_future_ == &res);

            future<U> *prom_fut = prom.matching_future_;
            if (prom_fut != nullptr) {
                // Splice fut's promise pointer (where "fut" is wherever the local
                // variable 'fut' above got moved to) to point to the new promise (and
                // vice versa).
                tpf_assert(prom_fut->matching_promise_ == &prom);
                prom_fut->matching_promise_ = nullptr;
                prom.matching_future_ = nullptr;

                prom_fut->matching_promise_ = res_promise;
                res.matching_promise_ = nullptr;
                res_promise->matching_future_ = prom_fut;
            } else {
                tpf_assert(prom.attached_callback_);
                // Splice prom's callback into res.matching_promise_.
                tpf_assert(!res_promise->attached_callback_);
                tpf_assert(res_promise->completion_notifies_.empty());
                res_promise->attached_callback_.swap(prom.attached_callback_);
                res_promise->matching_future_ = nullptr;
                res.matching_promise_ = nullptr;
            }
        }
    };
    our_prom->matching_future_ = nullptr;
    matching_promise_ = nullptr;
    tpf_assert(is_default_constructed());
    return fut;
}


template <class T, class U>
class cancellable_then_f_promise : public cancellable_then_f_promise_base<T>, public cancellable_promise<U> {
public:
    using cancellable_then_f_promise_base<T>::attach;
    NONCOPYABLE_MOVABLE(cancellable_then_f_promise);
    cancellable_then_f_promise() = default;
    void cancel() final {
        // TODO: Should this comparison be an assertion?  This promise should not exist in an unattached state.
        if (cancellable_then_f_promise_base<T>::backwards_pointed_promise_ != nullptr) {
            // So, backwards_pointed_promise_ exists... therefore it's not hooked up to a
            // future, it has an attached callback.

            cancellable_promise<T> *backwards_pointed_promise = cancellable_then_f_promise_base<T>::backwards_pointed_promise_;
            tpf_assert(backwards_pointed_promise->attached_promise_ptr_ == this);
            tpf_assert(backwards_pointed_promise->matching_future_ == nullptr);
            tpf_assert(backwards_pointed_promise->attached_callback_);

            // Detach backwards_pointed_promise_/attached_promise_ptr_ pair.
            backwards_pointed_promise->attached_promise_ptr_ = nullptr;
            cancellable_then_f_promise_base<T>::backwards_pointed_promise_ = nullptr;

            // Detach attached callback.
            {
                typename base_promise<T>::attached_callback_type cb;
                cb.swap(backwards_pointed_promise->attached_callback_);
            }

            // This cancel() call will call our destructor.
            backwards_pointed_promise->cancel();
        }
    }
};

template <class T>
template <class U>
cancellable_future<U>
cancellable_future<T>::cancellable_then_f(std::move_only_function<cancellable_future<U> (T&&)>&& fn) && {
    tpf_assert(!future<T>::is_default_constructed());
    if (future<T>::value_.has_value()) {
        return swap_out(fn)(*swap_out(future<T>::value_));
    }

    tpf_assert(future<T>::matching_promise_ != nullptr);
    // This is basically the only material difference from then_f -- we define prom with a
    // cancel() implementation that cancels upstream.
    cancellable_then_f_promise<T, U> prom;
    cancellable_future<U> fut(&prom);

    cancellable_promise<T> *our_prom = matching_promise();
    tpf_assert(our_prom->matching_future_ == this);
    prom.attach(our_prom);

    tpf_assert(!our_prom->attached_callback_);
    our_prom->attached_callback_ = [MC(prom), fn = swap_out(fn)](T&& value) mutable {
        cancellable_future<U> res = swap_out(fn)(std::move(value));

        if (res.value_.has_value()) {
            // res should be detached from its promise because it has a value.
            tpf_assert(res.matching_promise_ == nullptr);
            // If we have a value just supply it, which detaches prom from prom_fut, perhaps invokes callback
            // TODO: Should we move the whole optional for some performance or other reason like calling *res.value_'s destructor first?
            // TODO: We're in a callback.  Are we really calling recursively?  We could blow the stack.
            // TODO: Duplicate TODOs with future<T>::then_f ^^^.
            std::move(prom).supply_value_and_detach(std::move(*res.value_));
            res.value_.reset();
        } else {
            // res must have a promise.
            tpf_assert(res.matching_promise_ != nullptr);
            base_promise<U> *res_promise = res.matching_promise_;
            tpf_assert(res_promise->matching_future_ == &res);

            future<U> *prom_fut = prom.matching_future_;
            if (prom_fut != nullptr) {
                // Splice fut's promise pointer (where "fut" is wherever the local
                // variable 'fut' above got moved to) to point to the new promise (and
                // vice versa).
                tpf_assert(prom_fut->matching_promise_ == &prom);
                prom_fut->matching_promise_ = nullptr;
                prom.matching_future_ = nullptr;

                prom_fut->matching_promise_ = res_promise;
                res.matching_promise_ = nullptr;
                res_promise->matching_future_ = prom_fut;
            } else {
                tpf_assert(prom.attached_callback_);
                // Splice prom's callback into res.matching_promise_.
                tpf_assert(!res_promise->attached_callback_);
                tpf_assert(res_promise->completion_notifies_.empty());
                res_promise->attached_callback_.swap(prom.attached_callback_);
                res_promise->matching_future_ = nullptr;
                res.matching_promise_ = nullptr;
            }
        }
    };
    our_prom->matching_future_ = nullptr;
    future<T>::matching_promise_ = nullptr;
    tpf_assert(future<T>::is_default_constructed());
    return fut;
}


}  // namespace el

#endif  // TPF_EL_FUTURE_HPP
