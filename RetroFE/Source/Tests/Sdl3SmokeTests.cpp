#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef RETROFE_HAVE_D3D12
#include "../Video/D3D12VideoInterop.h"
#include <d3d12.h>
#include <wrl/client.h>
#endif
#ifdef RETROFE_HAVE_FFMPEG
#include "../Video/FFmpegDmaBuf.h"
#endif
#include "../SDL.h"
#include "../Database/Configuration.h"
#include "../Control/UserInput.h"
#include "../Sound/AudioBus.h"
#include "../Sound/Sound.h"
#include "../Graphics/Font.h"
#include "../Graphics/TextEngineAtlas.h"
#include "../Graphics/GeometryBatch.h"
#include "../Graphics/Component/Text.h"
#include "../Graphics/FontCache.h"
#include "../Video/GStreamerVideo.h"
#include "../Video/GlibLoop.h"
#include "../Video/VideoPool.h"
#include "../Video/VideoFactory.h"
#include "../Graphics/Component/VideoComponent.h"
#include "../Graphics/Component/Image.h"
#include "../Graphics/Page.h"
#include "../Graphics/PageBuilder.h"
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <cmath>
#include <filesystem>

std::vector<std::string> settingsFromCLI;

namespace {
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << ": " << SDL_GetError() << '\n';
        SDL_Log("REQUIRE FAILED: %s: %s", message, SDL_GetError());
        FILE* f = fopen("test_failure.txt", "w");
        if (f) {
            fprintf(f, "%s: %s\n", message, SDL_GetError());
            fclose(f);
        }
        std::exit(EXIT_FAILURE);
    }
}

SDL_Color pixel(SDL_Renderer* renderer, int x, int y) {
    SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
    require(surface != nullptr, "Read rendered pixels");
    SDL_Color result{};
    require(SDL_ReadSurfacePixel(surface, x, y, &result.r, &result.g, &result.b, &result.a), "Read pixel");
    SDL_DestroySurface(surface);
    return result;
}

void batchChecks(SDL_Renderer* renderer, SDL_Texture* texture) {
    auto* alternate = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 4, 4);
    require(alternate != nullptr, "Create alternate atlas");
    Uint32 pixels[16];
    std::fill_n(pixels, 16, 0xffffffffu);
    require(SDL_UpdateTexture(alternate, nullptr, pixels, 16), "Upload alternate atlas");
    require(SDL_SetTextureBlendMode(alternate, SDL_BLENDMODE_BLEND), "Set alternate blend mode");

    auto capture = [&]() {
        auto* image = SDL_RenderReadPixels(renderer, nullptr);
        require(image != nullptr, "Capture geometry result");
        auto* rgba = SDL_ConvertSurface(image, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(image);
        require(rgba != nullptr, "Normalize geometry capture");
        return rgba;
    };
    for (bool offscreen : {false, true}) {
        SDL_Surface* reference = nullptr;
        for (bool batched : {false, true}) {
            require(SDL_RenderClear(renderer), "Clear batch comparison");
            GeometryBatch batch;
            // Exceed capacity, alternate A/B/A, overlap translucent quads, and
            // change tint while a run is pending. Mirror/output rotation are
            // also exercised by the caller's later renderChecks invocation.
            for (int i = 0; i < 320; ++i) {
                auto* atlas = (i == 280 || i == 285) ? alternate : texture;
                require(SDL_SetTextureColorMod(atlas, Uint8(30 + i % 220), 80, 140), "Set run tint");
                SDL_Rect src{0, 0, 4, 4};
                SDL_FRect dst{float(4 + i % 25), float(4 + i % 21), 12, 10};
                if (offscreen) {
                    require(SDL_SetTextureAlphaMod(atlas, 110), "Set offscreen alpha");
                    if (batched) require(batch.appendTexture(renderer, atlas, src, dst), "Append atlas quad");
                    else {
                        SDL_FRect source{0, 0, 4, 4};
                        require(SDL_RenderTexture(renderer, atlas, &source, &dst), "Draw immediate atlas quad");
                    }
                } else {
                    ViewInfo view;
                    view.Angle = (i % 3 == 0) ? 27.0f : 0.0f;
                    view.ContainerX = view.ContainerY = 8;
                    view.ContainerWidth = view.ContainerHeight = 40;
                    view.hasReflection = i % 4 == 0;
                    view.reflectionMask = 15;
                    view.ReflectionScale = 0.6f;
                    view.ReflectionAlpha = 0.4f;
                    if (batched) require(SDL::appendCopyF(batch, atlas, 0.4f, &src, &dst, view, 64, 64), "Append transformed quad");
                    else require(SDL::renderCopyF(atlas, 0.4f, &src, &dst, view, 64, 64), "Draw immediate transformed quad");
                }
            }
            require(batch.flush(), "Flush geometry batch");
            auto* actual = capture();
            if (!batched) reference = actual;
            else {
                require(reference->w == actual->w && reference->h == actual->h, "Batch dimensions match");
                for (int y = 0; y < actual->h; ++y) {
                    require(std::memcmp(static_cast<Uint8*>(reference->pixels) + y * reference->pitch,
                        static_cast<Uint8*>(actual->pixels) + y * actual->pitch, actual->w * 4) == 0,
                        "Ordered batching preserves every pixel");
                }
                SDL_DestroySurface(reference);
                SDL_DestroySurface(actual);
            }
        }
    }
    SDL_DestroyTexture(alternate);
}

