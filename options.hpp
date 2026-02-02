#ifndef TPF_OPTIONS_HPP
#define TPF_OPTIONS_HPP

#include <string>

struct Options {
    std::string fifo_path;
};

Options parse_command_line(int argc, const char *const *argv);

#endif  // TPF_OPTIONS_HPP
