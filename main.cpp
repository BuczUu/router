#include <iostream>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <chrono>
#include <ctime>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <poll.h>
#include <fstream>

using namespace std;

const int PORT = 54321;
const int INFINITY_DISTANCE = 0xFFFFFFFF;
const int UPDATE_INTERVAL = 20;
const int ROUTE_TIMEOUT = 3 * UPDATE_INTERVAL;
const int GARBAGE_COLLECTION_INTERVAL = 5 * UPDATE_INTERVAL;

struct NetworkAddress {
    uint32_t ip;
    uint8_t mask;

    bool operator<(const NetworkAddress& other) const {
        return tie(ip, mask) < tie(other.ip, other.mask);
    }

    bool operator==(const NetworkAddress& other) const {
        return ip == other.ip && mask == other.mask;
    }
};

struct RouteInfo {
    uint32_t distance;
    uint32_t next_hop;
    time_t last_update;

    bool is_directly_connected() const { return next_hop == 0; }
};

class Router {
private:
    map<NetworkAddress, RouteInfo> routing_table;
    vector<pair<NetworkAddress, uint32_t>> directly_connected;
    vector<uint32_t> broadcast_addresses;
    mutex table_mutex;
    int sockfd;
    bool running;
    thread update_thread;
    thread receive_thread;
    thread display_thread;

public:
    Router() : sockfd(-1), running(false) {}
    ~Router() { stop(); }

    bool initialize(const string& config_file) {
        ifstream file(config_file);
        if (!file.is_open()) {
            cerr << "Cannot open config file: " << config_file << endl;
            return false;
        }

        int interface_count;
        file >> interface_count;
        file.ignore();

        for (int i = 0; i < interface_count; ++i) {
            string ip_cidr, dummy;
            uint32_t distance;
            file >> ip_cidr >> dummy >> distance;

            size_t slash_pos = ip_cidr.find('/');
            string ip_str = ip_cidr.substr(0, slash_pos);
            int mask = stoi(ip_cidr.substr(slash_pos + 1));

            struct in_addr addr;
            inet_pton(AF_INET, ip_str.c_str(), &addr);
            uint32_t ip = ntohl(addr.s_addr);
            NetworkAddress network{ip & (0xFFFFFFFF << (32 - mask)), (uint8_t)mask};

            directly_connected.emplace_back(network, distance);
            broadcast_addresses.push_back(ip | (~(0xFFFFFFFF << (32 - mask))));
        }

        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        int broadcast = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

        struct sockaddr_in servaddr{};
        servaddr.sin_family = AF_INET;
        servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
        servaddr.sin_port = htons(PORT);
        bind(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr));

        return true;
    }

    void start() {
        running = true;
        update_thread = thread(&Router::update_loop, this);
        receive_thread = thread(&Router::receive_loop, this);
        display_thread = thread(&Router::display_loop, this);
    }

    void stop() {
        running = false;
        if (update_thread.joinable()) update_thread.join();
        if (receive_thread.joinable()) receive_thread.join();
        if (display_thread.joinable()) display_thread.join();
        if (sockfd >= 0) close(sockfd);
    }

