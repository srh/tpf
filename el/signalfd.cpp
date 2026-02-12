#include "el/signalfd.hpp"

#include <cinttypes>
#include <sys/signalfd.h>

namespace el {

future<expected<uint32_t, read_error>> SignalFd::read() {
    tpf_assert(read_promise_.is_default_constructed());

    future<expected<uint32_t, read_error>> fut(&read_promise_);

    if (read_ready_) {
        try_doing_read();
    }
    return fut;
}

void SignalFd::try_doing_read() {
    tpf_setupf("Pipe::try_doing_read\n");
    tpf_assert(read_ready_);
    tpf_assert(read_promise_.is_active());

    static_assert(sizeof(struct signalfd_siginfo) == 128);
    struct signalfd_siginfo read_buf;

try_again:
    ssize_t res = ::read(fd_.get(), &read_buf, sizeof(read_buf));
    expected<int, read_error> signals_expec;
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
        signals_expec = unexpected(read_error{"read", errsv});
    } else {
        tpf_setupf("SignalFd::read: read returns %zd with ssi_signo %" PRIu32 "\n", res, read_buf.ssi_signo);
        // Successful read case...

        // read_ready_ remains untouched; even if we had read_buf being multiple
        // signalfd_siginfo structs, we could not infer read completeness from short reads
        // with signalfd (according to current documentation).

        // The OS is supposed to return us unsliced structs.
        tpf_assert(res == sizeof(struct signalfd_siginfo));
        uint32_t signo = read_buf.ssi_signo;
        signals_expec.emplace(signo);
    }

    std::move(read_promise_).supply_value_and_detach(std::move(signals_expec));
}


}  // namespace el
