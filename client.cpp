// client.cpp - Interactive commit message generator
#include <signal.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ANSI color codes
namespace Color {
const std::string RESET = "\033[0m";
const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string BLUE = "\033[34m";
const std::string MAGENTA = "\033[35m";
const std::string CYAN = "\033[36m";
const std::string BOLD = "\033[1m";
const std::string DIM = "\033[2m";
}  // namespace Color

const std::string REQUEST_PIPE = "/tmp/commitgen_request";
const std::string RESPONSE_PIPE = "/tmp/commitgen_response";
const std::string PID_FILE = "/tmp/commitgen_server.pid";

// Get single keypress without waiting for Enter
char get_keypress() {
    struct termios oldt, newt;
    char ch;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    ch = getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return ch;
}

// Get line input with prompt
std::string get_input(const std::string& prompt) {
    std::cout << prompt;
    std::string input;
    std::getline(std::cin, input);
    return input;
}

// Check if server is running
bool is_server_running() {
    struct stat buffer;
    if (stat(PID_FILE.c_str(), &buffer) != 0) {
        return false;
    }
    std::ifstream pid_file(PID_FILE);
    pid_t server_pid;
    if (pid_file >> server_pid) {
        return (kill(server_pid, 0) == 0);
    }
    return false;
}

// Print functions
void print_error(const std::string& msg) {
    std::cerr << Color::RED << "✗ " << Color::RESET << msg << std::endl;
}

void print_success(const std::string& msg) {
    std::cout << Color::GREEN << "✓ " << Color::RESET << msg << std::endl;
}

void print_info(const std::string& msg) {
    std::cout << Color::CYAN << "→ " << Color::RESET << msg << std::endl;
}

void print_warning(const std::string& msg) {
    std::cout << Color::YELLOW << "⚠ " << Color::RESET << msg << std::endl;
}

void clear_line() {
    std::cout << "\r\033[K";
}

std::string repeat_char(const std::string& ch, int count) {
    std::string result;
    for (int i = 0; i < count; i++) {
        result += ch;
    }
    return result;
}

std::string escape_for_shell(const std::string& msg) {
    std::string escaped;
    for (char c : msg) {
        if (c == '\'') {
            escaped += "'\\''";  // End quote, escaped quote, start quote
        } else {
            escaped += c;
        }
    }
    return escaped;
}

void print_header(const std::string& text) {
    std::cout << "\n" << Color::BOLD << Color::CYAN;
    std::cout << "┌─" << repeat_char("─", text.length() + 2) << "─┐\n";
    std::cout << "│ " << text << "   │\n";
    std::cout << "└─" << repeat_char("─", text.length() + 2) << "─┘\n";
    std::cout << Color::RESET;
}

void print_divider() {
    std::cout << Color::DIM << "─────────────────────────────────────────" << Color::RESET << std::endl;
}

// Check if path is a git repository
bool is_git_repo(const std::string& path) {
    std::string git_dir = path + "/.git";
    struct stat buffer;
    return (stat(git_dir.c_str(), &buffer) == 0);
}

// Execute command and get output
std::string execute_command(const std::string& cmd, const std::string& working_dir = "") {
    std::string full_cmd = cmd;
    if (!working_dir.empty()) {
        full_cmd = "cd \"" + working_dir + "\" && " + cmd;
    }
    full_cmd += " 2>&1";

    std::array<char, 256> buffer;
    std::string result;

    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(full_cmd.c_str(), "r"), pclose);
    if (!pipe) {
        throw std::runtime_error("Failed to execute command: " + cmd);
    }

    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }

    return result;
}

// Execute command silently (for git operations)
int execute_silent(const std::string& cmd, const std::string& working_dir = "") {
    std::string full_cmd = cmd;
    if (!working_dir.empty()) {
        full_cmd = "cd \"" + working_dir + "\" && " + cmd;
    }
    full_cmd += " >/dev/null 2>&1";
    return system(full_cmd.c_str());
}

// Get list of changed files
std::vector<std::string> get_changed_files(const std::string& repo_path, bool staged = true) {
    std::string cmd = staged ? "git diff --cached --name-only" : "git diff --name-only";
    std::string output = execute_command(cmd, repo_path);

    std::vector<std::string> files;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty()) {
            files.push_back(line);
        }
    }
    return files;
}

