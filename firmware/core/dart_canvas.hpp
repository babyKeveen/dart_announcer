#pragma once
#include "dart_font.hpp"
#include "dart_types.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace dart {

class Canvas {
public:
    static constexpr int WIDTH = 800;
    static constexpr int HEIGHT = 480;
    static constexpr int EPD_BUFFER_SIZE = 48000; // 800 * 480 / 8 bytes

private:
    // Waveshare 7.5" V2 native layout: 1 bit per pixel (800 x 480 / 8 = 48,000 bytes)
    // 1 = White, 0 = Black (native UC8179 / PIL mode 1 / BMP format)
    uint8_t buffer[EPD_BUFFER_SIZE];

public:
    Canvas() {
        clear(false); // Default to white
    }

    const uint8_t* get_buffer() const { return buffer; }
    uint8_t* get_buffer() { return buffer; }

    void clear(bool black = false) {
        std::memset(buffer, black ? 0x00 : 0xFF, sizeof(buffer));
    }

    void invert_canvas() {
        for (size_t i = 0; i < sizeof(buffer); ++i) {
            buffer[i] = ~buffer[i];
        }
    }

    inline void set_pixel(int x, int y, bool black) {
        if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT) {
            int byte_idx = y * 100 + (x >> 3);
            uint8_t mask = 0x80 >> (x & 7);
            if (black) {
                buffer[byte_idx] &= ~mask;
            } else {
                buffer[byte_idx] |= mask;
            }
        }
    }

    inline bool get_pixel(int x, int y) const {
        if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT) {
            int byte_idx = y * 100 + (x >> 3);
            uint8_t mask = 0x80 >> (x & 7);
            return (buffer[byte_idx] & mask) == 0; // true if black
        }
        return false;
    }

    void draw_line(int x0, int y0, int x1, int y1, bool black) {
        int dx = std::abs(x1 - x0);
        int dy = std::abs(y1 - y0);
        int sx = (x0 < x1) ? 1 : -1;
        int sy = (y0 < y1) ? 1 : -1;
        int err = dx - dy;

        while (true) {
            set_pixel(x0, y0, black);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 > -dy) {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx) {
                err += dx;
                y0 += sy;
            }
        }
    }

    void draw_rect(int x, int y, int w, int h, bool black) {
        draw_line(x, y, x + w - 1, y, black);
        draw_line(x, y + h - 1, x + w - 1, y + h - 1, black);
        draw_line(x, y, x, y + h - 1, black);
        draw_line(x + w - 1, y, x + w - 1, y + h - 1, black);
    }

    void fill_rect(int x, int y, int w, int h, bool black) {
        if (w <= 0 || h <= 0) return;
        int x2 = std::min(WIDTH - 1, x + w - 1);
        int y2 = std::min(HEIGHT - 1, y + h - 1);
        int x1 = std::max(0, x);
        int y1 = std::max(0, y);
        for (int j = y1; j <= y2; ++j) {
            for (int i = x1; i <= x2; ++i) {
                set_pixel(i, j, black);
            }
        }
    }

    void draw_char(int x, int y, char c, bool black, int scale = 1) {
        const uint8_t* glyph = get_font_char(c);
        for (int col = 0; col < 5; ++col) {
            uint8_t line = glyph[col];
            for (int row = 0; row < 7; ++row) {
                if (line & (1 << row)) {
                    if (scale == 1) {
                        set_pixel(x + col, y + row, black);
                    } else {
                        fill_rect(x + col * scale, y + row * scale, scale, scale, black);
                    }
                }
            }
        }
    }

    int draw_string(int x, int y, const std::string& text, bool black, int scale = 1) {
        int cur_x = x;
        int char_spacing = 1 * scale;
        int char_width = 5 * scale;
        size_t i = 0;
        while (i < text.length()) {
            unsigned char c = (unsigned char)text[i];
            // Check for UTF-8 bullet '•' (0xE2 0x80 0xA2)
            if (c == 0xE2 && i + 2 < text.length() && (unsigned char)text[i+1] == 0x80 && (unsigned char)text[i+2] == 0xA2) {
                int dot_size = std::max(2, 2 * scale);
                int dot_x = cur_x + (char_width - dot_size) / 2;
                int dot_y = y + (7 * scale - dot_size) / 2;
                fill_rect(dot_x, dot_y, dot_size, dot_size, black);
                cur_x += char_width + char_spacing;
                i += 3;
                continue;
            }
            // Check for degree symbol '°' (0xC2 0xB0)
            if (c == 0xC2 && i + 1 < text.length() && (unsigned char)text[i+1] == 0xB0) {
                int d_size = std::max(2, 2 * scale);
                draw_rect(cur_x + scale, y, d_size, d_size, black);
                cur_x += char_width + char_spacing;
                i += 2;
                continue;
            }
            // Check for UTF-8 Irish vowels (á, é, í, ó, ú / Á, É, Í, Ó, Ú)
            if (c == 0xC3 && i + 1 < text.length()) {
                unsigned char c2 = (unsigned char)text[i+1];
                char mapped = ' ';
                switch (c2) {
                    case 0xA1: mapped = 'a'; break; // á
                    case 0x81: mapped = 'A'; break; // Á
                    case 0xA9: mapped = 'e'; break; // é
                    case 0x89: mapped = 'E'; break; // É
                    case 0xAD: mapped = 'i'; break; // í
                    case 0x8D: mapped = 'I'; break; // Í
                    case 0xB3: mapped = 'o'; break; // ó
                    case 0x93: mapped = 'O'; break; // Ó
                    case 0xBA: mapped = 'u'; break; // ú
                    case 0x9A: mapped = 'U'; break; // Ú
                    default:   mapped = ' '; break;
                }
                draw_char(cur_x, y, mapped, black, scale);
                cur_x += char_width + char_spacing;
                i += 2;
                continue;
            }
            // Check for UTF-8 dashes (en-dash 0xE2 0x80 0x93, em-dash 0xE2 0x80 0x94)
            if (c == 0xE2 && i + 2 < text.length() && (unsigned char)text[i+1] == 0x80 && ((unsigned char)text[i+2] == 0x93 || (unsigned char)text[i+2] == 0x94)) {
                draw_char(cur_x, y, '-', black, scale);
                cur_x += char_width + char_spacing;
                i += 3;
                continue;
            }
            // Unknown non-ASCII byte: skip multi-byte UTF-8 sequence
            if (c > 127) {
                if ((c & 0xE0) == 0xC0) i += 2;
                else if ((c & 0xF0) == 0xE0) i += 3;
                else if ((c & 0xF8) == 0xF0) i += 4;
                else i += 1;
                continue;
            }
            draw_char(cur_x, y, (char)c, black, scale);
            cur_x += char_width + char_spacing;
            i++;
        }
        return cur_x;
    }

    int get_string_width(const std::string& text, int scale = 1) const {
        if (text.empty()) return 0;
        int count = 0;
        size_t i = 0;
        while (i < text.length()) {
            unsigned char c = (unsigned char)text[i];
            if (c == 0xE2 && i + 2 < text.length() && (unsigned char)text[i+1] == 0x80 && (unsigned char)text[i+2] == 0xA2) {
                count++;
                i += 3;
            } else if (c == 0xC2 && i + 1 < text.length() && (unsigned char)text[i+1] == 0xB0) {
                count++;
                i += 2;
            } else if (c == 0xC3 && i + 1 < text.length()) {
                count++;
                i += 2;
            } else if (c == 0xE2 && i + 2 < text.length() && (unsigned char)text[i+1] == 0x80 && ((unsigned char)text[i+2] == 0x93 || (unsigned char)text[i+2] == 0x94)) {
                count++;
                i += 3;
            } else if (c > 127) {
                if ((c & 0xE0) == 0xC0) i += 2;
                else if ((c & 0xF0) == 0xE0) i += 3;
                else if ((c & 0xF8) == 0xF0) i += 4;
                else i += 1;
            } else {
                count++;
                i++;
            }
        }
        return count * (6 * scale) - (1 * scale);
    }

    void draw_battery(int x, int y, int percentage, bool color = false, int scale = 2) {
        percentage = std::max(0, std::min(100, percentage));
        int bw = 18 * scale;
        int bh = 9 * scale;
        int nub_w = 2 * scale;
        int nub_h = 5 * scale;

        // Battery body outline
        draw_rect(x, y, bw, bh, color);
        // Positive terminal nub
        fill_rect(x + bw, y + (bh - nub_h) / 2, nub_w, nub_h, color);

        // Clear interior to opposite color first
        bool bg = !color;
        fill_rect(x + scale, y + scale, bw - 2 * scale, bh - 2 * scale, bg);

        // Fill inner gauge proportional to percentage
        int max_fill = bw - 4 * scale;
        int fill_width = (percentage * max_fill) / 100;
        if (fill_width > 0) {
            fill_rect(x + 2 * scale, y + 2 * scale, fill_width, bh - 4 * scale, color);
        }
    }

    // Multimodal Commuter Display Layout:
    // Section 1 (Y: 0–220): DART (Next 2 Trains)
    // Section 2 (Y: 221–443): Dublin Bus (Next 2 Buses)
    // Footer (Y: 444–480): Single line for Time | Weather | Battery
    void render_commute_board(
        const BoardData& board,
        int battery_percent = -1,
        const std::string& filter_dir = "Southbound",
        int rain_chance = -1,
        float temp_c = -99.0f
    ) {
        clear(false); // Crisp white background

        // ================= SECTION 1: DART =================
        // Title Bar (Y: 0 to 38)
        fill_rect(0, 0, WIDTH, 38, true);
        std::string dart_st = board.station_name.empty() ? "SUTTON" : board.station_name;
        std::transform(dart_st.begin(), dart_st.end(), dart_st.begin(), ::toupper);
        std::string dart_dir = (filter_dir.empty() || filter_dir == "Both") ? "SOUTHBOUND" : filter_dir;
        std::transform(dart_dir.begin(), dart_dir.end(), dart_dir.begin(), ::toupper);

        std::string dart_title = "DART  •  " + dart_st + "  •  " + dart_dir;
        draw_string(24, 9, dart_title, false, 3);

        // Next 2 DART Departures
        if (board.departures.empty()) {
            std::string msg1 = "NO UPCOMING DART DEPARTURES";
            std::string msg2 = "NO TRAINS SCHEDULED (NEXT 90 MIN)";
            int w1 = get_string_width(msg1, 3);
            int w2 = get_string_width(msg2, 3);
            draw_string((WIDTH - w1) / 2, 102, msg1, true, 3);
            draw_string((WIDTH - w2) / 2, 134, msg2, true, 3);
        } else {
            size_t max_trains = std::min((size_t)2, board.departures.size());
            int y_trains[2] = {59, 123};
            for (size_t i = 0; i < max_trains; ++i) {
                const auto& dep = board.departures[i];
                int ry = y_trains[i];

                // Due in badge (solid black rectangle with white text)
                fill_rect(24, ry + 10, 116, 56, true);
                std::string due_txt = (dep.due_in <= 0) ? "DUE" : (std::to_string(dep.due_in) + " MIN");
                int due_w = get_string_width(due_txt, 3);
                int due_x = 24 + (116 - due_w) / 2;
                draw_string(due_x, ry + 27, due_txt, false, 3);

                // Destination (scale 5 for <= 12 chars, scale 4 for longer)
                std::string dest = dep.destination;
                std::transform(dest.begin(), dest.end(), dest.begin(), ::toupper);
                if (dest.length() > 16) dest = dest.substr(0, 15) + ".";
                int dest_scale = (dest.length() <= 12) ? 5 : 4;
                int dest_y = (dest_scale == 5) ? (ry + 20) : (ry + 24);
                draw_string(160, dest_y, dest, true, dest_scale);

                // Expected time
                std::string exp_txt = "EXP: " + (dep.expected.empty() ? "--:--" : dep.expected);
                draw_string(520, ry + 27, exp_txt, true, 3);

                // Status
                std::string st = dep.status;
                if (st.empty() || st == "No Information") st = "ON TIME";
                std::transform(st.begin(), st.end(), st.begin(), ::toupper);
                if (st.find("DELAY") != std::string::npos || st.find("LATE") != std::string::npos) {
                    int sw = get_string_width(st, 2);
                    fill_rect(670, ry + 23, sw + 12, 26, true);
                    draw_string(676, ry + 28, st, false, 2);
                } else {
                    draw_string(675, ry + 27, st, true, 3);
                }
            }
        }

        // ================= SECTION DIVIDER =================
        fill_rect(0, 221, WIDTH, 2, true);

        // ================= SECTION 2: DUBLIN BUS =================
        // Title Bar (Y: 223 to 261)
        fill_rect(0, 223, WIDTH, 38, true);
        std::string bus_rt = board.bus_route.empty() ? "H1" : board.bus_route;
        std::string bus_st = board.bus_stop_name.empty() ? "STOP 917 (ADMIRAL PARK)" : board.bus_stop_name;
        std::transform(bus_rt.begin(), bus_rt.end(), bus_rt.begin(), ::toupper);
        std::transform(bus_st.begin(), bus_st.end(), bus_st.begin(), ::toupper);

        std::string bus_title = "DUBLIN BUS  •  ROUTE " + bus_rt + "  •  " + bus_st;
        draw_string(24, 232, bus_title, false, 3);

        // Next 2 Bus Departures
        if (board.bus_departures.empty()) {
            std::string msg1 = "NO UPCOMING BUS DEPARTURES";
            std::string msg2 = "NO BUSES SCHEDULED (NEXT 90 MIN)";
            int w1 = get_string_width(msg1, 3);
            int w2 = get_string_width(msg2, 3);
            draw_string((WIDTH - w1) / 2, 325, msg1, true, 3);
            draw_string((WIDTH - w2) / 2, 357, msg2, true, 3);
        } else {
            size_t max_buses = std::min((size_t)2, board.bus_departures.size());
            int y_buses[2] = {282, 346};
            for (size_t i = 0; i < max_buses; ++i) {
                const auto& bus = board.bus_departures[i];
                int ry = y_buses[i];

                // Due in badge
                fill_rect(24, ry + 10, 116, 56, true);
                std::string due_txt = (bus.due_in <= 0) ? "DUE" : (std::to_string(bus.due_in) + " MIN");
                int due_w = get_string_width(due_txt, 3);
                int due_x = 24 + (116 - due_w) / 2;
                draw_string(due_x, ry + 27, due_txt, false, 3);

                // Destination
                std::string dest = bus.destination;
                std::transform(dest.begin(), dest.end(), dest.begin(), ::toupper);
                if (dest.length() > 16) dest = dest.substr(0, 15) + ".";
                int dest_scale = (dest.length() <= 12) ? 5 : 4;
                int dest_y = (dest_scale == 5) ? (ry + 20) : (ry + 24);
                draw_string(160, dest_y, dest, true, dest_scale);

                // Expected / Scheduled time
                std::string exp_txt = "EXP: " + (bus.expected.empty() ? "--:--" : bus.expected);
                draw_string(520, ry + 27, exp_txt, true, 3);

                // Status (Live / Scheduled)
                std::string st = bus.status;
                std::transform(st.begin(), st.end(), st.begin(), ::toupper);
                draw_string(675, ry + 27, st, true, 3);
            }
        }

        // ================= FOOTER: SINGLE LINE FOR TIME | WEATHER | BATTERY =================
        // Divider Line at Y: 444
        draw_line(0, 444, WIDTH, 444, true);

        // 1. Time (Left, X: 24)
        std::string clock = board.query_time.empty() ? "--:--" : board.query_time;
        std::string d_str = board.server_date.empty() ? "" : board.server_date;
        std::transform(d_str.begin(), d_str.end(), d_str.begin(), ::toupper);
        std::string time_str = "TIME: " + clock + (d_str.empty() ? "" : ("  " + d_str));
        draw_string(24, 455, time_str, true, 2);

        // Separator 1
        draw_string(275, 455, "|", true, 2);

        // 2. Weather (Center, X: 295)
        char w_buf[64];
        if (temp_c > -50.0f && rain_chance >= 0) {
            snprintf(w_buf, sizeof(w_buf), "WEATHER: %.1f°C  •  RAIN: %d%%", temp_c, rain_chance);
        } else if (temp_c > -50.0f) {
            snprintf(w_buf, sizeof(w_buf), "WEATHER: %.1f°C", temp_c);
        } else {
            strcpy(w_buf, "WEATHER: --°C");
        }
        draw_string(295, 455, w_buf, true, 2);

        if (rain_chance >= 40) {
            int ww = get_string_width(w_buf, 2);
            fill_rect(295 + ww + 10, 452, 92, 20, true);
            draw_string(295 + ww + 16, 455, "UMBRELLA", false, 2);
        }

        // Separator 2
        draw_string(545, 455, "|", true, 2);

        // 3. Battery (Right, X: 565)
        std::string bat_s = "BATTERY: " + (battery_percent >= 0 ? (std::to_string(battery_percent) + "%") : "--%");
        draw_string(565, 455, bat_s, true, 2);

        // Battery Icon
        int bat_icon_x = 736;
        int bat_icon_y = 453;
        draw_rect(bat_icon_x, bat_icon_y, 34, 16, true);
        fill_rect(bat_icon_x + 34, bat_icon_y + 4, 3, 8, true);
        if (battery_percent > 0) {
            int fw = std::min(26, std::max(2, (battery_percent * 26) / 100));
            fill_rect(bat_icon_x + 4, bat_icon_y + 4, fw, 8, true);
        }
    }

    // Scheduled Down Period: Clock & Tomorrow's Weather Screen (800x480)
    void render_clock_weather_screen(
        const std::string& station_name,
        const std::string& time_str,
        const std::string& date_str,
        int temp_max,
        int temp_min,
        int rain_chance,
        int battery_percent = -1,
        int resume_hour = 6
    ) {
        clear(false); // White background

        // 1. Header (Black banner with white text)
        fill_rect(0, 0, WIDTH, 66, true);
        std::string title = station_name + " STANDBY";
        std::transform(title.begin(), title.end(), title.begin(), ::toupper);
        title = "DART • " + title;
        draw_string(24, 18, title, false, 4);

        if (battery_percent >= 0) {
            draw_battery(WIDTH - 70, 24, battery_percent, false, 2);
            std::string bat_str = std::to_string(battery_percent) + "%";
            int bs_w = get_string_width(bat_str, 2);
            draw_string(WIDTH - 76 - bs_w, 26, bat_str, false, 2);
        }

        // 2. Main split: Big Digital Clock (left) & Tomorrow's Forecast (right)
        // Clock at scale 6 (30x42 per char)
        std::string clk = time_str.empty() ? "23:45" : time_str;
        draw_string(40, 130, clk, true, 6);

        // Date below clock
        std::string dt = date_str.empty() ? "NIGHT STANDBY MODE" : date_str;
        draw_string(44, 200, dt, true, 3);

        // Schedule status note
        char sched_buf[48];
        snprintf(sched_buf, sizeof(sched_buf), "RESUMES %02d:00 AM", resume_hour);
        draw_rect(44, 250, 260, 40, true);
        draw_string(60, 260, sched_buf, true, 3);

        draw_string(44, 320, "LOW POWER DEEP SLEEP MODE", true, 2);

        // Vertical divider separating clock from weather
        draw_line(400, 80, 400, 420, true);

        // Right side: Tomorrow's Weather
        fill_rect(430, 96, 340, 36, true);
        draw_string(446, 106, "TOMORROW COMMUTE WEATHER", false, 2);

        // Temperature (scale 4)
        char temp_buf[32];
        snprintf(temp_buf, sizeof(temp_buf), "%dC / %dC", temp_max, temp_min);
        draw_string(440, 156, temp_buf, true, 4);

        // Rain badge (inverted if rain >= 40%)
        char rain_buf[32];
        snprintf(rain_buf, sizeof(rain_buf), "RAIN CHANCE: %d%%", rain_chance);
        int rw = get_string_width(rain_buf, 3);
        if (rain_chance >= 40) {
            fill_rect(436, 230, rw + 16, 40, true);
            draw_string(444, 238, rain_buf, false, 3);
        } else {
            draw_rect(436, 230, rw + 16, 40, true);
            draw_string(444, 238, rain_buf, true, 3);
        }

        // Commuter advice
        if (rain_chance >= 40) {
            draw_string(440, 300, "BRING UMBRELLA - HEAVY RAIN", true, 2);
        } else {
            draw_string(440, 300, "CLEAR COMMUTE - NO UMBRELLA NEEDED", true, 2);
        }
        draw_string(440, 335, "NORMAL SCHEDULED SERVICE EXPECTED", true, 2);

        // 3. Footer Bar
        draw_line(0, 436, WIDTH - 1, 436, true);
        draw_string(24, 450, "STANDBY CLOCK & WEATHER  •  OPEN-METEO ENGINE", true, 2);
        draw_string(WIDTH - 210, 450, "ADAFRUIT ESP32-S3", true, 2);
    }

    // Wi-Fi Captive Portal Setup Screen (800x480)
    void render_wifi_setup_screen(const std::string& ap_name) {
        clear(false); // White background

        // 1. Header (Black banner with white text)
        fill_rect(0, 0, WIDTH, 66, true);
        draw_string(24, 18, "DART FRAME • WI-FI SETUP", false, 4);

        // 2. Body instructions
        draw_string(40, 100, "1. Connect your phone or computer to the Wi-Fi hotspot:", true, 3);

        // Box around AP name
        draw_rect(40, 140, WIDTH - 80, 56, true);
        draw_string(60, 152, ap_name, true, 4);

        draw_string(40, 226, "2. A setup portal window will pop up automatically.", true, 3);
        draw_string(40, 270, "   If not, open any browser and navigate to: http://192.168.4.1", true, 2);
        draw_string(40, 310, "3. Select your home Wi-Fi network and enter password.", true, 3);
        draw_string(40, 350, "4. Select your primary DART station and commute direction.", true, 3);

        // 3. Footer
        draw_line(0, 436, WIDTH - 1, 436, true);
        draw_string(24, 450, "SETUP PORTAL: HTTP://192.168.4.1  •  PORT 80", true, 2);
        draw_string(WIDTH - 220, 450, "WAVESHARE 7.5\" HAT", true, 2);
    }

    // Export format matching Waveshare 7.5" V2 native RAM (800 x 480 / 8 = 48,000 bytes)
    // 1 = White, 0 = Black (native UC8179)
    void export_waveshare_v2_buffer(uint8_t* out_buf) const {
        std::memcpy(out_buf, buffer, EPD_BUFFER_SIZE);
    }

    // Generate terminal preview string downsampled to 100 columns for SSH inspection
    std::string generate_terminal_preview() const {
        std::string out;
        out.reserve(102 * 62 * 4);

        out += "+";
        for (int x = 0; x < 100; ++x) out += "-";
        out += "+\n";

        for (int y = 0; y < HEIGHT; y += 8) {
            out += "|";
            for (int x = 0; x < WIDTH; x += 8) {
                // Downsample 8x8 block
                int black_count = 0;
                for (int dy = 0; dy < 8; ++dy) {
                    for (int dx = 0; dx < 8; ++dx) {
                        if (get_pixel(x + dx, y + dy)) black_count++;
                    }
                }
                out += (black_count >= 16) ? "\u2588" : " ";
            }
            out += "|\n";
        }

        out += "+";
        for (int x = 0; x < 100; ++x) out += "-";
        out += "+\n";

        return out;
    }

    std::string render_ascii_screen() const {
        return generate_terminal_preview();
    }

    template <typename StreamT>
    void print_ascii_screen(StreamT& out) const {
        out.println("+----------------------------------------------------------------------------------------------------+");
        char row_buf[104];
        for (int y = 0; y < HEIGHT; y += 8) {
            int idx = 0;
            row_buf[idx++] = '|';
            for (int x = 0; x < WIDTH; x += 8) {
                int black_count = 0;
                for (int dy = 0; dy < 8; ++dy) {
                    for (int dx = 0; dx < 8; ++dx) {
                        if (get_pixel(x + dx, y + dy)) black_count++;
                    }
                }
                row_buf[idx++] = (black_count >= 16) ? '#' : ' ';
            }
            row_buf[idx++] = '|';
            row_buf[idx] = '\0';
            out.println(row_buf);
        }
        out.println("+----------------------------------------------------------------------------------------------------+");
    }

    // Save a standard uncompressed 1-bit Windows BMP image (800x480, 48,062 bytes)
    bool save_bmp(const std::string& path) const {
        std::ofstream f(path, std::ios::binary);
        if (!f.is_open()) return false;

        int row_bytes = 100; // 800 / 8 = 100 bytes (multiple of 4)
        int image_size = row_bytes * HEIGHT; // 48,000 bytes
        int file_size = 14 + 40 + 8 + image_size; // 48,062 bytes

        uint8_t file_header[14] = {
            'B', 'M',
            (uint8_t)(file_size & 0xFF), (uint8_t)((file_size >> 8) & 0xFF),
            (uint8_t)((file_size >> 16) & 0xFF), (uint8_t)((file_size >> 24) & 0xFF),
            0, 0, 0, 0,
            62, 0, 0, 0
        };

        uint8_t info_header[40] = {
            40, 0, 0, 0,
            (uint8_t)(WIDTH & 0xFF), (uint8_t)((WIDTH >> 8) & 0xFF), 0, 0,
            (uint8_t)(HEIGHT & 0xFF), (uint8_t)((HEIGHT >> 8) & 0xFF), 0, 0,
            1, 0, 1, 0, 0, 0, 0, 0,
            (uint8_t)(image_size & 0xFF), (uint8_t)((image_size >> 8) & 0xFF),
            (uint8_t)((image_size >> 16) & 0xFF), (uint8_t)((image_size >> 24) & 0xFF),
            0, 0, 0, 0, 0, 0, 0, 0,
            2, 0, 0, 0, 0, 0, 0, 0
        };

        uint8_t color_table[8] = {
            0, 0, 0, 0,       // Color 0: Black
            255, 255, 255, 0  // Color 1: White
        };

        f.write((char*)file_header, 14);
        f.write((char*)info_header, 40);
        f.write((char*)color_table, 8);

        // BMP rows are stored bottom-to-top
        for (int y = HEIGHT - 1; y >= 0; --y) {
            f.write((char*)(buffer + y * 100), 100);
        }

        return true;
    }

    // Export 800x480 uncompressed 1-bit BMP directly to memory buffer (48,062 bytes)
    size_t export_bmp_buffer(uint8_t* out, size_t max_len) const {
        int row_bytes = 100;
        int image_size = row_bytes * HEIGHT;
        int file_size = 14 + 40 + 8 + image_size;
        if (max_len < (size_t)file_size || out == nullptr) return 0;

        uint8_t file_header[14] = {
            'B', 'M',
            (uint8_t)(file_size & 0xFF), (uint8_t)((file_size >> 8) & 0xFF),
            (uint8_t)((file_size >> 16) & 0xFF), (uint8_t)((file_size >> 24) & 0xFF),
            0, 0, 0, 0,
            62, 0, 0, 0
        };

        uint8_t info_header[40] = {
            40, 0, 0, 0,
            (uint8_t)(WIDTH & 0xFF), (uint8_t)((WIDTH >> 8) & 0xFF), 0, 0,
            (uint8_t)(HEIGHT & 0xFF), (uint8_t)((HEIGHT >> 8) & 0xFF), 0, 0,
            1, 0, 1, 0, 0, 0, 0, 0,
            (uint8_t)(image_size & 0xFF), (uint8_t)((image_size >> 8) & 0xFF),
            (uint8_t)((image_size >> 16) & 0xFF), (uint8_t)((image_size >> 24) & 0xFF),
            0, 0, 0, 0, 0, 0, 0, 0,
            2, 0, 0, 0, 0, 0, 0, 0
        };

        uint8_t color_table[8] = {
            0, 0, 0, 0,       // Color 0: Black
            255, 255, 255, 0  // Color 1: White
        };

        size_t offset = 0;
        std::memcpy(out + offset, file_header, 14); offset += 14;
        std::memcpy(out + offset, info_header, 40); offset += 40;
        std::memcpy(out + offset, color_table, 8);  offset += 8;

        for (int y = HEIGHT - 1; y >= 0; --y) {
            std::memcpy(out + offset, buffer + y * 100, 100);
            offset += 100;
        }

        return file_size;
    }
};

} // namespace dart
