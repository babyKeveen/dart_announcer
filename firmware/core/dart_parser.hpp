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

    static std::string strip_html_tags(const std::string& input) {
        std::string out;
        out.reserve(input.length());
        bool in_tag = false;
        for (char c : input) {
            if (c == '<') in_tag = true;
            else if (c == '>') in_tag = false;
            else if (!in_tag) out += c;
        }
        size_t first = out.find_first_not_of(" \t\r\n\xC2\xA0");
        if (first == std::string::npos) return "";
        size_t last = out.find_last_not_of(" \t\r\n\xC2\xA0");
        return out.substr(first, last - first + 1);
    }

    static std::string find_time_hhmm(const std::string& text) {
        for (size_t i = 0; i + 4 < text.length(); ++i) {
            if (std::isdigit((unsigned char)text[i]) && std::isdigit((unsigned char)text[i+1]) &&
                text[i+2] == ':' &&
                std::isdigit((unsigned char)text[i+3]) && std::isdigit((unsigned char)text[i+4])) {
                return text.substr(i, 5);
            }
        }
        return "";
    }

    static std::vector<BusDeparture> parse_bus_html(const std::string& html, const std::string& route_filter = "H1", const std::string& ref_time = "") {
        std::vector<BusDeparture> departures;
        if (html.empty()) return departures;

        int ref_mins = -1;
        if (ref_time.length() >= 5 && ref_time[2] == ':') {
            int rh = std::atoi(ref_time.substr(0, 2).c_str());
            int rm = std::atoi(ref_time.substr(3, 2).c_str());
            ref_mins = rh * 60 + rm;
        }

        std::string target_rt = route_filter;
        std::transform(target_rt.begin(), target_rt.end(), target_rt.begin(), ::toupper);

        size_t pos = 0;
        while (pos < html.length()) {
            size_t tr_start = html.find("<tr", pos);
            if (tr_start == std::string::npos) break;
            size_t tr_end = html.find("</tr>", tr_start);
            if (tr_end == std::string::npos) break;

            std::string row = html.substr(tr_start, tr_end - tr_start + 5);
            pos = tr_end + 5;

            // Find all <td> chunks
            std::vector<std::string> tds;
            size_t td_pos = 0;
            while (td_pos < row.length()) {
                size_t td_start = row.find("<td", td_pos);
                if (td_start == std::string::npos) break;
                size_t td_content = row.find('>', td_start);
                if (td_content == std::string::npos || td_content >= row.length()) break;
                size_t td_end = row.find("</td>", td_content);
                if (td_end == std::string::npos) break;

                tds.push_back(row.substr(td_content + 1, td_end - td_content - 1));
                td_pos = td_end + 5;
            }

            if (tds.size() < 3) continue;

            // 1. Route
            std::string route = strip_html_tags(tds[0]);
            std::string route_up = route;
            std::transform(route_up.begin(), route_up.end(), route_up.begin(), ::toupper);
            if (!target_rt.empty() && route_up != target_rt) {
                continue;
            }

            // 2. Destination (strip vehicle div and newlines if present)
            std::string dest_cell = tds[1];
            size_t div_pos = dest_cell.find("<div");
            if (div_pos != std::string::npos) {
                dest_cell = dest_cell.substr(0, div_pos);
            }
            std::string dest = strip_html_tags(dest_cell);
            size_t nl_pos = dest.find_first_of("\r\n");
            if (nl_pos != std::string::npos) {
                dest = dest.substr(0, nl_pos);
            }

            // 3. Scheduled time
            std::string sched = find_time_hhmm(tds[2]);

            // 4. Expected time (if 4th td exists and has a valid time)
            std::string exp = "";
            bool is_live = false;
            if (tds.size() >= 4) {
                exp = find_time_hhmm(tds[3]);
                if (!exp.empty()) {
                    is_live = true;
                }
            }
            if (exp.empty()) exp = sched;

            BusDeparture dep;
            dep.route = route.empty() ? route_filter : route;
            dep.destination = dest.empty() ? "Dublin City" : dest;
            dep.scheduled = sched.empty() ? "--:--" : sched;
            dep.expected = exp.empty() ? "--:--" : exp;
            dep.status = is_live ? "Live" : "Sched";

            // Calculate due_in if ref_mins is available
            if (ref_mins >= 0 && dep.expected.length() >= 5) {
                int bh = std::atoi(dep.expected.substr(0, 2).c_str());
                int bm = std::atoi(dep.expected.substr(3, 2).c_str());
                int bus_mins = bh * 60 + bm;
                int diff = bus_mins - ref_mins;
                if (diff < -720) diff += 1440;
                else if (diff > 720) diff -= 1440;

                if (diff < -5) {
                    // Already departed
                    continue;
                }
                dep.due_in = std::max(0, diff);
            } else {
                dep.due_in = 0;
            }

            departures.push_back(dep);
        }

        std::sort(departures.begin(), departures.end(), [](const BusDeparture& a, const BusDeparture& b) {
            return a.due_in < b.due_in;
        });

        return departures;
    }
};

} // namespace dart
