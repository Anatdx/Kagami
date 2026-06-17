#include "core/daemon.hpp"

#include "core/command.hpp"
#include "core/json.hpp"
#include "core/runtime.hpp"

#include <cerrno>
#include <chrono>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace kagami {

namespace fs = std::filesystem;

static int print_status_json();

static std::string now_text() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    return buffer;
}

static void append_log(const std::string& message) {
    std::error_code ec;
    fs::create_directories(runtime_data_dir(), ec);
    std::ofstream out(runtime_log_file(), std::ios::app);
    if (out) {
        out << now_text() << " " << message << "\n";
    }
}

static std::string join_request(const std::vector<std::string>& args, std::size_t start) {
    std::ostringstream out;
    for (std::size_t i = start; i < args.size(); ++i) {
        if (i > start) {
            out << '\t';
        }
        out << args[i];
    }
    out << '\n';
    return out.str();
}

static std::vector<std::string> split_request(const std::string& request) {
    std::vector<std::string> args;
    std::string item;
    for (char c : request) {
        if (c == '\n' || c == '\r') {
            break;
        }
        if (c == '\t') {
            args.push_back(item);
            item.clear();
            continue;
        }
        item.push_back(c);
    }
    if (!item.empty() || !args.empty()) {
        args.push_back(item);
    }
    return args;
}

static std::string response_json(bool ok, int exit_code, const std::string& out, const std::string& err) {
    std::ostringstream json;
    json << "{"
         << "\"ok\":" << (ok ? "true" : "false") << ","
         << "\"exit_code\":" << exit_code << ","
         << "\"stdout\":" << json_quote(out) << ","
         << "\"stderr\":" << json_quote(err)
         << "}\n";
    return json.str();
}

static std::string status_json(bool running) {
    const auto pid_text = [&]() -> std::string {
        std::ifstream in(runtime_pid_file());
        std::string line;
        return std::getline(in, line) ? line : "";
    }();

    std::ostringstream out;
    out << "{"
        << "\"running\":" << (running ? "true" : "false") << ","
        << "\"data_dir\":" << json_quote(runtime_data_dir().string()) << ","
        << "\"config_file\":" << json_quote(runtime_config_file().string()) << ","
        << "\"socket\":" << json_quote(runtime_socket_file().string()) << ","
        << "\"pid_file\":" << json_quote(runtime_pid_file().string()) << ","
        << "\"pid\":" << json_quote(pid_text) << ","
        << "\"log_file\":" << json_quote(runtime_log_file().string())
        << "}\n";
    return out.str();
}

#if defined(__linux__) || defined(__APPLE__)
static bool write_all(int fd, const std::string& data) {
    const char* ptr = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        const ssize_t written = write(fd, ptr, left);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        ptr += written;
        left -= static_cast<std::size_t>(written);
    }
    return true;
}

static std::string read_all(int fd) {
    std::string data;
    char buffer[1024] = {};
    for (;;) {
        const ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        data.append(buffer, static_cast<std::size_t>(n));
        if (data.find('\n') != std::string::npos) {
            break;
        }
    }
    return data;
}

static int open_client_socket(std::string& error) {
    const auto path = runtime_socket_file().string();
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        error = "socket path is too long: " + path;
        return -1;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::strerror(errno);
        return -1;
    }

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = std::strerror(errno);
        close(fd);
        return -1;
    }
    return fd;
}

static int send_request(const std::vector<std::string>& args, std::size_t start, std::string& response, std::string& error) {
    const int fd = open_client_socket(error);
    if (fd < 0) {
        return 1;
    }
    if (!write_all(fd, join_request(args, start))) {
        error = std::strerror(errno);
        close(fd);
        return 1;
    }
    shutdown(fd, SHUT_WR);
    response = read_all(fd);
    close(fd);
    return 0;
}

static bool daemon_running() {
    std::vector<std::string> ping = {"daemon", "ping"};
    std::string response;
    std::string error;
    return send_request(ping, 0, response, error) == 0 && response.find("\"ok\":true") != std::string::npos;
}

