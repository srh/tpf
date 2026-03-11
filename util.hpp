#ifndef TPF_UTIL_HPP_
#define TPF_UTIL_HPP_

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>

using std::expected;
using std::unexpected;
using std::make_unique;
using std::unique_ptr;
using namespace std::literals::string_literals;

#define NONCOPYABLE(typ) typ(const typ&) = delete; \
void operator=(const typ&) = delete

#define NONCOPYABLE_MOVABLE(typ) NONCOPYABLE(typ); \
typ(typ&&) = default; \
typ& operator=(typ&&) = default

// Move-capture
#define MC(name) name = std::move(name)

#define tpf_assert(pred) do { \
        if (!(pred)) { \
            fprintf(stderr, "Assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #pred); \
            abort(); \
        } \
    } while (false)


#define tpf_assertf(pred, fmt, ...) do { \
        if (!(pred)) { \
            fprintf(stderr, "Assertion failed at %s:%d: %s: " fmt "\n", __FILE__, __LINE__, #pred, ##__VA_ARGS__); \
            abort(); \
        } \
    } while (false)

// TODO: Remove.  Purpose is to avoid leaving stray printfs in initial code setup printfs.
#define tpf_setupf(...) printf(__VA_ARGS__)


struct strerror_buf {
    NONCOPYABLE(strerror_buf);
    // errmsg might not point into buf
    char buf[128];
    char *errmsg;
    explicit strerror_buf(int errsv) {
        errmsg = strerror_r(errsv, buf, std::size(buf));
    }
    const char *msg() const {
        return errmsg;
    }
};

struct errsv_error {
    const char *source = nullptr;
    int errsv = 0;

    std::string make_msg() const {
        return std::string(source) + " failed: " + strerror_buf(errsv).msg();
    }
};

// The purpose of these specific error types is for the programmer (i.e. me) to have
// visibility into the potential sources of error... when it is convenient to do so.  It
// may be overspecified.  For example, Pipe::close returning expected<close_errsv,
// epoll_ctl_error> is an icky interface.
struct epoll_ctl_error : errsv_error {};
struct epoll_wait_error : errsv_error {};
struct epoll_create_error : errsv_error {};
struct read_error : errsv_error {};
struct write_error : errsv_error {};
struct signalfd_error : errsv_error {};
struct accept_error : errsv_error {};

// Naming might need to change: STL uses _error suffix for exception types
struct message_error {
    explicit message_error(const errsv_error& err) : message(err.make_msg()) {}
    explicit message_error(std::string m) : message(std::move(m)) {}
    const std::string& msg() const { return message; }
    const char *c_str() const { return message.c_str(); }
    std::string message;
};

// Some errno value after a close(2) call failed... useless.
struct close_errsv {
    int errsv = 0;
};

// guarantees the parameter is left in empty state (which the move constructor doesn't do)
// Style decision: uses lvalue reference instead of rvalue reference to avoid redundant std::move.
template <class T>
std::optional<T> swap_out(std::optional<T>& value) {
    std::optional<T> ret;
    ret.swap(value);
    return ret;
}

// guarantees the parameter is left in empty state (which the move constructors doesn't do)
template <class F>
std::move_only_function<F> swap_out(std::move_only_function<F>& value) {
    std::move_only_function<F> ret;
    ret.swap(value);
    return ret;
}

#endif  // TPF_UTIL_HPP_
