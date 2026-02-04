// server.cpp - Simplified (no git commands, just processes diff text)
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "commitgen.h"

namespace fs = std::filesystem;

// ANSI color codes
namespace Color {
const std::string RESET = "\033[0m";
const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string BLUE = "\033[34m";
const std::string CYAN = "\033[36m";
const std::string BOLD = "\033[1m";
const std::string DIM = "\033[2m";
}  // namespace Color

// Paths
const std::string REQUEST_PIPE = "/tmp/commitgen_request";
const std::string RESPONSE_PIPE = "/tmp/commitgen_response";
const std::string STATUS_FILE = "/tmp/commitgen_status";
const std::string PID_FILE = "/tmp/commitgen_server.pid";

// Global pointer for signal handling
std::unique_ptr<CommitGen> generator;
volatile sig_atomic_t running = 1;

void print_banner() {
    std::cout << Color::CYAN;
    std::cout << R"(
   ____                          _ _    ____
  / ___|___  _ __ ___  _ __ ___ (_) |_ / ___| ___ _ __
 | |   / _ \| '_ ` _ \| '_ ` _ \| | __| |  _ / _ \ '_ \
 | |__| (_) | | | | | | | | | | | | |_| |_| |  __/ | | |
  \____\___/|_| |_| |_|_| |_| |_|_|\__|\____|\___|_| |_|

)" << Color::RESET;
    std::cout << Color::DIM << "  AI-powered commit message generator\n" << Color::RESET << std::endl;
}

void print_status(const std::string& msg) {
    std::cout << Color::CYAN << "[" << Color::RESET << "•" << Color::CYAN << "] " << Color::RESET << msg << std::endl;
}

void print_success(const std::string& msg) {
    std::cout << Color::GREEN << "[✓] " << Color::RESET << msg << std::endl;
}

void print_error(const std::string& msg) {
    std::cerr << Color::RED << "[✗] " << Color::RESET << msg << std::endl;
}

void print_request(const std::string& preview) {
    std::cout << Color::YELLOW << "[→] " << Color::RESET << "Request: " << Color::DIM << preview << Color::RESET
              << std::endl;
}

void print_response() {
    std::cout << Color::GREEN << "[←] " << Color::RESET << "Response sent" << std::endl;
}

// Signal handler
void signal_handler(int signum) {
    std::cout << "\n";
    print_status("Shutting down...");
    running = 0;

    unlink(REQUEST_PIPE.c_str());
    unlink(RESPONSE_PIPE.c_str());
    unlink(STATUS_FILE.c_str());
    unlink(PID_FILE.c_str());

    print_success("Server stopped");
    exit(0);
}

bool is_server_already_running() {
    struct stat buffer;
    if (stat(PID_FILE.c_str(), &buffer) == 0) {
        std::ifstream pid_file(PID_FILE);
        pid_t existing_pid;
        if (pid_file >> existing_pid) {
            if (kill(existing_pid, 0) == 0) {
                return true;
            }
        }
        unlink(PID_FILE.c_str());
    }
    return false;
}

void write_pid_file() {
    std::ofstream pid_file(PID_FILE);
    pid_file << getpid() << std::endl;
}

void cleanup() {
    unlink(REQUEST_PIPE.c_str());
    unlink(RESPONSE_PIPE.c_str());
    unlink(STATUS_FILE.c_str());
    unlink(PID_FILE.c_str());
}

void start_server(const std::string& model_path) {
    print_banner();

    // Load model
    print_status("Loading model: " + model_path);
    std::cout << Color::DIM << "   This may take a moment..." << Color::RESET << std::flush;

    generator = std::make_unique<CommitGen>(model_path);

    // Wait for model to load with spinner
    const char spinner[] = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏";
    int spin_idx = 0;
    while (!generator->is_ready()) {
        std::cout << "\r" << Color::DIM << "   Loading " << spinner[spin_idx++ % 10] << Color::RESET << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "\r" << std::string(20, ' ') << "\r";
    print_success("Model loaded");

    // Create pipes
    unlink(REQUEST_PIPE.c_str());
    unlink(RESPONSE_PIPE.c_str());
    mkfifo(REQUEST_PIPE.c_str(), 0666);
    mkfifo(RESPONSE_PIPE.c_str(), 0666);

    // Status file
    std::ofstream status(STATUS_FILE);
    status << "running";
    status.close();

    write_pid_file();

    std::cout << "\n";
    print_success("Server running on PID " + std::to_string(getpid()));
    std::cout << Color::DIM << "   Press Ctrl+C to stop\n" << Color::RESET << std::endl;

    // Main loop
    while (running) {
        try {
            int request_fd = open(REQUEST_PIPE.c_str(), O_RDONLY | O_NONBLOCK);
            if (request_fd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            // Wait for data with select
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(request_fd, &read_fds);

            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000;  // 100ms

            if (select(request_fd + 1, &read_fds, nullptr, nullptr, &tv) <= 0) {
                close(request_fd);
                continue;
            }

            // Read request
            std::string request;
            char buffer[4096];
            ssize_t bytes;
            while ((bytes = read(request_fd, buffer, sizeof(buffer) - 1)) > 0) {
                buffer[bytes] = '\0';
                request += buffer;
            }
            close(request_fd);

            if (request.empty()) {
                continue;
            }

            // Trim
            while (!request.empty() && (request.back() == '\n' || request.back() == '\r')) {
                request.pop_back();
            }

            // Preview
            std::string preview = request.length() > 60 ? request.substr(0, 57) + "..." : request;
            // Replace newlines in preview
            for (char& c : preview) {
                if (c == '\n')
                    c = ' ';
            }
            print_request(preview);

            // Generate
            std::string commit_msg;
            if (request == "--test") {
                commit_msg = "test: verify commit generation pipeline";
            } else if (request.find("diff") != std::string::npos || request.find("+++") != std::string::npos
                       || request.find("---") != std::string::npos) {
                commit_msg = generator->generate(request);
            } else {
                commit_msg = "ERROR: Invalid request - expected git diff content";
            }

            // Send response
            std::ofstream response_pipe(RESPONSE_PIPE);
            response_pipe << commit_msg;
            response_pipe.close();

            print_response();

        } catch (const std::exception& e) {
            print_error(e.what());

            std::ofstream response_pipe(RESPONSE_PIPE);
            response_pipe << "ERROR: " << e.what();
            response_pipe.close();
        }
    }
}

void stop_server() {
    if (!is_server_already_running()) {
        print_error("Server is not running");
        return;
    }

    std::ifstream pid_file(PID_FILE);
    pid_t server_pid;
    if (pid_file >> server_pid) {
        if (kill(server_pid, SIGTERM) == 0) {
            print_success("Stop signal sent to PID " + std::to_string(server_pid));
        } else {
            print_error("Failed to stop server");
        }
    }

    cleanup();
}

void check_status() {
    if (is_server_already_running()) {
        std::ifstream pid_file(PID_FILE);
        pid_t server_pid;
        if (pid_file >> server_pid) {
            print_success("Server running (PID: " + std::to_string(server_pid) + ")");
        }
    } else {
        print_error("Server is not running");
    }
}

void show_usage(const std::string& prog_name) {
    print_banner();

    std::cout << Color::BOLD << "USAGE:" << Color::RESET << "\n";
    std::cout << "  " << prog_name << " --start <model_path>   Start the server\n";
    std::cout << "  " << prog_name << " --stop                 Stop the server\n";
    std::cout << "  " << prog_name << " --status               Check server status\n\n";

    std::cout << Color::BOLD << "EXAMPLES:" << Color::RESET << "\n";
    std::cout << Color::DIM << "  # Start with a GGUF model" << Color::RESET << "\n";
    std::cout << "  " << prog_name << " --start ~/models/codellama-7b.Q4_K_M.gguf\n\n";

    std::cout << Color::DIM << "  # Start with an Ollama model blob" << Color::RESET << "\n";
    std::cout << "  " << prog_name << " --start ~/.ollama/models/blobs/sha256-abc123\n\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        show_usage(argv[0]);
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "--start" || cmd == "-s") {
        if (argc < 3) {
            print_error("Missing model path");
            std::cout << Color::DIM << "Usage: " << argv[0] << " --start <model_path>" << Color::RESET << std::endl;
            return 1;
        }

        if (is_server_already_running()) {
            print_error("Server is already running");
            return 1;
        }

        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
        signal(SIGHUP, SIG_IGN);

        try {
            start_server(argv[2]);
        } catch (const std::exception& e) {
            print_error(std::string("Fatal: ") + e.what());
            cleanup();
            return 1;
        }

    } else if (cmd == "--stop") {
        stop_server();

    } else if (cmd == "--status") {
        check_status();

    } else if (cmd == "--help" || cmd == "-h") {
        show_usage(argv[0]);

    } else {
        print_error("Unknown command: " + cmd);
        show_usage(argv[0]);
        return 1;
    }

    return 0;
}
