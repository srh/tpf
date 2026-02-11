#include "el/pipe.hpp"

#include <cinttypes>
#include <fcntl.h>
#include <format>
#include <sys/stat.h>
#include <unistd.h>

#include "util.hpp"

namespace el {

// TODO: Move.
std::string format_epoll_events(uint32_t events) {
    std::string builder;
    uint32_t recognized_flags = 0;
#ifdef note_ev
#error "note_ev already defined"
#endif
#define note_ev(flag) if (events & flag) { builder += #flag "|"; recognized_flags |= flag; }
    note_ev(EPOLLIN);
    note_ev(EPOLLOUT);
    note_ev(EPOLLRDHUP);
    note_ev(EPOLLPRI);
    note_ev(EPOLLERR);
    note_ev(EPOLLHUP);
    // Non-event flags, but print if we have this pollution
    note_ev(EPOLLET);
    note_ev(EPOLLONESHOT);
    note_ev(EPOLLWAKEUP);
    note_ev(EPOLLEXCLUSIVE);
#undef note_ev
    // chop off last " |" if non-empty
    builder.resize(std::max<size_t>(builder.size(), 1) - 1);
    if (recognized_flags != events) {
        builder += "|" + std::to_string(events);
    }
    return builder;
}


expected<unique_ptr<Pipe>, message_error> make_pipe_from_fd(Loop *loop, int fd) {
    {
        // Assert the FD is a pipe/fifo.
        // TODO: Pipe could be used with stream sockets, too, almost.
        // Character devices (or some) seem not to like being edge-triggered; investigate if ever relevant
        struct stat statbuf;
        int stat_res = ::fstat(fd, &statbuf);
        if (stat_res == -1) {
            int errsv = errno;
            return unexpected(message_error("fstat of presumed pipe failed: "s + strerror_buf(errsv).msg()));
        }
        if (!S_ISFIFO(statbuf.st_mode)) {
            return unexpected(message_error{std::format("st_mode & S_IFMT is 0{:o}", S_IFMT & (size_t)statbuf.st_mode)});
        }
    }

    int flags = fcntl(fd, F_GETFL);
    if (!(flags & O_NONBLOCK)) {
        flags |= O_NONBLOCK;
        int res = fcntl(fd, F_SETFL, flags);
        if (res != 0) {
            int errsv = errno;
            return unexpected(message_error("fcntl of pipe failed: "s + strerror_buf(errsv).msg()));
        }
    }
    return make_unique<Pipe>(loop, fd);
}

void Pipe::on_update(Loop *loop, uint32_t events) {
    tpf_setupf("on_update has events %" PRIu32 ": %s\n", events, format_epoll_events(events).c_str());
    bool is_destructed = false;

    const bool read_update = events & (EPOLLIN | EPOLLHUP | EPOLLERR);
    const bool write_update = events & (EPOLLOUT | EPOLLHUP | EPOLLERR);

    if (read_update) {
        if (!read_ready_) {
            read_ready_ = true;
            if (read_promise_.is_active()) {
                destruct_pointer_ = &is_destructed;
                try_doing_read();
            }
        }
    }
    if (!is_destructed) {
        destruct_pointer_ = nullptr;
        if (write_update && !is_destructed) {
            if (!write_ready_) {
                write_ready_ = true;
                if (write_promise_.is_active()) {
                    // No need to update destruct_pointer_ here because we don't use is_destructed again.
                    try_doing_write();
                }
            }
        }
    }
}

future<expected<ssize_t, read_error>> Pipe::read(void *buf, size_t nbytes) {
    tpf_assert(read_promise_.is_default_constructed());
    tpf_assert(read_buf_ == nullptr);
    tpf_assert(read_nbytes_ == 0);
    future<expected<ssize_t, read_error>> fut(&read_promise_);
    read_buf_ = buf;
    read_nbytes_ = nbytes;

    if (read_ready_) {
        try_doing_read();
    }
    return fut;
}

void Pipe::try_doing_read() {
    tpf_setupf("Pipe::try_doing_read\n");
    tpf_assert(read_ready_);
    tpf_assert(read_promise_.is_active());
    tpf_assert(read_buf_ != nullptr);
 try_again:
    ssize_t res = ::read(fd_.get(), read_buf_, read_nbytes_);
    expected<ssize_t, read_error> nbytes_expec;
    if (res == -1) {
        int errsv = errno;
        tpf_setupf("Pipe::read: read returns %zd, errno = %d\n", res, errsv);
        if (errsv == EINTR) {
            goto try_again;
        }
        read_ready_ = false;
        if (errsv == EAGAIN || errsv == EWOULDBLOCK) {
            return;
        }
        nbytes_expec = unexpected(read_error{"read", errsv});
    } else {
        tpf_setupf("Pipe::read: read returns %zd\n", res);
        // Successful read case...
        if (__linux__) {
            // True for pipes: we don't need to read until EAGAIN.
            read_ready_ = (res == read_nbytes_);
        } else {
            // don't touch read_ready...
            // Actually, revisit this with other unices.
            static_assert(__linux__);
        }
        nbytes_expec.emplace(res);
    }

    read_buf_ = nullptr;
    read_nbytes_ = 0;
    std::move(read_promise_).supply_value_and_detach(std::move(nbytes_expec));
}

future<expected<ssize_t, write_error>> Pipe::write(const void *buf, size_t nbytes) {
    tpf_assert(write_promise_.is_default_constructed());
    tpf_assert(write_buf_ == nullptr);
    tpf_assert(write_nbytes_ == 0);
    future<expected<ssize_t, write_error>> fut(&write_promise_);
    write_buf_ = buf;
    write_nbytes_ = nbytes;

    if (write_ready_) {
        try_doing_write();
    }
    return fut;
}

void Pipe::try_doing_write() {
    tpf_assert(write_ready_);
    tpf_assert(write_promise_.is_active());
    tpf_assert(write_buf_ != nullptr);
 try_again:
    ssize_t res = ::write(fd_.get(), write_buf_, write_nbytes_);
    expected<ssize_t, write_error> nbytes_expec;
    if (res == -1) {
        int errsv = errno;
        if (errsv == EINTR) {
            goto try_again;
        }
        write_ready_ = false;
        if (errsv == EAGAIN || errsv == EWOULDBLOCK) {
            return;
        }
        nbytes_expec = unexpected(write_error{"write", errsv});
    } else {
        // Successful write case...
        if (__linux__) {
            // True for pipes: We don't need to write until EAGAIN.
            write_ready_ = (res == write_nbytes_);
        } else {
            // don't touch write_ready.  In fact as with read, revisit this on other
            // unices.
            static_assert(__linux__);
        }
        nbytes_expec.emplace(res);
    }

    write_buf_ = nullptr;
    write_nbytes_ = 0;
    std::move(write_promise_).supply_value_and_detach(std::move(nbytes_expec));
}

expected<close_errsv, epoll_ctl_error> Pipe::deregister_and_close() {
    int fd = std::move(fd_).release();
    auto result = loop_->unregister_for_epoll(this, fd);
    if (result.has_value()) {
        // TODO: Is close non-blocking?
        int res = ::close(fd);
        int errsv = res == 0 ? 0 : errno;

        static_assert(__linux__, "Linux-specific EINTR behavior here.");
        return close_errsv{errsv};
    } else {
        return unexpected(result.error());
    }
}

future<expected<close_errsv, epoll_ctl_error>> Pipe::close(unique_ptr<Pipe>&& pipe) {
    tpf_assert(pipe->read_promise_.is_default_constructed());
    tpf_assert(pipe->write_promise_.is_default_constructed());

    auto errsv_expec = pipe->deregister_and_close();

    // Destruct pipe before callba... returning.
    pipe.reset();

    return future<expected<close_errsv, epoll_ctl_error>>(std::move(errsv_expec));

}

}  // namespace el
