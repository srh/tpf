#include <fcntl.h>
#include <sys/types.h>
#include <netdb.h>
#include <sys/socket.h>

#include <csignal>
#include <cstdio>
#include <expected>
#include <span>
#include <variant>

#include "options.hpp"
#include "util.hpp"

#include "el/future.hpp"
#include "el/listen_socket.hpp"
#include "el/loop.hpp"
#include "el/pipe.hpp"
#include "el/signalfd.hpp"
#include "el/wait_any.hpp"

using el::future;
using el::cancellable_future;
using el::self_cancellable_future;
using el::Unit;
using el::interrupt_future;

struct Context {
    el::Loop *loop;
    interrupt_future *interruptor;

    Context(el::Loop *_loop, interrupt_future *_interruptor) : loop(_loop), interruptor(_interruptor) {}
    Context with_interruptor(interrupt_future *replacement_interruptor) const {
        return Context(loop, replacement_interruptor);
    }
};

struct Buf {
    std::unique_ptr<char[]> owned;
    size_t length = 0;
    explicit Buf(size_t n) : owned(new char[n]), length(n) { }
    std::span<char> get() const { return {owned.get(), length}; }
    char *ptr() const { return owned.get(); }
};

struct write_all_error {
    size_t bytes_written;
    std::variant<write_error, el::interrupt_result> err;
};

// TODO: Do this natively (in Pipe) without the callback fluff.
// TODO: If we do cancel, we should have some means of returning the partially completed write's byte count.
future<expected<void, write_all_error>> write_all_helper(Context ctx, el::Pipe *pipe, const void *buf, ssize_t count, size_t bytes_written) {
    auto write_fut = el::wait_interruptible(ctx.interruptor, pipe->write(buf, count));

    return std::move(write_fut).then([ctx, pipe, buf, count, bytes_written](expected<expected<ssize_t, write_error>, el::interrupt_result>&& nbytes_expec_ir) mutable {
        if (!nbytes_expec_ir.has_value()) {
            return el::make_future(expected<void, write_all_error>(unexpected(write_all_error{bytes_written, nbytes_expec_ir.error()})));
        }
        auto& nbytes_expec = nbytes_expec_ir.value();
        if (!nbytes_expec.has_value()) {
            return el::make_future(expected<void, write_all_error>(unexpected(write_all_error{bytes_written, nbytes_expec.error()})));
        }

        size_t written_nbytes = static_cast<size_t>(nbytes_expec.value());
        tpf_assert(written_nbytes <= count);
        if (written_nbytes == count) {
            return el::make_future(expected<void, write_all_error>());
        }

        const char *charbuf = static_cast<const char *>(buf);
        charbuf += written_nbytes;
        size_t remaining_count = count - written_nbytes;
        return write_all_helper(ctx, pipe, charbuf, remaining_count, bytes_written + written_nbytes);
    });
}

future<expected<void, write_all_error>> write_all(Context ctx, el::Pipe *pipe, const void *buf, ssize_t count) {
    size_t bytes_written = 0;
    return write_all_helper(ctx, pipe, buf, count, bytes_written);
}

