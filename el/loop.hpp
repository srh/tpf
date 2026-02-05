#ifndef TPF_EL_LOOP_HPP
#define TPF_EL_LOOP_HPP

#include <sys/epoll.h>
#include <unistd.h>

#include <cstdint>
#include <utility>
#include <vector>
#include <functional>

#include "fd.hpp"
#include "util.hpp"

namespace el {
class Loop;

class EpollRegistrant {
protected:
    // Protected, not private, only because we use loop == loop_ in an assertion.
    friend class Loop;
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

class Loop {
private:
    Fd epoll_fd_;
    bool mid_step_ = false;

    // registrants_ have back-pointing indices (registered_index_) that need to be
    // maintained
    std::vector<EpollRegistrant *> registrants_;
    // TODO: Avoid use of std::move_only_function (generally)
    std::vector<std::move_only_function<void ()>> enqueued_actions_;

public:
    // Not movable as long as EpollRegistrant has pointers to us.
    NONCOPYABLE(Loop);

    [[nodiscard]] static expected<int, epoll_create_error> make_epoll_fd();
    explicit Loop(int epoll_fd);

    bool has_stuff_to_do() const;
    [[nodiscard]] expected<void, epoll_wait_error> full_step();

    void schedule(std::move_only_function<void ()>&& action);

    [[nodiscard]] expected<void, epoll_ctl_error> register_for_epoll(EpollRegistrant *registrant, int fd, EpollInOut inout);
    [[nodiscard]] static expected<void, epoll_ctl_error> unregister_for_epoll(EpollRegistrant *registrant, int fd);

private:
    [[nodiscard]] expected<void, epoll_wait_error> handle_wakeups(bool blocking_wait);
};

}  // namespace el

#endif  // TPF_EL_LOOP_HPP
