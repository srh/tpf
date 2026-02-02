#ifndef TPF_EL_LOOP_HPP
#define TPF_EL_LOOP_HPP

#include <unistd.h>

#include <utility>

#include "util.hpp"

namespace el {

struct Fd {
    int fd_ = -1;
    NONCOPYABLE(Fd);
    Fd(Fd&& other) {
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    Fd& operator=(Fd&& other) {
        Fd tmp(std::move(other));
        std::swap(fd_, tmp.fd_);
        return *this;
    }
    Fd() = default;
    ~Fd() {
        if (fd_ != -1) {
            int res = ::close(fd_);
            // Don't check result.  TODO: For any type of fd?  Any platform?
            (void)res;
            static_assert(__linux__);
        }
    }
    void init(int fd) {
        tpf_assert(empty());
        fd_ = fd;
    }
    bool empty() const {
        return fd_ == -1;
    }
    int get() const {
        return fd_;
    }
    int release() && {
        int ret = fd_;
        fd_ = -1;
        return ret;
    }
};

struct Loop {
    Fd epoll_fd_;
    NONCOPYABLE_MOVABLE(Loop);

    Loop();

};

}  // namespace el

#endif  // TPF_EL_LOOP_HPP
