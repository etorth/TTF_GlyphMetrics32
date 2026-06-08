#include <SDL.h>
#include <SDL_ttf.h>
#include <algorithm>
#include <charconv>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// 1. 返回的数据结构定义
struct SingleGlyphData {
    SDL_Surface* min_surface; // 包含字符最小包围盒的 Surface
    int left_padding;         // 预留的左边空白像素数量
    int right_padding;        // 预留的右边空白像素数量
    int baseline_y;           // 基线相对于返回的 Surface 最上一行的像素数量
    int advance;              // glyph 排版推进宽度，空格等无像素字符也需要保留
    int metrics_minx;         // TTF_GlyphMetrics32() 返回的 minx
    int metrics_maxx;         // TTF_GlyphMetrics32() 返回的 maxx
    int metrics_miny;         // TTF_GlyphMetrics32() 返回的 miny
    int metrics_maxy;         // TTF_GlyphMetrics32() 返回的 maxy
    int metrics_advance;      // TTF_GlyphMetrics32() 返回的 advance
    int tex_orig_w;           // TTF_RenderGlyph32_Blended() 返回 surface 的原始宽度
    int tex_orig_h;           // TTF_RenderGlyph32_Blended() 返回 surface 的原始高度
    int glyph_is_provided32;   // TTF_GlyphIsProvided32() 的返回值
};

struct RenderedGlyphData {
    SDL_Surface* surface;
    int baseline_y;
    int left_padding;
    int right_padding;
    int advance;
};

struct ProgramOptions {
    const char* font_file = nullptr;
    int font_size = 0;
    std::string_view utf8_text;
    bool draw_texture_box = false;
    bool draw_bounding_box = false;
    bool draw_baseline = false;
};

ProgramOptions g_options;
std::vector<std::string_view> g_chars_to_draw;
std::vector<SingleGlyphData> g_glyphs;
std::vector<RenderedGlyphData> g_horizontal_cropped_surfaces;
std::vector<RenderedGlyphData> g_glyph32_surfaces;
std::vector<RenderedGlyphData> g_utf8_char_surfaces;

TTF_Font* g_font = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_single_text_texture = nullptr;
SDL_Color g_text_color = { 255, 255, 255, 255 };

int g_single_text_w = 0;
int g_single_text_h = 0;
int g_single_text_baseline_y = 0;

void PrintUsage(std::ostream& os, const char* program) {
    os << "Usage: " << program
       << " --font-file <font-file>"
       << " --font-size <font-size-in-TTF_Open>"
       << " --utf8-text <utf8-string-to-render>"
       << " [--draw-texture-box]"
       << " [--draw-bounding-box]"
       << " [--draw-baseline]\n";
}

bool IsOptionWithValue(std::string_view arg, std::string_view option) {
    return arg == option ||
           (arg.size() > option.size() && arg.compare(0, option.size(), option) == 0 &&
            arg[option.size()] == '=');
}

bool ReadOptionValue(int argc, char* argv[], int& index, std::string_view option, std::string_view& value) {
    const std::string_view arg = argv[index];
    if (arg == option) {
        if (index + 1 >= argc) {
            std::cerr << option << " requires a value\n";
            return false;
        }
        value = argv[++index];
        return true;
    }

    value = arg.substr(option.size() + 1);
    return true;
}

bool ParsePositiveInt(std::string_view value, int& output) {
    output = 0;
    const auto parse_result = std::from_chars(value.data(), value.data() + value.size(), output);
    return parse_result.ec == std::errc{} && parse_result.ptr == value.data() + value.size() && output > 0;
}

