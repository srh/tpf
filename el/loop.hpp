#ifndef TPF_EL_LOOP_HPP
#define TPF_EL_LOOP_HPP

#include <cstdint>
#include <unistd.h>

#include <utility>
#include <vector>
#include <bits/move_only_function.h>
#include <sys/epoll.h>

#include "util.hpp"

namespace el {
struct Loop;

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

struct EpollRegistrant {
    Loop *loop_ = nullptr;
    size_t registered_index_ = SIZE_MAX;
    virtual void on_update(Loop *loop, uint32_t events) = 0;
};

struct EpollInOut {
    uint32_t events = 0;
    static EpollInOut in() { return { .events = EPOLLIN }; }
    static EpollInOut out() { return { .events = EPOLLOUT }; }
    static EpollInOut inout() { return { .events = EPOLLIN | EPOLLOUT }; }
};

struct Loop {
    Fd epoll_fd_;
    bool mid_step_ = false;

    // registrants_ have back-pointing indices (registered_index_) that need to be maintained
    std::vector<EpollRegistrant *> registrants_;
    // TODO: Avoid use of std::move_only_function (generally)
    std::vector<std::move_only_function<void ()>> enqueued_actions_;

    // Not movable as long as EpollRegistrant has pointers to us.
    NONCOPYABLE(Loop);

    Loop();

    bool has_stuff_to_do() const;
    void full_step();

    void schedule(std::move_only_function<void ()>&& action);

    void register_for_epoll(EpollRegistrant *registrant, int fd, EpollInOut inout);
    static void unregister_for_epoll(EpollRegistrant *registrant, int fd);

private:
    void handle_wakeups(bool blocking_wait);
};

}  // namespace el

#endif  // TPF_EL_LOOP_HPP
