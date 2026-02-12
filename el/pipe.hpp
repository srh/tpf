#ifndef TPF_EL_PIPE_HPP
#define TPF_EL_PIPE_HPP

#include "el/fd_registrant.hpp"
#include "el/future.hpp"
#include "el/loop.hpp"

namespace el {

// One end of a pipe.
class Pipe : private EpollRegistrantWithFd {
    // Pipe is meant for use with unique_ptr.
    NONCOPYABLE(Pipe);

private:
    // We start by assuming they're ready for a first read/write.
    bool read_ready_ = true;
    bool write_ready_ = true;

    void *read_buf_ = nullptr;
    size_t read_nbytes_ = 0;
    promise<expected<ssize_t, read_error>> read_promise_;

    const void *write_buf_ = nullptr;
    size_t write_nbytes_ = 0;
    promise<expected<ssize_t, write_error>> write_promise_;

    // TODO: This is icky, very hackish.
    // TODO: Could point at a thread-local instead of nullptr, avoid conditional check.
    bool *destruct_pointer_ = nullptr;

public:
    // fd must be an O_NONBLOCK pipe fd
    Pipe(Loop *loop, int fd) : EpollRegistrantWithFd(loop, fd, EpollInOut::inout()) {
    }

    ~Pipe() noexcept(false) {
        if (destruct_pointer_ != nullptr) {
            tpf_assert(!*destruct_pointer_);
            *destruct_pointer_ = true;
            destruct_pointer_ = nullptr;
        }
        // TODO: Could we cleanly cancel pending read and write operations?
    }

    // TODO: These have to be interruptible.
    future<expected<ssize_t, read_error>> read(void *buf, size_t nbytes);
    future<expected<ssize_t, write_error>> write(const void *buf, size_t nbytes);
    static future<expected<close_errsv, epoll_ctl_error>> close(unique_ptr<Pipe>&& pipe);

private:
    void on_update(Loop *loop, uint32_t events) override;
    void try_doing_read();
    void try_doing_write();
};

[[nodiscard]] expected<unique_ptr<Pipe>, message_error> make_pipe_from_fd(Loop *loop, int fd);


}


#endif  // TPF_EL_PIPE_HPP
