#include <cstdio>

#include "util.hpp"

#include "el/loop.hpp"

void go(el::Loop *loop, std::move_only_function<void ()>&& on_complete) {
    tpf_setupf("go()...\n");
    loop->schedule(std::move(on_complete));
}

int main(int argc, const char **argv) {
    tpf_assert(argc > 0);
    printf("Hello, world!\n");

    {
        el::Loop loop;
        tpf_setupf("Constructed el::Loop\n");

        loop.schedule([&loop] { go(&loop, [] {
            tpf_setupf("Finished.\n");
        }); });

        while (loop.has_stuff_to_do()) {
            loop.full_step();
        }
    }

    return 0;
}
