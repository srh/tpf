#include <cstdio>

#include <fcntl.h>

#include "options.hpp"
#include "util.hpp"

#include "el/loop.hpp"
#include "el/pipe.hpp"

void echo_on_pipe(el::Loop *loop, unique_ptr<el::Pipe>&& pipe, std::move_only_function<void ()>&& on_complete) {
    // TODO: Implement.
    el::Pipe::close(loop, std::move(pipe), [MC(on_complete)](int close_errsv) mutable {
        if (close_errsv != 0) {
            // TODO: Janky error messaging.
            tpf_setupf("close() errored on pipe: %s", strerror_buf(close_errsv).msg());
        }
        on_complete();
    });
}

void go(el::Loop *loop, const Options& opts, std::move_only_function<void ()>&& on_complete) {
    tpf_setupf("go()...\n");

 try_again:
    // TODO: Can't open block?
    int fd = ::open(opts.fifo_path.c_str(), O_RDWR);
    if (fd == -1) {
        int errsv = errno;
        if (errsv == EINTR) {
            goto try_again;
        }
        // TODO: Pardon?  We're in an event loop that doesn't catch exceptions.
        throw clumsy_error("Error opening file "s + opts.fifo_path + ": " + strerror_buf(errsv).msg());
    }

    unique_ptr<el::Pipe> pipe = el::make_pipe_from_fd(loop, fd);

    echo_on_pipe(loop, std::move(pipe), [MC(on_complete)] mutable {
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