// Get git diff for specific file
std::string get_git_diff(const std::string& repo_path, const std::string& file_path = "", bool staged = true) {
    if (!is_git_repo(repo_path)) {
        throw std::runtime_error("Not a git repository: " + repo_path);
    }

    std::string cmd = "git diff";
    if (staged) {
        cmd += " --cached";
    }

    if (!file_path.empty()) {
        cmd += " -- \"" + file_path + "\"";
    }

    std::string diff = execute_command(cmd, repo_path);

    while (!diff.empty() && (diff.back() == '\n' || diff.back() == ' ')) {
        diff.pop_back();
    }

    return diff;
}

// Send request to server
std::string send_request(const std::string& request) {
    if (!is_server_running()) {
        throw std::runtime_error("Server not running. Start with: commitgen-server --start <model_path>");
    }

    std::ofstream request_pipe(REQUEST_PIPE);
    if (!request_pipe) {
        throw std::runtime_error("Failed to connect to server");
    }
    request_pipe << request;
    request_pipe.close();

    std::string response;
    auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::seconds(60);

    std::cout << Color::DIM << "Generating";
    int dots = 0;

    while (std::chrono::steady_clock::now() - start < timeout) {
        std::ifstream response_pipe(RESPONSE_PIPE);
        if (response_pipe) {
            std::stringstream buffer;
            buffer << response_pipe.rdbuf();
            response = buffer.str();
            if (!response.empty()) {
                response_pipe.close();
                clear_line();
                return response;
            }
        }

        if (++dots % 10 == 0) {
            std::cout << "." << std::flush;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << Color::RESET << std::endl;
    throw std::runtime_error("Server timeout");
}

// Commit result structure
struct CommitResult {
    std::string file;
    std::string message;
    bool accepted;
    bool committed;
};

// Interactive commit for a single file
CommitResult interactive_commit(const std::string& repo_path, const std::string& file, int current, int total,
                                bool auto_accept) {
    CommitResult result;
    result.file = file;
    result.accepted = false;
    result.committed = false;

    std::cout << "\n";
    std::cout << Color::BOLD << Color::BLUE << "┌──────────────────────────────────────────┐" << Color::RESET << "\n";
    std::cout << Color::BOLD << Color::BLUE << "│" << Color::RESET;
    std::cout << " File " << Color::YELLOW << current << "/" << total << Color::RESET;
    std::cout << ": " << Color::GREEN << file << Color::RESET;

    int padding = 40 - 8 - std::to_string(current).length() - std::to_string(total).length() - file.length();
    if (padding > 0)
        std::cout << std::string(padding, ' ');
    std::cout << Color::BOLD << Color::BLUE << "│" << Color::RESET << "\n";
    std::cout << Color::BOLD << Color::BLUE << "└──────────────────────────────────────────┘" << Color::RESET << "\n";

    // Get diff
    std::string diff;
    try {
        diff = get_git_diff(repo_path, file, true);
    } catch (...) {
        diff = execute_command("git diff --cached -- \"" + file + "\"", repo_path);
    }

    if (diff.empty()) {
        print_warning("No diff available for this file");
        return result;
    }

    // Generate commit message
    std::string commit_msg;
    try {
        commit_msg = send_request(diff);
    } catch (const std::exception& e) {
        print_error(e.what());
        return result;
    }

    // Trim
    while (!commit_msg.empty() && (commit_msg.back() == '\n' || commit_msg.back() == ' ')) {
        commit_msg.pop_back();
    }

    result.message = commit_msg;

    // Display message
    std::cout << "\n" << Color::BOLD << "Suggested commit message:" << Color::RESET << "\n";
    std::cout << Color::YELLOW << "─────────────────────────────────────────" << Color::RESET << "\n";
    std::cout << commit_msg << "\n";
    std::cout << Color::YELLOW << "─────────────────────────────────────────" << Color::RESET << "\n";

    // Auto-accept mode
    if (auto_accept) {
        result.accepted = true;

        execute_silent("git add '" + escape_for_shell(file) + "'", repo_path);

        std::string commit_cmd =
            "git commit -m '" + escape_for_shell(commit_msg) + "' -- '" + escape_for_shell(file) + "'";
        int ret = execute_silent(commit_cmd, repo_path);

        if (ret == 0) {
            result.committed = true;
            print_success("Committed: " + file);
        } else {
            print_error("Failed to commit: " + file);
        }
        return result;
    }

    // Interactive prompt
    std::cout << "\n";
    std::cout << Color::GREEN << "[y]" << Color::RESET << " Accept & commit  ";
    std::cout << Color::YELLOW << "[e]" << Color::RESET << " Edit message  ";
    std::cout << Color::RED << "[n]" << Color::RESET << " Skip  ";
    std::cout << Color::MAGENTA << "[q]" << Color::RESET << " Quit\n";
    std::cout << "\n" << Color::BOLD << "Your choice: " << Color::RESET;

    while (true) {
        char choice = get_keypress();
        std::cout << choice << std::endl;

        if (choice == 'y' || choice == 'Y') {
            result.accepted = true;

            execute_silent("git add '" + escape_for_shell(file) + "'", repo_path);

            std::string commit_cmd =
                "git commit -m '" + escape_for_shell(commit_msg) + "' -- '" + escape_for_shell(file) + "'";
            int ret = execute_silent(commit_cmd, repo_path);

            if (ret == 0) {
                result.committed = true;
                print_success("Committed: " + file);
            } else {
                print_error("Failed to commit: " + file);
            }
            break;

        } else if (choice == 'e' || choice == 'E') {
            std::cout << "\n"
                      << Color::CYAN << "Enter new commit message (or press Enter to keep current):" << Color::RESET
                      << "\n";
            std::cout << Color::DIM << "> " << Color::RESET;

            std::string new_msg;
            std::getline(std::cin, new_msg);

            if (!new_msg.empty()) {
                commit_msg = new_msg;
                result.message = commit_msg;
            }

            result.accepted = true;

            execute_silent("git add '" + escape_for_shell(file) + "'", repo_path);

            std::string commit_cmd =
                "git commit -m '" + escape_for_shell(commit_msg) + "' -- '" + escape_for_shell(file) + "'";
            int ret = execute_silent(commit_cmd, repo_path);

            if (ret == 0) {
                result.committed = true;
                print_success("Committed: " + file);
            } else {
                print_error("Failed to commit: " + file);
            }
            break;

        } else if (choice == 'n' || choice == 'N') {
            print_info("Skipped: " + file);
            break;

        } else if (choice == 'q' || choice == 'Q') {
            result.file = "__QUIT__";
            return result;

        } else {
            std::cout << Color::BOLD << "Your choice: " << Color::RESET;
        }
    }

    return result;
}

// Show usage
void show_usage(const std::string& prog_name) {
    std::cout << Color::BOLD << "CommitGen" << Color::RESET << " - AI-powered commit message generator\n\n";

    std::cout << Color::BOLD << "USAGE:" << Color::RESET << "\n";
    std::cout << "  " << prog_name << " [OPTIONS]\n\n";

    std::cout << Color::BOLD << "OPTIONS:" << Color::RESET << "\n";
    std::cout << "  " << Color::GREEN << "-p, --path <dir>" << Color::RESET
              << "      Git repository path (default: current directory)\n";
    std::cout << "  " << Color::GREEN << "-f, --file <file>" << Color::RESET
              << "     Generate commit for specific file only\n";
    std::cout << "  " << Color::GREEN << "-e, --each" << Color::RESET
              << "            Interactive mode: commit each file separately\n";
    std::cout << "  " << Color::GREEN << "-a, --all" << Color::RESET
              << "             Generate single commit for all staged changes\n";
    std::cout << "  " << Color::GREEN << "-u, --unstaged" << Color::RESET
              << "        Use unstaged changes instead of staged\n";
    std::cout << "  " << Color::GREEN << "-l, --list" << Color::RESET << "            List changed files\n";
    std::cout << "  " << Color::GREEN << "-s, --status" << Color::RESET << "          Check server status\n";
    std::cout << "  " << Color::GREEN << "-y, --yes" << Color::RESET
              << "             Auto-accept all commits (no prompts)\n";
    std::cout << "  " << Color::GREEN << "-h, --help" << Color::RESET << "            Show this help message\n\n";

    std::cout << Color::BOLD << "EXAMPLES:" << Color::RESET << "\n";
    std::cout << Color::DIM << "  # Generate commit for all staged changes" << Color::RESET << "\n";
    std::cout << "  " << prog_name << "\n\n";

    std::cout << Color::DIM << "  # Interactive mode - commit each file separately" << Color::RESET << "\n";
    std::cout << "  " << prog_name << " --each\n\n";

    std::cout << Color::DIM << "  # Generate commit for a specific file" << Color::RESET << "\n";
    std::cout << "  " << prog_name << " -f src/main.cpp\n\n";

    std::cout << Color::DIM << "  # Interactive mode for another repository" << Color::RESET << "\n";
    std::cout << "  " << prog_name << " --path ~/projects/myapp --each\n";
}

// Parse arguments
struct Options {
    std::string repo_path = ".";
    std::string file_path = "";
    bool staged = true;
    bool list_files = false;
    bool show_status = false;
    bool show_help = false;
    bool each_file = false;
    bool auto_accept = false;
};

Options parse_args(int argc, char** argv) {
    Options opts;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            opts.show_help = true;
        } else if (arg == "-s" || arg == "--status") {
            opts.show_status = true;
        } else if (arg == "-l" || arg == "--list") {
            opts.list_files = true;
        } else if (arg == "-u" || arg == "--unstaged") {
            opts.staged = false;
        } else if (arg == "-a" || arg == "--all") {
            opts.file_path = "";
            opts.each_file = false;
        } else if (arg == "-e" || arg == "--each") {
            opts.each_file = true;
        } else if ((arg == "-p" || arg == "--path") && i + 1 < argc) {
            opts.repo_path = argv[++i];
        } else if ((arg == "-f" || arg == "--file") && i + 1 < argc) {
            opts.file_path = argv[++i];
        } else if (arg[0] != '-') {
            opts.file_path = arg;
        } else if (arg == "-y" || arg == "--yes") {
            opts.auto_accept = true;
        }
    }

    // Resolve repo path
    if (opts.repo_path == ".") {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            opts.repo_path = cwd;
        }
    } else {
        if (opts.repo_path[0] == '~') {
            const char* home = getenv("HOME");
            if (home) {
                opts.repo_path = std::string(home) + opts.repo_path.substr(1);
            }
        }
        char resolved[PATH_MAX];
        if (realpath(opts.repo_path.c_str(), resolved)) {
            opts.repo_path = resolved;
        }
    }

    return opts;
}

