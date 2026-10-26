// From Amp/TurboChook, MIT licence, air/OS contributors.
// Render an HVIF preview with the same native renderer used by Tracker.
// Haiku: g++ -std=c++17 tools/render-icon.cpp -o build-haiku/render-icon -lbe -ltranslation
// Usage: render-icon input.hvif output.png size [light|dark]
#include <Application.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <File.h>
#include <IconUtils.h>
#include <TranslatorFormats.h>
#include <TranslatorRoster.h>
#include <View.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 4 || argc > 5 || (argc == 5 && std::strcmp(argv[4], "light")
        && std::strcmp(argv[4], "dark"))) {
        std::fprintf(stderr, "Usage: %s input.hvif output.png size [light|dark]\n", argv[0]);
        return 1;
    }
    char* end = nullptr;
    const long size = std::strtol(argv[3], &end, 10);
    if (*end != '\0' || size < 16 || size > 1024) {
        std::fprintf(stderr, "Size must be between 16 and 1024 pixels.\n");
        return 1;
    }
    BApplication app("application/x-vnd.airOS-airTime-icon-renderer");
    BFile input(argv[1], B_READ_ONLY);
    off_t length = 0;
    if (input.InitCheck() != B_OK || input.GetSize(&length) != B_OK
        || length < 4 || length > 1024 * 1024) {
        std::fprintf(stderr, "Cannot read HVIF input.\n");
        return 1;
    }
    std::vector<uint8> bytes(static_cast<size_t>(length));
    if (input.Read(bytes.data(), bytes.size()) != length)
        return 1;

    BBitmap bitmap(BRect(0, 0, size - 1, size - 1), 0, B_RGBA32);
    status_t result = BIconUtils::GetVectorIcon(bytes.data(), bytes.size(), &bitmap);
    BBitmap background(bitmap.Bounds(), B_BITMAP_ACCEPTS_VIEWS, B_RGB32);
    if (result == B_OK && argc == 5) {
        result = background.InitCheck();
        if (result == B_OK && background.Lock()) {
            BView view(background.Bounds(), "preview", B_FOLLOW_NONE, 0);
            background.AddChild(&view);
            view.SetHighColor(std::strcmp(argv[4], "dark") == 0
                ? rgb_color{39, 52, 72, 255} : rgb_color{248, 249, 251, 255});
            view.FillRect(view.Bounds());
            view.SetDrawingMode(B_OP_ALPHA);
            view.DrawBitmap(&bitmap);
            view.Sync();
            background.RemoveChild(&view);
            background.Unlock();
        } else if (result == B_OK) {
            result = B_ERROR;
        }
    }
    if (result == B_OK) {
        BFile output(argv[2], B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
        result = output.InitCheck();
        if (result == B_OK) {
            BBitmapStream stream(argc == 5 ? &background : &bitmap);
            result = BTranslatorRoster::Default()->Translate(
                &stream, nullptr, nullptr, &output, B_PNG_FORMAT);
            BBitmap* detached = nullptr;
            stream.DetachBitmap(&detached);
        }
    }
    if (result != B_OK) {
        std::fprintf(stderr, "Render failed: %s\n", std::strerror(result));
        return 1;
    }
    std::printf("%s: %ld x %ld, native HVIF render\n", argv[2], size, size);
    return 0;
}
