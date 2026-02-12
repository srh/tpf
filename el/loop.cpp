#include "el/loop.hpp"

#include <sys/epoll.h>

#include <cerrno>

#include "util.hpp"

namespace el {

expected<int, epoll_create_error> Loop::make_epoll_fd() {
    int cloexec = EPOLL_CLOEXEC;  // TODO: Do we want CLOEXEC?
    int fd = epoll_create1(cloexec);

    if (fd == -1) {
        int errsv = errno;
        return unexpected(epoll_create_error{"epoll_create1", errsv});
    }

    return fd;
}

Loop::Loop(int epoll_fd) : epoll_fd_{epoll_fd} {
}

bool Loop::has_stuff_to_do() const {
    tpf_assert(!mid_step_);
    // TODO: It is possible that we have epoll registrants that are not waiting for
    // anything. We should return false or error about a resource leak (assuming there are
    // no enqueued actions, etc.) in that case.
    return (!enqueued_actions_.empty() || !registrants_.empty());
}

expected<void, epoll_ctl_error> Loop::register_for_epoll(EpollRegistrant *registrant, int fd, EpollInOut inout) {
    tpf_assert(this->epoll_fd_.has_value());
    tpf_assert(registrant->registered_index_ == SIZE_MAX);
    struct epoll_event event = {
        .events = static_cast<uint32_t>(EPOLLIN | EPOLLOUT | EPOLLET),
        .data = epoll_data_t { .ptr = registrant },
    };
    int res = epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &event);
    if (res != 0) {
        int errsv = errno;
        return unexpected(epoll_ctl_error{"EPOLL_CTL_ADD", errsv});
    }

    registrant->loop_ = this;
    registrant->registered_index_ = registrants_.size();
    registrants_.push_back(registrant);
    return expected<void, epoll_ctl_error>();
}

expected<void, epoll_ctl_error> Loop::unregister_for_epoll(EpollRegistrant *registrant, int fd) {
    tpf_assert(registrant->loop_ != nullptr);
    Loop *loop = registrant->loop_;
    tpf_assert(loop->epoll_fd_.has_value());
    tpf_assert(registrant->registered_index_ != SIZE_MAX);
    // event is ignored, but must be non-null before Linux 2.6.9.
    struct epoll_event event = { 0 };
    int res = epoll_ctl(loop->epoll_fd_.get(), EPOLL_CTL_DEL, fd, &event);
    if (res != 0) {
        int errsv = errno;
        return unexpected(epoll_ctl_error{"EPOLL_CTL_DEL", errsv});
    }

    size_t index = registrant->registered_index_;
    loop->registrants_.back()->registered_index_ = index;
    loop->registrants_[index] = loop->registrants_.back();
    loop->registrants_.pop_back();

    registrant->registered_index_ = SIZE_MAX;
    registrant->loop_ = nullptr;
    return expected<void, epoll_ctl_error>();
}

expected<void, epoll_wait_error> Loop::handle_wakeups(bool blocking_wait) {
    tpf_assert(epoll_fd_.has_value());

    const int MAXEVENTS = 2048;
    struct epoll_event events[MAXEVENTS];

 try_again:
    int res = epoll_wait(epoll_fd_.get(), events, MAXEVENTS, blocking_wait ? -1 : 0);

    if (res == -1) {
        int errsv = errno;
        if (errsv == EINTR) {
            // Signal interruption.  res would be 0 if this were a timeout.
            goto try_again;
        }
        return unexpected(epoll_wait_error{"epoll_wait", errsv});
    }

    for (int i = 0; i < res; ++i) {
        struct epoll_event *event = &events[i];

        // TODO: Think twice about whether this is allowed to invoke callbacks here. What
        // if we're reading and writing from the same fd, and one callback destroys the
        // object?
        auto *registrant = static_cast<EpollRegistrant *>(event->data.ptr);
        registrant->on_update(this, event->events);
    }

    return expected<void, epoll_wait_error>();
}

expected<void, epoll_wait_error> Loop::full_step() {
    tpf_assert(!mid_step_);
    mid_step_ = true;

    bool blocking_wait = enqueued_actions_.empty();

    auto result = handle_wakeups(blocking_wait);
    if (!result.has_value()) {
        return result;
    }

    tpf_assert(spare_actions_buf_.empty());
    spare_actions_buf_.swap(enqueued_actions_);
    for (auto& action : spare_actions_buf_) {
        std::move_only_function<void ()> ac;
        ac.swap(action);
        ac();
        // ac is destroyed immediately after running
        // TODO(alloc) don't use std::move_only_function
    }
    spare_actions_buf_.clear();

    mid_step_ = false;
    return expected<void, epoll_wait_error>();
}

void Loop::schedule(std::move_only_function<void ()>&& action) {
    enqueued_actions_.push_back(std::move(action));
}

}  // namespace el
