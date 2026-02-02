#include "options.hpp"

#include <optional>

#include "util.hpp"

Options parse_command_line(int argc, const char *const *argv) {
    std::optional<std::string> fifo_path;

    int i = 1;
    while (i < argc) {
        if (0 == strcmp(argv[i], "--fifo")) {
            ++i;
            if (i == argc) {
                throw clumsy_error("--fifo missing argument");
            }
            std::string path = argv[i];
            ++i;
            if (fifo_path.has_value()) {
                throw clumsy_error("--fifo already defined");
            }
            fifo_path.emplace(std::move(path));
        } else {
            throw clumsy_error("Unrecognized argument");
        }
    }
    if (!fifo_path.has_value()) {
        throw clumsy_error("--fifo not specified");
    }

    return Options {
        .fifo_path = std::move(*fifo_path),
    };
}

