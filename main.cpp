#include <SDL.h>
#include <SDL_ttf.h>
#include <algorithm>
#include <charconv>
#include <iostream>
#include <string_view>
#include <system_error>
#include <vector>

// 1. 返回的数据结构定义
struct SingleGlyphData {
    SDL_Surface* min_surface; // 包含字符最小包围盒的 Surface
    int left_padding;         // 预留的左边空白像素数量
    int right_padding;        // 预留的右边空白像素数量
    int baseline_y;           // 基线相对于返回的 Surface 最上一行的像素数量
};

struct ProgramOptions {
    const char* font_file = nullptr;
    int font_size = 0;
    std::string_view utf8_text;
    bool show_box = false;
    bool show_baseline = false;
    bool show_single_texture = false;
};

void PrintUsage(std::ostream& os, const char* program) {
    os << "Usage: " << program
       << " --font-file <font-file>"
       << " --font-size <font-size-in-TTF_Open>"
       << " --utf8-text <utf8-string-to-render>"
       << " [--show-box]"
       << " [--show-baseline]"
       << " [--show-single-texture]\n";
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
        if (arg == "--show-box") {
            options.show_box = true;
            continue;
        }
        if (arg == "--show-baseline") {
            options.show_baseline = true;
            continue;
        }
        if (arg == "--show-single-texture") {
            options.show_single_texture = true;
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

// 获取单字排版数据与最小 Surface
SingleGlyphData GetSingleGlyphMinBoundingBox(TTF_Font* font, std::string_view utf8_char, SDL_Color color) {
    SingleGlyphData data = { nullptr, 0, 0, 0 };
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
    data.baseline_y = maxy;

    const int glyph_w = maxx - minx;
    const int glyph_h = maxy - miny;
    if (glyph_w <= 0 || glyph_h <= 0) {
        return data;
    }

    SDL_Surface* rendered = TTF_RenderGlyph32_Blended(font, ch, color);
    if (!rendered) {
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

int main(int argc, char* argv[]) {
    ProgramOptions options;
    bool help_requested = false;
    if (!ParseOptions(argc, argv, options, help_requested)) {
        PrintUsage(std::cerr, argv[0]);
        return 1;
    }
    if (help_requested) {
        PrintUsage(std::cout, argv[0]);
        return 0;
    }

    std::vector<std::string_view> chars_to_draw;
    if (!SplitUTF8String(options.utf8_text, chars_to_draw)) {
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
    SDL_Window* window = SDL_CreateWindow("SDL2_ttf Metrics Demo", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 400, SDL_WINDOW_SHOWN);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    TTF_Font* font = TTF_OpenFont(options.font_file, options.font_size);
    if (!font) {
        std::cerr << "字体加载失败: " << options.font_file << ". Error: " << TTF_GetError() << "\n";
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return -1;
    }

    SDL_Color textColor = { 255, 255, 255, 255 }; // 白色
    SDL_Texture* single_text_texture = nullptr;
    int single_text_w = 0;
    int single_text_h = 0;

    if (options.show_single_texture) {
        SDL_Surface* single_text_surface = TTF_RenderUTF8_Blended(font, options.utf8_text.data(), textColor);
        if (!single_text_surface) {
            std::cerr << "TTF_RenderUTF8_Blended failed: " << TTF_GetError() << "\n";
            TTF_CloseFont(font);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            TTF_Quit();
            SDL_Quit();
            return -1;
        }

        single_text_w = single_text_surface->w;
        single_text_h = single_text_surface->h;
        single_text_texture = SDL_CreateTextureFromSurface(renderer, single_text_surface);
        SDL_FreeSurface(single_text_surface);
        if (!single_text_texture) {
            std::cerr << "SDL_CreateTextureFromSurface failed: " << SDL_GetError() << "\n";
            TTF_CloseFont(font);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            TTF_Quit();
            SDL_Quit();
            return -1;
        }
    }

    // 提取并缓存所有字符的解耦数据
    std::vector<SingleGlyphData> glyphs;
    std::cout << "char\ttexture_width_px\ttexture_height_px\tleft_padding_px\tright_padding_px\tbaseline_from_top_px\n";
    for (auto sv : chars_to_draw) {
        glyphs.push_back(GetSingleGlyphMinBoundingBox(font, sv, textColor));
        std::cout.write(sv.data(), static_cast<std::streamsize>(sv.size()));
        const auto& glyph = glyphs.back();
        std::cout << '\t' << (glyph.min_surface ? glyph.min_surface->w : 0)
                  << '\t' << (glyph.min_surface ? glyph.min_surface->h : 0)
                  << '\t' << glyph.left_padding
                  << '\t' << glyph.right_padding
                  << '\t' << glyph.baseline_y << '\n';
    }

    bool quit = false;
    SDL_Event e;

    while (!quit) {
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) quit = true;
        }

        // 清屏（深蓝色背景）
        SDL_SetRenderDrawColor(renderer, 20, 30, 45, 255);
        SDL_RenderClear(renderer);

        // --- 核心绘制逻辑开始 ---
        const int row_x = 100; // 虚拟光标起始 X
        const int first_row_baseline_y = options.show_single_texture ? 120 : 200;
        const int second_row_baseline_y = first_row_baseline_y + std::max(TTF_FontLineSkip(font), options.font_size) + 40;

        if (options.show_baseline) {
            // 辅助线：绘制一条红色的水平基线，用以肉眼验证文字是否完美对齐
            int output_w = 0;
            int output_h = 0;
            SDL_GetRendererOutputSize(renderer, &output_w, &output_h);
            SDL_SetRenderDrawColor(renderer, 255, 60, 60, 255);
            SDL_RenderDrawLine(renderer, 0, first_row_baseline_y, output_w - 1, first_row_baseline_y);
            if (options.show_single_texture) {
                SDL_RenderDrawLine(renderer, 0, second_row_baseline_y, output_w - 1, second_row_baseline_y);
            }
        }

        int cursor_x = row_x;
        for (const auto& glyph : glyphs) {
            if (glyph.min_surface) {
                SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, glyph.min_surface);

                SDL_Rect dest;
                // 1. 考虑左留白，定位 X 轴
                dest.x = cursor_x + glyph.left_padding;
                // 2. 利用相对基线坐标，完美对齐 Y 轴
                dest.y = first_row_baseline_y - glyph.baseline_y;
                dest.w = glyph.min_surface->w;
                dest.h = glyph.min_surface->h;

                if (options.show_box) {
                    // 辅助线：绘制每个字符实际 Surface 的绿色外包围盒边框，观察其紧凑程度
                    SDL_SetRenderDrawColor(renderer, 60, 255, 60, 100);
                    SDL_RenderDrawRect(renderer, &dest);
                }

                // 绘制字符最小包围盒
                SDL_RenderCopy(renderer, tex, nullptr, &dest);

                // 3. 绘制完毕后，光标推进当前字符的排版全宽，也就是 advance
                cursor_x += (glyph.left_padding + glyph.min_surface->w + glyph.right_padding);

                SDL_DestroyTexture(tex);
            } else {
                // 如果是空格等无像素内容，依然需要向前推进 advance 距离
                // 这里本例均是可见字符，故略过
            }
        }

        if (single_text_texture) {
            SDL_Rect dest {
                row_x,
                second_row_baseline_y - TTF_FontAscent(font),
                single_text_w,
                single_text_h
            };

            if (options.show_box) {
                SDL_SetRenderDrawColor(renderer, 60, 255, 60, 100);
                SDL_RenderDrawRect(renderer, &dest);
            }
            SDL_RenderCopy(renderer, single_text_texture, nullptr, &dest);
        }
        // --- 核心绘制逻辑结束 ---

        SDL_RenderPresent(renderer);
    }

    // 清理资源
    for (auto& glyph : glyphs) {
        if (glyph.min_surface) SDL_FreeSurface(glyph.min_surface);
    }
    if (single_text_texture) SDL_DestroyTexture(single_text_texture);
    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();

    return 0;
}
