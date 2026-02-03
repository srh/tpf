#include "options.hpp"

#include <format>
#include <optional>

#include "util.hpp"

bool try_string_option(int argc, const char *const *argv, int *i, const char *name, std::optional<std::string> *out) {
    if (0 != strcmp(argv[*i], name)) {
        return false;
    }
    ++*i;
    if (*i == argc) {
        throw clumsy_error(std::format("{} missing argument", name));
    }
    const char *param = argv[*i];
    ++*i;
    if (out->has_value()) {
        throw clumsy_error(std::format("{} already defined", name));
    }
    out->emplace(param);
    return true;
}

Options parse_command_line(int argc, const char *const *argv) {
    std::optional<std::string> in_fifo_path;
    std::optional<std::string> out_fifo_path;

    int i = 1;
    while (i < argc) {
        if (try_string_option(argc, argv, &i, "--in-fifo", &in_fifo_path)) {
            continue;
        }
        if (try_string_option(argc, argv, &i, "--out-fifo", &out_fifo_path)) {
            continue;
        }
        throw clumsy_error(std::format("Unrecognized argument: {}", argv[i]));
    }
    if (!in_fifo_path.has_value()) {
        throw clumsy_error("--in-fifo not specified");
    }

    if (!out_fifo_path.has_value()) {
        throw clumsy_error("--out-fifo not specified");
    }

    return Options {
        .in_fifo_path = std::move(*in_fifo_path),
        .out_fifo_path = std::move(*out_fifo_path),
    };
}

