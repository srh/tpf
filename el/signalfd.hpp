#ifndef TPF_EL_SIGNALFD_HPP
#define TPF_EL_SIGNALFD_HPP

#include "util.hpp"
#include "el/fd_registrant.hpp"
#include "el/future.hpp"
#include "el/loop.hpp"

namespace el {

class SignalFd : private EpollRegistrantWithFd {
    NONCOPYABLE(SignalFd);

private:
    bool read_ready_ = true;

    promise<expected<uint32_t, read_error>> read_promise_;

public:
    // Expects to be passed a non-blocking signalfd
    SignalFd(Loop *loop, int fd) : EpollRegistrantWithFd(loop, fd, EpollInOut::in()) { }

    ~SignalFd() {
        // TODO: Could we cleanly cancel pending read and write operations?
    }

    future<expected<uint32_t, read_error>> read();
    static future<expected<close_errsv, epoll_ctl_error>> close(unique_ptr<SignalFd>&& signalfd);

private:
    void on_update(Loop *loop, uint32_t events) override;
    void try_doing_read();
};

}  // namespace el

#endif  // TPF_EL_SIGNALFD_HPP
