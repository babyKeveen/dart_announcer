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

    // High-level 800x480 kitchen commute board layout for Waveshare 7.5" HAT
    void render_commute_board(
        const BoardData& board,
        int battery_percent = -1,
        const std::string& filter_dir = "",
        int rain_chance = -1,
        float temp_c = -99.0f
    ) {
        clear(false); // Crisp white background

        // 1. Header (Black banner with white text, Y: 0 to 66)
        fill_rect(0, 0, WIDTH, 66, true);

        // Station name + direction badge
        std::string title = board.station_name.empty() ? "DART COMMUTE" : board.station_name;
        std::transform(title.begin(), title.end(), title.begin(), ::toupper);
        title = "DART • " + title;
        draw_string(24, 18, title, false, 4); // White text, scale 4 (20x28)
        int title_w = get_string_width(title, 4);

        if (!filter_dir.empty() && filter_dir != "Both") {
            std::string dir_badge = "[" + filter_dir + "]";
            std::transform(dir_badge.begin(), dir_badge.end(), dir_badge.begin(), ::toupper);
            draw_string(24 + title_w + 14, 26, dir_badge, false, 2);
        }

        // Digital Clock on top-right (Large kitchen wall clock scale 5, 35px high)
        std::string clock = board.query_time.empty() ? "--:--" : board.query_time;
        int clock_w = get_string_width(clock, 5);
        int clock_x = WIDTH - clock_w - 24;
        draw_string(clock_x, 15, clock, false, 5);

        // Battery Module next to Clock
        int bat_anchor = clock_x - 20;
        if (battery_percent >= 0) {
            draw_battery(bat_anchor - 42, 24, battery_percent, false, 2);
            std::string bat_str = std::to_string(battery_percent) + "%";
            int bs_w = get_string_width(bat_str, 2);
            draw_string(bat_anchor - 48 - bs_w, 26, bat_str, false, 2);
            bat_anchor = bat_anchor - 54 - bs_w;
        }

        // 2. Weather Telemetry Strip (Large scale 3, 21px high, readable across room)
        std::string weather_text;
        if (rain_chance >= 0) {
            char wbuf[64];
            if (temp_c > -50.0f) {
                snprintf(wbuf, sizeof(wbuf), "CHANCE OF RAIN: %d%%  •  %.1f°C", rain_chance, temp_c);
            } else {
                snprintf(wbuf, sizeof(wbuf), "CHANCE OF RAIN: %d%%", rain_chance);
            }
            weather_text = wbuf;
        } else {
            weather_text = "WEATHER DATA UNAVAILABLE";
        }

        int wt_w = get_string_width(weather_text, 3);
        if (rain_chance >= 40) {
            // Prominent inverted badge when high rain chance expected
            fill_rect(20, 71, wt_w + 14, 27, true);
            draw_string(26, 74, weather_text, false, 3);
        } else {
            draw_string(24, 74, weather_text, true, 3);
        }
        draw_line(20, 102, WIDTH - 20, 102, true);

        // 3. Column Headers Strip (Y: 108 to 136)
        fill_rect(20, 108, WIDTH - 40, 28, true);
        draw_string(28, 114, "#", false, 2);
        draw_string(64, 114, "DESTINATION", false, 2);
        draw_string(370, 114, "DUE IN", false, 2);
        draw_string(540, 114, "EXPECTED", false, 2);
        draw_string(660, 114, "STATUS", false, 2);

        // 4. Departures Table (Up to 5 rows, Y: 138 to 440)
        if (!board.success && !board.error_message.empty()) {
            draw_string(140, 240, "ERROR CONNECTING TO IRISH RAIL API", true, 3);
            draw_string(180, 280, board.error_message.substr(0, 45), true, 2);
        } else if (board.departures.empty()) {
            draw_string(160, 240, "NO UPCOMING DART DEPARTURES", true, 3);
            draw_string(220, 280, "CHECK TIMETABLE (NEXT 90 MIN)", true, 2);
        } else {
            int y_starts[5] = {140, 200, 260, 320, 380};
            size_t max_rows = std::min((size_t)5, board.departures.size());

            for (size_t i = 0; i < max_rows; ++i) {
                const auto& dep = board.departures[i];
                int row_y = y_starts[i];

                // Horizontal row divider line
                draw_line(20, row_y + 54, WIDTH - 20, row_y + 54, true);

                // Row index number (1, 2, 3, 4, 5)
                std::string idx_str = std::to_string(i + 1);
                draw_string(28, row_y + 16, idx_str, true, 3);

                // Destination name (Bold large scale 4, 28px tall!)
                std::string dest = dep.destination;
                std::transform(dest.begin(), dest.end(), dest.begin(), ::toupper);
                if (dest.length() > 12) {
                    dest = dest.substr(0, 11) + ".";
                }
                draw_string(64, row_y + 12, dest, true, 4);

                // Due in time (Bold large scale 4, 28px tall countdown!)
                std::string due_text;
                if (dep.due_in <= 0) {
                    due_text = "DUE";
                } else {
                    due_text = std::to_string(dep.due_in) + " MIN";
                }
                draw_string(370, row_y + 12, due_text, true, 4);

                // Expected clock time (scale 3, 21px tall)
                std::string exp_time = dep.expected.empty() ? "--:--" : dep.expected;
                draw_string(540, row_y + 16, exp_time, true, 3);

                // Status column (scale 3, 21px tall)
                std::string st = dep.status;
                if (st.empty() || st == "No Information") {
                    st = "ON TIME";
                } else {
                    std::transform(st.begin(), st.end(), st.begin(), ::toupper);
                }

                if (st.find("DELAY") != std::string::npos || st.find("LATE") != std::string::npos) {
                    int sw = get_string_width(st, 2);
                    fill_rect(656, row_y + 14, sw + 12, 24, true);
                    draw_string(662, row_y + 18, st, false, 2);
                } else {
                    draw_string(660, row_y + 16, st, true, 3);
                }
            }
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
