#include "options.hpp"

#include <expected>
#include <format>
#include <optional>

#include "util.hpp"

expected<bool, message_error> try_string_option(int argc, const char *const *argv, int *i, const char *name, std::optional<std::string> *out) {
    if (0 != strcmp(argv[*i], name)) {
        return false;
    }
    ++*i;
    if (*i == argc) {
        return unexpected(message_error(std::format("{} missing argument", name)));
    }
    const char *param = argv[*i];
    ++*i;
    if (out->has_value()) {
        return unexpected(message_error(std::format("{} already defined", name)));
    }
    out->emplace(param);
    return true;
}

expected<Options, message_error> parse_command_line(int argc, const char *const *argv) {
    std::optional<std::string> in_fifo_path;
    std::optional<std::string> out_fifo_path;

    int i = 1;
    while (i < argc) {
        // TODO: Error handling code needs to be slicker here; maybe go back to exceptions (locally).
        auto res = try_string_option(argc, argv, &i, "--in-fifo", &in_fifo_path);
        if (!res.has_value()) {
            return unexpected(res.error());
        }
        if (res.value()) {
            continue;
        }
        res = try_string_option(argc, argv, &i, "--out-fifo", &out_fifo_path);
        if (!res.has_value()) {
            return unexpected(res.error());
        }
        if (res.value()) {
            continue;
        }
        return unexpected(message_error{std::format("Unrecognized argument: {}", argv[i])});
    }
    if (!in_fifo_path.has_value()) {
        throw unexpected(message_error{"--in-fifo not specified"});
    }

    if (!out_fifo_path.has_value()) {
        throw unexpected(message_error{"--out-fifo not specified"});
    }

    auto ret = Options {
        .in_fifo_path = std::move(*in_fifo_path),
        .out_fifo_path = std::move(*out_fifo_path),
    };
    return ret;
}

