#include "el/signalfd.hpp"

#include <cinttypes>
#include <signal.h>
#include <sys/signalfd.h>

namespace el {

sigset_t sigint_sigterm_sigset() {
    sigset_t sigset;
    // TODO: Check errcodes.
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);
    return sigset;
}

// TODO: SignalBlockGuard could behave without exceptions.
SignalBlockGuard::SignalBlockGuard(sigset_t sigset) : blocked_set_(sigset) {
    int res = sigprocmask(SIG_BLOCK, &sigset, nullptr);
    if (res == -1) {
        int errsv = errno;
        throw std::runtime_error("sigprocmask failed: "s + strerror_buf(errsv).msg());
    }
}

expected<int, signalfd_error> SignalBlockGuard::make_signalfd() {
    tpf_assert(!unblocked_);
    // TODO: Cloexec?  Sure.
    int res = signalfd(-1, &blocked_set_, SFD_NONBLOCK | SFD_CLOEXEC);
    if (res == -1) {
        int errsv = errno;
        return unexpected(signalfd_error{"signalfd", errsv});
    }
    return res;
}

SignalBlockGuard::~SignalBlockGuard() noexcept(false) {
    if (!unblocked_) {
        unblock();
    }
}

void SignalBlockGuard::unblock() {
    // Always set unblocked_ without considering error, so destructor doesn't retry.
    unblocked_ = true;
    int res = sigprocmask(SIG_UNBLOCK, &blocked_set_, nullptr);
    if (res == -1) {
        int errsv = errno;
        throw std::runtime_error("sigprocmask failed to set mask: "s + strerror_buf(errsv).msg());
    }
}

cancellable_future<expected<uint32_t, read_error>> SignalFd::read() {
    tpf_assert(read_promise_.is_default_constructed_but_for_completions());

    cancellable_future<expected<uint32_t, read_error>> fut(&read_promise_);

    if (read_ready_) {
        try_doing_read();
    }
    return fut;
}

void SignalFd::on_update(Loop *loop, uint32_t events) {
    tpf_setupf("SignalFd::on_update has events %" PRIu32 ": %s\n", events, format_epoll_events(events).c_str());
    const bool read_update = events & (EPOLLIN | EPOLLHUP | EPOLLERR);
    if (read_update) {
        if (!read_ready_) {
            read_ready_ = true;
            if (read_promise_.is_active()) {
                try_doing_read();
            }
        }
    }
}

void SignalFd::try_doing_read() {
    tpf_setupf("SignalFd::try_doing_read\n");
    tpf_assert(read_ready_);
    tpf_assert(read_promise_.is_active());

    static_assert(sizeof(struct signalfd_siginfo) == 128);
    struct signalfd_siginfo read_buf;

try_again:
    ssize_t res = ::read(fd_.get(), &read_buf, sizeof(read_buf));
    expected<int, read_error> signals_expec;
    if (res == -1) {
        int errsv = errno;
        tpf_setupf("SignalFd::read: read returns %zd, errno = %d\n", res, errsv);
        if (errsv == EINTR) {
            goto try_again;
        }
        read_ready_ = false;
        if (errsv == EAGAIN || errsv == EWOULDBLOCK) {
            return;
        }
        signals_expec = unexpected(read_error{"read", errsv});
    } else {
        tpf_setupf("SignalFd::read: read returns %zd with ssi_signo %" PRIu32 "\n", res, read_buf.ssi_signo);
        // Successful read case...

        // read_ready_ remains untouched; even if we had read_buf being multiple
        // signalfd_siginfo structs, we could not infer read completeness from short reads
        // with signalfd (according to current documentation).

        // The OS is supposed to return us unsliced structs.
        tpf_assert(res == sizeof(struct signalfd_siginfo));
        uint32_t signo = read_buf.ssi_signo;
        signals_expec.emplace(signo);
    }

    std::move(read_promise_).supply_value_and_detach(std::move(signals_expec));
}

cancellable_future<expected<close_errsv, epoll_ctl_error>> SignalFd::close(unique_ptr<SignalFd>&& signal_fd) {
    tpf_assert(signal_fd->read_promise_.is_default_constructed_but_for_completions());

    auto errsv_expec = signal_fd->deregister_and_close();
    signal_fd.reset();

    return cancellable_future<expected<close_errsv, epoll_ctl_error>>(std::move(errsv_expec));
}

}  // namespace el
