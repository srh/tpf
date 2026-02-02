#include "el/loop.hpp"

#include <sys/epoll.h>

#include <cerrno>

#include "util.hpp"

namespace el {

Loop::Loop() {
    int cloexec = EPOLL_CLOEXEC;  // TODO: Do we want CLOEXEC?
    int fd = epoll_create1(cloexec);

    if (fd == -1) {
        int errsv = errno;
        throw clumsy_error("epoll_create1 error: "s + strerror_buf(errsv).msg());
    }

    epoll_fd_.init(fd);
}

bool Loop::has_stuff_to_do() const {
    tpf_assert(!mid_step_);
    // TODO: It is possible that we have epoll registrants that are not waiting for
    // anything. We should return false or error about a resource leak (assuming there are
    // no enqueued actions, etc.) in that case.
    return (!enqueued_actions_.empty() || !registrants_.empty());
}

void Loop::register_for_epoll(EpollRegistrant *registrant, int fd, EpollInOut inout) {
    tpf_assert(this->epoll_fd_.has_value());
    tpf_assert(registrant->registered_index_ == SIZE_MAX);
    struct epoll_event event = {
        .events = static_cast<uint32_t>(EPOLLIN | EPOLLOUT | EPOLLET),
        .data = epoll_data_t { .ptr = registrant },
    };
    int res = epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &event);
    if (res != 0) {
        int errsv = errno;
        throw clumsy_error(strerror_buf(errsv).msg());
    }

    registrant->loop_ = this;
    registrant->registered_index_ = registrants_.size();
    registrants_.push_back(registrant);
}

void Loop::unregister_for_epoll(EpollRegistrant *registrant, int fd) {
    tpf_assert(registrant->loop_ != nullptr);
    Loop *loop = registrant->loop_;
    tpf_assert(loop->epoll_fd_.has_value());
    tpf_assert(registrant->registered_index_ != SIZE_MAX);
    // event is ignored, but must be non-null before Linux 2.6.9.
    struct epoll_event event = { 0 };
    int res = epoll_ctl(loop->epoll_fd_.get(), EPOLL_CTL_DEL, fd, &event);
    if (res != 0) {
        int errsv = errno;
        throw clumsy_error(strerror_buf(errsv).msg());
    }

    size_t index = registrant->registered_index_;
    loop->registrants_.back()->registered_index_ = index;
    loop->registrants_[index] = loop->registrants_.back();
    loop->registrants_.pop_back();

    registrant->registered_index_ = SIZE_MAX;
    registrant->loop_ = nullptr;
}

void Loop::handle_wakeups(bool blocking_wait) {
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
        throw clumsy_error("epoll_wait error: "s + strerror_buf(errsv).msg());
    }

    for (int i = 0; i < res; ++i) {
        struct epoll_event *event = &events[i];

        // TODO: Think twice about whether this is allowed to invoke callbacks here. What
        // if we're reading and writing from the same fd, and one callback destroys the
        // object?
        auto *registrant = static_cast<EpollRegistrant *>(event->data.ptr);
        registrant->on_update(this, event->events);
    }

}

void Loop::full_step() {
    tpf_assert(!mid_step_);
    mid_step_ = true;

    bool blocking_wait = enqueued_actions_.empty();

    handle_wakeups(blocking_wait);

    decltype(enqueued_actions_) actions;
    actions.swap(enqueued_actions_);
    for (auto& action : actions) {
        std::move_only_function<void ()> ac;
        ac.swap(action);
        ac();
        // ac is destroyed immediately after running
        // TODO(alloc) don't use std::move_only_function
    }
    // TODO(alloc) we destruct actions (and regrow enqueued_actions_).

    mid_step_ = false;
}

void Loop::schedule(std::move_only_function<void ()>&& action) {
    enqueued_actions_.push_back(std::move(action));
}

}  // namespace el