static int serve_foreground() {
    std::error_code ec;
    fs::create_directories(runtime_data_dir(), ec);
    if (ec) {
        std::cerr << "failed to create " << runtime_data_dir() << ": " << ec.message() << "\n";
        return 1;
    }

    const auto socket_path = runtime_socket_file().string();
    if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        std::cerr << "socket path is too long: " << socket_path << "\n";
        return 1;
    }

    fs::remove(runtime_socket_file(), ec);
    const int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "socket: " << std::strerror(errno) << "\n";
        return 1;
    }

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "bind " << socket_path << ": " << std::strerror(errno) << "\n";
        close(server_fd);
        return 1;
    }
    chmod(socket_path.c_str(), 0600);

    if (listen(server_fd, 8) != 0) {
        std::cerr << "listen: " << std::strerror(errno) << "\n";
        close(server_fd);
        return 1;
    }

    {
        std::ofstream pid(runtime_pid_file(), std::ios::trunc);
        if (pid) {
            pid << getpid() << "\n";
        }
    }
    append_log("kagamid started pid=" + std::to_string(getpid()));

    bool stopping = false;
    while (!stopping) {
        const int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            append_log(std::string("accept failed: ") + std::strerror(errno));
            continue;
        }

        const auto request = read_all(client_fd);
        auto request_args = split_request(request);
        if (request_args.empty()) {
            write_all(client_fd, response_json(false, 1, "", "empty daemon request"));
            close(client_fd);
            continue;
        }

        if (request_args[0] == "daemon" && request_args.size() >= 2 && request_args[1] == "stop") {
            write_all(client_fd, response_json(true, 0, "stopping\n", ""));
            stopping = true;
            close(client_fd);
            continue;
        }

        if (request_args[0] == "daemon" && request_args.size() >= 2 && request_args[1] == "ping") {
            write_all(client_fd, response_json(true, 0, "pong\n", ""));
            close(client_fd);
            continue;
        }

        if (request_args[0] == "daemon" && request_args.size() >= 2 && request_args[1] == "status") {
            write_all(client_fd, response_json(true, 0, status_json(true), ""));
            close(client_fd);
            continue;
        }

        const auto result = run_command_capture(request_args);
        write_all(client_fd, response_json(result.exit_code == 0, result.exit_code, result.stdout_text, result.stderr_text));
        close(client_fd);
    }

    append_log("kagamid stopped");
    close(server_fd);
    fs::remove(runtime_socket_file(), ec);
    fs::remove(runtime_pid_file(), ec);
    return 0;
}

static int start_background() {
    if (daemon_running()) {
        return print_status_json();
    }

    std::error_code ec;
    fs::create_directories(runtime_data_dir(), ec);
    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "fork: " << std::strerror(errno) << "\n";
        return 1;
    }
    if (pid == 0) {
        setsid();
        const int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            close(null_fd);
        }
        const int log_fd = open(runtime_log_file().c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        _exit(serve_foreground());
    }

    for (int i = 0; i < 20; ++i) {
        if (daemon_running()) {
            return print_status_json();
        }
        usleep(50000);
    }
    std::cerr << "kagamid did not become ready\n";
    return 1;
}
#endif

static int print_status_json() {
#if defined(__linux__) || defined(__APPLE__)
    const bool running = daemon_running();
#else
    const bool running = false;
#endif
    std::cout << status_json(running);
    return 0;
}

int run_daemon_command(const std::vector<std::string>& args) {
    const std::string sub = args.size() > 1 ? args[1] : "";
    if (sub == "status") {
        return print_status_json();
    }

#if defined(__linux__) || defined(__APPLE__)
    if (sub == "serve") {
        return serve_foreground();
    }
    if (sub == "start") {
        return start_background();
    }
    if (sub == "call") {
        if (args.size() < 3) {
            std::cerr << "daemon call requires a command\n";
            return 1;
        }
        std::string response;
        std::string error;
        const int rc = send_request(args, 2, response, error);
        if (rc != 0) {
            std::cerr << "daemon unavailable: " << error << "\n";
            return rc;
        }
        std::cout << response;
        return 0;
    }
    if (sub == "ping" || sub == "stop") {
        std::string response;
        std::string error;
        const int rc = send_request(args, 0, response, error);
        if (rc != 0) {
            std::cerr << "daemon unavailable: " << error << "\n";
            return rc;
        }
        std::cout << response;
        return 0;
    }
#else
    if (sub == "serve" || sub == "start" || sub == "call" || sub == "ping" || sub == "stop") {
        std::cerr << "daemon sockets are not supported on this platform\n";
        return 1;
    }
#endif

    std::cerr << "usage: kagamid daemon status|start|serve|call|ping|stop\n";
    return 1;
}

} // namespace kagami