private:
    void send_updates() {
        lock_guard<mutex> lock(table_mutex);
        time_t now = time(nullptr);

        struct sockaddr_in dest_addr{};
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(PORT);

        for (uint32_t broadcast_ip : broadcast_addresses) {
            dest_addr.sin_addr.s_addr = htonl(broadcast_ip);

            for (const auto& [network, info] : routing_table) {
                if (now - info.last_update > ROUTE_TIMEOUT) continue;

                uint8_t packet[9];
                uint32_t network_ip = htonl(network.ip);
                memcpy(packet, &network_ip, 4);
                packet[4] = network.mask;

                uint32_t distance = (info.distance >= INFINITY_DISTANCE) ?
                                    INFINITY_DISTANCE : htonl(info.distance);
                memcpy(packet + 5, &distance, 4);

                sendto(sockfd, packet, sizeof(packet), 0,
                       (struct sockaddr*)&dest_addr, sizeof(dest_addr));
            }
        }
    }

    void update_loop() {
        while (running) {
            send_updates();
            cleanup_old_routes();
            this_thread::sleep_for(chrono::seconds(UPDATE_INTERVAL));
        }
    }

    void cleanup_old_routes() {
        lock_guard<mutex> lock(table_mutex);
        time_t now = time(nullptr);

        for (auto it = routing_table.begin(); it != routing_table.end(); ) {
            const RouteInfo& info = it->second;

            if (info.is_directly_connected() ||
                now - info.last_update <= GARBAGE_COLLECTION_INTERVAL) {
                ++it;
            } else {
                it = routing_table.erase(it);
            }
        }
    }

    void receive_loop() {
        struct pollfd fd;
        fd.fd = sockfd;
        fd.events = POLLIN;

        while (running) {
            int ret = poll(&fd, 1, 1000);

            if (ret > 0 && (fd.revents & POLLIN)) {
                struct sockaddr_in cliaddr;
                socklen_t len = sizeof(cliaddr);
                uint8_t buffer[9];

                ssize_t n = recvfrom(sockfd, buffer, sizeof(buffer), 0,
                                     (struct sockaddr*)&cliaddr, &len);
                if (n == sizeof(buffer)) {
                    process_packet(ntohl(cliaddr.sin_addr.s_addr), buffer);
                }
            }
        }
    }

    void process_packet(uint32_t src_ip, const uint8_t* packet) {
        uint32_t network_ip = ntohl(*(uint32_t*)packet);
        uint8_t mask = packet[4];
        uint32_t distance = ntohl(*(uint32_t*)(packet + 5));

        lock_guard<mutex> lock(table_mutex);
        time_t now = time(nullptr);

        uint32_t best_dist = INFINITY_DISTANCE;
        for (const auto& [net, dist] : directly_connected) {
            if ((src_ip & (0xFFFFFFFF << (32 - net.mask))) == net.ip) {
                best_dist = dist;
                break;
            }
        }

        if (best_dist == INFINITY_DISTANCE) return;

        NetworkAddress dest{network_ip, mask};
        uint32_t new_dist = (distance == INFINITY_DISTANCE) ?
                            INFINITY_DISTANCE : min(best_dist + distance, (uint32_t)INFINITY_DISTANCE);

        auto it = routing_table.find(dest);
        if (it == routing_table.end() || new_dist < it->second.distance || it->second.next_hop == src_ip) {
            routing_table[dest] = {new_dist, src_ip, now};
        }
    }

    void display_loop() {
        while (running) {
            this_thread::sleep_for(chrono::seconds(UPDATE_INTERVAL));

            lock_guard<mutex> lock(table_mutex);
            time_t now = time(nullptr);

            cout << "\nRouting table at " << ctime(&now);

            // Najpierw wyświetl bezpośrednie połączenia
            for (const auto& [net, dist] : directly_connected) {
                char ip[INET_ADDRSTRLEN];
                struct in_addr addr;
                addr.s_addr = htonl(net.ip);
                inet_ntop(AF_INET, &addr, ip, sizeof(ip));
                cout << ip << "/" << (int)net.mask << " distance "
                     << dist << " connected directly\n";
            }

            // Następnie wyświetl pozostałe trasy, pomijając te które są bezpośrednie
            for (const auto& [net, info] : routing_table) {
                if (now - info.last_update > ROUTE_TIMEOUT) continue;

                // Sprawdź czy to nie jest bezpośrednie połączenie
                bool is_direct = false;
                for (const auto& [direct_net, dist] : directly_connected) {
                    if (net == direct_net) {
                        is_direct = true;
                        break;
                    }
                }
                if (is_direct) continue;

                char ip[INET_ADDRSTRLEN], nh[INET_ADDRSTRLEN];
                struct in_addr addr;
                addr.s_addr = htonl(net.ip);
                inet_ntop(AF_INET, &addr, ip, sizeof(ip));

                addr.s_addr = htonl(info.next_hop);
                inet_ntop(AF_INET, &addr, nh, sizeof(nh));

                cout << ip << "/" << (int)net.mask << " distance "
                     << (info.distance == INFINITY_DISTANCE ? "unreachable" : to_string(info.distance))
                     << " via " << nh << "\n";
            }
        }
    }
};

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <config_file>" << endl;
        return 1;
    }

    Router router;
    if (!router.initialize(argv[1])) {
        cerr << "Router initialization failed" << endl;
        return 1;
    }

    router.start();
    cout << "Router running with config: " << argv[1] << ". Press Enter to stop..." << endl;
    cin.get();
    router.stop();
    return 0;
}