void renderChecks(Configuration& config, bool checkPixels) {
    require(SDL::initialize(config), "Initialize RetroFE SDL3 backend");
    auto* renderer = SDL::getRenderer(0);
    require(renderer != nullptr, "Create renderer");
    require(SDL_SetRenderTarget(renderer, SDL::getRenderTarget(0)), "Set target");
    require(SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) && SDL_RenderClear(renderer), "Clear target");

    SDL_Surface* surface = SDL_CreateSurface(4, 4, SDL_PIXELFORMAT_RGBA32);
    require(surface != nullptr, "Create image surface");
    require(SDL_FillSurfaceRect(surface, nullptr, SDL_MapSurfaceRGBA(surface, 255, 255, 255, 255)), "Fill image");
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    require(texture != nullptr, "Create texture");
    require(SDL_SetTextureColorMod(texture, 200, 100, 50), "Set texture tint");
    require(SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND), "Set alpha blend");
    // Borderless output can be larger than the requested 64x64 layout.
    auto layoutPixel = [&](int x, int y) {
        auto* target = SDL_GetRenderTarget(renderer);
        return pixel(renderer, x * target->w / 64, y * target->h / 64);
    };
    ViewInfo view;
    SDL_FRect dest{8, 8, 16, 16};
    require(SDL::renderCopyF(texture, 0.5f, nullptr, &dest, view, 64, 64), "Draw tinted alpha geometry");
    if (checkPixels) {
        const auto color = layoutPixel(16, 16);
        require(color.r >= 95 && color.r <= 105 && color.g >= 45 && color.g <= 55 && color.b >= 20 && color.b <= 30,
            "SDL3 float vertex colors preserve tint and alpha");
        require(layoutPixel(2, 2).r == 0, "Geometry stays inside destination");
    }
    require(SDL_RenderClear(renderer), "Clear for reflection");
    view.hasReflection = true;
    view.reflectionMask = 2;
    view.ReflectionScale = 1.0f;
    view.ReflectionAlpha = 0.5f;
    require(SDL::renderCopyF(texture, 1.0f, nullptr, &dest, view, 64, 64), "Draw reflection");
    if (checkPixels) {
        require(layoutPixel(16, 16).r >= 195, "Primary image remains opaque");
        const auto reflected = layoutPixel(16, 32);
        require(reflected.r >= 95 && reflected.r <= 105, "Reflection has independent alpha");
    }
    require(SDL_RenderClear(renderer), "Clear for clipping");
    view.hasReflection = false;
    view.ContainerX = 16;
    view.ContainerY = 8;
    view.ContainerWidth = 8;
    view.ContainerHeight = 16;
    require(SDL::renderCopyF(texture, 1.0f, nullptr, &dest, view, 64, 64), "Draw clipped geometry");
    if (checkPixels) {
        require(layoutPixel(12, 16).r == 0, "Container clips left side");
        require(layoutPixel(20, 16).r >= 195, "Container preserves visible side");
    }
    require(SDL_RenderClear(renderer), "Clear for glyph gradient");
    require(SDL_SetTextureColorMod(texture, 255, 255, 255), "Use white glyph atlas");
    ViewInfo gradientView;
    gradientView.ContainerX = 8;
    gradientView.ContainerY = 16;
    gradientView.ContainerWidth = 40;
    gradientView.ContainerHeight = 8;
    GeometryBatch gradientBatch;
    const SDL_Rect glyphSrc{0, 0, 4, 4};
    const SDL_FColor top{1, 1, 1, 1};
    const SDL_FColor bottom{0, 0, 0, 1};
    const SDL_FRect firstGlyph{8, 8, 16, 16};
    const SDL_FRect secondGlyph{28, 8, 16, 16};
    require(SDL::appendCopyFGradient(gradientBatch, texture, 1.0f, &glyphSrc,
        &firstGlyph, gradientView, 64, 64, top, bottom), "Append clipped glyph gradient");
    require(SDL::appendCopyFGradient(gradientBatch, texture, 1.0f, &glyphSrc,
        &secondGlyph, gradientView, 64, 64, top, bottom), "Restart gradient for next glyph");
    require(gradientBatch.flush(), "Draw glyph gradients");
    if (checkPixels) {
        const auto upper = layoutPixel(16, 17).r;
        const auto lower = layoutPixel(16, 22).r;
        require(upper > lower && upper < 200 && layoutPixel(16, 12).r == 0,
            "Clipping retains each glyph's original gradient position");
        const auto nextUpper = layoutPixel(36, 17).r;
        require(std::abs(int(upper) - int(nextUpper)) <= 8,
            "Gradient restarts at each glyph");
    }
    // Verify coalesced renderer flush contract
    require(SDL::beginVideoFrame(renderer), "beginVideoFrame resets flush state");
    require(SDL::flushVideoRenderer(renderer), "First video flush succeeds");
    require(SDL::flushVideoRenderer(renderer), "Subsequent video flush in same update phase coalesces");
    require(SDL::beginVideoFrame(renderer), "beginVideoFrame resets flush token for next frame");
    require(SDL::flushVideoRenderer(renderer), "Subsequent frame video flush succeeds");

    batchChecks(renderer, texture);
    SDL_DestroyTexture(texture);
    require(SDL_SetRenderTarget(renderer, nullptr), "Restore backbuffer");
    require(SDL_RenderTexture(renderer, SDL::getRenderTarget(0), nullptr, nullptr), "Present target");
    require(SDL_RenderPresent(renderer), "Present frame");
}

void inputChecks(Configuration& config) {
    for (const auto* key : {"up", "down", "left", "right", "select", "back", "quit"})
        config.setProperty(std::string("controls.") + key, std::string("joy0button0"));
    UserInput input(config);
    require(input.initialize(), "Initialize input bindings");
    SDL_VirtualJoystickDesc desc{};
    SDL_INIT_INTERFACE(&desc);
    desc.nbuttons = 1;
    desc.name = "RetroFE SDL3 test joystick";
    const auto id = SDL_AttachVirtualJoystick(&desc);
    require(id != 0, "Attach virtual joystick");
    auto* joystick = SDL_OpenJoystick(id);
    require(joystick != nullptr, "Open virtual joystick");
    SDL_Event event;
    // Assign this virtual device first; physical controllers on the test host
    // must not change the configured slot under test.
    event = {};
    event.type = SDL_EVENT_JOYSTICK_ADDED;
    event.jdevice.which = id;
    input.update(event);
    while (SDL_PollEvent(&event)) {}
    require(SDL_SetJoystickVirtualButton(joystick, 0, true), "Press virtual button");
    SDL_UpdateJoysticks();
    while (SDL_PollEvent(&event)) {
        const auto original = event;
        if (event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN && event.jbutton.which == id)
            input.update(event);
        if (event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN)
            require(event.jbutton.which == original.jbutton.which, "Input mapping preserves event instance ID");
    }
    input.updateKeystate();
    require(input.keystate(UserInput::KeyCodeSelect), "Configured joy0 maps to SDL3 instance ID");
    SDL_CloseJoystick(joystick);
    require(SDL_DetachVirtualJoystick(id), "Detach virtual joystick");
    while (SDL_PollEvent(&event)) input.update(event);
    input.updateKeystate();
    require(!input.keystate(UserInput::KeyCodeSelect), "Disconnect clears held input");
}

