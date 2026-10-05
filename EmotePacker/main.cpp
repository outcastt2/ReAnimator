// ReSkateEmotePacker: builds the chat emote pack ReSkate.dll bakes in (assets/emotes) from a
// folder of pictures and GIFs. Every file becomes one emote named after the file; animated
// GIFs keep their frames and timing. Output: emotes.json + emotes.png in Better Chat's format
// (Extension/UI/Overlay/chat_emotes.h), within the limits ReSkate's loader accepts.
//
//   ReSkateEmotePacker <folder> [output folder] [--size <px>] [--max-frames <n>]
//                      [--max-width <px>] [--name <pack name>]
//
// Dropping a folder on the exe writes the pack next to it, in "<folder>_pack".
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/path_text.h"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
using dingosdk::Json;
using dingosdk::path_utf8;

namespace {
// ReSkate's loader limits (chat_emotes.cpp): frames at most 256 x 256, names of letters,
// digits and _ up to 32 characters, 4096 frames in all, a texture at most 8192 x 8192.
constexpr int max_side = 256, max_texture = 8192;
constexpr std::size_t max_name = 32, max_total_frames = 4096;

struct Options {
    fs::path input, output;
    int size = 56;           // emote height in pixels (wide emotes may be shorter)
    int max_width = 2048;    // atlas width
    std::size_t max_frames = 64; // per animated emote; longer GIFs are thinned evenly
    std::string name = "ReSkate";
};
struct Image {
    int width{}, height{};
    std::vector<unsigned char> rgba; // straight alpha
};
struct Frame {
    Image image;
    unsigned time{}; // ms
    int x{}, y{};    // in the atlas
};
struct Emote {
    std::string name, file;
    std::vector<Frame> frames;
};

void check(HRESULT result, const char *what) {
    if (FAILED(result)) {
        char text[160];
        std::snprintf(text, sizeof text, "%s failed (0x%08lx)", what, static_cast<unsigned long>(result));
        throw std::runtime_error(text);
    }
}
// Kept for the life of the process: releasing it after COM has shut down would crash on exit.
IWICImagingFactory &factory() {
    static IWICImagingFactory *value = [] {
        IWICImagingFactory *f{};
        check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)), "Starting Windows Imaging");
        return f;
    }();
    return *value;
}
Image to_rgba(IWICBitmapSource *source) {
    ComPtr<IWICFormatConverter> converter;
    check(factory().CreateFormatConverter(&converter), "Creating a format converter");
    check(converter->Initialize(source, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom), "Converting to RGBA");
    UINT w{}, h{};
    check(converter->GetSize(&w, &h), "Reading the size");
    Image image{static_cast<int>(w), static_cast<int>(h)};
    image.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    check(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(image.rgba.size()), image.rgba.data()), "Reading pixels");
    return image;
}
std::optional<unsigned> metadata_number(IWICMetadataQueryReader *reader, const wchar_t *path) {
    PROPVARIANT value;
    PropVariantInit(&value);
    std::optional<unsigned> result;
    if (reader && SUCCEEDED(reader->GetMetadataByName(path, &value))) {
        if (value.vt == VT_UI1) result = value.bVal;
        else if (value.vt == VT_UI2) result = value.uiVal;
        else if (value.vt == VT_UI4) result = value.ulVal;
    }
    PropVariantClear(&value);
    return result;
}

