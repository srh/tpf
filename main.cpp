#include <cstdio>

#include "util.hpp"

#include "el/loop.hpp"

int main(int argc, const char **argv) {
    tpf_assert(argc > 0);
    printf("Hello, world!\n");

    {
        el::Loop loop;
        printf("Constructed el::Loop\n");
    }

    return 0;
}
