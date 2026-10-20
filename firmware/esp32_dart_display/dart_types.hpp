#pragma once
#include <string>
#include <vector>

namespace dart {

struct Departure {
    std::string train_code;
    std::string origin;
    std::string destination;
    int due_in = 0;              // minutes until arrival (0 = Due)
    std::string scheduled;       // e.g. "08:45"
    std::string expected;        // e.g. "08:46"
    std::string direction;       // "Northbound", "Southbound"
    std::string train_type;      // "DART", "Commuter"
    std::string status;          // "En Route", "No Information"
};

struct BusDeparture {
    std::string route = "H1";
    std::string destination = "Abbey St Lower";
    int due_in = 0;              // minutes until arrival (0 = Due)
    std::string scheduled = "--:--"; // e.g. "08:37"
    std::string expected = "--:--";  // e.g. "08:37"
    std::string status = "Sched";    // "Live", "Sched"
};

struct BoardData {
    std::string station_name;
    std::string station_code;
    std::string query_time;      // e.g. "08:45"
    std::string server_date;     // e.g. "09 Oct 2026"
    std::vector<Departure> departures; // DART departures
    std::string bus_route = "H1 & H2";
    std::string bus_stop_name = "H1 (Stop 917) • H2 (Stop 929)";
    std::vector<BusDeparture> bus_departures; // Dublin Bus departures
    bool success = false;
    std::string error_message;
    bool bus_success = false;
    std::string bus_error_message;
};

} // namespace dart
