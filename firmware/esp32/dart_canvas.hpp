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
    static constexpr int WIDTH = 250;
    static constexpr int HEIGHT = 122;
    static constexpr int EPD_BUFFER_SIZE = 4000; // 16 bytes * 250 rows

private:
    // 0 = White, 1 = Black
    uint8_t pixels[WIDTH * HEIGHT];

public:
    Canvas() {
        clear(false); // Default to white
    }

    void clear(bool black = false) {
        std::memset(pixels, black ? 1 : 0, sizeof(pixels));
    }

    inline void set_pixel(int x, int y, bool black) {
        if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT) {
            pixels[y * WIDTH + x] = black ? 1 : 0;
        }
    }

    inline bool get_pixel(int x, int y) const {
        if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT) {
            return pixels[y * WIDTH + x] != 0;
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
        for (int j = y; j < y + h; ++j) {
            for (int i = x; i < x + w; ++i) {
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
        for (char c : text) {
            draw_char(cur_x, y, c, black, scale);
            cur_x += char_width + char_spacing;
        }
        return cur_x;
    }

    int get_string_width(const std::string& text, int scale = 1) const {
        if (text.empty()) return 0;
        return (int)text.length() * (6 * scale) - (1 * scale);
    }

    void draw_battery(int x, int y, int percentage) {
        percentage = std::max(0, std::min(100, percentage));
        // Battery body (width: 18, height: 9)
        draw_rect(x, y, 18, 9, true);
        // Battery positive terminal nub
        fill_rect(x + 18, y + 2, 2, 5, true);

        // Fill inner gauge
        int fill_width = (percentage * 14) / 100;
        if (fill_width > 0) {
            fill_rect(x + 2, y + 2, fill_width, 5, true);
        }
    }

    // High-level kitchen morning commute board layout
    void render_commute_board(const BoardData& board, int battery_percent = -1, const std::string& filter_dir = "") {
        clear(false); // White background

        // 1. Header (Black banner with white text)
        fill_rect(0, 0, WIDTH, 19, true);

        // Station name + direction badge
        std::string title = board.station_name.empty() ? "DART COMMUTE" : board.station_name;
        std::transform(title.begin(), title.end(), title.begin(), ::toupper);
        if (!filter_dir.empty() && filter_dir != "Both") {
            title += (filter_dir == "Southbound") ? " [SB]" : " [NB]";
        }
        draw_string(4, 3, title, false, 2); // White text, scale 2 (10x14)

        // Clock & Battery on top-right
        int right_pos = WIDTH - 4;
        if (battery_percent >= 0) {
            draw_battery(right_pos - 20, 5, battery_percent);
            right_pos -= 26;
        }

        std::string clock = board.query_time.empty() ? "--:--" : board.query_time;
        int clock_w = get_string_width(clock, 2);
        draw_string(right_pos - clock_w, 3, clock, false, 2);

        // 2. Departures Section (Rows at Y: 22, 50, 78)
        if (!board.success && !board.error_message.empty()) {
            draw_string(10, 45, "ERROR CONNECTING TO API", true, 2);
            draw_string(10, 70, board.error_message.substr(0, 30), true, 1);
        } else if (board.departures.empty()) {
            draw_string(15, 45, "NO UPCOMING DART TRAINS", true, 2);
            draw_string(25, 70, "CHECK TIMETABLE (NEXT 90 MIN)", true, 1);
        } else {
            int y_starts[3] = {23, 51, 79};
            size_t max_rows = std::min((size_t)3, board.departures.size());

            for (size_t i = 0; i < max_rows; ++i) {
                const auto& dep = board.departures[i];
                int row_y = y_starts[i];

                // Subtle dotted divider between rows
                if (i > 0) {
                    for (int dx = 2; dx < WIDTH - 2; dx += 3) {
                        set_pixel(dx, row_y - 2, true);
                    }
                }

                // Train number circle / index badge
                std::string idx_str = std::to_string(i + 1);
                draw_string(5, row_y + 3, idx_str, true, 2);

                // Destination name (bold/large)
                std::string dest = dep.destination;
                std::transform(dest.begin(), dest.end(), dest.begin(), ::toupper);
                // Truncate if destination name is very long
                if (dest.length() > 11) {
                    dest = dest.substr(0, 10) + ".";
                }
                draw_string(22, row_y + 3, dest, true, 2);

                // Due in badge / time
                std::string due_text;
                if (dep.due_in <= 0) {
                    due_text = "DUE";
                } else {
                    due_text = std::to_string(dep.due_in) + "m";
                }

                int due_w = get_string_width(due_text, 2);
                int badge_x = WIDTH - due_w - 8;
                int badge_y = row_y + 1;
                int badge_h = 18;
                int badge_pad = 4;

                if (dep.due_in <= 0) {
                    // Inverted badge for "DUE" (blinking/high urgency)
                    fill_rect(badge_x - badge_pad, badge_y, due_w + badge_pad * 2, badge_h, true);
                    draw_string(badge_x, row_y + 3, due_text, false, 2);
                } else {
                    draw_rect(badge_x - badge_pad, badge_y, due_w + badge_pad * 2, badge_h, true);
                    draw_string(badge_x, row_y + 3, due_text, true, 2);
                }

                // Expected clock time next to destination if room
                if (!dep.expected.empty() && dest.length() <= 8) {
                    draw_string(132, row_y + 5, dep.expected, true, 1);
                }
            }
        }

        // 3. Footer Bar (Y: 106 to 121)
        draw_line(0, 106, WIDTH - 1, 106, true);
        draw_string(4, 110, "LIVE IRISH RAIL REALTIME", true, 1);
        std::string footer_status = "DART COMMUTE";
        int fs_w = get_string_width(footer_status, 1);
        draw_string(WIDTH - fs_w - 4, 110, footer_status, true, 1);
    }

    // Export format matching Waveshare 2.13" V2 native RAM (16 bytes * 250 lines)
    // 1 = White, 0 = Black
    void export_waveshare_v2_buffer(uint8_t* out_buf) const {
        std::memset(out_buf, 0xFF, EPD_BUFFER_SIZE); // Default all white

        for (int x = 0; x < WIDTH; ++x) {
            for (int y = 0; y < HEIGHT; ++y) {
                bool is_black = get_pixel(x, y);

                // Rotate 90 degrees landscape into physical coordinates:
                // x_phys = y (0..121), y_phys = 249 - x (0..249)
                int x_phys = y;
                int y_phys = (WIDTH - 1) - x;

                if (x_phys >= 0 && x_phys < 128 && y_phys >= 0 && y_phys < 250) {
                    int byte_idx = (x_phys / 8) + y_phys * 16;
                    int bit_idx = 7 - (x_phys % 8);

                    if (is_black) {
                        out_buf[byte_idx] &= ~(1 << bit_idx); // Clear bit = Black
                    } else {
                        out_buf[byte_idx] |= (1 << bit_idx);  // Set bit = White
                    }
                }
            }
        }
    }

    // Generate terminal preview string using Unicode half blocks for SSH inspection
    std::string generate_terminal_preview() const {
        std::string out;
        out.reserve(WIDTH * (HEIGHT / 2 + 2) * 4);

        out += "+";
        for (int x = 0; x < WIDTH; ++x) out += "-";
        out += "+\n";

        for (int y = 0; y < HEIGHT; y += 2) {
            out += "|";
            for (int x = 0; x < WIDTH; ++x) {
                bool top = get_pixel(x, y);
                bool bottom = (y + 1 < HEIGHT) ? get_pixel(x, y + 1) : false;

                if (top && bottom) {
                    out += "\u2588"; // Full black block █
                } else if (top && !bottom) {
                    out += "\u2580"; // Top half block ▀
                } else if (!top && bottom) {
                    out += "\u2584"; // Bottom half block ▄
                } else {
                    out += " ";      // Space (white)
                }
            }
            out += "|\n";
        }

        out += "+";
        for (int x = 0; x < WIDTH; ++x) out += "-";
        out += "+\n";

        return out;
    }

    // Save a standard uncompressed 1-bit Windows BMP image (250x122)
    bool save_bmp(const std::string& path) const {
        std::ofstream f(path, std::ios::binary);
        if (!f.is_open()) return false;

        int row_bytes = ((WIDTH + 31) / 32) * 4; // 32-bit aligned row size
        int image_size = row_bytes * HEIGHT;
        int file_size = 14 + 40 + 8 + image_size;

        uint8_t file_header[14] = {
            'B', 'M',
            (uint8_t)(file_size & 0xFF), (uint8_t)((file_size >> 8) & 0xFF),
            (uint8_t)((file_size >> 16) & 0xFF), (uint8_t)((file_size >> 24) & 0xFF),
            0, 0, 0, 0,
            62, 0, 0, 0 // Offset to pixel data (14 + 40 + 8 = 62)
        };

        uint8_t info_header[40] = {
            40, 0, 0, 0,
            (uint8_t)(WIDTH & 0xFF), (uint8_t)((WIDTH >> 8) & 0xFF), 0, 0,
            (uint8_t)(HEIGHT & 0xFF), (uint8_t)((HEIGHT >> 8) & 0xFF), 0, 0,
            1, 0,       // 1 color plane
            1, 0,       // 1 bit per pixel
            0, 0, 0, 0, // No compression
            (uint8_t)(image_size & 0xFF), (uint8_t)((image_size >> 8) & 0xFF),
            (uint8_t)((image_size >> 16) & 0xFF), (uint8_t)((image_size >> 24) & 0xFF),
            0, 0, 0, 0,
            0, 0, 0, 0,
            2, 0, 0, 0, // 2 colors used
            0, 0, 0, 0
        };

        // Color table: Index 0 = Black, Index 1 = White
        uint8_t color_table[8] = {
            0, 0, 0, 0,       // Color 0: Black
            255, 255, 255, 0  // Color 1: White
        };

        f.write((char*)file_header, 14);
        f.write((char*)info_header, 40);
        f.write((char*)color_table, 8);

        // BMP rows are stored bottom-to-top
        std::vector<uint8_t> row(row_bytes, 0);
        for (int y = HEIGHT - 1; y >= 0; --y) {
            std::fill(row.begin(), row.end(), 0);
            for (int x = 0; x < WIDTH; ++x) {
                bool black = get_pixel(x, y);
                if (!black) {
                    // White is bit 1
                    row[x / 8] |= (1 << (7 - (x % 8)));
                }
            }
            f.write((char*)row.data(), row_bytes);
        }

        return true;
    }
};

} // namespace dart
