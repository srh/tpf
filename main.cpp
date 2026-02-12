#include <fcntl.h>

#include <cstdio>
#include <expected>
#include <span>

#include "options.hpp"
#include "util.hpp"

#include "el/future.hpp"
#include "el/loop.hpp"
#include "el/pipe.hpp"

struct Buf {
    std::unique_ptr<char[]> owned;
    size_t length = 0;
    explicit Buf(size_t n) : owned(new char[n]), length(n) { }
    std::span<char> get() const { return {owned.get(), length}; }
    char *ptr() const { return owned.get(); }
};


el::future<expected<void, message_error>> echo_with_buf(el::Loop *loop, Buf&& buf, unique_ptr<el::Pipe>&& in_pipe, unique_ptr<el::Pipe>&& out_pipe) {
    std::span<char> slice = buf.get();
    auto read_fut = in_pipe->read(slice.data(), slice.size());
    return std::move(read_fut).then([loop, MC(buf), MC(in_pipe), MC(out_pipe)](expected<ssize_t, read_error>&& nbytes) mutable {
        if (!nbytes.has_value()) {
            return el::future{expected<void, message_error>(unexpected(message_error(nbytes.error())))};
        }
        if (nbytes == 0) {
            tpf_setupf("EOF.  Ending loop.\n");
            return el::Pipe::close(std::move(in_pipe))
            .then( [](expected<close_errsv, epoll_ctl_error>&& errsv_expec) mutable {
                if (!errsv_expec.has_value()) {
                    return el::future{expected<void, message_error>(unexpected(message_error(errsv_expec.error())))};
                }
                if (errsv_expec.value().errsv != 0) {
                    // TODO: Janky error messaging.
                    tpf_setupf("close() errored on pipe: %s\n", strerror_buf(errsv_expec.value().errsv).msg());
                }
                return el::future{expected<void, message_error>()};
            });
        } else {
            tpf_setupf("Read %zu bytes: %.*s\n", nbytes.value(), (int)nbytes.value(), buf.ptr());
            auto write_fut = out_pipe->write(buf.ptr(), nbytes.value());
            return std::move(write_fut).then([MC(buf), nbytes, loop, MC(in_pipe), MC(out_pipe)](expected<ssize_t, write_error>&& nbytes_expec) mutable {
                if (!nbytes_expec.has_value()) {
                    return el::future{expected<void, message_error>(unexpected(message_error{nbytes_expec.error()}))};
                }
                size_t written_nbytes = static_cast<size_t>(nbytes_expec.value());
                tpf_setupf("Wrote %zu bytes: %.*s\n", written_nbytes, (int)written_nbytes, buf.ptr());
                if (written_nbytes != nbytes) {
                    // TODO: Write all bytes in a loop.
                    // Uh, wouldn't we get an error?
                    return el::future{expected<void, message_error>(unexpected(message_error{"write was short"}))};
                }
                // TODO: We might be infinitely recursing here.
                return echo_with_buf(loop, std::move(buf), std::move(in_pipe), std::move(out_pipe));
            });
        }
    });
}


el::future<expected<void, message_error>> echo_on_pipe(el::Loop *loop, unique_ptr<el::Pipe>&& in_pipe, unique_ptr<el::Pipe>&& out_pipe) {
    const size_t TOY_BUF_SIZE = 128;
    Buf buf(TOY_BUF_SIZE);
    return echo_with_buf(loop, std::move(buf), std::move(in_pipe), std::move(out_pipe));
}

expected<unique_ptr<el::Pipe>, message_error> open_pipe(el::Loop *loop, const char *path, int oflag) {
 try_again:
    // TODO: Can't open block?
    int fd = ::open(path, oflag | O_NONBLOCK | O_CLOEXEC);
    if (fd == -1) {
        int errsv = errno;
        if (errsv == EINTR) {
            goto try_again;
        }
        return unexpected(message_error{"Error opening file "s + path + ": " + strerror_buf(errsv).msg()});
    }

    return el::make_pipe_from_fd(loop, fd);
}

el::future<expected<void, message_error>> go(el::Loop *loop, const Options& opts) {
    tpf_setupf("go()...\n");

    auto in_pipe_expec = open_pipe(loop, opts.in_fifo_path.c_str(), O_RDONLY);
    if (!in_pipe_expec.has_value()) {
        return el::future{expected<void, message_error>(unexpected(in_pipe_expec.error()))};
    }
    // Silly(?) experiment using references with .value() on expected<_,_>.
    unique_ptr<el::Pipe>& in_pipe = in_pipe_expec.value();
    auto out_pipe_expec = open_pipe(loop, opts.out_fifo_path.c_str(), O_WRONLY);
    if (!out_pipe_expec.has_value()) {
        return el::future{expected<void, message_error>(unexpected(out_pipe_expec.error()))};
    }
    unique_ptr<el::Pipe>& out_pipe = out_pipe_expec.value();

    return echo_on_pipe(loop, std::move(in_pipe), std::move(out_pipe))
    .then([](expected<void, message_error>&& err) mutable {
        tpf_setupf("Echo completed...\n");
        return el::future{std::move(err)};
    });
}

int main(int argc, const char **argv) {
    expected<Options, message_error> opts_expec = parse_command_line(argc, argv);
    if (!opts_expec.has_value()) {
        tpf_setupf("Command line parsing failed: %s\n", opts_expec.error().c_str());
    }
    Options& opts = opts_expec.value();

    tpf_assert(argc > 0);
    printf("Hello, world!\n");

    {
        expected<int, epoll_create_error> fd_expec = el::Loop::make_epoll_fd();
        if (!fd_expec.has_value()) {
            tpf_setupf("%s\n", fd_expec.error().make_msg().c_str());
            return 1;
        }
        el::Loop loop(fd_expec.value());
        tpf_setupf("Constructed el::Loop\n");

        loop.schedule([&loop, MC(opts)] mutable {
            go(&loop, opts)
            .wait_with_callback([](expected<void, message_error> void_expec) {
                if (!void_expec.has_value()) {
                    tpf_setupf("Finished with error: %s\n", void_expec.error().msg().c_str());
                } else {
                    tpf_setupf("Finished.\n");
                }
            });
        });

        while (loop.has_stuff_to_do()) {
            expected<void, epoll_wait_error> result = loop.full_step();
            if (!result.has_value()) {
                tpf_setupf("Exiting loop early with epoll_wait_error: %s\n", result.error().make_msg().c_str());
                return 1;
            }
        }
    }

    return 0;
}