// Every frame of a picture as it shows: a GIF's frames are composited onto its canvas with
// their offsets, transparency and disposal, as a browser plays them. Other formats: frame 1.
std::vector<Frame> decode(const fs::path &file) {
    ComPtr<IWICBitmapDecoder> decoder;
    check(factory().CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder),
          "Opening the picture");
    GUID container{};
    decoder->GetContainerFormat(&container);
    UINT count{};
    check(decoder->GetFrameCount(&count), "Counting frames");
    std::vector<Frame> frames;
    if (container != GUID_ContainerFormatGif || count <= 1) {
        ComPtr<IWICBitmapFrameDecode> frame;
        check(decoder->GetFrame(0, &frame), "Reading the picture");
        frames.push_back({to_rgba(frame.Get()), 0});
        return frames;
    }
    ComPtr<IWICMetadataQueryReader> global;
    decoder->GetMetadataQueryReader(&global);
    const int width = static_cast<int>(metadata_number(global.Get(), L"/logscrdesc/Width").value_or(0));
    const int height = static_cast<int>(metadata_number(global.Get(), L"/logscrdesc/Height").value_or(0));
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) throw std::runtime_error("the GIF has no usable canvas size");
    std::vector<unsigned char> canvas(static_cast<std::size_t>(width) * height * 4, 0), saved;
    struct Rect { int x{}, y{}, w{}, h{}; } previous;
    unsigned previous_disposal{};
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        check(decoder->GetFrame(i, &frame), "Reading a GIF frame");
        ComPtr<IWICMetadataQueryReader> reader;
        frame->GetMetadataQueryReader(&reader);
        const auto piece = to_rgba(frame.Get());
        const Rect rect{static_cast<int>(metadata_number(reader.Get(), L"/imgdesc/Left").value_or(0)),
                        static_cast<int>(metadata_number(reader.Get(), L"/imgdesc/Top").value_or(0)), piece.width, piece.height};
        const auto delay = metadata_number(reader.Get(), L"/grctlext/Delay").value_or(10);
        const auto disposal = metadata_number(reader.Get(), L"/grctlext/Disposal").value_or(0);
        // The previous frame leaves the canvas as its disposal says.
        if (i > 0 && previous_disposal == 2)
            for (int y = std::max(previous.y, 0); y < std::min(previous.y + previous.h, height); ++y)
                for (int x = std::max(previous.x, 0); x < std::min(previous.x + previous.w, width); ++x)
                    std::fill_n(canvas.begin() + (static_cast<std::size_t>(y) * width + x) * 4, 4, static_cast<unsigned char>(0));
        else if (i > 0 && previous_disposal == 3 && !saved.empty())
            canvas = saved;
        if (disposal == 3) saved = canvas;
        for (int y = 0; y < piece.height; ++y)
            for (int x = 0; x < piece.width; ++x) {
                const int cx = rect.x + x, cy = rect.y + y;
                if (cx < 0 || cy < 0 || cx >= width || cy >= height) continue;
                const auto *src = piece.rgba.data() + (static_cast<std::size_t>(y) * piece.width + x) * 4;
                if (src[3] == 0) continue; // transparent pixels leave what is underneath
                std::copy_n(src, 4, canvas.begin() + (static_cast<std::size_t>(cy) * width + cx) * 4);
            }
        // Browsers show delays under 20 ms as 100 ms.
        frames.push_back({{width, height, canvas}, delay < 2 ? 100U : delay * 10U});
        previous = rect;
        previous_disposal = disposal;
    }
    return frames;
}

// Scaled to `height` (wide pictures to at most 256 wide), in premultiplied alpha so edges do
// not darken.
Image scale(const Image &image, int height) {
    int h = height, w = std::max(1, static_cast<int>(std::lround(static_cast<double>(image.width) * height / image.height)));
    if (w > max_side) {
        h = std::max(1, static_cast<int>(std::lround(static_cast<double>(h) * max_side / w)));
        w = max_side;
    }
    if (w == image.width && h == image.height) return image;
    ComPtr<IWICBitmap> bitmap;
    check(factory().CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppRGBA, image.width * 4,
                                           static_cast<UINT>(image.rgba.size()), const_cast<BYTE *>(image.rgba.data()), &bitmap),
          "Loading pixels");
    ComPtr<IWICFormatConverter> premultiplied;
    check(factory().CreateFormatConverter(&premultiplied), "Creating a format converter");
    check(premultiplied->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                    WICBitmapPaletteTypeCustom), "Premultiplying");
    ComPtr<IWICBitmapScaler> scaler;
    check(factory().CreateBitmapScaler(&scaler), "Creating a scaler");
    check(scaler->Initialize(premultiplied.Get(), w, h,
                             w < image.width ? WICBitmapInterpolationModeFant : WICBitmapInterpolationModeHighQualityCubic),
          "Scaling");
    return to_rgba(scaler.Get());
}

// Consecutive identical frames become one, showing for their combined time; more than
// `limit` frames are thinned evenly, each kept frame showing for the ones it replaces.
std::vector<Frame> tidy(std::vector<Frame> frames, std::size_t limit) {
    std::vector<Frame> merged;
    for (auto &frame : frames) {
        if (!merged.empty() && merged.back().image.rgba == frame.image.rgba) merged.back().time += frame.time;
        else merged.push_back(std::move(frame));
    }
    if (merged.size() <= limit) return merged;
    std::vector<Frame> thinned;
    for (std::size_t i = 0; i < limit; ++i) {
        const auto from = i * merged.size() / limit, to = (i + 1) * merged.size() / limit;
        Frame kept = std::move(merged[from]);
        for (auto j = from + 1; j < to; ++j) kept.time += merged[j].time;
        thinned.push_back(std::move(kept));
    }
    return thinned;
}

