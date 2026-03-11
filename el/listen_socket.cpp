#include "el/listen_socket.hpp"

#include <cinttypes>
#include <sys/socket.h>

namespace el {

cancellable_future<expected<Fd, accept_error>> ListenSocket::accept() {
    tpf_assert(accept_promise_.is_default_constructed_but_for_completions());

    cancellable_future<expected<Fd, accept_error>> fut(&accept_promise_);

    if (accept_ready_) {
        try_doing_accept();
    }
    return fut;
}

void ListenSocket::on_update(Loop *loop, uint32_t events) {
    tpf_setupf("ListenSocket::on_update has events %" PRIu32 ": %s\n", events, format_epoll_events(events).c_str());
    const bool accept_update = events & (EPOLLIN | EPOLLHUP | EPOLLERR);
    if (accept_update) {
        if (!accept_ready_) {
            accept_ready_ = true;
            if (accept_promise_.is_active()) {
                try_doing_accept();
            }
        }
    }
}

void ListenSocket::try_doing_accept() {
    tpf_setupf("ListenSocket::try_doing_accept\n");
    tpf_assert(accept_ready_);
    tpf_assert(accept_promise_.is_active());

 try_again:
    struct sockaddr_storage storage;
    memset(&storage, 0, sizeof(storage));
    socklen_t addrlen = 0;
    int fd = ::accept4(fd_.get(), reinterpret_cast<struct sockaddr *>(&storage), &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
    expected<Fd, accept_error> sockfd_expec;
    if (fd == -1) {
        int errsv = errno;
        tpf_setupf("ListenSocket::accept: accept4 returns -1, errsv = %d\n", errsv);
        if (errsv == EINTR) {
            goto try_again;
        }
        accept_ready_ = false;
        if (errsv == EAGAIN || errsv == EWOULDBLOCK) {
            return;
        }
        sockfd_expec = unexpected(accept_error{"accept4", errsv});
    } else {
        tpf_setupf("ListenSocket::accept: accept4 returns fd %d\n", fd);
        sockfd_expec.emplace(fd);
    }

    std::move(accept_promise_).supply_value_and_detach(std::move(sockfd_expec));
}

cancellable_future<expected<close_errsv, epoll_ctl_error>> ListenSocket::close(unique_ptr<ListenSocket>&& listen_socket) {
    tpf_assert(listen_socket->accept_promise_.is_default_constructed_but_for_completions());

    auto errsv_expec = listen_socket->deregister_and_close();
    listen_socket.reset();

    return make_cancellable_future(std::move(errsv_expec));
}


}  // namespace el