void mediaChecks(const std::string& assets) {
    require(AudioBus::instance().mixer() != nullptr, "Open SDL3 audio mixer");
    Sound sound(assets + "/layouts/Arcades/sounds/select.wav", "");
    require(sound.allocate(), "Decode packaged WAV sound");
    sound.play();
    require(sound.isPlaying(), "Play sound through SDL3 track");
    sound.play();
    require(sound.isPlaying(), "Play overlapping sound voices");
    sound.free();
    require(!sound.isPlaying(), "Release sound tracks");
    require(TTF_Init(), "Initialize SDL3_ttf");
    {
        FontManager font(assets + "/retrofe/OpenSans.ttf", 24, {255,255,255,255}, true, 1, 0);
        require(font.initialize(), "Open gradient and outline font faces");
        require(font.getWidth("RetroFE") > 0, "Measure text using SDL_ttf shaping");
        const std::string accented = "caf\xc3\xa9";
        const int coldWidth = font.getWidth(accented);
        auto* mip = const_cast<FontManager::MipLevel*>(font.getMipLevelForSize(24));
        require(mip->outlineFont && TTF_GetFontOutline(mip->font) == 0 &&
            TTF_GetFontOutline(mip->outlineFont) == mip->outlinePx,
            "Outline uses a separate font face");
        const Uint32 fillGeneration = TTF_GetFontGeneration(mip->font);
        auto* shaped = font.getTextEngineAtlas(mip);
        require(shaped != nullptr, "Create shaped text atlas");
        require(shaped->prewarmAscii() && shaped->prewarmAscii(),
            "Prewarm and reuse printable ASCII glyphs without drawing");
        float shapedWidth = 0.0f;
        require(shaped->measure("", 1.0f, shapedWidth) && shapedWidth == 0.0f,
            "Empty outlined text has zero width");
        require(shaped->measure("office caf\xc3\xa9", 1.0f, shapedWidth) && shapedWidth > 0,
            "Shape Unicode text with the SDL3_ttf text engine");
        std::vector<std::string> wrappedLines;
        require(shaped->wrapLines("one two three four five six", 1.0f, 90.0f, wrappedLines) &&
            wrappedLines.size() > 1, "SDL3_ttf wraps scrolling text into rows");
        std::string joinedWords;
        for (const auto& line : wrappedLines)
            for (char ch : line) if (ch != ' ') joinedWords += ch;
        require(joinedWords == "onetwothreefourfivesix", "Wrapped rows retain all text");
        wrappedLines.clear();
        const std::string japanese = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe6\x96\x87\xe7\xab\xa0\xe3\x82\x92\xe6\x8a\x98\xe3\x82\x8a\xe8\xbf\x94\xe3\x81\x99";
        require(shaped->wrapLines(japanese, 1.0f, 50.0f, wrappedLines) &&
            wrappedLines.size() > 1, "SDL3_ttf wraps CJK text without ASCII spaces");
        std::string joinedJapanese;
        for (const auto& line : wrappedLines) joinedJapanese += line;
        require(joinedJapanese == japanese, "CJK wrapping retains every UTF-8 byte");
        require(shaped->draw("office caf\xc3\xa9", 1, 1, 1.0f, {255, 255, 255, 255}),
            "Draw shaped gradient and outlined glyphs through paged atlas");
        require(shaped->draw("office caf\xc3\xa9", 1, 1, 1.0f, {120, 190, 255, 255}),
            "Reuse shaped glyphs and atlas pages when color changes");
        TTF_Font* underlinedFont = TTF_CopyFont(mip->font);
        require(underlinedFont != nullptr, "Copy font for underline drawing");
        TTF_SetFontStyle(underlinedFont, TTF_STYLE_UNDERLINE);
        TTF_Text* underlinedText = TTF_CreateText(nullptr, underlinedFont, "test", 4);
        require(underlinedText && TTF_UpdateText(underlinedText), "Shape underlined text");
        SDL_Rect underline{};
        for (int i = 0; i < underlinedText->internal->num_ops; ++i) {
            if (underlinedText->internal->ops[i].cmd == TTF_DRAW_COMMAND_FILL)
                underline = underlinedText->internal->ops[i].fill.rect;
        }
        require(underline.w > 0 && underline.h > 0, "Underline produces a fill operation");
        SDL_Renderer* renderer = SDL::getRenderer(0);
        require(renderer && SDL_SetRenderTarget(renderer, SDL::getRenderTarget(0)),
            "Select text test render target");
        require(SDL_RenderClear(renderer), "Clear for underline drawing");
        {
            TextEngineAtlas underlined(renderer, underlinedFont, nullptr, 0,
                {0, 0, 0, 0}, false);
            require(underlined.draw("test", 10, 10, 1.0f, {255, 0, 0, 255}),
                "Draw SDL3_ttf underline fill operation");
        }
        const auto underlinePixel = pixel(renderer,
            10 + underline.x + underline.w / 2,
            10 + underline.y + underline.h / 2);
        require(underlinePixel.r > 100, "Underline fill operation reaches renderer");
        TTF_DestroyText(underlinedText);
        TTF_CloseFont(underlinedFont);
        require(TTF_GetFontGeneration(mip->font) == fillGeneration,
            "Shaped outline glyphs do not invalidate the fill font");
        require(font.prepareSize(36), "Prepare a second shaped font size");
        const auto* shapedMip = font.getMipLevelForSize(36);
        require(shapedMip && shapedMip->fontSize == 36 &&
            font.getTextEngineAtlas(shapedMip)->draw("caf\xc3\xa9", 1, 1, 1.0f,
                {180, 220, 255, 255}),
            "The text engine uses an atlas for each prepared size");
        require(TTF_GetFontGeneration(mip->font) == fillGeneration,
            "Drawing outlined glyphs does not invalidate fill text");
        require(coldWidth == font.getWidth(accented), "Width is independent of atlas population");
        const auto generation = font.getResourceGeneration();
        font.deInitialize();
        require(font.getResourceGeneration() != generation, "Font teardown invalidates cached resources");
        require(font.getWidth(accented) == 0, "Deinitialized font has no stale metrics");
        require(font.initialize(), "Rebuild font resources");
        require(font.getWidth(accented) == coldWidth, "Font reload retains measurement");
        require(font.getWidth(std::string("A\0B", 3)) == font.getWidth("A"), "Embedded NUL terminates measurement");
        require(font.getWidth("A\xc3") > 0, "Truncated UTF-8 is handled safely");

        Configuration pageConfig;
        Page page(pageConfig, 64, 64);
        Text label(accented, page, &font, 0);
        label.baseViewInfo.FontSize = 24;
        label.draw();
        require(font.getMipLevelForSize(24)->textEngineAtlas != nullptr,
            "Text renders accented Latin outside the ASCII preload");
        require(font.initialize(), "Reload font used by existing text");
        label.draw();
        require(font.getMipLevelForSize(24)->textEngineAtlas != nullptr,
            "Text rebuilds glyph cache after resource reload");
        FontManager larger(assets + "/retrofe/OpenSans.ttf", 48, {255,255,255,255}, true, 1, 0);
        require(larger.initialize(), "Prepare second font at layout load time");
        label.baseViewInfo.font = &larger;
        label.baseViewInfo.FontSize = 48; // The scale remains 1.0 across this switch.
        label.draw();
        require(larger.getMipLevelForSize(48)->textEngineAtlas != nullptr,
            "Text rebuilds glyph cache when font changes at the same scale");
        label.baseViewInfo.font = &font;
        {
            FontManager mipped(assets + "/retrofe/OpenSans.ttf", 96, {255,255,255,255}, true, 4, 0);
            require(mipped.initialize(), "Prebuild reference size font");
            require(mipped.getMipLevelForSize(96) != nullptr, "Reference size prepared");
            require(mipped.getMipLevelForSize(24)->fontSize == 96, "Unprepared query returns reference ceiling");
            for (int size : {12, 18, 24, 36, 48, 72}) {
                require(mipped.prepareSize(float(size)), "Prepare font size");
            }
            for (int size : {12, 18, 24, 36, 48, 72, 96}) {
                const auto* level = mipped.getMipLevelForSize(float(size));
                require(level && level->fontSize == size && level->font && level->outlineFont,
                    "Each mip owns its fill and outline font faces");
                require(level->outlinePx == std::max(1, int(std::lround(4.0 * size / 96))),
                    "Outline thickness follows raster size");
            }
            require(mipped.getMipLevelForSize(24.1f)->fontSize == 36, "Fractional requests select the ceiling mip");
            require(mipped.getMipLevelForSize(1)->fontSize == 12, "Small sizes use the minimum prepared mip");
            require(mipped.getMipLevelForSize(120)->fontSize == 96, "Selection does not open unprepared fonts");
            require(mipped.prepareSize(26) && mipped.prepareSize(34), "Prepare sizes found in active layouts");
            require(mipped.getMipLevelForSize(26)->fontSize == 26 && mipped.getMipLevelForSize(34)->fontSize == 34,
                "Exact layout sizes override ladder choices");
            label.baseViewInfo.font = &mipped;
            label.baseViewInfo.FontSize = 26;
            label.draw();
            label.baseViewInfo.FontSize = 34;
            label.draw();
            require(mipped.getMipLevelForSize(26)->textEngineAtlas != nullptr &&
                mipped.getMipLevelForSize(34)->textEngineAtlas != nullptr,
                "Text changes mip at constant scale and uses each mip's Unicode atlas");
            label.baseViewInfo.font = &font;
            require(mipped.prepareSize(120), "Prepare an explicit size above loadFontSize");
            require(mipped.getMaxFontSize() == 96, "Reference size remains stable");
            require(mipped.prepareHeight(65), "Prepare hiscore line height");
            const auto* level = mipped.getMipLevelForHeight(65);
            require(level && level->height >= 65, "Height selector avoids upscaling");
            float shapedAA = 0.0f;
            require(mipped.getTextEngineAtlas(level)->measure("AA", 1.0f, shapedAA) &&
                mipped.getWidth("AA", *level) == static_cast<int>(std::lround(shapedAA)),
                "Measurement uses selected font and outline");
            const int heightSize = level->fontSize;
            mipped.deInitialize();
            require(mipped.initialize(), "Rebuild all prepared sizes after renderer reload");
            require(mipped.getMipLevelForSize(26)->fontSize == 26 && mipped.getMipLevelForSize(120)->fontSize == 120 &&
                mipped.getMipLevelForHeight(65)->fontSize == heightSize, "Exact sizes survive resource reload");
        }
        {
            FontCache cache;
            require(cache.initialize(), "Initialize layout font cache");
            const auto path = assets + "/retrofe/OpenSans.ttf";
            require(cache.loadFont(path, 48, {255,255,255,255}, false, 0, 0) &&
                cache.loadFont(path, 24, {255,255,255,255}, false, 0, 0), "Load larger layout before smaller layout");
            require(cache.getFont(path, 24, false, 0, 0)->getMaxFontSize() == 24,
                "Layout default size is independent of previously loaded larger fonts");

            Configuration pbConfig;
            PageBuilder builder("Arcades", "layout", pbConfig, &cache);
            const std::string fontAttr = std::filesystem::absolute(path).generic_string();

            // Verify hiscore content-dependent shrinking step heights
            rapidxml::xml_document<> hiscoreDoc;
            std::string hiscoreStr = "<reloadableHiscores font=\"" + fontAttr +
                "\" loadFontSize=\"96\" fontSize=\"60\"/>";
            std::vector<char> hiscoreBuf(hiscoreStr.begin(), hiscoreStr.end());
            hiscoreBuf.push_back('\0');
            hiscoreDoc.parse<0>(hiscoreBuf.data());
            auto* hiscoreNode = hiscoreDoc.first_node("reloadableHiscores");
            require(hiscoreNode != nullptr, "Parse hiscore test XML");
            auto* hiscoreFont = builder.addFont(hiscoreNode, nullptr, 0);
            require(hiscoreFont != nullptr, "Prepare hiscore font through PageBuilder");
            require(hiscoreFont->getMipLevelForHeight(60) != nullptr &&
                hiscoreFont->getMipLevelForHeight(60)->height >= 60, "Exact hiscore line height prepared");
            // Check that 0.75 (45), 0.50 (30), and 0.35 (21) step heights were prepared:
            require(hiscoreFont->getMipLevelForHeight(45) != nullptr &&
                hiscoreFont->getMipLevelForHeight(30) != nullptr &&
                hiscoreFont->getMipLevelForHeight(21) != nullptr,
                "Hiscore shrinking step heights pre-prepared");

            // Verify fontSize tween preparation
            rapidxml::xml_document<> tweenDoc;
            std::string tweenStr = "<reloadableText font=\"" + fontAttr +
                "\" loadFontSize=\"96\" fontSize=\"28\">"
                "  <onEnter>"
                "    <set duration=\"0.5\">"
                "      <animate type=\"fontSize\" from=\"28\" to=\"56\"/>"
                "    </set>"
                "  </onEnter>"
                "</reloadableText>";
            std::vector<char> tweenBuf(tweenStr.begin(), tweenStr.end());
            tweenBuf.push_back('\0');
            tweenDoc.parse<0>(tweenBuf.data());
            auto* tweenNode = tweenDoc.first_node("reloadableText");
            require(tweenNode != nullptr, "Parse tween test XML");
            auto* tweenFont = builder.addFont(tweenNode, nullptr, 0);
            require(tweenFont != nullptr, "Prepare tween font through PageBuilder");
            require(tweenFont->getMipLevelForSize(28)->fontSize == 28, "Tween start fontSize prepared");
            require(tweenFont->getMipLevelForSize(56)->fontSize == 56, "Tween target fontSize prepared");

            // Verify fallback font loading and symbol rendering
            const auto fallbackPath = assets + "/retrofe/Symbola.ttf";
            if (std::filesystem::exists(fallbackPath)) {
                FontManager fallbackMgr(path, 24, {255, 255, 255, 255}, false, 1, 0, fallbackPath);
                require(fallbackMgr.initialize(), "Initialize FontManager with fallback font");
                require(fallbackMgr.getFallbackFontPath() == fallbackPath, "Fallback font path preserved");

                auto* fbMip = const_cast<FontManager::MipLevel*>(fallbackMgr.getMipLevelForSize(24));
                require(fbMip && fbMip->fallbackFont, "Fallback font is attached to the mip");
                TTF_Font* plainPrimary = TTF_OpenFont(path.c_str(), 24);
                require(plainPrimary != nullptr, "Open primary font without fallback for coverage check");
                Uint32 fallbackOnlySymbol = 0;
                for (Uint32 candidate = 0x2190; candidate <= 0x2BFF; ++candidate) {
                    if (!TTF_FontHasGlyph(plainPrimary, candidate) &&
                        TTF_FontHasGlyph(fbMip->fallbackFont, candidate)) {
                        fallbackOnlySymbol = candidate;
                        break;
                    }
                }
                TTF_CloseFont(plainPrimary);
                require(fallbackOnlySymbol != 0, "Symbola provides a glyph missing from OpenSans");
                char symbolUtf8[5]{};
                SDL_UCS4ToUTF8(fallbackOnlySymbol, symbolUtf8);
                const std::string textWithoutSymbol = "AB";
                const std::string textWithSymbol = std::string("A") + symbolUtf8 + "B";
                const int w1 = fallbackMgr.getWidth(textWithoutSymbol);
                const int w2 = fallbackMgr.getWidth(textWithSymbol);
                require(w2 > w1, "Width of text with symbol reflects fallback glyph advance");

                TTF_Text* fallbackProbe = TTF_CreateText(nullptr, fbMip->font,
                    textWithSymbol.c_str(), textWithSymbol.size());
                require(fallbackProbe && TTF_UpdateText(fallbackProbe),
                    "Shape fallback-only symbol through SDL_ttf");
                bool usedFallback = false;
                for (int i = 0; i < fallbackProbe->internal->num_ops; ++i) {
                    const auto& op = fallbackProbe->internal->ops[i];
                    if (op.cmd == TTF_DRAW_COMMAND_COPY &&
                        op.copy.glyph_font == fbMip->fallbackFont) usedFallback = true;
                }
                TTF_DestroyText(fallbackProbe);
                require(usedFallback, "SDL_ttf selects the fallback face for missing symbols");

                if (auto* fbEngine = fallbackMgr.getTextEngineAtlas(fbMip)) {
                    float fbShapedWidth = 0.0f;
                    require(fbEngine->measure(textWithSymbol, 1.0f, fbShapedWidth), "Measure text with symbol via TextEngineAtlas");
                    require(fbShapedWidth > 0.0f, "Shaped symbol has positive width");
                    require(fbEngine->draw(textWithSymbol, 0, 0, 1.0f, {255, 255, 255, 255}), "Draw text with symbol via TextEngineAtlas");
                }

                // Verify fallback font loading through PageBuilder XML
                const std::string fbAttr = std::filesystem::absolute(fallbackPath).generic_string();
                rapidxml::xml_document<> fbDoc;
                std::string fbStr = "<reloadableHiscores font=\"" + fontAttr +
                    "\" fallbackFont=\"" + fbAttr +
                    "\" loadFontSize=\"96\" fontSize=\"32\"/>";
                std::vector<char> fbBuf(fbStr.begin(), fbStr.end());
                fbBuf.push_back('\0');
                fbDoc.parse<0>(fbBuf.data());
                auto* fbNode = fbDoc.first_node("reloadableHiscores");
                require(fbNode != nullptr, "Parse hiscore with fallback font XML");
                auto* fbFont = builder.addFont(fbNode, nullptr, 0);
                require(fbFont != nullptr, "Prepare hiscore font with fallback through PageBuilder");
                require(fbFont->getFallbackFontPath() == fbAttr, "PageBuilder applied fallback font from XML");

                // Verify cache isolation between font with fallback and font without fallback
                auto* noFbFont = cache.getFont(fontAttr, 96, false, 0, 0, "");
                auto* withFbFont = cache.getFont(fontAttr, 96, false, 0, 0, fbAttr);
                require(noFbFont != nullptr, "Font without fallback exists in cache");
                require(withFbFont != nullptr, "Font with fallback exists in cache");
                require(noFbFont != withFbFont, "FontCache isolates font instances by fallback font key");

                rapidxml::xml_document<> noFallbackDoc;
                std::string noFallbackStr = "<text font=\"" + fontAttr +
                    "\" fallbackFont=\"none\" loadFontSize=\"96\" fontSize=\"24\"/>";
                std::vector<char> noFallbackBuf(noFallbackStr.begin(), noFallbackStr.end());
                noFallbackBuf.push_back('\0');
                noFallbackDoc.parse<0>(noFallbackBuf.data());
                auto* disabledFont = builder.addFont(noFallbackDoc.first_node("text"), nullptr, 0);
                require(disabledFont && disabledFont->getFallbackFontPath().empty(),
                    "Component can disable automatic symbol fallback");
            }
        }
    }
    TTF_Quit();
    gst_init(nullptr, nullptr);
    for (const char* name : {"playbin", "appsink", "appsrc", "audioconvert",
            "audioresample", "videoconvert", "perspective", "goom",
            "wavescope", "synaescope", "spectrascope"}) {
        auto* factory = gst_element_factory_find(name);
        require(factory != nullptr, name);
        gst_object_unref(factory);
    }
    GlibLoop::instance().start();
    {
        auto video = VideoFactory::createVideo(0, 0, false, -1, nullptr);
        require(video->open(assets + "/layouts/Arcades/video/splash.mp4"), "Open packaged video");
        const Uint64 deadline = SDL_GetTicks() + 10000;
        while (!video->getTexture() && !video->hasError() && SDL_GetTicks() < deadline) {
            video->updateFrame();
            require(SDL::beginVideoFrame(SDL::getRenderer(0)), "Submit preroll transfers");
            SDL_Delay(10);
        }
        require(video->getTexture() != nullptr, "Decode video into an SDL3 texture");
        if (Configuration::HardwareVideoAccel) require(video->usingGpuTexture(), "Hardware test must use native GPU texture interop");
        std::cout << "Video path: " << (video->usingGpuTexture() ? "native GPU texture" : "CPU upload") << '\n';
        video->resume();
        const Uint64 playbackDeadline = SDL_GetTicks() + 2000;
        unsigned presented = 0;
        while (SDL_GetTicks() < playbackDeadline) {
            video->updateFrame();
            auto* renderer = SDL::getRenderer(0);

            require(SDL::beginVideoFrame(renderer), "Submit native video transfers");
            require(SDL_SetRenderTarget(renderer, nullptr), "Set video backbuffer");
            const auto dim = video->getDimensions();
            SDL_FRect source{0, 0, float(dim.w), float(dim.h)};
            require(SDL_RenderTexture(renderer, video->getTexture(), &source, nullptr), "Render decoded video texture");
            SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
            SDL_FRect overlay{4, 4, 8, 8};
            require(SDL_RenderFillRect(renderer, &overlay), "Draw overlay above video");
            if (presented == 0) require(pixel(renderer, 6, 6).r == 255, "Video preserves overlay rendering");
            require(SDL_RenderPresent(renderer), "Present decoded video");
            ++presented;
            SDL_Delay(10);
        }
        require(presented > 10, "Repeated video frames render successfully");
        if (Configuration::HardwareVideoAccel) require(video->gpuFrameCount() > 10, "Decode and copy multiple distinct GPU frames");
        video->pause();
        video->resume();
        require(video->unload(), "Unload video for instance reuse");
        const Uint64 unloadDeadline = SDL_GetTicks() + 8000;
        while (!video->isReadyForReuse() && SDL_GetTicks() < unloadDeadline) SDL_Delay(10);
        require(video->isReadyForReuse(), "Video instance drains for reuse");
        require(video->open(assets + "/layouts/Arcades/video/splash.mp4"), "Reopen video on retained instance");
        const Uint64 reopenDeadline = SDL_GetTicks() + 10000;
        while (!video->getTexture() && !video->hasError() && SDL_GetTicks() < reopenDeadline) {
            video->updateFrame();
            require(SDL::beginVideoFrame(SDL::getRenderer(0)), "Submit preroll transfers");
            SDL_Delay(10);
        }
        require(video->getTexture() != nullptr, "Decode after instance reuse");
        if (Configuration::HardwareVideoAccel) require(video->usingGpuTexture(), "GPU interop survives instance reuse");
        video->stop();
        if (dynamic_cast<GStreamerVideo*>(video.get())) {
            require(!video->isReadyForReuse(), "NULL teardown remains pending until main-thread completion");
            GStreamerVideo::waitForControlTasks();
            require(video->isReadyForReuse(), "NULL teardown completes on control worker");
        }
    }
    // Start more instances than the per-monitor transition budget allows.
    // Alternate playback intent so queued starts and active playback coexist.
    if (VideoFactory::backend() == "gstreamer") {
        std::vector<std::shared_ptr<IVideo>> videos;
        const std::string file = assets + "/layouts/Arcades/video/splash.mp4";
        for (int i = 0; i < 6; ++i) {
            auto video = VideoFactory::createVideo(0, 0, false, -1, nullptr);
            require(video && video->open(file), "Open mixed-state concurrent video");
            videos.push_back(std::move(video));
        }
        const Uint64 deadline = SDL_GetTicks() + 10000;
        while (SDL_GetTicks() < deadline) {
            bool allReady = true;
            for (size_t i = 0; i < videos.size(); ++i) {
                auto& video = videos[i];
                require(!video->hasError(), "Mixed-state decoder remains healthy");
                video->updateFrame();
                if (i % 2 == 0 && video->isPipelineReady()) video->resume();
                allReady &= video->getTexture() != nullptr;
            }
            require(SDL::beginVideoFrame(SDL::getRenderer(0)), "Submit mixed-state video transfers");
            if (allReady) break;
            SDL_Delay(5);
        }
        for (size_t i = 0; i < videos.size(); ++i) {
            require(videos[i]->getTexture() != nullptr, "Every mixed-state instance prerolls");
            require(i % 2 == 0 ? videos[i]->isPlaying() : !videos[i]->isPlaying(),
                "Mixed-state playback intent is preserved");
            videos[i]->stop();
        }
        GStreamerVideo::waitForControlTasks();
    }
    GStreamerVideo::waitForControlTasks();
    GlibLoop::instance().stop();
}
}

