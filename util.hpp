#ifndef TPF_UTIL_HPP_
#define TPF_UTIL_HPP_

#include <cstdio>
#include <cstdlib>

#define tpf_assert(pred) do { \
        if (!(pred)) { \
            fprintf(stderr, "Assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #pred); \
            abort(); \
        } \
    } while (false)


#endif  // TPF_UTIL_HPP_