bool ParseOptions(int argc, char* argv[], ProgramOptions& options, bool& help_requested) {
    help_requested = false;
    bool has_font_file = false;
    bool has_font_size = false;
    bool has_utf8_text = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        std::string_view value;

        if (arg == "--help" || arg == "-h") {
            help_requested = true;
            return true;
        }
        if (IsOptionWithValue(arg, "--font-file")) {
            if (!ReadOptionValue(argc, argv, i, "--font-file", value)) {
                return false;
            }
            if (value.empty()) {
                std::cerr << "--font-file requires a non-empty value\n";
                return false;
            }
            options.font_file = value.data();
            has_font_file = true;
            continue;
        }
        if (IsOptionWithValue(arg, "--font-size")) {
            if (!ReadOptionValue(argc, argv, i, "--font-size", value)) {
                return false;
            }
            if (!ParsePositiveInt(value, options.font_size)) {
                std::cerr << "--font-size must be a positive integer\n";
                return false;
            }
            has_font_size = true;
            continue;
        }
        if (IsOptionWithValue(arg, "--utf8-text")) {
            if (!ReadOptionValue(argc, argv, i, "--utf8-text", value)) {
                return false;
            }
            options.utf8_text = value;
            has_utf8_text = true;
            continue;
        }
        if (arg == "--draw-texture-box") {
            options.draw_texture_box = true;
            continue;
        }
        if (arg == "--draw-bounding-box") {
            options.draw_bounding_box = true;
            continue;
        }
        if (arg == "--draw-baseline") {
            options.draw_baseline = true;
            continue;
        }
        std::cerr << "Unknown option: " << arg << '\n';
        return false;
    }

    if (!has_font_file || !has_font_size || !has_utf8_text) {
        std::cerr << "Missing required option:";
        if (!has_font_file) {
            std::cerr << " --font-file";
        }
        if (!has_font_size) {
            std::cerr << " --font-size";
        }
        if (!has_utf8_text) {
            std::cerr << " --utf8-text";
        }
        std::cerr << '\n';
        return false;
    }

    return true;
}

bool DecodeSingleUTF8(std::string_view sv, Uint32& out) {
    out = 0;
    const auto byte = [&](std::size_t i) {
        return static_cast<unsigned char>(sv[i]);
    };

    if (sv.size() == 1 && byte(0) <= 0x7F) {
        out = byte(0);
        return true;
    }
    if (sv.size() == 2 && byte(0) >= 0xC2 && byte(0) <= 0xDF &&
        (byte(1) & 0xC0) == 0x80) {
        out = ((byte(0) & 0x1F) << 6) | (byte(1) & 0x3F);
        return true;
    }
    if (sv.size() == 3 && (byte(1) & 0xC0) == 0x80 && (byte(2) & 0xC0) == 0x80) {
        if ((byte(0) == 0xE0 && byte(1) < 0xA0) || (byte(0) == 0xED && byte(1) >= 0xA0) ||
            byte(0) < 0xE0 || byte(0) > 0xEF) {
            return false;
        }
        out = ((byte(0) & 0x0F) << 12) | ((byte(1) & 0x3F) << 6) | (byte(2) & 0x3F);
        return true;
    }
    if (sv.size() == 4 && (byte(1) & 0xC0) == 0x80 && (byte(2) & 0xC0) == 0x80 &&
        (byte(3) & 0xC0) == 0x80) {
        if ((byte(0) == 0xF0 && byte(1) < 0x90) || (byte(0) == 0xF4 && byte(1) > 0x8F) ||
            byte(0) < 0xF0 || byte(0) > 0xF4) {
            return false;
        }
        out = ((byte(0) & 0x07) << 18) | ((byte(1) & 0x3F) << 12) |
              ((byte(2) & 0x3F) << 6) | (byte(3) & 0x3F);
        return true;
    }
    return false;
}

bool SplitUTF8String(std::string_view text, std::vector<std::string_view>& chars) {
    chars.clear();

    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t len = 0;
        if (c <= 0x7F) {
            len = 1;
        } else if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
        } else {
            return false;
        }

        if (i + len > text.size()) {
            return false;
        }

        std::string_view utf8_char = text.substr(i, len);
        Uint32 ignored = 0;
        if (!DecodeSingleUTF8(utf8_char, ignored)) {
            return false;
        }

        chars.push_back(utf8_char);
        i += len;
    }

    return !chars.empty();
}

