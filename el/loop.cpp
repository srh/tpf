#include "el/loop.hpp"

#include <sys/epoll.h>

#include <cerrno>

#include "util.hpp"

namespace el {

Loop::Loop() {
    int cloexec = EPOLL_CLOEXEC;  // TODO: ?
    int fd = epoll_create1(cloexec);

    if (fd == -1) {
        int errsv = errno;
        throw clumsy_error("epoll_create1 error: "s + strerror_buf(errsv).msg());
    }

    epoll_fd_.init(fd);
}

}  // namespace el
