#ifndef TPF_EL_PIPE_HPP
#define TPF_EL_PIPE_HPP

#include "el/loop.hpp"

namespace el {

// One end of a pipe.
struct Pipe : EpollRegistrant {
    // Pipe is meant for use with unique_ptr.
    NONCOPYABLE(Pipe);
    Fd fd_;
    // We start by assuming they're ready for a first read/write.
    bool read_ready_ = true;
    bool write_ready_ = true;

    void *read_buf_ = nullptr;
    size_t read_nbytes_ = 0;
    using read_cb_type = std::move_only_function<void (int, ssize_t)>;
    read_cb_type waiting_read_cb_;

    const void *write_buf_ = nullptr;
    size_t write_nbytes_ = 0;
    using write_cb_type = std::move_only_function<void (int, ssize_t)>;
    write_cb_type waiting_write_cb_;

    // fd must be an O_NONBLOCK pipe fd
    Pipe(Loop *loop, int fd) : fd_(fd) {
        loop->register_for_epoll(this, fd, EpollInOut::inout());
    }

    ~Pipe() {
        // We do this why?  Cleaner errors if we throw a clumsy_exception?
        // TODO: Could we cleanly cancel pending read and write operations?
        if (fd_.has_value()) {
            deregister_and_close();
        }
        tpf_assert(!fd_.has_value());
    }

    void on_update(Loop *loop, uint32_t events) override;

    // TODO: These have to be interruptible.
    void read(Loop *loop, void *buf, size_t nbytes, read_cb_type&& read_cb);
    void write(Loop *loop, const void *buf, size_t nbytes, write_cb_type&& write_cb);
    static void close(Loop *loop, unique_ptr<Pipe>&& pipe, std::move_only_function<void (int)>&& close_cb);

private:
    void try_doing_read(Loop *loop);
    void try_doing_write(Loop *loop);
    int deregister_and_close();
};

unique_ptr<Pipe> make_pipe_from_fd(Loop *loop, int fd);


}


#endif  // TPF_EL_PIPE_HPP
