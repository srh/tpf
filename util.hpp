#ifndef TPF_UTIL_HPP_
#define TPF_UTIL_HPP_

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

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

// TODO: Remove.  Purpose is to avoid leaving stray printfs in initial code setup printfs.
#define tpf_setupf(...) printf(__VA_ARGS__)

// TODO: Remove all uses.
using clumsy_error = std::runtime_error;

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


#endif  // TPF_UTIL_HPP_