#ifdef RETROFE_HAVE_FFMPEG
void dmaBufMetadataChecks() {
    auto make = [] { return std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); }); };
    auto source=make(), mapped=make();
    source->hw_frames_ctx=av_buffer_allocz(sizeof(AVHWFramesContext));
    require(source->hw_frames_ctx != nullptr, "Allocate DMA-BUF test context");
    auto* hw=reinterpret_cast<AVHWFramesContext*>(source->hw_frames_ctx->data);
    hw->width=1920; hw->height=1088; hw->sw_format=AV_PIX_FMT_NV12;
    source->width=1920; source->height=1080; source->colorspace=AVCOL_SPC_BT709;
    source->chroma_location=AVCHROMA_LOC_LEFT;
    AVDRMFrameDescriptor descriptor{};
    mapped->data[0]=reinterpret_cast<uint8_t*>(&descriptor);
    descriptor.nb_objects=2; descriptor.nb_layers=2;
    descriptor.objects[0]={10,4194304,0}; descriptor.objects[1]={11,2097152,0};
    descriptor.layers[0].format=0x20203852; descriptor.layers[1].format=0x38385247;
    descriptor.layers[0].nb_planes=descriptor.layers[1].nb_planes=1;
    descriptor.layers[0].planes[0]={0,128,2048}; descriptor.layers[1].planes[0]={1,256,2048};
    auto imported=ffmpegDmaBufFrame(source,mapped);
    require(imported.planeCount==2 && imported.planes[1].fd==11 && imported.planes[1].offset==256 &&
        imported.height==1088 && imported.crop.h==1080 && imported.chromaX==0 && imported.chromaY==1,
        "DMA-BUF adapter preserves multi-FD planes, padding, crop and chroma");
    require(imported.owner.get()==mapped.get(), "DMA-BUF adapter retains mapped-frame ownership");
    descriptor.layers[1].planes[0].object_index=2;
    bool rejected=false;
    try { ffmpegDmaBufFrame(source,mapped); } catch (const std::exception&) { rejected=true; }
    require(rejected, "Invalid DMA-BUF object index is rejected");
    descriptor.layers[1].planes[0].object_index=1;
    hw->sw_format=AV_PIX_FMT_P010LE;
    descriptor.layers[0].format=0x20363152; descriptor.layers[1].format=0x32335247;
    require(ffmpegDmaBufFrame(source,mapped).fourcc==0x30313050, "P010 separate layers preserve storage format");
    source->color_trc=AVCOL_TRC_SMPTE2084;
    rejected=false;
    try { ffmpegDmaBufFrame(source,mapped); } catch (const std::exception&) { rejected=true; }
    require(rejected, "HDR cannot silently use SDR EGL conversion");
}
#endif

