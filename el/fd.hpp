#ifndef TPF_EL_FD_HPP
#define TPF_EL_FD_HPP

#include "util.hpp"

namespace el {

class Fd {
private:
    int fd_ = -1;
public:
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
    explicit Fd(int fd) : fd_(fd) { }
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
    bool has_value() const {
        return fd_ != -1;
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

}

#endif  // TPF_EL_FD_HPP