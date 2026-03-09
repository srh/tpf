#ifndef TPF_OPTIONS_HPP
#define TPF_OPTIONS_HPP

#include <string>

#include "util.hpp"

struct FifoModeOptions {
    std::string in_fifo_path;
    std::string out_fifo_path;
};

struct SocketModeOptions {
    uint16_t listen_port;
};

struct ClientSocketModeOptions {
    std::string hostname;
    uint16_t portno;
};

struct Options {
    std::optional<FifoModeOptions> fifo_mode;
    std::optional<SocketModeOptions> socket_mode;
    std::optional<ClientSocketModeOptions> client_socket_mode;
    bool help = false;
};

expected<Options, message_error> parse_command_line(int argc, const char *const *argv);
void print_help_message();
void print_usage_message();

#endif  // TPF_OPTIONS_HPP