#ifdef RETROFE_HAVE_D3D12
void deferredNativeChecks() {
    auto* renderer=SDL::getRenderer(0);
    auto* device=static_cast<ID3D12Device*>(SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),
        SDL_PROP_RENDERER_D3D12_DEVICE_POINTER,nullptr));
    if(!device) return;
    D3D12VideoInterop interop(renderer);
    require(interop.available(), "Create deferred native test interop");
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width=64; desc.Height=64; desc.DepthOrArraySize=1; desc.MipLevels=1;
    desc.Format=DXGI_FORMAT_NV12; desc.SampleDesc.Count=1;
    require(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&resource))), "Create native test surface");
    require(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))), "Create delayed producer fence");
    auto owner=std::make_shared<int>(0);
    for(uint64_t value=1;value<=2;++value) {
        interop.invalidateFrame();
        require(!interop.currentTexture(), "Invalidation hides previous presentation");
        require(!interop.copyNative(resource.Get(),fence.Get(),value,0,1,64,64,
            SDL_COLORSPACE_BT709_LIMITED,owner) && interop.deferred(), "Pending first frame is not drawable");
        require(SDL::beginVideoFrame(renderer) && !interop.currentTexture(), "Unsignaled frame stays hidden after beginFrame");
        require(SUCCEEDED(fence->Signal(value)), "Release delayed producer");
        require(SDL::beginVideoFrame(renderer) && interop.currentTexture(), "Submission exposes frame without another decoder update");
        interop.discardFrames();
    }
}
#endif

