#ifndef TPF_EL_LISTEN_SOCKET_HPP
#define TPF_EL_LISTEN_SOCKET_HPP

#include "el/fd_registrant.hpp"
#include "el/future.hpp"
#include "el/loop.hpp"

namespace el {

class ListenSocket;

using listen_socket_accept_promise = default_cancellable_promise<expected<Fd, accept_error>>;

class ListenSocket : private EpollRegistrantWithFd {
    // Like Pipe, is meant for use with unique_ptr.
    NONCOPYABLE(ListenSocket);

private:
    bool accept_ready_ = true;
    listen_socket_accept_promise accept_promise_;

public:
    ListenSocket(Loop *loop, int fd) :
        EpollRegistrantWithFd(loop, fd, EpollInOut::in()),
        accept_promise_{} {}

    ~ListenSocket() noexcept(false) {
        // TODO: Could we cleanly cancel pending read and write operations?
    }

    cancellable_future<expected<Fd, accept_error>> accept();

    static cancellable_future<expected<close_errsv, epoll_ctl_error>> close(unique_ptr<ListenSocket>&& listen_socket);

private:
    void on_update(Loop *loop, uint32_t events) override;
    void try_doing_accept();
};

}  // namespace el

#endif  // TPF_EL_LISTEN_SOCKET_HPP