// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/test_input.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <ctime>

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"

#include "ui/layer.h"
#include "ui/painter.h"
#include "ui/platform_window.h"
#include "ui/view.h"

#include "ui/window.h"

namespace cutefish::ui {

void TestInput::movePointer(Window& window, PointF position)
{
    window.handlePointerMotion(position.x, position.y);
}

void TestInput::leavePointer(Window& window)
{
    window.handlePointerLeave();
}

void TestInput::press(Window& window, PointF position, MouseButton button, uint32_t modifiers)
{
    window.handlePointerMotion(position.x, position.y);
    window.handlePointerButton(button, true, modifiers);
}

void TestInput::release(Window& window, PointF position, MouseButton button)
{
    window.handlePointerMotion(position.x, position.y);
    window.handlePointerButton(button, false, window.m_modifiers);
}

void TestInput::click(Window& window, PointF position, MouseButton button)
{
    press(window, position, button);
    release(window, position, button);
}

void TestInput::scroll(Window& window, PointF position, const ScrollEvent& event)
{
    window.handlePointerMotion(position.x, position.y);
    window.handlePointerScroll(event);
}

void TestInput::gesture(Window& window, const GestureEvent& event)
{
    window.handleGesture(event);
}

void TestInput::hold(Window& window)
{
    window.handleHold();
}

void TestInput::configure(Window& window, int width, int height, const WindowStates& states)
{
    window.handleConfigure(width, height, states);
}

void TestInput::configureBounds(Window& window, int width, int height)
{
    window.handleConfigureBounds(width, height);
}

void TestInput::resizeInteractively(Window& window, SizeF size)
{
    window.handleInteractiveResize(true);
    window.handleConfigure(int(size.width), int(size.height), window.states());
    window.handleInteractiveResize(false);
}

void TestInput::setKeyboardFocus(Window& window, bool focused)
{
    window.handleKeyboardFocus(focused);
}

void TestInput::pressKey(Window& window, const KeyEvent& event)
{
    window.handleKey(event, true);
}

void TestInput::releaseKey(Window& window, const KeyEvent& event)
{
    window.handleKey(event, false);
}

void TestInput::typeKey(Window& window, uint32_t key, uint32_t modifiers, const std::string& text)
{
    KeyEvent event;
    event.key = key;
    event.modifiers = modifiers;
    event.text = text;
    pressKey(window, event);
    releaseKey(window, event);
}

void TestInput::typeText(Window& window, const std::string& text)
{
    for (size_t i = 0; i < text.size();) {
        size_t length = 1;
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead >= 0xf0)
            length = 4;
        else if (lead >= 0xe0)
            length = 3;
        else if (lead >= 0xc0)
            length = 2;
        const std::string character = text.substr(i, length);
        typeKey(window, length == 1 ? uint32_t(character[0]) : 0, NoModifier, character);
        i += length;
    }
}

void TestInput::inputMethod(Window& window, const TextInputEvent& event)
{
    window.handleTextInput(event);
}

Image TestInput::presentFrame(Window& window, uint64_t frameTimeNs)
{
    if (!frameTimeNs) {
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        frameTimeNs = uint64_t(now.tv_sec) * 1000000000 + uint64_t(now.tv_nsec);
    }
    if (!window.renderFrame(frameTimeNs))
        return {};
    const Image* image = window.m_platform->presentedImage();
    return image ? *image : Image();
}

Rect TestInput::inkRect(View& view, double scale)
{
    // Room for shadows and rings on every side.
    constexpr int margin = 64;
    const RectF& geometry = view.geometry();
    const int width = int(std::ceil(geometry.width * scale)) + 2 * margin;
    const int height = int(std::ceil(geometry.height * scale)) + 2 * margin;
    Image image(width, height);
    const SkImageInfo info = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    const sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, image.bits(), size_t(width) * 4);
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    Painter painter(surface->getCanvas(), scale);
    painter.translate({margin / scale, margin / scale});
    view.paint(painter);

    int left = INT_MAX;
    int top = INT_MAX;
    int right = INT_MIN;
    int bottom = INT_MIN;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (image.pixel(x, y).a == 0)
                continue;
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x + 1);
            bottom = std::max(bottom, y + 1);
        }
    }
    if (left > right)
        return {};
    return {left - margin, top - margin, right - left, bottom - top};
}

View* TestInput::toolTip(Window& window)
{
    return window.m_toolTip.get();
}

int TestInput::moveRequests(Window& window)
{
    return window.m_platform->moveRequests();
}

size_t TestInput::damageWatches(Window& window)
{
    return window.m_frameDamage->watches.size();
}

void TestInput::setGlassAvailable(bool available)
{
    PlatformWindow::setHeadlessGlassAvailable(available);
}

void TestInput::setVerifiesLayout(bool verifies)
{
    Window::setVerifiesLayout(verifies);
}

bool TestInput::verifiesLayout()
{
    return Window::verifiesLayout();
}

int TestInput::layoutMismatches()
{
    return Window::layoutMismatches();
}

std::optional<GlassMaterial> TestInput::backgroundGlass(Window& window)
{
    return window.m_platform->requestedGlass();
}

uint32_t TestInput::dragEnter(Window& window, PointF position, DropOffer* offer, uint32_t modifiers)
{
    window.handleDragEnter(position.x, position.y, offer, modifiers);
    return window.m_dropAction;
}

uint32_t TestInput::dragMove(Window& window, PointF position, uint32_t modifiers)
{
    return window.handleDragMotion(position.x, position.y, modifiers);
}

void TestInput::dragLeave(Window& window)
{
    window.handleDragLeave();
}

uint32_t TestInput::drop(Window& window)
{
    return window.handleDrop();
}

} // namespace cutefish::ui
