#include "el/fd_registrant.hpp"

namespace el {

EpollRegistrantWithFd::EpollRegistrantWithFd(Loop *loop, int fd, EpollInOut read_write) : fd_(fd) {
    expected<void, epoll_ctl_error> result = loop->register_for_epoll(this, fd, read_write);
    if (!result.has_value()) {
        // epoll_ctl is supposed to succeed... just throw.
        throw std::runtime_error(result.error().make_msg());
    }
}
EpollRegistrantWithFd::~EpollRegistrantWithFd() noexcept(false) {
    // TODO: Could we cleanly cancel pending read and write operations?
    if (fd_.has_value()) {
        expected<close_errsv, epoll_ctl_error> result = deregister_and_close();
        if (!result.has_value()) {
            // epoll_ctl with EPOLL_CTL_DEL is really supposed to succeed... just throw, I mean terminate.
            throw std::runtime_error(result.error().make_msg());
        }
        // I guess we ignore the close_errsv here.
    }
}

[[nodiscard]]
expected<close_errsv, epoll_ctl_error> EpollRegistrantWithFd::deregister_and_close() {
    int fd = std::move(fd_).release();
    auto result = loop_->unregister_for_epoll(this, fd);
    if (result.has_value()) {
        // TODO: Is close non-blocking?
        int res = ::close(fd);
        int errsv = res == 0 ? 0 : errno;

        static_assert(__linux__, "Linux-specific EINTR behavior here.");
        return close_errsv{errsv};
    } else {
        return unexpected(result.error());
    }
}

}  // namespace el
