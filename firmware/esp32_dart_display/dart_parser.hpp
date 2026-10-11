#pragma once
#include "dart_types.hpp"
#include <algorithm>
#include <cstdlib>
#include <string>

namespace dart {

class Parser {
public:
    static std::string extract_tag(const std::string& xml, const std::string& tag, size_t start_pos = 0, size_t* out_end = nullptr) {
        std::string open_tag = "<" + tag + ">";
        std::string close_tag = "</" + tag + ">";

        size_t open_pos = xml.find(open_tag, start_pos);
        if (open_pos == std::string::npos) {
            return "";
        }
        size_t val_start = open_pos + open_tag.length();
        size_t close_pos = xml.find(close_tag, val_start);
        if (close_pos == std::string::npos) {
            return "";
        }
        if (out_end) {
            *out_end = close_pos + close_tag.length();
        }

        std::string val = xml.substr(val_start, close_pos - val_start);
        size_t first = val.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = val.find_last_not_of(" \t\r\n");
        return val.substr(first, (last - first + 1));
    }

    static BoardData parse_station_xml(const std::string& xml, const std::string& direction_filter = "") {
        BoardData board;
        if (xml.empty()) {
            board.error_message = "Empty XML payload";
            return board;
        }

        // Check for Web Service fault or error
        if (xml.find("<faultstring>") != std::string::npos || xml.find("InvalidOperationException") != std::string::npos) {
            board.error_message = "Irish Rail API returned a service fault";
            return board;
        }

        board.query_time = extract_tag(xml, "Querytime");
        if (board.query_time.length() >= 5) {
            board.query_time = board.query_time.substr(0, 5); // Format: "HH:MM"
        }
        board.server_date = extract_tag(xml, "Traindate");

        size_t search_pos = 0;
        while (true) {
            size_t item_start = xml.find("<objStationData>", search_pos);
            if (item_start == std::string::npos) {
                break;
            }
            size_t item_end = xml.find("</objStationData>", item_start);
            if (item_end == std::string::npos) {
                break;
            }

            std::string item_xml = xml.substr(item_start, item_end - item_start + 17);
            search_pos = item_end + 17;

            Departure dep;
            dep.train_code = extract_tag(item_xml, "Traincode");
            dep.origin = extract_tag(item_xml, "Origin");
            dep.destination = extract_tag(item_xml, "Destination");

            std::string due_str = extract_tag(item_xml, "Duein");
            dep.due_in = due_str.empty() ? 0 : std::atoi(due_str.c_str());

            dep.scheduled = extract_tag(item_xml, "Schdepart");
            if (dep.scheduled.empty()) dep.scheduled = extract_tag(item_xml, "Scharrival");

            dep.expected = extract_tag(item_xml, "Expdepart");
            if (dep.expected.empty()) dep.expected = extract_tag(item_xml, "Exparrival");

            dep.direction = extract_tag(item_xml, "Direction");
            dep.train_type = extract_tag(item_xml, "Traintype");
            dep.status = extract_tag(item_xml, "Status");

            if (board.station_name.empty()) {
                board.station_name = extract_tag(item_xml, "Stationfullname");
            }
            if (board.station_code.empty()) {
                board.station_code = extract_tag(item_xml, "Stationcode");
            }

            // Direction filtering
            if (!direction_filter.empty() && direction_filter != "Both") {
                if (dep.direction != direction_filter) {
                    continue;
                }
            }

            if (!dep.destination.empty()) {
                board.departures.push_back(dep);
            }
        }

        // Sort upcoming departures by due_in ascending
        std::sort(board.departures.begin(), board.departures.end(), [](const Departure& a, const Departure& b) {
            return a.due_in < b.due_in;
        });

        board.success = true;
        return board;
    }
};

} // namespace dart
