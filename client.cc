#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

int main(int argc, char const *argv[]) {
    int sock = 0, port = 9709, opt = 1;
	int resp_port = 9710;
    struct sockaddr_in serv_addr;
	struct sockaddr_in resp_addr;
	int new_sock, addrlen;

	if (argc < 3) {
		std::cerr << "usage: ./client <num_trials> <ip> [port]" << std::endl;
		return -1;
	}
    const char* num_trials = argv[1];
    const char* ip = argv[2];

	if (argc == 4)
		port = atoi(argv[3]);

	/************* Send request **************/

    // Create socket file descriptor
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        std::cerr << "Socket creation error" << std::endl;
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if(inet_pton(AF_INET, ip, &serv_addr.sin_addr)<=0) {
        std::cerr << "Invalid address/ Address not supported" << std::endl;
        return -1;
    }

    // Connect to the server
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "Connection Failed" << std::endl;
        return -1;
    }

    // Send message to the server
    send(sock, num_trials, strlen(num_trials), 0);

	close(sock);

	if (atoi(num_trials) == 0)
		return 0;

	/************* Wait response *************/

	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        std::cerr << "Socket creation error" << std::endl;
        return -1;
	}
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
		std::cerr << "Setsockopt error" << std::endl;
		return -1;
	}
	resp_addr.sin_family = AF_INET;
	resp_addr.sin_addr.s_addr = INADDR_ANY;
	resp_addr.sin_port = htons(resp_port);
	if (bind(sock, (struct sockaddr *)&resp_addr, sizeof(resp_addr)) < 0) {
		std::cerr << "Bind failed" << std::endl;
		return -1;
	}
	if (listen(sock, 3) < 0) {
		std::cerr << "Listen error" << std::endl;
		return -1;
	}

	addrlen = sizeof(resp_addr);
	if ((new_sock = accept(sock, (struct sockaddr *) &resp_addr, (socklen_t *) &addrlen)) < 0) {
		std::cerr << "Accept error" << std::endl;
		return -1;
	}

	char buffer[256] = {0};
	read(new_sock, buffer, 255);
	std::cout << buffer << std::endl;
	close(new_sock);
	close(sock);

    return 0;
}
