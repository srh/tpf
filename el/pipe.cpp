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

    bool read_update = events & (EPOLLIN | EPOLLHUP | EPOLLERR);
    bool write_update = events & (EPOLLOUT | EPOLLHUP | EPOLLERR);

    if (read_update) {
        if (!read_ready_) {
            read_ready_ = true;
            if (waiting_read_cb_ != nullptr) {
                try_doing_read(loop, false);
            }
        }
    }
    if (write_update) {
        if (!write_ready_) {
            write_ready_ = true;
            if (waiting_write_cb_) {
                try_doing_write(loop, false);
            }
        }
    }
}

void Pipe::read(Loop *loop, void *buf, size_t nbytes, read_cb_type&& read_cb) {
    tpf_assert(loop == loop_);

    // The nbytes == 0 case has been considered.  Instead of scheduling the callback
    // immediately, we perform a ::read only once edge-triggered, which may reveal some
    // sort of error status.  (We could reverse this decision though.)
    tpf_assert(!waiting_read_cb_);
    tpf_assert(read_buf_ == nullptr);
    tpf_assert(read_nbytes_ == 0);
    waiting_read_cb_.swap(read_cb);
    read_buf_ = buf;
    read_nbytes_ = nbytes;

    if (read_ready_) {
        try_doing_read(loop, true);
    }
}

future<expected<ssize_t, read_error>> Pipe::read(Loop *loop, void *buf, size_t nbytes) {
    promise<expected<ssize_t, read_error>> prom;
    future<expected<ssize_t, read_error>> fut(&prom);
    read(loop, buf, nbytes, [MC(prom)](expected<ssize_t, read_error> result) mutable {
       prom.supply_value_and_detach(std::move(result));
    });
    return fut;
}

void Pipe::try_doing_read(Loop *loop, bool avoid_reentrancy) {
    tpf_setupf("Pipe::try_doing_read\n");
    tpf_assert(read_ready_);
    tpf_assert(waiting_read_cb_);
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

    read_cb_type cb;
    cb.swap(waiting_read_cb_);
    read_buf_ = nullptr;
    read_nbytes_ = 0;

    if (avoid_reentrancy) {
        loop->schedule([MC(cb), nbytes_expec] mutable { cb(std::move(nbytes_expec)); });
    } else {
        cb(std::move(nbytes_expec));
    }
}

void Pipe::write(Loop *loop, const void *buf, size_t nbytes, write_cb_type&& write_cb) {
    tpf_assert(loop == loop_);

    tpf_assert(!waiting_write_cb_);
    tpf_assert(write_buf_ == nullptr);
    tpf_assert(write_nbytes_ == 0);
    waiting_write_cb_.swap(write_cb);
    write_buf_ = buf;
    write_nbytes_ = nbytes;

    if (write_ready_) {
        try_doing_write(loop, true);
    }
}

void Pipe::try_doing_write(Loop *loop, bool avoid_reentrancy) {
    tpf_assert(write_ready_);
    tpf_assert(waiting_write_cb_);
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

    write_cb_type cb;
    cb.swap(waiting_write_cb_);
    write_buf_ = nullptr;
    write_nbytes_ = 0;

    if (avoid_reentrancy) {
        loop->schedule([MC(cb), nbytes_expec] mutable { cb(nbytes_expec); });
    } else {
        cb(nbytes_expec);
    }
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

void Pipe::close(Loop *loop, unique_ptr<Pipe>&& pipe, std::move_only_function<void (expected<close_errsv, epoll_ctl_error>)>&& close_cb) {
    tpf_assert(!pipe->waiting_read_cb_);
    tpf_assert(!pipe->waiting_write_cb_);
    tpf_assert(loop == pipe->loop_);

    auto errsv_expec = pipe->deregister_and_close();

    // Destruct pipe before callback.
    pipe.reset();

    loop->schedule([MC(close_cb), errsv_expec] mutable { close_cb(errsv_expec); });
}

}  // namespace el
