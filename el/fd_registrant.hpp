#ifndef TPF_EL_FD_REGISTRANT_HPP
#define TPF_EL_FD_REGISTRANT_HPP

#include "el/loop.hpp"

namespace el {

class EpollRegistrantWithFd : public EpollRegistrant {
    NONCOPYABLE(EpollRegistrantWithFd);
protected:
    Fd fd_;

    EpollRegistrantWithFd(Loop *loop, int fd, EpollInOut read_write);
    ~EpollRegistrantWithFd() noexcept(false);

    [[nodiscard]] expected<close_errsv, epoll_ctl_error> deregister_and_close();
};


}  // namespace el

#endif  // TPF_EL_FD_REGISTRANT_HPP