future<expected<void, message_error>> echo_with_buf(Context ctx, Buf&& buf, el::Pipe *in_pipe, el::Pipe *out_pipe) {
    std::span<char> slice = buf.get();
    auto read_fut = in_pipe->read(slice.data(), slice.size());
    future<size_t> interrupt_fut = el::wait_any(*ctx.interruptor, read_fut);
    return std::move(interrupt_fut).then([ctx, MC(buf), in_pipe, out_pipe, MC(read_fut)](size_t index) mutable {
        if (index == 0) {
            read_fut.cancel();
            // Interrupted.
            return el::future{expected<void, message_error>(unexpected(message_error("interrupted")))};
        }
        tpf_assert(read_fut.has_value());
        expected<ssize_t, read_error> nbytes = std::move(read_fut.value());

        if (!nbytes.has_value()) {
            return el::future{expected<void, message_error>(unexpected(message_error(nbytes.error())))};
        }
        if (nbytes == 0) {
            tpf_setupf("EOF.  Ending loop.\n");
            return el::future{expected<void, message_error>()};
        } else {
            size_t nbytes_u = static_cast<size_t>(nbytes.value());
            tpf_setupf("Read %zu bytes: %.*s\n", nbytes_u, (int)nbytes_u, buf.ptr());
            auto write_fut = write_all(ctx, out_pipe, buf.ptr(), nbytes_u);
            return std::move(write_fut).then([MC(buf), nbytes_u, ctx, in_pipe, out_pipe](expected<void, write_all_error>&& result) mutable {
                if (!result.has_value()) {
                    tpf_setupf("Partial write of %zu bytes: %.*s\n", result.error().bytes_written, (int)result.error().bytes_written, buf.ptr());
                    if (std::get_if<el::interrupt_result>(&result.error().err)) {
                        return el::future{expected<void, message_error>(unexpected(message_error{"interrupted"}))};
                    }
                    write_error err = std::get<write_error>(std::move(result.error().err));
                    return el::future{expected<void, message_error>(unexpected(message_error{err}))};
                }
                tpf_setupf("Wrote %zu bytes: %.*s\n", nbytes_u, (int)nbytes_u, buf.ptr());

                // TODO: We might be infinitely recursing here.
                return echo_with_buf(ctx, std::move(buf), in_pipe, out_pipe);
            });
        }
    });
}


el::future<expected<void, message_error>> echo_on_pipe(Context ctx, unique_ptr<el::Pipe>&& in_pipe, unique_ptr<el::Pipe>&& out_pipe) {
    const size_t TOY_BUF_SIZE = 128;
    Buf buf(TOY_BUF_SIZE);
    el::Pipe *in_ptr = in_pipe.get();
    el::Pipe *out_ptr = out_pipe.get();
    return echo_with_buf(ctx, std::move(buf), in_ptr, out_ptr).then([MC(in_pipe), MC(out_pipe)](auto&& val) mutable {
        // TODO: likewise with signalfd (and echo_on_socket) figure out how to do teardown logging.
        return el::Pipe::close(std::move(in_pipe))
        .then( [MC(val), MC(out_pipe)](expected<close_errsv, epoll_ctl_error>&& errsv_expec) mutable {
            if (!errsv_expec.has_value()) {
                return el::future{expected<void, message_error>(unexpected(message_error(errsv_expec.error())))};
            }
            if (errsv_expec.value().errsv != 0) {
                // TODO: Janky error messaging.
                tpf_setupf("close() errored on pipe: %s\n", strerror_buf(errsv_expec.value().errsv).msg());
            }
            return el::Pipe::close(std::move(out_pipe))
            .then([MC(val)](expected<close_errsv, epoll_ctl_error>&& errsv_expec) mutable {
                if (!errsv_expec.has_value()) {
                    return el::future{expected<void, message_error>(unexpected(message_error(errsv_expec.error())))};
                }
                if (errsv_expec.value().errsv != 0) {
                    // TODO: Janky error messaging.
                    tpf_setupf("close() errored on pipe: %s\n", strerror_buf(errsv_expec.value().errsv).msg());
                }
                return el::make_future(std::move(val));
            });
        });
    });
}

el::future<expected<void, message_error>> echo_on_socket(Context ctx, unique_ptr<el::Pipe>&& bidir_pipe) {
    const size_t TOY_BUF_SIZE = 128;
    Buf buf(TOY_BUF_SIZE);
    el::Pipe *ptr = bidir_pipe.get();
    return echo_with_buf(ctx, std::move(buf), ptr, ptr).then([MC(bidir_pipe)](auto&& val) mutable {
        return el::Pipe::close(std::move(bidir_pipe))
        .then( [MC(val)](expected<close_errsv, epoll_ctl_error>&& errsv_expec) mutable {
            if (!errsv_expec.has_value()) {
                return el::future{expected<void, message_error>(unexpected(message_error(errsv_expec.error())))};
            }
            if (errsv_expec.value().errsv != 0) {
                // TODO: Janky error messaging.
                tpf_setupf("close() errored on pipe: %s\n", strerror_buf(errsv_expec.value().errsv).msg());
            }
            return el::make_future(std::move(val));
        });
    });
}

