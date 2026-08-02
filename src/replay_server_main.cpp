#include <iostream>
#include <cstdlib>
#include "udp_server.hpp"

int main(int argc, char* argv[]) {
    const char* path  = "C:/data/itch_data";
    double      speed = 0.0;  // 0 = unlimited

    if (argc > 1) path  = argv[1];
    if (argc > 2) speed = std::atof(argv[2]);

    std::printf("ExchangeCore Replay Server\n");
    std::printf("==========================\n");
    std::printf("File  : %s\n", path);
    std::printf("Speed : %s\n", speed == 0 ? "unlimited" : argv[2]);
    std::printf("Port  : %d\n\n", UdpServer::DEFAULT_PORT);

    UdpServer server;
    if (!server.init("127.0.0.1", UdpServer::DEFAULT_PORT)) {
        std::printf("error: failed to init UDP socket\n");
        return 1;
    }

    server.replay_file(path, speed);

    std::printf("\nStats:\n");
    std::printf("  Packets sent : %llu\n", server.packets_sent());
    std::printf("  Bytes sent   : %.1f GB\n",
                server.bytes_sent() / 1e9);

    return 0;
}