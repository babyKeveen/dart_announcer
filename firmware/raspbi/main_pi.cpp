#include "../core/dart_canvas.hpp"
#include "../core/dart_parser.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

// Helper to execute curl command to fetch live Irish Rail data without external C++ library dependencies
std::string fetch_url(const std::string& url) {
    std::string cmd = "curl -s -m 10 \"" + url + "\"";
    char buffer[256];
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return "";
    }
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    return result;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char* argv[]) {
    std::string station = "SUTTN";
    std::string direction = "";
    int battery = 88;
    std::string xml_file = "";
    bool save_bmp = true;
    bool save_bin = true;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--station" && i + 1 < argc) {
            station = argv[++i];
        } else if (arg == "--direction" && i + 1 < argc) {
            direction = argv[++i];
        } else if (arg == "--battery" && i + 1 < argc) {
            battery = std::atoi(argv[++i]);
        } else if (arg == "--file" && i + 1 < argc) {
            xml_file = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: ./dart_sim [options]\n"
                      << "  --station <code>     Station code (default: SUTTN)\n"
                      << "  --direction <dir>    Direction filter: Northbound, Southbound, or empty (Both)\n"
                      << "  --battery <pct>      Simulated battery percentage (default: 88)\n"
                      << "  --file <path>        Use local XML fixture instead of live API\n"
                      << "  --help               Show this message\n";
            return 0;
        }
    }

    std::cout << "====================================================\n";
    std::cout << " DART Announce - Raspberry Pi Simulation & Prototyper\n";
    std::cout << " Target: Waveshare 2.13\" V2 (250x122)\n";
    std::cout << "====================================================\n\n";

    std::string xml_data;
    if (!xml_file.empty()) {
        std::cout << "[Source] Loading local XML fixture: " << xml_file << "\n";
        xml_data = read_file(xml_file);
    } else {
        std::string url = "http://api.irishrail.ie/realtime/realtime.asmx/getStationDataByCodeXML_WithNumMins?StationCode=" + station + "&NumMins=90";
        std::cout << "[Source] Fetching live Irish Rail API for station: " << station << "\n";
        std::cout << "[URL] " << url << "\n";
        xml_data = fetch_url(url);
    }

    if (xml_data.empty()) {
        std::cerr << "[ERROR] Failed to obtain XML data from Irish Rail!\n";
        return 1;
    }

    // 1. Parse Irish Rail XML using shared core parser
    dart::BoardData board = dart::Parser::parse_station_xml(xml_data, direction);
    std::cout << "[Parsed] Station: " << board.station_name << " (" << board.station_code << ")\n";
    std::cout << "[Parsed] Server Time: " << board.query_time << "\n";
    std::cout << "[Parsed] Upcoming departures: " << board.departures.size() << "\n\n";

    for (size_t i = 0; i < board.departures.size() && i < 5; ++i) {
        const auto& d = board.departures[i];
        std::cout << "  " << (i + 1) << ". " << d.destination
                  << " | Due: " << d.due_in << "m (" << d.expected << ")"
                  << " | Dir: " << d.direction << "\n";
    }
    std::cout << "\n";

    // 2. Render to shared 250x122 canvas
    dart::Canvas canvas;
    canvas.render_commute_board(board, battery, direction);

    // 3. Print Visual Terminal Preview (ASCII / Unicode blocks)
    std::cout << "[E-Paper 250x122 Visual Simulation]:\n";
    std::cout << canvas.generate_terminal_preview() << "\n";

    // 4. Export exact Waveshare 2.13" V2 4,000-byte hardware buffer
    if (save_bin) {
        uint8_t epd_buf[dart::Canvas::EPD_BUFFER_SIZE];
        canvas.export_waveshare_v2_buffer(epd_buf);
        std::ofstream bin_f("screen_2in13.bin", std::ios::binary);
        if (bin_f.is_open()) {
            bin_f.write((char*)epd_buf, sizeof(epd_buf));
            std::cout << "[Saved] screen_2in13.bin (" << sizeof(epd_buf) << " bytes raw hardware buffer)\n";
        }
    }

    // 5. Export 1-bit Windows BMP image
    if (save_bmp) {
        if (canvas.save_bmp("preview_2in13.bmp")) {
            std::cout << "[Saved] preview_2in13.bmp (250x122 uncompressed 1-bit bitmap)\n";
        }
    }

    std::cout << "\nSimulation completed successfully!\n";
    return 0;
}