expected<unique_ptr<el::Pipe>, message_error> open_fifo(el::Loop *loop, const char *path, int oflag) {
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

    return el::make_pipe_from_fifo(loop, el::Fd{fd});
}

struct addrinfo_list {
    NONCOPYABLE(addrinfo_list);
    struct addrinfo *elems = nullptr;
    addrinfo_list(addrinfo_list&& movee) : elems(movee.elems) {
        movee.elems = 0;
    }
    addrinfo_list& operator=(addrinfo_list&& movee) {
        addrinfo_list tmp(std::move(movee));
        std::swap(elems, tmp.elems);
        return *this;
    }
    addrinfo_list() = default;
    ~addrinfo_list() noexcept {
        if (elems != nullptr) {
            freeaddrinfo(elems);
        }
    }
};

expected<addrinfo_list, message_error> get_addr_info(const char *hostname, uint16_t portno) {
    addrinfo_list addrinfos;

    retry_getaddrinfo:
        const char *service = nullptr;
    // TODO: Definitely don't pass hints or anything like that.
    struct addrinfo *hints = nullptr;
    int res = getaddrinfo(hostname, service, hints, &addrinfos.elems);
    if (res != 0) {
        tpf_assert(addrinfos.elems == nullptr);
        if (res == EAI_SYSTEM) {
            int errsv = errno;
            // Unclear if possible, handling EINTR anyway.
            if (errsv == EINTR) {
                goto retry_getaddrinfo;
            }
            return unexpected(message_error{"Error calling getaddrinfo: "s + strerror_buf(errsv).msg()});
        }
        return unexpected(message_error{"Error calling getaddrinfo: "s + gai_strerror(res)});
    }

    tpf_assert(addrinfos.elems != nullptr);

    return addrinfos;
}

struct AddrInfoAndSocketResult {
    addrinfo_list addrinfos;
    // Pointer into addrinfos;
    struct sockaddr *addr;
    socklen_t addrlen;
    el::Fd sockfd;
};

// Calls getaddrinfo and socket(2) with SOCK_STREAM | extra_sock_flags.  Recommended: SOCK_CLOEXEC | SOCK_NONBLOCK.
expected<AddrInfoAndSocketResult, message_error> open_generic_stream_socket(const char *hostname, uint16_t portno, int extra_sock_flags) {
    // TODO: Blocking.  getaddrinfo and presumably connect as well.
    auto addrinfo_expec = get_addr_info(hostname, portno);
    if (!addrinfo_expec.has_value()) {
        return unexpected(addrinfo_expec.error());
    }
    addrinfo_list& addrinfos = addrinfo_expec.value();
    socklen_t addrlen = addrinfos.elems->ai_addrlen;
    struct sockaddr *addr = addrinfos.elems->ai_addr;

    // TODO: Don't look at ai_next or anything.  No sir, just try the first address.

    sa_family_t family = addr->sa_family;

    if (family != AF_INET && family != AF_INET6) {
        return unexpected(message_error{"Unsupported socket family resolved: "s + std::to_string(addr->sa_family)});
    }

    // TODO: use SOCK_NONBLOCK, handle EINPROGRESS with connect.  Or do that with accept first.
    try_socket_again:
       int fd_raw = socket(addr->sa_family, SOCK_STREAM | extra_sock_flags, 0);
    if (fd_raw == -1) {
        int errsv = errno;
        // Unclear from immediate manpage reading if this is possible.
        if (errsv == EINTR) {
            goto try_socket_again;
        }
        return unexpected(message_error{"Error opening tcp socket: "s + strerror_buf(errsv).msg()});
    }

    return AddrInfoAndSocketResult{
        .addrinfos = std::move(addrinfos),
        .addr = addr,
        .addrlen = addrlen,
        .sockfd = el::Fd{fd_raw},
    };
}

