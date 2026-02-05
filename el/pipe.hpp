#ifndef TPF_EL_PIPE_HPP
#define TPF_EL_PIPE_HPP

#include "el/loop.hpp"

namespace el {

// One end of a pipe.
class Pipe : private EpollRegistrant {
    // Pipe is meant for use with unique_ptr.
    NONCOPYABLE(Pipe);

private:
    Fd fd_;
    // We start by assuming they're ready for a first read/write.
    bool read_ready_ = true;
    bool write_ready_ = true;

    void *read_buf_ = nullptr;
    size_t read_nbytes_ = 0;
    using read_cb_type = std::move_only_function<void (expected<ssize_t, read_error>)>;
    read_cb_type waiting_read_cb_;

    const void *write_buf_ = nullptr;
    size_t write_nbytes_ = 0;
    using write_cb_type = std::move_only_function<void (expected<ssize_t, write_error>)>;
    write_cb_type waiting_write_cb_;

public:
    // fd must be an O_NONBLOCK pipe fd
    Pipe(Loop *loop, int fd) : fd_(fd) {
        expected<void, epoll_ctl_error> result = loop->register_for_epoll(this, fd, EpollInOut::inout());
        if (!result.has_value()) {
            // epoll_ctl is supposed to succeed... just throw.
            throw std::runtime_error(result.error().make_msg());
        }
    }

    ~Pipe() noexcept(false) {
        // TODO: Could we cleanly cancel pending read and write operations?
        if (fd_.has_value()) {
            expected<close_errsv, epoll_ctl_error> result = deregister_and_close();
            if (!result.has_value()) {
                // epoll_ctl with EPOLL_CTL_DEL is really supposed to succeed... just throw, I mean terminate.
                throw std::runtime_error(result.error().make_msg());
            }
            // I guess we ignore the close_errsv here.
        }
        tpf_assert(!fd_.has_value());
    }

    // TODO: These have to be interruptible.
    void read(Loop *loop, void *buf, size_t nbytes, read_cb_type&& read_cb);
    void write(Loop *loop, const void *buf, size_t nbytes, write_cb_type&& write_cb);
    static void close(Loop *loop, unique_ptr<Pipe>&& pipe, std::move_only_function<void (expected<close_errsv, epoll_ctl_error>)>&& close_cb);

private:
    void on_update(Loop *loop, uint32_t events) override;
    void try_doing_read(Loop *loop, bool avoid_reentrancy);
    void try_doing_write(Loop *loop, bool avoid_reentrancy);
    [[nodiscard]] expected<close_errsv, epoll_ctl_error> deregister_and_close();
};

[[nodiscard]] expected<unique_ptr<Pipe>, message_error> make_pipe_from_fd(Loop *loop, int fd);


}


#endif  // TPF_EL_PIPE_HPP
