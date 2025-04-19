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
#include <ifaddrs.h>
#include <net/if.h>


using namespace std;

const int PORT = 54321;
const int INFINITY_DISTANCE = 10;
const int UPDATE_INTERVAL = 10;
const int ROUTE_TIMEOUT = 3 * UPDATE_INTERVAL;
const int GARBAGE_COLLECTION_INTERVAL = 6 * UPDATE_INTERVAL;

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


    vector<pair<NetworkAddress, uint32_t>> broadcast_to_network;

    map<NetworkAddress, RouteInfo> routing_table;
    vector<pair<NetworkAddress, uint32_t>> directly_connected;
    vector<uint32_t> broadcast_addresses;
    mutex table_mutex;
    int sockfd;
    bool running;
    thread update_thread;
    thread receive_thread;
    thread display_thread;
    vector<uint32_t> local_ips;

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

            // znaleźć pozycję '/' w ip_cidr
            size_t slash_pos = ip_cidr.find('/');
            // ip_str to adres IP bez maski
            string ip_str = ip_cidr.substr(0, slash_pos);
            // mask to maska podsieci
            int mask = stoi(ip_cidr.substr(slash_pos + 1));

            // konwertujemy adres IP na uint32_t
            struct in_addr addr;
            inet_pton(AF_INET, ip_str.c_str(), &addr);
            // konwertujemy adres IP z formatu binarnego na uint32_t
            uint32_t ip = ntohl(addr.s_addr);
            NetworkAddress network{ip & (0xFFFFFFFF << (32 - mask)), (uint8_t)mask};

            directly_connected.emplace_back(network, distance);
            broadcast_addresses.push_back(ip | (~(0xFFFFFFFF << (32 - mask))));
            routing_table[network] = {distance, 0, time(nullptr)};
            broadcast_to_network.emplace_back(network, ip | (~(0xFFFFFFFF << (32 - mask))));
            local_ips.push_back(ip);
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
    bool isReachable(const std::string& ip) {
        std::string cmd = "ping -c 1 -W 1 -b " + ip + " > /dev/null 2>&1";
        return system(cmd.c_str()) == 0;
    }

    void send_updates() {
        lock_guard<mutex> lock(table_mutex);
        time_t now = time(nullptr);

        struct sockaddr_in dest_addr{};
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(PORT);

        for (const auto& [network, broadcast_ip] : broadcast_to_network) {
            dest_addr.sin_addr.s_addr = htonl(broadcast_ip);
            char broadcast_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &dest_addr.sin_addr, broadcast_str, sizeof(broadcast_str));

            if (!isReachable(broadcast_str)) {
                /*
                cout << "Network " << network.ip << "/" << (int)network.mask
                     << " (broadcast: " << broadcast_str << ") is unreachable" << endl;
                */
                // Znajdź odpowiedni wpis w routing_table
                for (auto& [rt_network, rt_info] : routing_table) {
                    if (rt_network.ip == network.ip &&
                        rt_network.mask == network.mask &&
                        rt_info.is_directly_connected()) {
                        rt_info.distance = INFINITY_DISTANCE;
                        rt_info.last_update = now;

                        // funkcja do zmiany odleglosci na infinity w sciezce majacej ten adres jako next hop
                        set_distance_to_infinity_for_route(network);

                        break;
                    }
                }
                continue;
            }
            else {
                // jezeli nie ma networka w routing table to dodac z czasem
                // wpp zmien tylko odleglosc

                auto it = routing_table.find(network);
                uint32_t d = INFINITY_DISTANCE;

                for (const auto& [direct_net, dist] : directly_connected) {
                    if (direct_net == network) {
                        d = dist;
                        break;
                    }
                }

                if (it == routing_table.end()) {
                    routing_table[network] = {d,0,now};
                }
                else {
                    it->second.distance = d;
                }
            }

            for (const auto& [network, info] : routing_table) {
                // nie wysyłamy pakietów na temat sieci, która jest nieaktywne od paru i ma odległość nieskończoną
                // bezpośrednie sieci zostawiamy, a niebezpośrednie w cleanie usuwamy
                if ((now - info.last_update > ROUTE_TIMEOUT) && (info.distance == INFINITY_DISTANCE)) continue;

                uint8_t packet[9];
                uint32_t network_ip = htonl(network.ip);
                memcpy(packet, &network_ip, 4);
                packet[4] = network.mask;

                uint32_t distance = (info.distance >= INFINITY_DISTANCE) ?
                                    INFINITY_DISTANCE : htonl(info.distance);
                memcpy(packet + 5, &distance, 4);

                // wysyłamy pakiet na adres rozgłoszeniowy
                sendto(sockfd, packet, sizeof(packet), 0,
                                   (struct sockaddr*)&dest_addr, sizeof(dest_addr));



                /*
                if (n < 0) {
                    cerr << "Error sending packet to " << inet_ntoa(dest_addr.sin_addr) << endl;

                    // ustawiamy odległość odbiorcy na nieskończoność
                    auto it = routing_table.find(network);
                    if (it != routing_table.end()) {
                        cout << "x" << endl;
                        it->second.distance = INFINITY_DISTANCE;
                    }
                } else {
                    //cout << "Sent update to " << inet_ntoa(dest_addr.sin_addr) << endl;
                }
                 */
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

    // funkcja do ustwienia odleglosci na infinity w sciezce majacej ten adres jako next hop
    void set_distance_to_infinity_for_route(const NetworkAddress& network) {
        for (auto& [net, info] : routing_table) {
            // musimy przekonwertowac adres next_hop na adres sieci
            auto network_from_ip = info.next_hop & (0xFFFFFFFF << (32 - network.mask));

            if (network_from_ip == network.ip) {
                info.distance = INFINITY_DISTANCE;
            }
        }
    }



    void cleanup_old_routes() {
        lock_guard<mutex> lock(table_mutex);
        time_t now = time(nullptr);

        // sprawdzamy czy dostaliśmy pakiety od sąsiadów w ciagu ROUTE_TIMEOUT jesli nie to ustawiamy odległość tras przechodzących przez nich na nieskończoność
        for (auto& [network, info] : routing_table) {
            /*
            cout << "Network: " << network.ip << "/" << (int)network.mask
                 << ", Distance: " << info.distance
                 << ", Last update: " << ctime(&info.last_update)
                 << ", Now: " << ctime(&now);
            */
            // zmienic
            // bo gdy nie dostaniemy pakietu od sasiada przez tyle tur to cos jest nie tak
            // trzebabybylo zmienic kazda trace z tym adresem na nieskonczonosc
            if (info.distance != INFINITY_DISTANCE && now - info.last_update > ROUTE_TIMEOUT) {

                if (info.is_directly_connected()) {
                    cout << "aa" << endl;

                    set_distance_to_infinity_for_route(network);
                    // dla bezposrednich nie zmieniamy na infinity tylko zmieniamy odleglosci w sciezkach ktore przechodza przez ten adres
                }
                else {
                    cout << "bb" << endl;
                    info.distance = INFINITY_DISTANCE;
                    // zmieniamy na infinity bo to nie nasza bezposrednia siec
                    // i zmieniamy distance na nieskonczonosc
                }
            }
        }

        for (auto it = routing_table.begin(); it != routing_table.end(); ) {
            const RouteInfo& info = it->second;

            // nie usuwa trasy gdy np. 4 trasy maja unreachable tej samej sieci (bo aktualizujemy timestamp bo
            // usuwamy tylko trasy ktore maja distance = infinity i sa przestarzałe, ale nie usuwamy bezpośrednich
            if ((info.distance == INFINITY_DISTANCE) && (now - info.last_update > GARBAGE_COLLECTION_INTERVAL) && (!info.is_directly_connected())) {
                it = routing_table.erase(it);
            } else {
                it++;
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
                //cout << "Received packet from " << inet_ntoa(cliaddr.sin_addr) << endl;
                if (n == sizeof(buffer)) {
                    process_packet(ntohl(cliaddr.sin_addr.s_addr), buffer);
                }
            }
        }
    }

    void process_packet(uint32_t src_ip, const uint8_t* packet) {
        for (uint32_t my_ip : local_ips) {
            if (src_ip == my_ip) {
                //cout << "Ignored packet from self (" << src_ip << ")" << endl;
                return;
            }
        }
        uint32_t network_ip = ntohl(*(uint32_t*)packet);
        uint8_t mask = packet[4];
        uint32_t distance = ntohl(*(uint32_t*)(packet + 5));

        lock_guard<mutex> lock(table_mutex);
        time_t now = time(nullptr);



        // Znajdź koszt dotarcia do nadawcy (src_ip)
        uint32_t cost_to_sender = INFINITY_DISTANCE;
        for (const auto& [net, dist] : directly_connected) {
            // Sprawdź, czy nadawca należy do tej samej sieci
            // i zmieniamy odleglośc na oryginalna (bo mogła być nieskończona)
            if ((src_ip & (0xFFFFFFFF << (32 - net.mask))) == net.ip) {
                cost_to_sender = dist;
                break;
            }
        }

        // Jeśli nie mamy połączenia do nadawcy, odrzuć pakiet
        if (cost_to_sender == INFINITY_DISTANCE) return;

        // Obliczenie adresu sieci
        NetworkAddress dest{network_ip, mask};

        // Oblicz nową odległość (uwzględniając nieskończoność)
        uint32_t new_distance = (distance == INFINITY_DISTANCE) ?
                                INFINITY_DISTANCE :
                                min(cost_to_sender + distance, (uint32_t)INFINITY_DISTANCE);

        // mozna dodac jeszcze ze jak pobieramy informacje o jakiejś sieci to sprawdzamy (gdy odleglosc jest mniejsza) czy przypadkiem
        // next_hop nie jest nasza bezposrednia siecia (teraz tez dziala ale dopierop po paru turach sie dowiadujemy ze jakas sieć nie działa)

        // Sprawdź, czy mamy lepszą trasę lub czy jest to nowa trasa
        auto it = routing_table.find(dest);

        if (it == routing_table.end()) {
            // Nowa trasa - dodaj jeśli odległość jest lepsza niż nieskończoność
            if (new_distance < INFINITY_DISTANCE) {
                routing_table[dest] = {new_distance, src_ip, now};
            }
        } else {
            // Istniejąca trasa - aktualizuj jeśli nowa odległość jest lepsza
            if (new_distance < it->second.distance) {
                it->second = {new_distance, src_ip, now};
            }
            // jeśli nowa odległość jest równa to aktualizujemy czas
            else if ((new_distance == it->second.distance) && (new_distance != INFINITY_DISTANCE)) {
                it->second.last_update = now;
            }
        }
    }


    // wyswietlamy tablice routingu
    void display_loop() {
        while (running) {
            this_thread::sleep_for(chrono::seconds(UPDATE_INTERVAL));

            lock_guard<mutex> lock(table_mutex);
            time_t now = time(nullptr);

            // wyswietlamy brodkast i adresy sieci


            cout << "\nRouting table at " << ctime(&now);

            for (const auto& [net, info] : routing_table) {

                char ip[INET_ADDRSTRLEN], nh[INET_ADDRSTRLEN];
                struct in_addr addr;
                addr.s_addr = htonl(net.ip);
                inet_ntop(AF_INET, &addr, ip, sizeof(ip));

                addr.s_addr = htonl(info.next_hop);
                inet_ntop(AF_INET, &addr, nh, sizeof(nh));

                // wyswietlamy w zaleznosci czy direct czy nie
                if (info.is_directly_connected()) {
                    cout << ip << "/" << (int)net.mask
                         << (info.distance == INFINITY_DISTANCE ? " unreachable" : " distance " + to_string(info.distance))
                         << " connected directly \n";
                }
                else {
                    cout << ip << "/" << (int)net.mask << " distance "
                         << (info.distance == INFINITY_DISTANCE ? "unreachable" : to_string(info.distance))
                         << " via " << nh << "\n";
                }
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