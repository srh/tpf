#ifndef TPF_EL_FD_HPP
#define TPF_EL_FD_HPP

#include "util.hpp"

namespace el {

class Fd {
private:
    int fd_ = -1;
public:
    NONCOPYABLE(Fd);
    Fd(Fd&& other) noexcept {
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    Fd& operator=(Fd&& other) noexcept {
        Fd tmp(std::move(other));
        std::swap(fd_, tmp.fd_);
        return *this;
    }
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_(fd) { }
    ~Fd() noexcept {
        if (fd_ != -1) {
            // TODO: Log or warn(?) or debug-log?  Stylistically I want us to use Fd::close().
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
    // Generally, this is "preferable" to letting the destructor run.
    [[nodiscard]] int close() && {
        tpf_assert(fd_ != -1);
        // Don't check result for EINTR.  TODO: For any type of fd?  Any platform?
        static_assert(__linux__);
        int res = ::close(fd_);
        fd_ = -1;
        return res;
    }
    [[nodiscard]] int release() && {
        int ret = fd_;
        fd_ = -1;
        return ret;
    }
};

}

#endif  // TPF_EL_FD_HPP
