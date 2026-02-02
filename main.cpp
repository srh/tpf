#include <cstdio>

#include "util.hpp"

#include "el/loop.hpp"

int main(int argc, const char **argv) {
    tpf_assert(argc > 0);
    printf("Hello, world!\n");

    {
        el::Loop loop;
        tpf_setupf("Constructed el::Loop\n");

        while (loop.has_stuff_to_do()) {
            loop.full_step();
        }
    }

    return 0;
}