void ffmpegContractChecks() {
    const char* first = std::getenv("RETROFE_TEST_MEDIA_A");
    const char* second = std::getenv("RETROFE_TEST_MEDIA_B");
    if (VideoFactory::backend() != "ffmpeg" || !first || !second) return;
    auto video = VideoFactory::createVideo(0, 2, false, -1, nullptr);
    auto tick = [&] {
        video->updateFrame();
        auto* renderer = SDL::getRenderer(0);
        require(SDL::beginVideoFrame(renderer), "Submit contract-test video");
        SDL_SetRenderTarget(renderer, nullptr);
        SDL_RenderClear(renderer);
        if (auto* texture = video->getTexture()) require(SDL_RenderTexture(renderer, texture, nullptr, nullptr), "Render contract-test video");
        require(SDL_RenderPresent(renderer), "Present contract-test video");
        SDL_Delay(5);
    };
    auto waitFrame = [&] {
        const auto deadline = SDL_GetTicks() + 5000;
        while (!video->getTexture() && !video->hasError() && SDL_GetTicks() < deadline) tick();
        require(video->getTexture() && !video->hasError(), "New media prerolls");
    };
    require(video->open(first), "Open contract fixture");
    waitFrame();
    auto* retained = video->getTexture();
    video->resume();
    for (int i = 0; i < 30; ++i) tick();
    video->pause();
    const auto pausedAt = video->getCurrent();
    SDL_Delay(50);
    require(video->getCurrent() == pausedAt, "Pause freezes presentation clock");
    video->skipForwardp();
    require(!video->getTexture(), "Seek invalidates old pixels");
    waitFrame();
    require(video->getCurrent() >= pausedAt, "Percentage seek advances position");
    video->rewindAndPause();
    waitFrame();
    require(video->isPaused() && video->getCurrent() == 0, "Rewind remains paused at zero");
    require(video->unload(), "Unload retains resources");
    const auto drain = SDL_GetTicks() + 5000;
    while (!video->isReadyForReuse() && SDL_GetTicks() < drain) SDL_Delay(2);
    require(video->isReadyForReuse(), "Unload completes");
    video->open(first);
    waitFrame();
    require(video->getTexture() == retained, "Same-format reopen reuses texture allocation");
    video->prepareForRetarget();
    video->open(second);
    require(!video->getTexture(), "Retarget suppresses stale video");
    waitFrame();
    auto dim = video->getDimensions();
    require(dim.w == 128 && dim.h == 72, "Retarget accepts different dimensions");
    auto* renderer = SDL::getRenderer(0);
    SDL_RenderTexture(renderer, video->getTexture(), nullptr, nullptr);
    const auto blue = pixel(renderer, 32, 32);
    require(blue.b > blue.r + 80 && blue.b > blue.g + 80, "Decoded blue fixture has correct pixel colors");
    SDL_RenderPresent(renderer);
    if (const char* compatible = std::getenv("RETROFE_TEST_MEDIA_C")) {
        for (int cycle = 0; cycle < 12; ++cycle) {
            video->prepareForRetarget();
            video->open(cycle % 2 ? second : compatible);
            require(!video->getTexture(), "Compatible retarget hides old pixels");
            waitFrame();
            SDL_RenderTexture(renderer, video->getTexture(), nullptr, nullptr);
            const auto color = pixel(renderer, 32, 32);
            require(cycle % 2 ? color.b > color.r + 80 : color.r > color.b + 80,
                "Reused decoder presents the new file's color");
            SDL_RenderPresent(renderer);
        }
    }
    video->resume();
    const auto loopDeadline = SDL_GetTicks() + 7000;
    while (!video->hasFinishedLoops() && !video->hasError() && SDL_GetTicks() < loopDeadline) {
        tick();
        require(video->getTexture() != nullptr, "Automatic loop never blanks the presentation");
    }
    require(video->hasFinishedLoops() && video->isPaused(), "Finite loop count finishes and pauses");
    const int corners[]{32, 18, 96, 18, 32, 54, 96, 54};
    video->setPerspectiveCorners(corners);
    video->open(second);
    waitFrame();
    SDL_SetRenderDrawColor(renderer, 255, 0, 255, 255);
    SDL_RenderClear(renderer);
    const SDL_FRect fixtureRect{0, 0, 64, 64};
    SDL_RenderTexture(renderer, video->getTexture(), nullptr, &fixtureRect);
    const auto outside = pixel(renderer, 1, 1);
    const auto inside = pixel(renderer, 32, 32);
    require(outside.r == 255 && outside.b == 255 && inside.b > inside.r + 80,
        "Perspective preserves transparent exterior and transformed video");
    SDL_RenderPresent(renderer);
    video->setPerspectiveCorners(nullptr);
    video->open(std::string(second) + ".missing");
    const auto errorDeadline = SDL_GetTicks() + 3000;
    while (!video->hasError() && SDL_GetTicks() < errorDeadline) SDL_Delay(5);
    require(video->hasError() && !video->getTexture(), "Missing media reports failure without stale pixels");
    video->open(first);
    waitFrame();
    video->stop();
}

void concurrentVideoChecks(const std::string& file) {
    GlibLoop::instance().start();
    std::vector<std::shared_ptr<IVideo>> videos;
    for (int i = 0; i < 7; ++i) videos.push_back(VideoFactory::createVideo(0, 0, false, -1, nullptr));
    auto* renderer = SDL::getRenderer(0);
    for (int cycle = 0; cycle < 3; ++cycle) {
        std::vector<uint64_t> before;
        for (auto& video : videos) {
            require(video->open(file), "Open concurrent video");
            before.push_back(video->gpuFrameCount());
            require(video->getTexture() == nullptr, "Reopen invalidates old presentation");
            video->resume();
        }
        const auto deadline = SDL_GetTicks() + 4000;
        while (SDL_GetTicks() < deadline) {
            SDL_PumpEvents();
            for (auto& video : videos) {
                require(!video->hasError(), "Concurrent decoder remains healthy");
                video->updateFrame();
                if (video->getTexture()) video->resume();
            }
            require(SDL::beginVideoFrame(renderer), "Submit concurrent transfers");
            SDL_SetRenderTarget(renderer, nullptr);
            SDL_RenderClear(renderer);
            for (size_t i = 0; i < videos.size(); ++i) if (auto* texture = videos[i]->getTexture()) {
                SDL_FRect rect{float(i % 4) * 16, float(i / 4) * 32, 16, 32};
                require(SDL_RenderTexture(renderer, texture, nullptr, &rect), "Draw concurrent video");
            }
            // Deliberately no readback or CPU fence wait in this loop.
            require(SDL_RenderPresent(renderer), "Present concurrent videos");
            SDL_Delay(2);
        }
        for (size_t i = 0; i < videos.size(); ++i) {
            require(videos[i]->usingGpuTexture(), "All concurrent videos retain GPU interop");
            require(videos[i]->gpuFrameCount() > before[i] + 10, "Every concurrent decoder advances");
            require(videos[i]->unload(), "Unload concurrent video");
        }
        const auto drainDeadline = SDL_GetTicks() + 10000;
        for (auto& video : videos) {
            while (!video->isReadyForReuse() && SDL_GetTicks() < drainDeadline) SDL_Delay(2);
            require(video->isReadyForReuse(), "Concurrent instance becomes reusable");
        }
    }
    for (auto& video : videos) video->stop();
    videos.clear();
    GStreamerVideo::waitForControlTasks();
    GlibLoop::instance().stop();
}