expected<el::Fd, message_error> open_client_tcp_socket_helper(const char *hostname, uint16_t portno) {
    // TODO: Blocking calls to getaddrinfo, connect.
    int sock_nonblock_flag_absent = SOCK_CLOEXEC;
    auto result_expec = open_generic_stream_socket(hostname, portno, sock_nonblock_flag_absent);
    if (!result_expec.has_value()) {
        return unexpected(std::move(result_expec.error()));
    }
    AddrInfoAndSocketResult& result = result_expec.value();

    el::Fd& fd = result.sockfd;

    sa_family_t family = result.addr->sa_family;

    sockaddr_storage addr_inet;
    memset(&addr_inet, 0, sizeof(addr_inet));
    memcpy(&addr_inet, result.addr, result.addrlen);

    if (family == AF_INET) {
        struct sockaddr_in *addr_in = reinterpret_cast<sockaddr_in *>(&addr_inet);
        tpf_assert(addr_in->sin_family == AF_INET);
        addr_in->sin_port = htons(portno);
    } else {
        tpf_assert(family == AF_INET6);
        struct sockaddr_in6 *addr_in = reinterpret_cast<sockaddr_in6 *>(&addr_inet);
        tpf_assert(addr_in->sin6_family == AF_INET6);
        addr_in->sin6_port = htons(portno);
    }

    const struct sockaddr *addr = reinterpret_cast<const struct sockaddr *>(&addr_inet);

 try_connect_again:
    int res = connect(fd.get(), addr, result.addrlen);
    if (res == -1) {
        int errsv = errno;
        if (errsv == EINTR) {
            goto try_connect_again;
        }
        int discard = std::move(fd).close();  // Just close and ignore error.
        return unexpected(message_error{"Error connecting tcp socket: "s + strerror_buf(errsv).msg()});
    }

    tpf_assert(res == 0);

    int flags = fcntl(fd.get(), F_GETFL);
    // We set the cloexec flag (which we would check with F_GETFD, not F_GETFL), but not
    // SOCK_NONBLOCK, in our socket call in this function that created fd.
    tpf_assert(!(flags & O_NONBLOCK));
    flags |= O_NONBLOCK;
 try_fcntl_again:
    res = fcntl(fd.get(), F_SETFL, flags);
    if (res == -1) {
        int errsv = errno;
        if (errsv == EINTR) {
            // Not actually possible, really.
            goto try_fcntl_again;
        }
        int discard = std::move(fd).close();  // Just close and ignore error.
        return unexpected(message_error{"fcntl F_SETFL failed in open_client_tcp_socket_helper: "s + strerror_buf(errsv).msg()});
    }
    tpf_assert(res == 0);

    return std::move(fd);
}

expected<unique_ptr<el::Pipe>, message_error> open_client_tcp_socket(el::Loop *loop, const char *hostname, uint16_t portno) {
    auto fd_expec = open_client_tcp_socket_helper(hostname, portno);
    if (!fd_expec.has_value()) {
        return unexpected(fd_expec.error());
    }
    return el::make_pipe_from_sockfd(loop, std::move(fd_expec.value()));
}

