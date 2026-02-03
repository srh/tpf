#include <cstdio>

#include <fcntl.h>
#include <span>

#include "options.hpp"
#include "util.hpp"

#include "el/loop.hpp"
#include "el/pipe.hpp"

struct Buf {
    std::unique_ptr<char[]> owned;
    size_t length = 0;
    explicit Buf(size_t n) : owned(new char[n]), length(n) { }
    std::span<char> get() const { return {owned.get(), length}; }
    char *ptr() const { return owned.get(); }
};


void echo_with_buf(el::Loop *loop, Buf&& buf, unique_ptr<el::Pipe>&& in_pipe, unique_ptr<el::Pipe>&& out_pipe, std::move_only_function<void ()>&& on_complete) {
    std::span<char> slice = buf.get();
    auto *pipe_ptr = in_pipe.get();
    pipe_ptr->read(loop, slice.data(), slice.size(), [MC(on_complete), MC(buf), loop, MC(in_pipe), MC(out_pipe)](int errsv, ssize_t nbytes) mutable {
        if (errsv != 0) {
            throw clumsy_error("read error "s + strerror_buf(errsv).msg());
        }
        if (nbytes == 0) {
            tpf_setupf("EOF.  Ending loop.\n");
            loop->schedule([loop, MC(in_pipe), MC(out_pipe), MC(on_complete)] mutable {
                el::Pipe::close(loop, std::move(in_pipe), [MC(on_complete)](int close_errsv) mutable {
                    if (close_errsv != 0) {
                        // TODO: Janky error messaging.
                        tpf_setupf("close() errored on pipe: %s\n", strerror_buf(close_errsv).msg());
                    }
                    on_complete();
                });
            });
        } else {
            tpf_setupf("Read %zu bytes: %.*s\n", nbytes, (int)nbytes, buf.ptr());
            char *buf_ptr = buf.ptr();
            auto *pipe_ptr = out_pipe.get();
            pipe_ptr->write(loop, buf_ptr, nbytes, [MC(on_complete), MC(buf), nbytes, loop, MC(in_pipe), MC(out_pipe)](int errsv, ssize_t written_nbytes) mutable {
                if (errsv != 0) {
                    throw clumsy_error("write error "s + strerror_buf(errsv).msg());
                }
                tpf_setupf("Wrote %zu bytes: %.*s\n", written_nbytes, (int)written_nbytes, buf.ptr());
                if (written_nbytes != nbytes) {
                    // TODO: Write all bytes in a loop.
                    // Uh, wouldn't we get an error?
                    throw clumsy_error("write was short"s);
                }
                // Possibly excessive ->schedule.
                loop->schedule([loop, MC(buf), MC(in_pipe), MC(out_pipe), MC(on_complete)] mutable {
                    echo_with_buf(loop, std::move(buf), std::move(in_pipe), std::move(out_pipe), std::move(on_complete));
                });
            });
        }
    });
}


void echo_on_pipe(el::Loop *loop, unique_ptr<el::Pipe>&& in_pipe, unique_ptr<el::Pipe>&& out_pipe, std::move_only_function<void ()>&& on_complete) {
    const size_t TOY_BUF_SIZE = 128;
    Buf buf(TOY_BUF_SIZE);
    echo_with_buf(loop, std::move(buf), std::move(in_pipe), std::move(out_pipe), std::move(on_complete));
}

unique_ptr<el::Pipe> open_pipe(el::Loop *loop, const char *path, int oflag) {
 try_again:
    // TODO: Can't open block?
    int fd = ::open(path, oflag | O_NONBLOCK | O_CLOEXEC);
    if (fd == -1) {
        int errsv = errno;
        if (errsv == EINTR) {
            goto try_again;
        }
        // TODO: Pardon?  We're in an event loop that doesn't catch exceptions.
        throw clumsy_error("Error opening file "s + path + ": " + strerror_buf(errsv).msg());
    }

    return el::make_pipe_from_fd(loop, fd);
}

void go(el::Loop *loop, const Options& opts, std::move_only_function<void ()>&& on_complete) {
    tpf_setupf("go()...\n");

    unique_ptr<el::Pipe> in_pipe = open_pipe(loop, opts.in_fifo_path.c_str(), O_RDONLY);
    unique_ptr<el::Pipe> out_pipe = open_pipe(loop, opts.out_fifo_path.c_str(), O_WRONLY);

    echo_on_pipe(loop, std::move(in_pipe), std::move(out_pipe), [MC(on_complete)] mutable {
        tpf_setupf("Echo completed.\n");
        on_complete();
    });
}

int main(int argc, const char **argv) {
    Options opts = parse_command_line(argc, argv);

    tpf_assert(argc > 0);
    printf("Hello, world!\n");

    {
        el::Loop loop;
        tpf_setupf("Constructed el::Loop\n");

        loop.schedule([&loop, MC(opts)] mutable { go(&loop, opts, [] {
            tpf_setupf("Finished.\n");
        }); });

        while (loop.has_stuff_to_do()) {
            loop.full_step();
        }
    }

    return 0;
}
