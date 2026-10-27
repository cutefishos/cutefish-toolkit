// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <EGL/egl.h>
#include <memory>
#include <optional>
#include <vector>

#include "include/core/SkRefCnt.h"

#include "ui/platform_window.h"
#include "wayland/shell_surface.h"

class SkSurface;
struct wl_egl_window;

namespace cutefish::ui::gpu {
class EglContext;
}

namespace cutefish::ui::wayland {

class Connection;
class MainMenuMirror;

// A child surface that hears of the window's commits.
class SubsurfaceClient
{
public:
    virtual ~SubsurfaceClient() = default;
    // On the UI thread, right before the window commits a frame.
    virtual void parentCommitting() = 0;
    // On the UI thread, right after the window committed it.
    virtual void parentCommitted() = 0;
    // The window's surface is going.
    virtual void parentGone() = 0;
};

// An xdg_toplevel drawn with GLES through Skia.
class WaylandWindow final : public PlatformWindow, private ShellSurface::Delegate
{
public:
    explicit WaylandWindow(Window* window);
    // A toplevel on another connection, such as the session's trusted one.
    WaylandWindow(Window* window, Connection& connection, gpu::EglContext& context);
    WaylandWindow(Window* window, WaylandWindow* parent, const PopupPlacement& placement, bool grab);
    // A status item in the bar; the connection must offer them.
    WaylandWindow(Window* window, StatusItemClient* statusItem);
    ~WaylandWindow() override;

    bool isHeadless() const override { return false; }
    void setTitle(const std::string& title) override;
    void setAppId(const std::string& appId) override;
    void setMinimumSize(int width, int height) override;
    void setMaximumSize(int width, int height) override;
    void setMaximized(bool maximized) override;
    void setFullScreen(bool fullScreen) override;
    void minimize() override;
    void activate() override;
    void activateWithToken(const std::string& token) override;
    void exportHandle(std::function<void(const std::string&)> done) override;
    void requestActivationToken(std::function<void(const std::string&)> done) override;
    SizeF screenSize() const override;
    void setTransientParent(PlatformWindow* parent, bool modal) override;
    void setRestoreSize(int width, int height) override;
    void show(int width, int height) override;
    void setPopupPlacement(const PopupPlacement& placement, int width, int height) override;
    void startMove() override;
    void setCursorShape(CursorShape shape) override;
    void setBlur(const RectF& rect, double radius) override;
    void setBlurShape(const Path& path) override;
    void setGlass(const std::optional<GlassMaterial>& material) override;
    bool glassAvailable() const override;
    void setResizeAroundCenter(bool aroundCenter) override;
    void setSkipTaskbar(bool skip) override;
    void setAlwaysOnTop(bool onTop) override;
    void hideApplication() override;
    void setRestoreId(const std::string& id) override;
    void setPlacementAnchor(const std::optional<PlacementAnchor>& anchor) override;
    void setBodyShape(const Path& path) override;
    void setCastsShadow(bool castsShadow) override;
    void setOpacity(double opacity) override;
    bool setScreenPosition(PointF position) override;
    void setUserResizable(bool resizable) override;
    void setMainMenu(Menu* menu) override;
    void setStatusItemIdentifier(const std::string& identifier) override;
    void setStatusItemHighlight(bool highlighted) override;
    void setForeignParent(const std::string& handle, bool modal) override;
    bool setDesktopRole(const std::string& outputName) override;
    void setShellOutput(const std::string& outputName) override;
    void activateShellSurface() override;
    void setTextInput(const TextInputState* state) override;
    bool startDrag(DragSource source) override;

    bool present(SizeF size, double scale, bool opaque, const PaintFunction& paint) override;

    // Null for a popup not shown yet.
    ShellSurface* shell() const { return m_shell.get(); }
    Connection& connection() const { return *m_connection; }
    gpu::EglContext& gpuContext() const { return *m_gpu; }
    void addSubsurface(SubsurfaceClient* client);
    void removeSubsurface(SubsurfaceClient* client);

private:
    void configure(int width, int height, const WindowStates& states) override;
    void scaleChanged(double scale) override;
    void closeRequested() override;
    void frameDone() override;
    void presented(uint64_t timestampNs, uint32_t refreshNs) override;
    void discarded() override;
    void pointerEnter(double x, double y) override;
    void pointerLeave() override;
    void pointerMotion(double x, double y) override;
    void pointerButton(MouseButton button, bool pressed, uint32_t modifiers) override;
    void pointerScroll(const ScrollEvent& event) override;
    void keyboardEnter() override;
    void keyboardLeave() override;
    void key(const KeyEvent& event, bool pressed) override;
    void pointerHold() override;
    void touchDown(int32_t id, double x, double y) override;
    void touchMotion(int32_t id, double x, double y) override;
    void touchUp(int32_t id) override;
    void touchCancel() override;
    void textInput(const TextInputEvent& event) override;
    void configureBounds(int width, int height) override;
    void interactiveResize(bool active) override;
    uint32_t dragEnter(double x, double y, DropOffer* offer, uint32_t modifiers) override;
    uint32_t dragMotion(double x, double y, uint32_t modifiers) override;
    void dragLeave() override;
    uint32_t drop() override;
    void workArea(const RectF& area) override;
    void screenPosition(PointF position) override;
    void statusItemLimits(int maxWidth) override;
    void statusItemAppearance(bool dark, uint32_t foreground) override;
    void statusItemHighlight(bool highlighted) override;
    void statusItemPlaced(const RectF& rect) override;

    Connection* m_connection;
    StatusItemClient* m_statusItem = nullptr;
    gpu::EglContext* m_gpu;
    std::unique_ptr<ShellSurface> m_shell;
    // A popup is made on show(), when its size is known.
    WaylandWindow* m_parent = nullptr;
    PopupPlacement m_placement;
    bool m_grab = false;
    RectF m_blurRect;
    double m_blurRadius = 0;
    Path m_blurShape;
    std::optional<GlassMaterial> m_glass;
    double m_opacity = 1;
    std::vector<SubsurfaceClient*> m_subsurfaces;
    std::shared_ptr<MainMenuMirror> m_mainMenu;
    wl_egl_window* m_eglWindow = nullptr;
    EGLSurface m_eglSurface = EGL_NO_SURFACE;
    sk_sp<SkSurface> m_surface;
    int m_bufferWidth = 0;
    int m_bufferHeight = 0;
};

} // namespace cutefish::ui::wayland