expected<el::Fd, message_error> open_listen_socket_helper(const char *hostname, uint16_t portno, int listen_backlog) {
    // TODO: Blocking calls to getaddrinfo, connect.
    auto result_expec = open_generic_stream_socket(hostname, portno, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (!result_expec.has_value()) {
        return unexpected(std::move(result_expec.error()));
    }
    AddrInfoAndSocketResult& result = result_expec.value();

    el::Fd& fd = result.sockfd;

    sa_family_t family = result.addr->sa_family;

    // TODO: This with the portno being set is pretty duplicated with open_client_tcp_socket_helper.
    sockaddr_storage addr_inet;
    memset(&addr_inet, 0, sizeof(addr_inet));
    memcpy(&addr_inet, result.addr, result.addrlen);

    if (family == AF_INET) {
        struct sockaddr_in *addr_in = reinterpret_cast<sockaddr_in *>(&addr_inet);
        tpf_assert(addr_in->sin_family == AF_INET);
        addr_in->sin_port = htons(portno);
    } else {
        tpf_assert(family == AF_INET6);
        struct sockaddr_in6 *addr_in = reinterpret_cast<sockaddr_in6 *>(&addr_inet);
        tpf_assert(addr_in->sin6_family == AF_INET6);
        addr_in->sin6_port = htons(portno);
    }

    const struct sockaddr *addr = reinterpret_cast<const struct sockaddr *>(&addr_inet);

 try_bind_again:
    int res = ::bind(fd.get(), addr, result.addrlen);
    if (res == -1) {
        int errsv = errno;
        if (errsv == EINTR) {
            // Not actually possible?  Idk.
            goto try_bind_again;
        }
        int discard = std::move(fd).close();  // Just close and ignore error.
        return unexpected(message_error{"binding connecting tcp socket: "s + strerror_buf(errsv).msg()});
    }
    tpf_assert(res == 0);

 try_listen_again:
    res = ::listen(fd.get(), listen_backlog);
    if (res == -1) {
        int errsv = errno;
        if (errsv == EINTR) {
            goto try_listen_again;
        }
        int discard = std::move(fd).close();  // Just close and ignore error.
        return unexpected(message_error{"listening to tcp socket: "s + strerror_buf(errsv).msg()});
    }

    return std::move(fd);
}

struct InterruptorPair {
    unique_ptr<interrupt_future> future;
    el::interrupt_promise promise;
};

InterruptorPair make_interruptor_pair() {
    InterruptorPair ret;
    ret.future.reset(new interrupt_future(&ret.promise));
    return ret;
}

struct AcceptLoopState {
    NONCOPYABLE_MOVABLE(AcceptLoopState);
    std::vector<future<expected<void, message_error>>> futs;
    // TODO: Make use of interruptors for non-graceful shutdown.
    std::vector<InterruptorPair> interruptors;
    void add(future<expected<void, message_error>>&& fut, InterruptorPair&& interruptor) {
        futs.push_back(std::move(fut));
        interruptors.push_back(std::move(interruptor));
    }
    AcceptLoopState() = default;
    ~AcceptLoopState() {
        for (auto& fut : futs) {
            tpf_assert(!fut.is_active());
        }
        // TODO: We don't actually make use of the interruptors (yet)?  So we just cancel
        // the futures before we destruct them.
        for (auto& ip : interruptors) {
            ip.future->cancel();
        }
    }
};

struct WaitCompleteState {
    size_t i = 0;
    std::vector<message_error> errors;
};

future<expected<void, message_error>> wait_for_all_to_complete(Context ctx, AcceptLoopState&& wg, WaitCompleteState&& state) {
    if (state.i == wg.futs.size()) {
        if (state.errors.empty()) {
            return future{expected<void, message_error>()};
        }
        std::string message = "A number of errors:\n";
        for (auto&& err : state.errors) {
            message += err.message;
            message += '\n';
        }
        return future{expected<void, message_error>(unexpected(message_error{message}))};
    }

    auto fut = std::move(wg.futs.at(state.i));
    return std::move(fut).then([ctx, MC(wg), MC(state)](expected<void, message_error>&& result) mutable {
        if (!result.has_value()) {
            state.errors.push_back(result.error());
        }
        state.i += 1;
        return wait_for_all_to_complete(ctx, std::move(wg), std::move(state));
    });
}

el::future<expected<void, message_error>> accept_loop(
        Context ctx, std::unique_ptr<el::ListenSocket>&& listen_socket,
        AcceptLoopState&& wg) {

    auto accept_fut = el::wait_interruptible(ctx.interruptor, listen_socket->accept());
    return std::move(accept_fut).then([ctx, MC(listen_socket), MC(wg)](expected<expected<el::Fd, accept_error>, el::interrupt_result>&& fd_expec_ir) mutable {
        if (!fd_expec_ir.has_value()) {
            // TODO: Handle different types of interruption.  Well maybe we can't even
            // have an interruptor -- we need signals for varying amounts of gracefulness.


            // Interrupted.  So, uh, gracefully wait for subtasks in wg to clean up.

            return wait_for_all_to_complete(ctx, std::move(wg), WaitCompleteState{});
        }
        if (!fd_expec_ir.value().has_value()) {
            return wait_for_all_to_complete(ctx, std::move(wg), WaitCompleteState{}).then([error = std::move(fd_expec_ir.value().error())](auto&&) {
                return el::future{expected<void, message_error>(unexpected(std::move(error)))};
            });
        }

        // TODO: Echo on more than one socket at a time.
        auto pipe_expec = el::make_pipe_from_sockfd(ctx.loop, std::move(fd_expec_ir.value().value()));
        if (!pipe_expec.has_value()) {
            return el::future{expected<void, message_error>(unexpected(pipe_expec.error()))};
        }
        auto& pipe = pipe_expec.value();

        InterruptorPair interruptor = make_interruptor_pair();
        tpf_assert(interruptor.promise.completions_empty());
        auto echo_fut = echo_on_socket(Context(ctx.loop, interruptor.future.get()), std::move(pipe));
        wg.add(std::move(echo_fut), std::move(interruptor));
        tpf_assert(interruptor.promise.completions_empty());

        return accept_loop(ctx, std::move(listen_socket), std::move(wg));
    });

}

el::future<expected<void, message_error>> go_with_listen_socket(Context ctx, const SocketModeOptions& socket_opts) {
    // See man 2 listen, see /proc/sys/net/core/somaxconn.
    const int DEFAULT_LISTEN_BACKLOG = 1000000;
    expected<el::Fd, message_error> socket_expec = open_listen_socket_helper("localhost", socket_opts.listen_port, DEFAULT_LISTEN_BACKLOG);
    if (!socket_expec.has_value()) {
        return el::make_future<expected<void, message_error>>(unexpected(socket_expec.error()));
    }

    auto listen_socket = make_unique<el::ListenSocket>(ctx.loop, std::move(socket_expec.value()).release());

    return accept_loop(ctx, std::move(listen_socket), AcceptLoopState{});
}

el::future<expected<void, message_error>> go(Context ctx, const Options& opts) {
    tpf_setupf("go()...\n");

    if (opts.fifo_mode.has_value()) {
        auto& fifo_opts = opts.fifo_mode.value();
        auto in_pipe_expec = open_fifo(ctx.loop, fifo_opts.in_fifo_path.c_str(), O_RDONLY);
        if (!in_pipe_expec.has_value()) {
            return el::future{expected<void, message_error>(unexpected(in_pipe_expec.error()))};
        }
        // Silly(?) experiment using references with .value() on expected<_,_>.
        unique_ptr<el::Pipe>& in_pipe = in_pipe_expec.value();
        auto out_pipe_expec = open_fifo(ctx.loop, fifo_opts.out_fifo_path.c_str(), O_WRONLY);
        if (!out_pipe_expec.has_value()) {
            return el::future{expected<void, message_error>(unexpected(out_pipe_expec.error()))};
        }
        unique_ptr<el::Pipe>& out_pipe = out_pipe_expec.value();

        return echo_on_pipe(ctx, std::move(in_pipe), std::move(out_pipe))
        .then([](expected<void, message_error>&& err) mutable {
            tpf_setupf("Echo completed...\n");
            return el::future{std::move(err)};
        });
    } else if (opts.socket_mode.has_value()) {
        return go_with_listen_socket(ctx, opts.socket_mode.value());
    } else {
        tpf_assert(opts.client_socket_mode.has_value());
        auto& client_socket_opts = opts.client_socket_mode.value();
        auto client_pipe_expec = open_client_tcp_socket(ctx.loop, client_socket_opts.hostname.c_str(), client_socket_opts.portno);
        if (!client_pipe_expec.has_value()) {
            return el::future{expected<void, message_error>(unexpected(client_pipe_expec.error()))};
        }

        return echo_on_socket(ctx, std::move(client_pipe_expec.value()))
        .then([](expected<void, message_error>&& err) mutable {
            tpf_setupf("Echo completed...\n");
            return el::future{std::move(err)};
        });
    }
}

// This actually effectively takes ownership of the signal_fd.  Once we have more use for
// signals we might want to split this stream.
interrupt_future get_interrupt_future(el::SignalFd *signal_fd) {
    return signal_fd->read().cancellable_then([signal_fd](expected<uint32_t, read_error>&& res) mutable {
        if (!res.has_value()) {
            return cancellable_future<expected<void, read_error>>(unexpected(res.error()));
        }
        uint32_t signo = res.value();

        if (signo == SIGINT || signo == SIGTERM) {
            return cancellable_future<expected<void, read_error>>{expected<void, read_error>()};
        }

        return get_interrupt_future(signal_fd);
    });
}

int main(int argc, const char **argv) {
    expected<Options, message_error> opts_expec = parse_command_line(argc, argv);
    if (!opts_expec.has_value()) {
        printf("Command line parsing failed: %s\n", opts_expec.error().c_str());
        print_usage_message();
        return 2;
    }
    Options& opts = opts_expec.value();

    if (opts.help) {
        print_help_message();
        return 0;
    }


    tpf_assert(argc > 0);
    tpf_setupf("Hello, world!\n");

    {
        el::SignalBlockGuard signal_block_guard(el::sigint_sigterm_sigset());
        expected<int, signalfd_error> sigfd = signal_block_guard.make_signalfd();
        if (!sigfd.has_value()) {
            // TODO: stderr just here?
            fprintf(stderr, "%s\n", sigfd.error().make_msg().c_str());
            return 1;
        }

        expected<int, epoll_create_error> fd_expec = el::Loop::make_epoll_fd();
        if (!fd_expec.has_value()) {
            tpf_setupf("%s\n", fd_expec.error().make_msg().c_str());
            return 1;
        }
        el::Loop loop(fd_expec.value());
        tpf_setupf("Constructed el::Loop\n");

        unique_ptr<el::SignalFd> signal_fd = make_unique<el::SignalFd>(&loop, sigfd.value());

        loop.schedule([&loop, MC(signal_fd), MC(opts)] mutable {
            interrupt_future interruptor_flat =
                    get_interrupt_future(signal_fd.get());
            // TODO: Would be nice if we could make and pass interruptors without this allocation -- but it might require pointer/backpointer updates, so... ick.
            unique_ptr<interrupt_future> interruptor = make_unique<interrupt_future>(std::move(interruptor_flat));
            interrupt_future *interruptor_ptr = interruptor.get();

            go(Context{&loop, interruptor_ptr}, opts)
            .finally([MC(signal_fd), MC(interruptor)]() mutable {
                interruptor->cancel();

                return el::SignalFd::close(std::move(signal_fd)).then([](expected<close_errsv, epoll_ctl_error>&& ignore) mutable {
                    // TODO: Of course, figure out how we want to handle these errors, or unify logging of tear-down close errors.
                    if (!ignore.has_value()) {
                        tpf_setupf("epoll_ctl_error upon signalfd close: %s\n", strerror_buf(ignore.error().errsv).msg());
                    } else if (ignore.value().errsv != 0) {
                        tpf_setupf("close errored for signalfd: %s\n", strerror_buf(ignore.value().errsv).msg());
                    }

                    return make_future(Unit{});
                });
            })
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
