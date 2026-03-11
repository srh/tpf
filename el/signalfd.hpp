#ifndef TPF_EL_SIGNALFD_HPP
#define TPF_EL_SIGNALFD_HPP

#include "util.hpp"
#include "el/fd_registrant.hpp"
#include "el/future.hpp"
#include "el/loop.hpp"

namespace el {

sigset_t sigint_sigterm_sigset();

class SignalBlockGuard {
    NONCOPYABLE(SignalBlockGuard);
    // true after we unblocked the signals (so we don't try twice).
    bool unblocked_ = false;
    sigset_t blocked_set_;
public:
    explicit SignalBlockGuard(sigset_t sigset);
    expected<int, signalfd_error> make_signalfd();
    void unblock();
    ~SignalBlockGuard() noexcept(false);
};

class SignalFd;

using signalfd_read_promise = default_cancellable_promise<expected<uint32_t, read_error>>;

class SignalFd : private EpollRegistrantWithFd {
    NONCOPYABLE(SignalFd);

private:
    bool read_ready_ = true;

    signalfd_read_promise read_promise_;

public:
    // Expects to be passed a non-blocking signalfd
    SignalFd(Loop *loop, int fd) :
        EpollRegistrantWithFd(loop, fd, EpollInOut::in()),
        read_promise_() { }

    ~SignalFd() {
        // TODO: Could we cleanly cancel pending read and write operations?
    }

    cancellable_future<expected<uint32_t, read_error>> read();
    static cancellable_future<expected<close_errsv, epoll_ctl_error>> close(unique_ptr<SignalFd>&& signal_fd);

private:
    void on_update(Loop *loop, uint32_t events) override;
    void try_doing_read();
};

}  // namespace el

#endif  // TPF_EL_SIGNALFD_HPP
