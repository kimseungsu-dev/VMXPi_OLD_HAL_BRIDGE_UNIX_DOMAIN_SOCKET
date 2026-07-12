#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace
{
const char* const SOCKET_PATH = "/run/vmx-controller.sock";
const std::size_t MAX_LINE_LENGTH = 4096;

bool send_all(int fd, const std::string& data)
{
    std::size_t sent = 0;

    while (sent < data.size()) {
        const ssize_t result = send(
            fd,
            data.data() + sent,
            data.size() - sent,
            MSG_NOSIGNAL
        );

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (result == 0) {
            return false;
        }

        sent += static_cast<std::size_t>(result);
    }

    return true;
}

bool receive_line(int fd, std::string& line)
{
    line.clear();

    while (line.size() < MAX_LINE_LENGTH) {
        char character = '\0';

        const ssize_t result = recv(
            fd,
            &character,
            1,
            0
        );

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (result == 0) {
            return false;
        }

        if (character == '\n') {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            return true;
        }

        line.push_back(character);
    }

    errno = EMSGSIZE;
    return false;
}

void print_usage(const char* program)
{
    std::cerr
        << "Usage:\n"
        << "  " << program << " \"COMMAND\" [\"COMMAND\" ...]\n\n"
        << "Examples:\n"
        << "  " << program << " PING STATUS QUIT\n"
        << "  " << program << " \"DIO_OPEN_OUT 0\" \"DIO_READ 0\" QUIT\n"
        << "  " << program << " SERVER_STOP\n";
}
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }

    const int socket_fd = socket(
        AF_UNIX,
        SOCK_STREAM,
        0
    );

    if (socket_fd < 0) {
        std::perror("socket");
        return 1;
    }

    sockaddr_un address {};
    address.sun_family = AF_UNIX;

    if (std::strlen(SOCKET_PATH) >= sizeof(address.sun_path)) {
        std::cerr << "Socket path is too long.\n";
        close(socket_fd);
        return 1;
    }

    std::strncpy(
        address.sun_path,
        SOCKET_PATH,
        sizeof(address.sun_path) - 1
    );

    if (connect(
            socket_fd,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)
        ) < 0) {
        std::perror("connect");
        close(socket_fd);
        return 1;
    }

    std::string response;

    if (!receive_line(socket_fd, response)) {
        std::perror("receive greeting");
        close(socket_fd);
        return 1;
    }

    std::cout << response << '\n';

    for (int index = 1; index < argc; ++index) {
        const std::string command(argv[index]);

        if (command.empty()) {
            std::cerr << "Empty command is not allowed.\n";
            close(socket_fd);
            return 2;
        }

        if (command.find('\n') != std::string::npos ||
            command.find('\r') != std::string::npos) {
            std::cerr
                << "Command must not contain newline characters: "
                << command << '\n';

            close(socket_fd);
            return 2;
        }

        if (!send_all(socket_fd, command + "\n")) {
            std::perror("send");
            close(socket_fd);
            return 1;
        }

        if (!receive_line(socket_fd, response)) {
            std::perror("receive response");
            close(socket_fd);
            return 1;
        }

        std::cout << response << '\n';
    }

    close(socket_fd);
    return 0;
}
