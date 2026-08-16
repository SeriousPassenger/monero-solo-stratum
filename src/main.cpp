#include "monero_solo/config.hpp"
#include "monero_solo/build_timestamp.hpp"
#include "monero_solo/build_version.hpp"
#include "monero_solo/logger.hpp"
#include "monero_solo/runtime.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

namespace {

volatile std::sig_atomic_t stopping = 0;

extern "C" void handle_signal(int) { stopping = 1; }

void usage(std::ostream &output) {
    output << "Usage:\n"
              "  monero-solo-stratum --config PATH\n"
              "  monero-solo-stratum --check-config --config PATH\n"
              "  monero-solo-stratum --version\n"
              "  monero-solo-stratum --help\n";
}

} // namespace

int main(int argc, char **argv) {
    bool check_config = false;
    std::string config_path;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            usage(std::cout);
            return 0;
        }
        if (argument == "--version") {
            std::cout
                << "monero-solo-stratum " << MSS_VERSION << '\n'
                << "Built: " << MSS_BUILD_TIMESTAMP << '\n'
                << "Copyright (c) 2026 SeriousPassenger\n"
                << "License: MIT\n"
                << "Source: https://github.com/SeriousPassenger/monero-solo-stratum\n";
            return 0;
        }
        if (argument == "--check-config") {
            check_config = true;
            continue;
        }
        if (argument == "--config" && index + 1 < argc) {
            config_path = argv[++index];
            continue;
        }
        std::cerr << "Unknown or incomplete argument: " << argument << "\n";
        usage(std::cerr);
        return 1;
    }
    if (config_path.empty()) {
        std::cerr << "--config PATH is required\n";
        usage(std::cerr);
        return 1;
    }
    try {
        const auto config = monero_solo::load_config_file(config_path);
        if (check_config) {
            (void)monero_solo::logging::parse_severity(config.logging.level);
            monero_solo::logging::validate_file_configuration(
                config.logging.file);
            std::cout << "configuration valid\n";
            return 0;
        }
        monero_solo::Runtime runtime(config);
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        runtime.start();
        while (!stopping && runtime.running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const bool internal_failure = !stopping && !runtime.running();
        runtime.stop();
        return internal_failure ? 1 : 0;
    }
    catch (const std::exception &error) {
        std::cerr << (check_config ? "Configuration error: " : "Startup error: ")
                  << error.what() << "\n";
        return 1;
    }
}