void scrollingVideoStartupChecks(Configuration& config, const std::string& file) {
    Page page(config, 64, 64);
    GlibLoop::instance().start();
    {
        VideoComponent foreground(page, file, 0, 0, false, 9002);
        foreground.baseViewInfo.Width = foreground.baseViewInfo.Height = 32;
        foreground.baseViewInfo.X = foreground.baseViewInfo.Y = 0;
        foreground.baseViewInfo.Alpha = 1;
        VideoComponent video(page, file, 0, 0, false, 9001);
        video.baseViewInfo.Width = video.baseViewInfo.Height = 32;
        video.baseViewInfo.X = video.baseViewInfo.Y = 0;
        video.baseViewInfo.Alpha = 0;
        video.allocateGraphicsMemory();
        video.update(0);
        require(!video.isPaused() && !video.isPlaying(), "Transparent list video does not start preroll");
        video.baseViewInfo.Alpha = 1;
        video.baseViewInfo.X = 200;
        video.update(0);
        require(!video.isPaused() && !video.isPlaying(), "Offscreen list video does not start preroll");
        video.baseViewInfo.X = 0;
        const auto deadline = SDL_GetTicks() + 10000;
        while (!video.isPlaying() && SDL_GetTicks() < deadline) {
            video.update(0.001f);
            require(SDL::startVideoFrame(SDL::getRenderer(0)), "Start revealed startup frame");
            video.prepareVideoFrame();
            require(SDL::submitVideoFrame(SDL::getRenderer(0)), "Submit revealed startup frame");
            SDL_Delay(1);
        }
        require(video.isPlaying(), "List video starts when revealed");
        auto* renderer = SDL::getRenderer(0);
        require(SDL::startVideoFrame(renderer), "Start revealed video frame");
        video.prepareVideoFrame();
        require(SDL::submitVideoFrame(renderer), "Submit revealed video frame");
        SDL_SetRenderDrawColor(renderer, 255, 0, 255, 255);
        SDL_RenderClear(renderer);
        video.draw();
        const auto before = pixel(renderer, 16, 16);
        require(!(before.r == 255 && before.g == 0 && before.b == 255), "Video covers loading placeholder");
        video.preserveInstanceOnNextRecycle();
        require(video.recycleAsVideo(file, ""), "Same-file recycle retains playback");
        video.update(0.001f);
        require(video.isPlaying(), "Same-file recycle does not unload the active video");
        require(SDL::startVideoFrame(renderer), "Start retained video frame");
        video.prepareVideoFrame();
        require(SDL::submitVideoFrame(renderer), "Submit retained video frame");
        SDL_RenderClear(renderer);
        video.draw();
        const auto retained = pixel(renderer, 16, 16);
        require(!(retained.r == 255 && retained.g == 0 && retained.b == 255),
            "Same-file recycle retains a visible frame");
        require(video.recycleAsVideo(file + ".next", ""), "Recycle video for another game");
        SDL_RenderClear(renderer);
        video.draw();
        const auto after = pixel(renderer, 16, 16);
        require(after.r == 255 && after.g == 0 && after.b == 255,
            "Replacement never displays the previous game's frame");
        require(video.recycleAsVideo(file + ".next-again", ""), "Rapid recycle before a new frame");
        SDL_RenderClear(renderer);
        video.draw();
        const auto again = pixel(renderer, 16, 16);
        require(after.r == again.r && after.g == again.g && after.b == again.b,
            "Repeated fast recycling never displays stale frames");
    }
    VideoPool::cleanup(0, 9001);
    VideoPool::cleanup(0, 9002);
    GStreamerVideo::waitForControlTasks();
    GlibLoop::instance().stop();
}

void imageAsyncIOChecks(Configuration& config) {
    Page page(config, 64, 64);
    const auto directory = std::filesystem::temp_directory_path() /
        ("retrofe-image-tests-" + std::to_string(SDL_GetPerformanceCounter()));
    require(std::filesystem::create_directory(directory), "Create temporary image fixtures");
    const auto redPath = (directory / "red.png").string();
    const auto greenPath = (directory / "green.png").string();
    const auto corruptPath = (directory / "corrupt.png").string();
    const auto missingPath = (directory / "missing.png").string();
    auto saveImage = [&](const std::string& path, Uint8 red, Uint8 green) {
        auto* surface = SDL_CreateSurface(8, 8, SDL_PIXELFORMAT_RGBA32);
        require(surface != nullptr, "Create async image fixture");
        require(SDL_FillSurfaceRect(surface, nullptr, SDL_MapSurfaceRGBA(surface, red, green, 0, 255)), "Fill async image fixture");
        require(IMG_SavePNG(surface, path.c_str()), "Save async image fixture");
        SDL_DestroySurface(surface);
    };
    saveImage(redPath, 255, 0);
    saveImage(greenPath, 0, 255);
    require(SDL_SaveFile(corruptPath.c_str(), "not an image", 12), "Save corrupt fixture");
    auto settle = [&](Image& image) {
        // Each pass must cover I/O and decoding; another pass allows alt-file submission.
        for (int pass = 0; pass < 3 && !image.isGraphicsReadyForFirstRender(); ++pass) {
            Image::waitForAsyncLoads();
            image.pumpGraphicsPreparation();
        }
        require(image.isGraphicsReadyForFirstRender(), "Async image reached a terminal state");
    };
    auto checkColor = [&](Image& image, bool red) {
        require(image.hasLoadedImage(), "Image successfully decoded and uploaded");
        require(image.baseViewInfo.ImageWidth == 8 && image.baseViewInfo.ImageHeight == 8, "Image dimensions match fixture");
        image.baseViewInfo.X = image.baseViewInfo.Y = 0;
        image.baseViewInfo.Width = image.baseViewInfo.Height = 64;
        image.baseViewInfo.Alpha = 1;
        auto* renderer = SDL::getRenderer(0);
        auto* target = SDL::getRenderTarget(0);
        require(SDL_SetRenderTarget(renderer, target), "Select image-test target");
        require(SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) && SDL_RenderClear(renderer), "Clear image-test target");
        image.draw();
        const auto color = pixel(renderer, target->w / 2, target->h / 2);
        require(red ? color.r > 245 && color.g < 10 : color.g > 245 && color.r < 10,
            "Async image renders expected fixture pixels");
        require(SDL_SetRenderTarget(renderer, nullptr), "Restore image-test target");
    };
    {
        Image image(redPath, "", page);
        Image shared(redPath, "", page);
        image.allocateGraphicsMemory();
        shared.allocateGraphicsMemory();
        settle(image);
        settle(shared);
        checkColor(image, true);
        checkColor(shared, true);
        require(image.recycleAsImage(greenPath), "Recycle image");
        settle(image);
        checkColor(image, false);
        Image cached(greenPath, "", page);
        cached.allocateGraphicsMemory();
        require(cached.hasLoadedImage(), "Cached image is immediately available");

        for (const auto& badPath : {missingPath, corruptPath}) {
            Image fallback(badPath, redPath, page, 0, false, false);
            fallback.allocateGraphicsMemory();
            settle(fallback);
            checkColor(fallback, true);
            Image failed(badPath, "", page);
            failed.allocateGraphicsMemory();
            settle(failed);
            require(!failed.hasLoadedImage(), "Failed image is not reported as successfully loaded");
        }

        // Drop/recycle consumers before completion; obsolete work must not replace new artwork.
        Image recycled(redPath, "", page, 0, false, false);
        recycled.allocateGraphicsMemory();
        for (int i = 0; i < 30; ++i) {
            Image abandoned(i % 2 ? redPath : greenPath, "", page, 0, false, false);
            abandoned.allocateGraphicsMemory();
            recycled.recycleAsImage(i % 2 ? redPath : greenPath);
        }
        recycled.recycleAsImage(greenPath);
        settle(recycled);
        checkColor(recycled, false);

        // Shutdown with live requests, then restart without waiting on broken promises.
        Image pending(corruptPath, "", page, 0, false, false);
        pending.allocateGraphicsMemory();
        Image::shutdownAsyncIO();
        settle(pending);
        Image restarted(redPath, "", page, 0, false, false);
        restarted.allocateGraphicsMemory();
        settle(restarted);
        checkColor(restarted, true);
    }
    Image::shutdownAsyncIO();
    Image::cleanupTextureCache();
    for (const auto& path : {redPath, greenPath, corruptPath}) std::filesystem::remove(path);
    std::filesystem::remove(directory);
}