// Built from the wide name: path::string() throws on a character outside the ANSI code page,
// and one such file would stop the whole pack.
std::string emote_name(const fs::path &file) {
    std::string name;
    for (const wchar_t c : file.stem().wstring())
        name += c < 128 && std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(c) : '_';
    if (name.size() > max_name) name.resize(max_name);
    return name.empty() ? std::string("emote") : name;
}
std::string lowered(std::string text) {
    for (auto &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// Rows of frames across the atlas, tallest rows first.
int pack(std::vector<Emote> &emotes, int width) {
    std::vector<Frame *> frames;
    for (auto &e : emotes)
        for (auto &f : e.frames) frames.push_back(&f);
    std::stable_sort(frames.begin(), frames.end(), [](const Frame *a, const Frame *b) { return a->image.height > b->image.height; });
    constexpr int gap = 1;
    int x = 0, y = 0, row = 0;
    for (auto *f : frames) {
        if (x > 0 && x + f->image.width > width) {
            x = 0;
            y += row + gap;
            row = 0;
        }
        f->x = x;
        f->y = y;
        x += f->image.width + gap;
        row = std::max(row, f->image.height);
    }
    return y + row;
}

void write_png(const fs::path &file, int width, int height, const std::vector<unsigned char> &rgba) {
    // The PNG encoder takes BGRA everywhere.
    std::vector<unsigned char> bgra(rgba);
    for (std::size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    ComPtr<IWICStream> stream;
    check(factory().CreateStream(&stream), "Creating the output file");
    check(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE), "Opening the output file");
    ComPtr<IWICBitmapEncoder> encoder;
    check(factory().CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Creating the PNG encoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Starting the PNG");
    ComPtr<IWICBitmapFrameEncode> frame;
    check(encoder->CreateNewFrame(&frame, nullptr), "Adding the PNG image");
    check(frame->Initialize(nullptr), "Starting the PNG image");
    check(frame->SetSize(width, height), "Sizing the PNG");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format), "Choosing the PNG format");
    if (format != GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("the PNG encoder does not take 32-bit BGRA");
    check(frame->WritePixels(height, width * 4, static_cast<UINT>(bgra.size()), bgra.data()), "Writing the PNG pixels");
    check(frame->Commit(), "Finishing the PNG image");
    check(encoder->Commit(), "Finishing the PNG");
}

Options parse(int argc, wchar_t **argv) {
    Options o;
    std::vector<fs::path> positional;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const auto value = [&]() -> std::wstring {
            if (i + 1 >= argc) throw std::runtime_error("missing a value after an option");
            return argv[++i];
        };
        if (arg == L"--size") o.size = std::stoi(value());
        else if (arg == L"--max-frames") o.max_frames = static_cast<std::size_t>(std::stoul(value()));
        else if (arg == L"--max-width") o.max_width = std::stoi(value());
        else if (arg == L"--name") o.name = path_utf8(value());
        else if (arg == L"--help" || arg == L"-h" || arg == L"/?") throw std::invalid_argument("");
        else positional.emplace_back(arg);
    }
    if (positional.empty() || positional.size() > 2) throw std::invalid_argument("");
    o.input = fs::absolute(positional[0]);
    o.output = positional.size() == 2 ? fs::absolute(positional[1])
                                      : o.input.parent_path() / (o.input.filename().wstring() + L"_pack");
    if (o.size < 8 || o.size > max_side) throw std::runtime_error("--size must be 8 to 256");
    if (o.max_width < max_side || o.max_width > max_texture) throw std::runtime_error("--max-width must be 256 to 8192");
    if (o.max_frames < 1 || o.max_frames > 512) throw std::runtime_error("--max-frames must be 1 to 512");
    return o;
}

int run(const Options &o) {
    if (!fs::is_directory(o.input)) throw std::runtime_error("not a folder: " + path_utf8(o.input));
    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(o.input)) {
        const auto extension = lowered(path_utf8(entry.path().extension()));
        if (entry.is_regular_file() && (extension == ".png" || extension == ".gif" || extension == ".jpg" ||
                                        extension == ".jpeg" || extension == ".bmp" || extension == ".webp"))
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) throw std::runtime_error("no .png, .gif, .jpg, .bmp or .webp files in " + path_utf8(o.input));

    std::vector<Emote> emotes;
    std::set<std::string> names, folded;
    std::size_t total{}, failed{};
    for (const auto &file : files) {
        try {
            auto name = emote_name(file);
            if (std::none_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c); }))
                throw std::runtime_error("no letters or digits to name it by; rename the file");
            for (int n = 2; names.contains(name); ++n) {
                const auto suffix = "_" + std::to_string(n);
                name = emote_name(file).substr(0, max_name - suffix.size()) + suffix;
            }
            auto frames = decode(file);
            const bool animated = frames.size() > 1;
            for (auto &f : frames) f.image = scale(f.image, o.size);
            frames = tidy(std::move(frames), o.max_frames);
            if (!animated || frames.size() == 1) frames.front().time = 0;
            if (total + frames.size() > max_total_frames)
                throw std::runtime_error("the pack would pass ReSkate's " + std::to_string(max_total_frames) + " frames");
            total += frames.size();
            if (!folded.insert(lowered(name)).second)
                std::printf("  note: %s differs from another emote only in case; typing picks the first.\n", name.c_str());
            names.insert(name);
            std::printf("  :%s:  %dx%d%s\n", name.c_str(), frames.front().image.width, frames.front().image.height,
                        frames.size() > 1 ? (", " + std::to_string(frames.size()) + " frames").c_str() : "");
            emotes.push_back({name, path_utf8(file.filename()), std::move(frames)});
        } catch (const std::exception &e) {
            ++failed;
            std::printf("  skipped %s: %s\n", path_utf8(file.filename()).c_str(), e.what());
        }
    }
    if (emotes.empty()) throw std::runtime_error("no pictures could be read");

    const int height = pack(emotes, o.max_width);
    if (height > max_texture)
        throw std::runtime_error("the atlas would be " + std::to_string(height) +
                                 " pixels tall (ReSkate takes 8192): use a smaller --size or --max-frames");
    std::vector<unsigned char> atlas(static_cast<std::size_t>(o.max_width) * height * 4, 0);
    auto emote_list = Json::object();
    for (const auto &e : emotes) {
        for (const auto &f : e.frames)
            for (int y = 0; y < f.image.height; ++y)
                std::copy_n(f.image.rgba.data() + static_cast<std::size_t>(y) * f.image.width * 4, f.image.width * 4,
                            atlas.begin() + (static_cast<std::size_t>(f.y + y) * o.max_width + f.x) * 4);
        auto entry = Json::object();
        entry["size"] = Json::array({e.frames.front().image.width, e.frames.front().image.height});
        if (e.frames.size() == 1) {
            entry["pos"] = Json::array({e.frames.front().x, e.frames.front().y});
        } else {
            auto list = Json::array();
            for (const auto &f : e.frames) {
                auto frame = Json::object();
                frame["time"] = f.time;
                frame["pos"] = Json::array({f.x, f.y});
                list.push_back(std::move(frame));
            }
            entry["frames"] = std::move(list);
        }
        emote_list[e.name] = std::move(entry);
    }
    fs::create_directories(o.output);
    write_png(o.output / "emotes.png", o.max_width, height, atlas);
    auto document = Json::object();
    document["name"] = o.name;
    document["texture"] = "emotes.png";
    document["emotes"] = std::move(emote_list);
    {
        const auto text = document.dump(2);
        FILE *out{};
        if (_wfopen_s(&out, (o.output / "emotes.json").c_str(), L"wb") != 0 || !out)
            throw std::runtime_error("cannot write emotes.json");
        std::fwrite(text.data(), 1, text.size(), out);
        std::fclose(out);
    }
    std::printf("\n%zu emotes (%zu frames) in a %dx%d atlas%s.\nWrote %s\\emotes.json and emotes.png.\n"
                "Copy both into the repository's assets\\emotes and rebuild ReSkate.dll.\n",
                emotes.size(), total, o.max_width, height, failed ? (", " + std::to_string(failed) + " skipped").c_str() : "",
                path_utf8(o.output).c_str());
    return failed ? 2 : 0;
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // progress shows as it happens, even through a pipe
    SetConsoleOutputCP(CP_UTF8);               // file names are printed as UTF-8
    const bool own_console = [] {
        DWORD processes[2];
        return GetConsoleProcessList(processes, 2) == 1; // started by double-click or drag-and-drop
    }();
    int code = 1;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::printf("Windows Imaging could not start.\n");
    } else {
        try {
            code = run(parse(argc, argv));
        } catch (const std::invalid_argument &) {
            std::printf("ReSkateEmotePacker: builds ReSkate's chat emote pack from a folder of pictures and GIFs.\n\n"
                        "  ReSkateEmotePacker <folder> [output folder] [--size 56] [--max-frames 64]\n"
                        "                     [--max-width 2048] [--name ReSkate]\n\n"
                        "Each file becomes :FileName: (letters, digits and _). Animated GIFs keep their frames.\n"
                        "Writes emotes.json and emotes.png (default: <folder>_pack next to the folder).\n");
        } catch (const std::exception &e) {
            std::printf("Error: %s\n", e.what());
        }
        CoUninitialize();
    }
    if (own_console) {
        std::printf("\nPress Enter to close.");
        std::getchar();
    }
    return code;
}