std::string DisplayChar(std::string_view utf8_char) {
    if (utf8_char == " ") {
        return "<space>";
    }
    if (utf8_char == "\t") {
        return "<tab>";
    }
    return std::string(utf8_char);
}

// 获取单字排版数据与最小 Surface
SingleGlyphData GetSingleGlyphMinBoundingBox(TTF_Font* font, std::string_view utf8_char, SDL_Color color) {
    SingleGlyphData data = { nullptr, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (!font || utf8_char.empty()) return data;

    Uint32 ch = 0;
    if (!DecodeSingleUTF8(utf8_char, ch)) {
        std::cerr << "输入不是刚好一个合法 UTF-8 字符\n";
        return data;
    }
    data.glyph_is_provided32 = TTF_GlyphIsProvided32(font, ch);

    int minx, maxx, miny, maxy, advance;
    if (TTF_GlyphMetrics32(font, ch, &minx, &maxx, &miny, &maxy, &advance) != 0) {
        return data;
    }

    data.left_padding = minx;
    data.right_padding = advance - maxx;
    data.baseline_y = maxy;
    data.advance = advance;
    data.metrics_minx = minx;
    data.metrics_maxx = maxx;
    data.metrics_miny = miny;
    data.metrics_maxy = maxy;
    data.metrics_advance = advance;

    SDL_Surface* rendered = TTF_RenderGlyph32_Blended(font, ch, color);
    if (!rendered) {
        return data;
    }
    data.tex_orig_w = rendered->w;
    data.tex_orig_h = rendered->h;

    const int glyph_w = maxx - minx;
    const int glyph_h = maxy - miny;
    if (glyph_w <= 0 || glyph_h <= 0) {
        SDL_FreeSurface(rendered);
        return data;
    }

    data.min_surface = SDL_CreateRGBSurfaceWithFormat(0, glyph_w, glyph_h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!data.min_surface) {
        SDL_FreeSurface(rendered);
        return data;
    }

    SDL_FillRect(data.min_surface, nullptr, SDL_MapRGBA(data.min_surface->format, 0, 0, 0, 0));
    SDL_SetSurfaceBlendMode(rendered, SDL_BLENDMODE_NONE);

    // SDL_ttf 2.24 的 TTF_RenderGlyph32_* 实际走 TTF_RenderUTF8_*，返回的是行高 surface。
    // 单 glyph bitmap 在这个 surface 中的位置是 xstart + minx、ystart + ascent - maxy。
    SDL_Rect src {
        std::max(0, minx),
        std::max(0, TTF_FontAscent(font) - maxy),
        glyph_w,
        glyph_h
    };
    if (SDL_BlitSurface(rendered, &src, data.min_surface, nullptr) != 0) {
        SDL_FreeSurface(data.min_surface);
        data.min_surface = nullptr;
    }

    SDL_FreeSurface(rendered);
    return data;
}

RenderedGlyphData RenderSingleGlyph(TTF_Font* font, std::string_view utf8_char, SDL_Color color, bool render_as_utf8) {
    RenderedGlyphData data = { nullptr, font ? TTF_FontAscent(font) : 0, 0, 0, 0 };
    if (!font || utf8_char.empty()) return data;

    Uint32 ch = 0;
    if (!DecodeSingleUTF8(utf8_char, ch)) {
        std::cerr << "输入不是刚好一个合法 UTF-8 字符\n";
        return data;
    }

    int minx, maxx, miny, maxy, advance;
    bool has_visible_metrics = false;
    if (TTF_GlyphMetrics32(font, ch, &minx, &maxx, &miny, &maxy, &advance) == 0) {
        data.baseline_y = std::max(TTF_FontAscent(font), maxy);
        data.left_padding = minx;
        data.right_padding = advance - maxx;
        data.advance = advance;
        has_visible_metrics = (maxx - minx > 0) && (maxy - miny > 0);
    }

    if (render_as_utf8) {
        const std::string utf8_string(utf8_char);
        data.surface = TTF_RenderUTF8_Blended(font, utf8_string.c_str(), color);
    } else {
        data.surface = TTF_RenderGlyph32_Blended(font, ch, color);
    }

    if (!data.surface) {
        std::cerr << (render_as_utf8 ? "TTF_RenderUTF8_Blended" : "TTF_RenderGlyph32_Blended")
                  << " failed: " << TTF_GetError() << "\n";
    } else if (!has_visible_metrics) {
        data.left_padding = 0;
        data.right_padding = 0;
        data.advance = data.surface->w;
    }
    return data;
}

RenderedGlyphData RenderHorizontallyCroppedGlyph(TTF_Font* font, std::string_view utf8_char, SDL_Color color) {
    RenderedGlyphData data = { nullptr, font ? TTF_FontAscent(font) : 0, 0, 0, 0 };
    if (!font || utf8_char.empty()) return data;

    Uint32 ch = 0;
    if (!DecodeSingleUTF8(utf8_char, ch)) {
        std::cerr << "输入不是刚好一个合法 UTF-8 字符\n";
        return data;
    }

    int minx, maxx, miny, maxy, advance;
    if (TTF_GlyphMetrics32(font, ch, &minx, &maxx, &miny, &maxy, &advance) != 0) {
        return data;
    }

    data.left_padding = minx;
    data.right_padding = advance - maxx;
    data.baseline_y = std::max(TTF_FontAscent(font), maxy);
    data.advance = advance;

    SDL_Surface* rendered = TTF_RenderGlyph32_Blended(font, ch, color);
    if (!rendered) {
        std::cerr << "TTF_RenderGlyph32_Blended failed: " << TTF_GetError() << "\n";
        return data;
    }

    const int glyph_w = maxx - minx;
    if (glyph_w <= 0) {
        data.left_padding = 0;
        data.right_padding = 0;
        data.advance = rendered->w;
        data.surface = rendered;
        return data;
    }

    data.surface = SDL_CreateRGBSurfaceWithFormat(0, glyph_w, rendered->h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!data.surface) {
        SDL_FreeSurface(rendered);
        return data;
    }

    SDL_FillRect(data.surface, nullptr, SDL_MapRGBA(data.surface->format, 0, 0, 0, 0));
    SDL_SetSurfaceBlendMode(rendered, SDL_BLENDMODE_NONE);

    SDL_Rect src {
        std::max(0, minx),
        0,
        glyph_w,
        rendered->h
    };
    if (SDL_BlitSurface(rendered, &src, data.surface, nullptr) != 0) {
        SDL_FreeSurface(data.surface);
        data.surface = nullptr;
    }

    SDL_FreeSurface(rendered);
    return data;
}

void DrawTextureBox(const SDL_Rect& rect) {
    if (!g_options.draw_texture_box) {
        return;
    }

    SDL_SetRenderDrawColor(g_renderer, 60, 255, 60, 100);
    SDL_RenderDrawRect(g_renderer, &rect);
}

void DrawBoundingBox(const SDL_Rect& texture_dest, int left_padding, int right_padding) {
    if (!g_options.draw_bounding_box) {
        return;
    }

    SDL_Rect rect {
        texture_dest.x - left_padding,
        texture_dest.y,
        left_padding + texture_dest.w + right_padding,
        texture_dest.h
    };
    if (rect.w <= 0 || rect.h <= 0) {
        return;
    }

    SDL_SetRenderDrawColor(g_renderer, 255, 210, 60, 160);
    SDL_RenderDrawRect(g_renderer, &rect);
}

void DrawEmptyBoundingBox(int start_x, int baseline_y, int advance) {
    if (!g_options.draw_bounding_box || advance <= 0) {
        return;
    }

    SDL_Rect rect {
        start_x,
        baseline_y - TTF_FontAscent(g_font),
        advance,
        TTF_FontAscent(g_font) - TTF_FontDescent(g_font)
    };
    if (rect.h <= 0) {
        return;
    }

    SDL_SetRenderDrawColor(g_renderer, 255, 210, 60, 160);
    SDL_RenderDrawRect(g_renderer, &rect);
}

void DrawTexture(SDL_Texture* texture, const SDL_Rect& dest, int left_padding = 0, int right_padding = 0) {
    DrawBoundingBox(dest, left_padding, right_padding);
    DrawTextureBox(dest);
    SDL_RenderCopy(g_renderer, texture, nullptr, &dest);
}

void DrawSurface(SDL_Surface* surface, const SDL_Rect& dest, int left_padding = 0, int right_padding = 0) {
    SDL_Texture* texture = SDL_CreateTextureFromSurface(g_renderer, surface);
    if (!texture) {
        std::cerr << "SDL_CreateTextureFromSurface failed: " << SDL_GetError() << "\n";
        return;
    }

    DrawTexture(texture, dest, left_padding, right_padding);
    SDL_DestroyTexture(texture);
}

void DrawCroppedGlyphRow(int start_x, int baseline_y) {
    int cursor_x = start_x;
    for (const auto& glyph : g_glyphs) {
        const int texture_w = glyph.min_surface ? glyph.min_surface->w : 0;
        if (glyph.min_surface) {
            SDL_Rect dest {
                cursor_x + glyph.left_padding,
                baseline_y - glyph.baseline_y,
                texture_w,
                glyph.min_surface->h
            };
            DrawSurface(glyph.min_surface, dest, glyph.left_padding, glyph.right_padding);
        } else {
            DrawEmptyBoundingBox(cursor_x, baseline_y, glyph.advance);
        }

        cursor_x += glyph.advance;
    }
}

void DrawSingleTextTextureRow(int start_x, int baseline_y) {
    if (!g_single_text_texture) {
        return;
    }

    SDL_Rect dest {
        start_x,
        baseline_y - g_single_text_baseline_y,
        g_single_text_w,
        g_single_text_h
    };
    DrawTexture(g_single_text_texture, dest);
}

void DrawHorizontallyCroppedGlyphRow(int start_x, int baseline_y) {
    int cursor_x = start_x;
    for (const auto& glyph : g_horizontal_cropped_surfaces) {
        const int texture_w = glyph.surface ? glyph.surface->w : 0;
        if (glyph.surface) {
            SDL_Rect dest {
                cursor_x + glyph.left_padding,
                baseline_y - glyph.baseline_y,
                texture_w,
                glyph.surface->h
            };
            DrawSurface(glyph.surface, dest, glyph.left_padding, glyph.right_padding);
        } else {
            DrawEmptyBoundingBox(cursor_x, baseline_y, glyph.advance);
        }

        cursor_x += glyph.advance;
    }
}

void DrawGlyph32BlendedRow(int start_x, int baseline_y) {
    int cursor_x = start_x;
    for (const auto& glyph : g_glyph32_surfaces) {
        if (!glyph.surface) {
            DrawEmptyBoundingBox(cursor_x, baseline_y, glyph.advance);
            cursor_x += glyph.advance;
            continue;
        }

        SDL_Rect dest {
            cursor_x,
            baseline_y - glyph.baseline_y,
            glyph.surface->w,
            glyph.surface->h
        };
        DrawSurface(glyph.surface, dest, glyph.left_padding, glyph.right_padding);
        cursor_x += glyph.surface->w;
    }
}

void DrawUtf8CharBlendedRow(int start_x, int baseline_y) {
    int cursor_x = start_x;
    for (const auto& glyph : g_utf8_char_surfaces) {
        if (!glyph.surface) {
            DrawEmptyBoundingBox(cursor_x, baseline_y, glyph.advance);
            cursor_x += glyph.advance;
            continue;
        }

        SDL_Rect dest {
            cursor_x,
            baseline_y - glyph.baseline_y,
            glyph.surface->w,
            glyph.surface->h
        };
        DrawSurface(glyph.surface, dest, glyph.left_padding, glyph.right_padding);
        cursor_x += glyph.surface->w;
    }
}

int main(int argc, char* argv[]) {
    bool help_requested = false;
    if (!ParseOptions(argc, argv, g_options, help_requested)) {
        PrintUsage(std::cerr, argv[0]);
        return 1;
    }
    if (help_requested) {
        PrintUsage(std::cout, argv[0]);
        return 0;
    }

    if (!SplitUTF8String(g_options.utf8_text, g_chars_to_draw)) {
        std::cerr << "--utf8-text must be a non-empty valid UTF-8 string\n";
        return 1;
    }

    // 初始化 SDL2 与 SDL2_ttf
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL 初始化失败: " << SDL_GetError() << "\n";
        return -1;
    }
    if (TTF_Init() < 0) {
        std::cerr << "TTF 初始化失败: " << TTF_GetError() << "\n";
        SDL_Quit();
        return -1;
    }

    // 创建窗口和渲染器
    SDL_Window* window = SDL_CreateWindow("SDL2_ttf Metrics Demo", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600, SDL_WINDOW_SHOWN);
    g_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    g_font = TTF_OpenFont(g_options.font_file, g_options.font_size);
    if (!g_font) {
        std::cerr << "字体加载失败: " << g_options.font_file << ". Error: " << TTF_GetError() << "\n";
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return -1;
    }

    // TTF_RenderUTF8_Blended() 渲染整行时，真实基线位置是 font ascent 加上内部 ystart。
    // 像素字体经过 hinting 后，某些 glyph 的 maxy 可能比 TTF_FontAscent() 多 1px；
    // SDL_ttf 会用 ystart 把整行下移，避免裁掉这顶部 1px。第一行逐字绘制时按
    // 每个 glyph 的 maxy 对齐，所以第二行整段 texture 也要使用相同的有效基线。
    g_single_text_baseline_y = TTF_FontAscent(g_font);

    SDL_Surface* single_text_surface = TTF_RenderUTF8_Blended(g_font, g_options.utf8_text.data(), g_text_color);
    if (!single_text_surface) {
        std::cerr << "TTF_RenderUTF8_Blended failed: " << TTF_GetError() << "\n";
        TTF_CloseFont(g_font);
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return -1;
    }

    g_single_text_w = single_text_surface->w;
    g_single_text_h = single_text_surface->h;
    g_single_text_texture = SDL_CreateTextureFromSurface(g_renderer, single_text_surface);
    SDL_FreeSurface(single_text_surface);
    if (!g_single_text_texture) {
        std::cerr << "SDL_CreateTextureFromSurface failed: " << SDL_GetError() << "\n";
        TTF_CloseFont(g_font);
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return -1;
    }

    // 提取并缓存所有字符的解耦数据
    g_horizontal_cropped_surfaces.reserve(g_chars_to_draw.size());
    g_glyph32_surfaces.reserve(g_chars_to_draw.size());
    g_utf8_char_surfaces.reserve(g_chars_to_draw.size());

    std::cout << std::left
              << std::setw(10) << "char"
              << std::right
              << std::setw(12) << "ttf_min_x"
              << std::setw(12) << "ttf_max_x"
              << std::setw(12) << "ttf_min_y"
              << std::setw(12) << "ttf_max_y"
              << std::setw(14) << "ttf_advance"
              << std::setw(12) << "tex_orig_w"
              << std::setw(12) << "tex_orig_h"
              << std::setw(8) << "tex_w"
              << std::setw(8) << "tex_h"
              << std::setw(14) << "padding_left"
              << std::setw(15) << "padding_right"
              << std::setw(19) << "baseline_from_top"
              << std::setw(24) << "ttf_glyph_is_provided32"
              << '\n';
    for (auto sv : g_chars_to_draw) {
        g_glyphs.push_back(GetSingleGlyphMinBoundingBox(g_font, sv, g_text_color));
        const auto& glyph = g_glyphs.back();
        g_single_text_baseline_y = std::max(g_single_text_baseline_y, glyph.baseline_y);

        g_horizontal_cropped_surfaces.push_back(RenderHorizontallyCroppedGlyph(g_font, sv, g_text_color));
        g_glyph32_surfaces.push_back(RenderSingleGlyph(g_font, sv, g_text_color, false));
        g_utf8_char_surfaces.push_back(RenderSingleGlyph(g_font, sv, g_text_color, true));

        std::cout << std::left
                  << std::setw(10) << DisplayChar(sv)
                  << std::right
                  << std::setw(12) << glyph.metrics_minx
                  << std::setw(12) << glyph.metrics_maxx
                  << std::setw(12) << glyph.metrics_miny
                  << std::setw(12) << glyph.metrics_maxy
                  << std::setw(14) << glyph.metrics_advance
                  << std::setw(12) << glyph.tex_orig_w
                  << std::setw(12) << glyph.tex_orig_h
                  << std::setw(8) << (glyph.min_surface ? glyph.min_surface->w : 0)
                  << std::setw(8) << (glyph.min_surface ? glyph.min_surface->h : 0)
                  << std::setw(14) << glyph.left_padding
                  << std::setw(15) << glyph.right_padding
                  << std::setw(19) << glyph.baseline_y
                  << std::setw(24) << glyph.glyph_is_provided32
                  << '\n';
    }

    bool quit = false;
    SDL_Event e;

    while (!quit) {
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) quit = true;
        }

        SDL_SetRenderDrawColor(g_renderer, 20, 30, 45, 255);
        SDL_RenderClear(g_renderer);

        const int row_x = 100;
        const int row_gap = std::max(TTF_FontLineSkip(g_font), g_options.font_size) + 40;
        const int first_row_baseline_y = 80;
        const int second_row_baseline_y = first_row_baseline_y + row_gap;
        const int third_row_baseline_y = second_row_baseline_y + row_gap;
        const int fourth_row_baseline_y = third_row_baseline_y + row_gap;
        const int fifth_row_baseline_y = fourth_row_baseline_y + row_gap;

        if (g_options.draw_baseline) {
            int output_w = 0;
            int output_h = 0;
            SDL_GetRendererOutputSize(g_renderer, &output_w, &output_h);
            SDL_SetRenderDrawColor(g_renderer, 255, 60, 60, 255);
            SDL_RenderDrawLine(g_renderer, 0, first_row_baseline_y, output_w - 1, first_row_baseline_y);
            SDL_RenderDrawLine(g_renderer, 0, second_row_baseline_y, output_w - 1, second_row_baseline_y);
            SDL_RenderDrawLine(g_renderer, 0, third_row_baseline_y, output_w - 1, third_row_baseline_y);
            SDL_RenderDrawLine(g_renderer, 0, fourth_row_baseline_y, output_w - 1, fourth_row_baseline_y);
            SDL_RenderDrawLine(g_renderer, 0, fifth_row_baseline_y, output_w - 1, fifth_row_baseline_y);
        }

        DrawCroppedGlyphRow(row_x, first_row_baseline_y);
        DrawHorizontallyCroppedGlyphRow(row_x, second_row_baseline_y);
        DrawSingleTextTextureRow(row_x, third_row_baseline_y);
        DrawGlyph32BlendedRow(row_x, fourth_row_baseline_y);
        DrawUtf8CharBlendedRow(row_x, fifth_row_baseline_y);

        SDL_RenderPresent(g_renderer);
    }

    // 清理资源
    for (auto& glyph : g_glyphs) {
        if (glyph.min_surface) SDL_FreeSurface(glyph.min_surface);
    }
    for (auto& glyph : g_horizontal_cropped_surfaces) {
        if (glyph.surface) SDL_FreeSurface(glyph.surface);
    }
    for (auto& glyph : g_glyph32_surfaces) {
        if (glyph.surface) SDL_FreeSurface(glyph.surface);
    }
    for (auto& glyph : g_utf8_char_surfaces) {
        if (glyph.surface) SDL_FreeSurface(glyph.surface);
    }
    if (g_single_text_texture) SDL_DestroyTexture(g_single_text_texture);
    TTF_CloseFont(g_font);
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();

    return 0;
}
