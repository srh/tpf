#ifndef TPF_EL_PIPE_HPP
#define TPF_EL_PIPE_HPP

#include "el/fd_registrant.hpp"
#include "el/future.hpp"
#include "el/loop.hpp"

namespace el {

class Pipe;

// TODO (never?): Instead of pipe_, we could use offset-of-field pointer arithmetic to find Pipe *.
class pipe_read_promise : public cancellable_promise<expected<ssize_t, read_error>> {
    NONCOPYABLE(pipe_read_promise);
    friend class Pipe;
    void cancel() override;
    explicit pipe_read_promise(Pipe *pipe) : pipe_(pipe) {}
    Pipe *const pipe_;
};

class pipe_write_promise : public cancellable_promise<expected<ssize_t, write_error>> {
    NONCOPYABLE(pipe_write_promise);
    friend class Pipe;
    void cancel() override;
    explicit pipe_write_promise(Pipe *pipe) : pipe_(pipe) {}
    Pipe *const pipe_;
};

// One end of a pipe.
class Pipe : private EpollRegistrantWithFd {
    // Pipe is meant for use with unique_ptr.
    NONCOPYABLE(Pipe);

private:
    friend class pipe_read_promise;
    friend class pipe_write_promise;
    // We start by assuming they're ready for a first read/write.
    bool read_ready_ = true;
    bool write_ready_ = true;

    void *read_buf_ = nullptr;
    size_t read_nbytes_ = 0;
    pipe_read_promise read_promise_;

    const void *write_buf_ = nullptr;
    size_t write_nbytes_ = 0;
    pipe_write_promise write_promise_;

    // TODO: This is icky, very hackish.
    // TODO: Could point at a thread-local instead of nullptr, avoid conditional check.
    bool *destruct_pointer_ = nullptr;

public:
    // fd must be an O_NONBLOCK pipe fd
    Pipe(Loop *loop, int fd) : EpollRegistrantWithFd(loop, fd, EpollInOut::inout()), read_promise_{this}, write_promise_{this} {
    }

    ~Pipe() noexcept(false) {
        if (destruct_pointer_ != nullptr) {
            tpf_assert(!*destruct_pointer_);
            *destruct_pointer_ = true;
            destruct_pointer_ = nullptr;
        }
        // TODO: Could we cleanly cancel pending read and write operations?
    }

    cancellable_future<expected<ssize_t, read_error>> read(void *buf, size_t nbytes);
    cancellable_future<expected<ssize_t, write_error>> write(const void *buf, size_t nbytes);
    static cancellable_future<expected<close_errsv, epoll_ctl_error>> close(unique_ptr<Pipe>&& pipe);

private:
    void on_update(Loop *loop, uint32_t events) override;
    void try_doing_read();
    void try_doing_write();
};

[[nodiscard]] expected<unique_ptr<Pipe>, message_error> make_pipe_from_fifo(Loop *loop, Fd&& fd);
[[nodiscard]] expected<unique_ptr<Pipe>, message_error> make_pipe_from_sockfd(Loop *loop, Fd&& fd);
[[nodiscard]] expected<unique_ptr<Pipe>, message_error> make_pipe_or_socket_from_fd(Loop *loop, Fd&& fd, uintmax_t fd_type);

}


#endif  // TPF_EL_PIPE_HPP
