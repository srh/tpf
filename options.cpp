#include "options.hpp"

#include <expected>
#include <format>
#include <optional>

#include "util.hpp"

void print_help_message() {
    printf("tpf echos bytes between an input fifo and an output fifo.\n");
    print_usage_message();
}
void print_usage_message() {
    printf("%s",
        "Usage:\n"
        "  tpf --in-fifo in_path --out-fifo out_path\n"
        "  tpf --help\n");
}

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

template <class T, class Fn>
expected<bool, message_error> try_predicated_int_option(int argc, const char *const *argv, int *i, const char *name, std::optional<T> *out, const char *range_message, Fn&& predicate) {
    // TODO: Code dupe with general option handling.
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
    const char *param_end = param + strlen(param);
    T parse_result;
    auto [ptr, ec] = std::from_chars(param, param_end, parse_result);
    if (ec == std::errc()) {
        if (ptr != param_end) {
            goto not_valid_integer;
        }
        if (predicate(parse_result)) {
            out->emplace(parse_result);
            return true;
        } else {
            goto range_error;
        }
    } else if (ec == std::errc::result_out_of_range) {
    range_error:
        return unexpected(message_error(std::format("{} is out of range: {}", name, range_message)));
    } else {
    not_valid_integer:
        return unexpected(message_error(std::format("{} is not a valid integer", name)));
    }
}

expected<Options, message_error> parse_command_line(int argc, const char *const *argv) {
    std::optional<std::string> in_fifo_path;
    std::optional<std::string> out_fifo_path;
    std::optional<std::string> host;
    std::optional<uint16_t> port_no;
    bool help = false;

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

        // TODO: Port range checking should actually depend on whether we're a client or server.
        res = try_predicated_int_option<uint16_t>(
            argc, argv, &i, "--port", &port_no,
            "expecting value 7 or value in range 1024..65535",
            [](uint16_t val) { return val == 7 || val >= 1024; });
        if (!res.has_value()) {
            return unexpected(res.error());
        }
        if (res.value()) {
            continue;
        }

        res = try_string_option(argc, argv, &i, "--host", &host);
        if (!res.has_value()) {
            return unexpected(res.error());
        }
        if (res.value()) {
            continue;
        }

        if (0 != strcmp(argv[i], "--help")) {
            help = true;
            ++i;
            continue;
        }
        return unexpected(message_error{std::format("Unrecognized argument: {}", argv[i])});
    }

    std::optional<FifoModeOptions> fifo_mode_options;
    std::optional<SocketModeOptions> socket_mode_options;
    std::optional<ClientSocketModeOptions> client_socket_mode_options;
    // Validation/canonicalization pass
    if (!help) {
        if (port_no.has_value()) {

            if (in_fifo_path.has_value() || out_fifo_path.has_value()) {
                return unexpected(message_error{"--port is incompatible with --in-fifo or --out-fifo"});
            }

            if (host.has_value()) {
                client_socket_mode_options = { .hostname = std::move(*host), .portno = *port_no };
            } else {
                socket_mode_options = { .listen_port = *port_no };
            }

        } else {
            if (!in_fifo_path.has_value()) {
                return unexpected(message_error{"--in-fifo not specified"});
            }

            if (!out_fifo_path.has_value()) {
                return unexpected(message_error{"--out-fifo not specified"});
            }
            fifo_mode_options = {
                .in_fifo_path = std::move(*in_fifo_path),
                .out_fifo_path = std::move(*out_fifo_path),
            };
        }
    }

    auto ret = Options {
        .fifo_mode = std::move(fifo_mode_options),
        .socket_mode = std::move(socket_mode_options),
        .client_socket_mode = std::move(client_socket_mode_options),
        .help = help,
    };
    return ret;
}