int main(int argc, char** argv) {
    Options opts = parse_args(argc, argv);

    if (opts.show_help) {
        show_usage(argv[0]);
        return 0;
    }

    if (opts.show_status) {
        if (is_server_running()) {
            print_success("Server is running");
        } else {
            print_error("Server is not running");
            std::cout << Color::DIM << "Start with: commitgen-server --start <model_path>" << Color::RESET << std::endl;
        }
        return 0;
    }

    if (!is_git_repo(opts.repo_path)) {
        print_error("Not a git repository: " + opts.repo_path);
        return 1;
    }

    if (opts.list_files) {
        auto files = get_changed_files(opts.repo_path, opts.staged);
        if (files.empty()) {
            print_warning(opts.staged ? "No staged changes" : "No unstaged changes");
            return 0;
        }

        std::cout << Color::BOLD << (opts.staged ? "Staged" : "Unstaged") << " files:" << Color::RESET << "\n";
        for (const auto& f : files) {
            std::cout << "  " << Color::GREEN << f << Color::RESET << "\n";
        }
        return 0;
    }

    if (!is_server_running()) {
        print_error("Server is not running");
        std::cout << Color::DIM << "Start with: commitgen-server --start <model_path>" << Color::RESET << std::endl;
        return 1;
    }

    // ========== EACH FILE MODE ==========
    if (opts.each_file) {
        auto files = get_changed_files(opts.repo_path, opts.staged);

        if (files.empty()) {
            print_warning(opts.staged ? "No staged changes found" : "No unstaged changes found");
            return 1;
        }

        print_header("CommitGen - Interactive Mode");
        if (opts.auto_accept) {
            std::cout << Color::GREEN << "Auto-accept mode enabled" << Color::RESET << "\n";
        }
        std::cout << Color::DIM << "Found " << files.size() << " file(s) to commit\n" << Color::RESET;

        std::vector<CommitResult> results;
        int committed = 0;
        int skipped = 0;

        for (size_t i = 0; i < files.size(); i++) {
            CommitResult result = interactive_commit(opts.repo_path, files[i], i + 1, files.size(), opts.auto_accept);

            if (result.file == "__QUIT__") {
                std::cout << "\n";
                print_info("Quitting...");
                break;
            }

            results.push_back(result);

            if (result.committed) {
                committed++;
            } else {
                skipped++;
            }
        }

        // Summary
        std::cout << "\n";
        print_divider();
        std::cout << Color::BOLD << "Summary:" << Color::RESET << "\n";
        std::cout << "  " << Color::GREEN << "Committed: " << committed << Color::RESET << "\n";
        std::cout << "  " << Color::YELLOW << "Skipped:   " << skipped << Color::RESET << "\n";
        print_divider();

        return 0;
    }

    // ========== SINGLE COMMIT MODE (default) ==========
    try {
        std::string diff = get_git_diff(opts.repo_path, opts.file_path, opts.staged);

        if (diff.empty()) {
            if (!opts.file_path.empty()) {
                print_warning("No changes in file: " + opts.file_path);
            } else {
                print_warning(opts.staged ? "No staged changes found" : "No unstaged changes found");

                auto unstaged = get_changed_files(opts.repo_path, false);
                if (!unstaged.empty() && opts.staged) {
                    std::cout << Color::DIM << "Tip: Found " << unstaged.size()
                              << " unstaged file(s). Use 'git add' or try --unstaged" << Color::RESET << std::endl;
                }
            }
            return 1;
        }

        if (!opts.file_path.empty()) {
            print_info("Generating commit for: " + opts.file_path);
        } else {
            auto files = get_changed_files(opts.repo_path, opts.staged);
            print_info("Generating commit for " + std::to_string(files.size()) + " file(s)");
        }

        std::string commit_msg = send_request(diff);

        // Trim
        while (!commit_msg.empty() && (commit_msg.back() == '\n' || commit_msg.back() == ' ')) {
            commit_msg.pop_back();
        }

        std::cout << "\n" << Color::BOLD << "Suggested commit message:" << Color::RESET << "\n";
        std::cout << Color::YELLOW << "─────────────────────────────────────────" << Color::RESET << "\n";
        std::cout << commit_msg << "\n";
        std::cout << Color::YELLOW << "─────────────────────────────────────────" << Color::RESET << "\n";

        // Interactive prompt
        std::cout << "\n";
        std::cout << Color::GREEN << "[y]" << Color::RESET << " Accept & commit  ";
        std::cout << Color::YELLOW << "[e]" << Color::RESET << " Edit message  ";
        std::cout << Color::RED << "[n]" << Color::RESET << " Cancel\n";
        std::cout << "\n" << Color::BOLD << "Your choice: " << Color::RESET;

        while (true) {
            char choice = get_keypress();
            std::cout << choice << std::endl;

            if (choice == 'y' || choice == 'Y') {
                std::string escaped_msg = commit_msg;
                size_t pos = 0;
                while ((pos = escaped_msg.find('"', pos)) != std::string::npos) {
                    escaped_msg.replace(pos, 1, "\\\"");
                    pos += 2;
                }

                std::string commit_cmd = "git commit -m \"" + escaped_msg + "\"";
                int ret = execute_silent(commit_cmd, opts.repo_path);

                if (ret == 0) {
                    print_success("Changes committed!");
                } else {
                    print_error("Failed to commit");
                }
                break;

            } else if (choice == 'e' || choice == 'E') {
                std::cout << "\n" << Color::CYAN << "Enter new commit message:" << Color::RESET << "\n";
                std::cout << Color::DIM << "> " << Color::RESET;

                std::string new_msg;
                std::getline(std::cin, new_msg);

                if (!new_msg.empty()) {
                    commit_msg = new_msg;
                }

                std::string escaped_msg = commit_msg;
                size_t pos = 0;
                while ((pos = escaped_msg.find('"', pos)) != std::string::npos) {
                    escaped_msg.replace(pos, 1, "\\\"");
                    pos += 2;
                }

                std::string commit_cmd = "git commit -m \"" + escaped_msg + "\"";
                int ret = execute_silent(commit_cmd, opts.repo_path);

                if (ret == 0) {
                    print_success("Changes committed!");
                } else {
                    print_error("Failed to commit");
                }
                break;

            } else if (choice == 'n' || choice == 'N') {
                print_info("Cancelled");
                break;

            } else {
                std::cout << Color::BOLD << "Your choice: " << Color::RESET;
            }
        }

        return 0;

    } catch (const std::exception& e) {
        print_error(e.what());
        return 1;
    }
}