// Optional measurement mode: keep the renderer identical for CPU/GPU decode.
// Measure through SDL_RenderPresent and report observations without timing limits.
void startupBenchmark(const std::string& file, const std::string& alternate) {
    gst_init(nullptr, nullptr);
    GlibLoop::instance().start();
    for (int count : {1, 7}) {
        std::vector<std::shared_ptr<IVideo>> videos;
        for (int i = 0; i < count; ++i) videos.push_back(VideoFactory::createVideo(0, 0, false, -1, nullptr));
        for (int trial = 0; trial < 3; ++trial) {
            std::vector<Uint64> starts(count), ready(count);
            const auto batchStart = SDL_GetTicksNS();
            for (int i = 0; i < count; ++i) {
                starts[i] = SDL_GetTicksNS();
                require(videos[i]->open(trial % 2 ? alternate : file), "Open benchmark video");
            }
            int remaining = count;
            while (remaining && SDL_GetTicksNS() - batchStart < 15000000000ULL) {
                SDL_PumpEvents();
                SDL_Renderer* renderer = SDL::getRenderer(0);
                require(SDL::startVideoFrame(renderer), "Start benchmark video frame");
                for (int i = 0; i < count; ++i) {
                    if (ready[i]) continue;
                    videos[i]->updateFrame();
                    require(!videos[i]->hasError(), "Benchmark decode succeeds");
                }
                require(SDL::submitVideoFrame(renderer), "Submit benchmark transfers");
                require(SDL_SetRenderTarget(renderer, nullptr), "Select benchmark window");
                require(SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) && SDL_RenderClear(renderer),
                    "Clear benchmark frame");
                std::vector<bool> presented(count, false);
                for (int i = 0; i < count; ++i) {
                    auto* texture = videos[i]->getTexture();
                    if (!texture) continue;
                    require(videos[i]->usingGpuTexture() == Configuration::HardwareVideoAccel,
                        "Benchmark uses requested decode path");
                    SDL_FRect destination{float((i % 4) * 48), float((i / 4) * 48), 48.f, 48.f};
                    require(SDL_RenderTexture(renderer, texture, nullptr, &destination), "Render benchmark video");
                    presented[i] = !ready[i];
                }
                require(SDL_RenderPresent(renderer), "Present benchmark video frame");
                const Uint64 presentedAt = SDL_GetTicksNS();
                for (int i = 0; i < count; ++i) if (presented[i]) {
                    ready[i] = presentedAt;
                    --remaining;
                    std::cout << "STARTUP count=" << count << " trial=" << trial
                        << " video=" << i << " ms=" << double(ready[i] - starts[i]) / 1e6 << '\n';
                }
                if (remaining) SDL_Delay(1);
            }
            require(remaining == 0, "Benchmark first frames arrive");
            std::cout << "STARTUP_BATCH count=" << count << " trial=" << trial
                << " ms=" << double(SDL_GetTicksNS() - batchStart) / 1e6 << '\n';
            for (auto& video : videos) require(video->unload(), "Unload benchmark video for reuse");
            const auto drainStart = SDL_GetTicksNS();
            while (!std::all_of(videos.begin(), videos.end(), [](const auto& video) { return video->isReadyForReuse(); }) &&
                   SDL_GetTicksNS() - drainStart < 15000000000ULL) {
                SDL_PumpEvents();
                SDL_Delay(1);
            }
            require(std::all_of(videos.begin(), videos.end(), [](const auto& video) { return video->isReadyForReuse(); }),
                "All benchmark instances drain for reuse");
            std::cout << "STARTUP_DRAIN count=" << count << " trial=" << trial
                << " ms=" << double(SDL_GetTicksNS() - drainStart) / 1e6 << '\n';
        }
        for (auto& video : videos) video->stop();
    }
    GStreamerVideo::waitForControlTasks();
    GlibLoop::instance().stop();
}

int main(int argc, char** argv) {
    const bool hardware = argc > 2 && std::string(argv[2]) == "--hardware";
    const bool benchmark = argc > 3 && std::string(argv[3]) == "--startup-benchmark";
    if (!hardware && !benchmark) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
#ifdef _WIN32
    const char* hardwareRenderer = std::getenv("RETROFE_TEST_RENDERER");
    if (!hardwareRenderer) hardwareRenderer = "direct3d11";
#else
    const char* hardwareRenderer = std::getenv("RETROFE_TEST_RENDERER");
    if (!hardwareRenderer) hardwareRenderer = "opengl";
#endif
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    Configuration config;
    config.setProperty("log", std::string(benchmark ? "DEBUG,INFO,WARNING,ERROR" : "INFO,WARNING,ERROR"));
    require(Logger::initialize(hardware ? "sdl3-hardware-runtime.log" : "sdl3-software-runtime.log", &config), "Initialize runtime log");
    config.setProperty("SDLRenderDriver", std::string(hardware || benchmark ? hardwareRenderer : "software"));
    config.setProperty("HardwareVideoAccel", hardware);
    if (const auto* backend = std::getenv("RETROFE_TEST_VIDEO_BACKEND")) {
        config.setProperty("VideoBackend", std::string(backend));
        VideoFactory::setBackend(backend);
    }
    config.setProperty("screenOrder", std::string("0"));
    config.setProperty("horizontal0", 64);
    config.setProperty("vertical0", 64);
    config.setProperty("fullscreen0", false);
    config.setProperty("hideMouse", false);
    config.setProperty("vSync", false);
    config.setProperty("layoutScaleMode", std::string("stretch"));
    if (benchmark) {
        require(SDL::initialize(config), "Initialize benchmark renderer");
        const std::string file = argc > 4 ? argv[4] : std::string(argv[1]) + "/layouts/Arcades/video/splash.mp4";
        startupBenchmark(file, argc > 5 ? argv[5] : file);
        require(SDL::deInitialize(true), "Shutdown benchmark renderer");
        Logger::deInitialize();
        return EXIT_SUCCESS;
    }
    renderChecks(config, true);
    inputChecks(config);
    if (argc > 1) mediaChecks(argv[1]);
    imageAsyncIOChecks(config);
    ffmpegContractChecks();
#ifdef RETROFE_HAVE_D3D12
    deferredNativeChecks();
#endif
#ifdef RETROFE_HAVE_FFMPEG
    dmaBufMetadataChecks();
#endif
    if (argc > 1 && hardware) concurrentVideoChecks(std::string(argv[1]) + "/layouts/Arcades/video/splash.mp4");
    if (argc > 1) scrollingVideoStartupChecks(config, std::string(argv[1]) + "/layouts/Arcades/video/splash.mp4");
    Image::shutdownAsyncIO();
    require(SDL::deInitialize(false), "Unload video while retaining audio/input");
    renderChecks(config, true);
    Image::shutdownAsyncIO();
    require(SDL::deInitialize(true), "Full SDL shutdown");
    config.setProperty("mirror0", true);
    config.setProperty("rotation0", 1);
    config.setProperty("layoutScaleMode", std::string("fit"));
    renderChecks(config, false);
    Image::shutdownAsyncIO();
    require(SDL::deInitialize(true), "Shutdown mirrored rotated output");
    std::cout << "SDL3 rendering and lifecycle smoke tests passed\n";
    Logger::deInitialize();
    return EXIT_SUCCESS;
}
