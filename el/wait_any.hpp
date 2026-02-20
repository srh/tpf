#ifndef TPF_EL_WAIT_ANY_HPP
#define TPF_EL_WAIT_ANY_HPP

#include "el/future.hpp"

namespace el {

using interrupt_future = cancellable_future<expected<void, read_error>>;
struct interrupt_result {
    expected<void, read_error> result;
};

template <class T>
void check_evaled(size_t *i, future<T> *fut, bool *seen_ready) {
    // TODO: We pay with an extra conditional check to return the _first_ ready future
    // instead of the last ready future.  It's not a guaranteed semantic (yet?).
    // (We could always reverse the variadic list order.)
    if (*seen_ready) {
        return;
    }
    if (fut->has_value()) {
        *seen_ready = true;
        return;
    }
    tpf_assert(!fut->is_default_constructed());
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
    wait_any_state() : prom{} {
        for (size_t i = 0; i < N; ++i) {
            notifies[i].state = this;
        }
    }
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

        promise<size_t> prom_moved{std::move(prom)};
        delete this;
        std::move(prom_moved).supply_value_and_detach(std::move(index));
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
    state.release();

    return fut;
}

template <class T0, class T1>
future<size_t> await_notifications(future<T0>& fut0, future<T1>& fut1) {
    auto state = make_unique<wait_any_state<2>>();
    future<size_t> fut(&state->prom);

    fut0.register_notify(&state->notifies[0]);
    fut1.register_notify(&state->notifies[1]);
    state.release();

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
    state.release();

    return fut;
}

template <class... Ts>
future<size_t> wait_any(future<Ts>&... futs) {
    static_assert(sizeof...(Ts) > 0);
    // So we have a bunch of futures.
    size_t i = 0;
    bool seen_ready = false;
    char arr[] = { (check_evaled(&i, &futs, &seen_ready), '\0')... };
    if (seen_ready) {
        tpf_setupf("seen_ready true without waiting, i = %zu\n", i);
        return future<size_t>(std::move(i));
    }

    // So we have a bunch of futures, and none of them are ready.
    return await_notifications(futs...);
}

template <class T>
future<expected<T, interrupt_result>> wait_interruptible(interrupt_future *interruptor, cancellable_future<T>&& fut) {
    // TODO: We might micro-optimize this function.
    future<size_t> index_fut = wait_any(*interruptor, fut);
    return std::move(index_fut).then([interruptor, MC(fut)](size_t index_ready) mutable -> future<expected<T, interrupt_result>> {
        if (index_ready == 0) {
            fut.cancel();
            return make_future<expected<T, interrupt_result>>(unexpected(interrupt_result{interruptor->value()}));
        }
        return make_future<expected<T, interrupt_result>>(std::move(fut.value()));
    });
}


}  // namespace el

#endif  // TPF_EL_WAIT_ANY_HPP