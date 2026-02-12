#ifndef TPF_OPTIONS_HPP
#define TPF_OPTIONS_HPP

#include <string>

#include "util.hpp"

struct Options {
    std::string in_fifo_path;
    std::string out_fifo_path;
    bool help = false;
};

expected<Options, message_error> parse_command_line(int argc, const char *const *argv);
void print_help_message();
void print_usage_message();

#endif  // TPF_OPTIONS_HPP
