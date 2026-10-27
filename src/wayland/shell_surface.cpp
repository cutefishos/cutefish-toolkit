// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "wayland/shell_surface.h"

#include <algorithm>
#include <cmath>
#include <wayland-client.h>

#include <cutefish-shell-v1-client-protocol.h>
#include <cutefish-status-item-v1-client-protocol.h>
#include <cutefish-surface-blur-v1-client-protocol.h>
#include <cutefish-window-hints-v1-client-protocol.h>
#include <cutefish-window-placement-v1-client-protocol.h>
#include <fractional-scale-v1-client-protocol.h>
#include <presentation-time-client-protocol.h>
#include <viewporter-client-protocol.h>
#include <xdg-dialog-v1-client-protocol.h>
#include <xdg-foreign-unstable-v2-client-protocol.h>
#include <xdg-shell-client-protocol.h>

#include "wayland/connection.h"

namespace cutefish::ui::wayland {

struct ShellSurfaceListeners
{
    static void surfaceEnter(void* data, wl_surface*, wl_output* output)
    {
        static_cast<ShellSurface*>(data)->m_outputs.push_back(output);
    }

    static void surfaceLeave(void* data, wl_surface*, wl_output* output)
    {
        std::erase(static_cast<ShellSurface*>(data)->m_outputs, output);
    }

    static void surfaceConfigure(void* data, xdg_surface*, uint32_t serial)
    {
        auto* self = static_cast<ShellSurface*>(data);
        self->m_configureSerial = serial;
        self->m_configurePending = true;
        self->m_delegate->configure(self->m_pendingWidth, self->m_pendingHeight,
                                    self->m_pendingStates);
    }

    static void toplevelConfigure(void* data, xdg_toplevel*, int32_t width, int32_t height,
                                  wl_array* states)
    {
        auto* self = static_cast<ShellSurface*>(data);
        self->m_pendingWidth = width;
        self->m_pendingHeight = height;
        WindowStates pending;
        const auto* state = static_cast<const uint32_t*>(states->data);
        for (size_t i = 0; i < states->size / sizeof(uint32_t); ++i) {
            switch (state[i]) {
            case XDG_TOPLEVEL_STATE_ACTIVATED:
                pending.activated = true;
                break;
            case XDG_TOPLEVEL_STATE_MAXIMIZED:
                pending.maximized = true;
                break;
            case XDG_TOPLEVEL_STATE_FULLSCREEN:
                pending.fullScreen = true;
                break;
            case XDG_TOPLEVEL_STATE_TILED_LEFT:
            case XDG_TOPLEVEL_STATE_TILED_RIGHT:
            case XDG_TOPLEVEL_STATE_TILED_TOP:
            case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
                pending.tiled = true;
                break;
            default:
                break;
            }
        }
        self->m_pendingStates = pending;
    }

    static void toplevelClose(void* data, xdg_toplevel*)
    {
        static_cast<ShellSurface*>(data)->m_delegate->closeRequested();
    }

    static void toplevelConfigureBounds(void* data, xdg_toplevel*, int32_t width, int32_t height)
    {
        static_cast<ShellSurface*>(data)->m_delegate->configureBounds(width, height);
    }

    static void toplevelWmCapabilities(void*, xdg_toplevel*, wl_array*)
    {
    }

    static void popupConfigure(void* data, xdg_popup*, int32_t, int32_t, int32_t width,
                               int32_t height)
    {
        auto* self = static_cast<ShellSurface*>(data);
        self->m_pendingWidth = width;
        self->m_pendingHeight = height;
        // A popup is as active as the window the keyboard is with.
        self->m_pendingStates.activated = true;
    }

    static void popupDone(void* data, xdg_popup*)
    {
        static_cast<ShellSurface*>(data)->m_delegate->closeRequested();
    }

    static void popupRepositioned(void*, xdg_popup*, uint32_t)
    {
    }

    static void preferredScale(void* data, wp_fractional_scale_v1*, uint32_t scale)
    {
        static_cast<ShellSurface*>(data)->m_delegate->scaleChanged(scale / 120.0);
    }

    static void frameDone(void* data, wl_callback* callback, uint32_t)
    {
        auto* self = static_cast<ShellSurface*>(data);
        wl_callback_destroy(callback);
        self->m_frameCallback = nullptr;
        self->m_delegate->frameDone();
    }

    static void feedbackSyncOutput(void*, struct wp_presentation_feedback*, wl_output*)
    {
    }

    static void feedbackPresented(void* data, struct wp_presentation_feedback* feedback,
                                  uint32_t secondsHigh, uint32_t secondsLow, uint32_t nanoseconds,
                                  uint32_t refresh, uint32_t, uint32_t, uint32_t)
    {
        auto* self = static_cast<ShellSurface*>(data);
        self->m_feedbacks.erase(feedback);
        wp_presentation_feedback_destroy(feedback);
        const uint64_t seconds = (uint64_t(secondsHigh) << 32) | secondsLow;
        self->m_delegate->presented(seconds * 1000000000 + nanoseconds, refresh);
    }

    static void feedbackDiscarded(void* data, struct wp_presentation_feedback* feedback)
    {
        auto* self = static_cast<ShellSurface*>(data);
        self->m_feedbacks.erase(feedback);
        wp_presentation_feedback_destroy(feedback);
        self->m_delegate->discarded();
    }

    static void workArea(void* data, cutefish_shell_surface_v1*, int32_t x, int32_t y, int32_t width, int32_t height)
    {
        static_cast<ShellSurface*>(data)->m_delegate->workArea({double(x), double(y), double(width), double(height)});
    }

    static void statusItemConfigure(void* data, cutefish_status_item_v1*, uint32_t serial, int32_t height,
                                    int32_t maxWidth)
    {
        auto* self = static_cast<ShellSurface*>(data);
        self->m_configureSerial = serial;
        self->m_configurePending = true;
        self->m_delegate->statusItemLimits(maxWidth);
        // An item never has the keyboard; its popups take it through their grab.
        self->m_delegate->configure(0, height, WindowStates {});
    }

    static void statusItemAppearance(void* data, cutefish_status_item_v1*, uint32_t appearance, uint32_t foreground)
    {
        static_cast<ShellSurface*>(data)->m_delegate->statusItemAppearance(
                appearance == CUTEFISH_STATUS_ITEM_V1_APPEARANCE_DARK, foreground);
    }

    static void statusItemHighlight(void* data, cutefish_status_item_v1*, uint32_t highlighted)
    {
        static_cast<ShellSurface*>(data)->m_delegate->statusItemHighlight(highlighted != 0);
    }

    static void statusItemPlaced(void* data, cutefish_status_item_v1*, int32_t x, int32_t y, int32_t width,
                                 int32_t height)
    {
        static_cast<ShellSurface*>(data)->m_delegate->statusItemPlaced(
                {double(x), double(y), double(width), double(height)});
    }

    static constexpr cutefish_shell_surface_v1_listener shellSurface = {workArea};
    static constexpr cutefish_status_item_v1_listener statusItem = {
        statusItemConfigure,
        statusItemAppearance,
        statusItemHighlight,
        statusItemPlaced,
    };
    static constexpr wl_surface_listener surface = {surfaceEnter, surfaceLeave};
    static constexpr xdg_surface_listener xdgSurface = {surfaceConfigure};
    static constexpr xdg_toplevel_listener xdgToplevel = {
        toplevelConfigure,
        toplevelClose,
        toplevelConfigureBounds,
        toplevelWmCapabilities,
    };
    static constexpr xdg_popup_listener xdgPopup = {
        popupConfigure,
        popupDone,
        popupRepositioned,
    };
    static constexpr wp_fractional_scale_v1_listener fractionalScale = {preferredScale};
    static constexpr wl_callback_listener frame = {frameDone};
    static constexpr wp_presentation_feedback_listener feedback = {
        feedbackSyncOutput,
        feedbackPresented,
        feedbackDiscarded,
    };
};

// Path commands and wl_fixed points, as cutefish_surface_blur_v1 and
// cutefish_window_hints_v1 take them; a close is implied by the fill.
static std::vector<int32_t> pathValues(const Path& path)
{
    std::vector<int32_t> values;
    for (const Path::Element& element : path.elements()) {
        switch (element.type) {
        case Path::Element::Type::MoveTo:
            values.push_back(CUTEFISH_SURFACE_BLUR_V1_PATH_COMMAND_MOVE_TO);
            break;
        case Path::Element::Type::LineTo:
            values.push_back(CUTEFISH_SURFACE_BLUR_V1_PATH_COMMAND_LINE_TO);
            break;
        case Path::Element::Type::CurveTo:
            values.push_back(CUTEFISH_SURFACE_BLUR_V1_PATH_COMMAND_CUBIC_TO);
            break;
        case Path::Element::Type::CurveToData:
            break;
        }
        values.push_back(wl_fixed_from_double(element.x));
        values.push_back(wl_fixed_from_double(element.y));
    }
    return values;
}

ShellSurface::ShellSurface(Connection& connection, Delegate* delegate)
    : m_connection(connection)
    , m_delegate(delegate)
{
    createSurface();
    createXdgSurface();
    m_xdgToplevel = xdg_surface_get_toplevel(m_xdgSurface);
    xdg_toplevel_add_listener(m_xdgToplevel, &ShellSurfaceListeners::xdgToplevel, this);

    // The window draws its own frame; the compositor keeps the resize edges.
    if (connection.windowHints())
        cutefish_window_hints_v1_set_server_resize(connection.windowHints(), m_surface, 1);
}

static xdg_positioner* createPositioner(Connection& connection, const PopupPlacement& placement,
                                        int width, int height)
{
    xdg_positioner* positioner = xdg_wm_base_create_positioner(connection.wmBase());
    xdg_positioner_set_size(positioner, std::max(1, width), std::max(1, height));
    const RectF& rect = placement.anchorRect;
    const int left = int(std::floor(rect.x));
    const int top = int(std::floor(rect.y));
    xdg_positioner_set_anchor_rect(positioner, left, top,
                                   std::max(1, int(std::ceil(rect.right())) - left),
                                   std::max(1, int(std::ceil(rect.bottom())) - top));
    // The enums follow xdg_positioner's anchor and gravity values.
    xdg_positioner_set_anchor(positioner, uint32_t(placement.anchor));
    xdg_positioner_set_gravity(positioner, uint32_t(placement.gravity));
    xdg_positioner_set_offset(positioner, int(std::round(placement.offset.x)),
                              int(std::round(placement.offset.y)));
    uint32_t adjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_NONE;
    if (placement.flipX)
        adjustment |= XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X;
    if (placement.flipY)
        adjustment |= XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y;
    if (placement.slide)
        adjustment |= XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
                | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y;
    if (placement.resize)
        adjustment |= XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X
                | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y;
    xdg_positioner_set_constraint_adjustment(positioner, adjustment);
    return positioner;
}

ShellSurface::ShellSurface(Connection& connection, Delegate* delegate, StatusItemRole)
    : m_connection(connection)
    , m_delegate(delegate)
{
    createSurface();
    m_statusItem = cutefish_status_item_manager_v1_get_status_item(connection.statusItems(), m_surface);
    cutefish_status_item_v1_add_listener(m_statusItem, &ShellSurfaceListeners::statusItem, this);
}

ShellSurface::ShellSurface(Connection& connection, Delegate* delegate, ShellSurface* parent,
                           const PopupPlacement& placement, int width, int height, bool grab)
    : m_connection(connection)
    , m_delegate(delegate)
    , m_parent(parent)
{
    createSurface();
    createXdgSurface();

    xdg_positioner* positioner = createPositioner(connection, placement, width, height);
    // A status item's popup has no xdg parent; the item adopts it, before the
    // grab and the first commit.
    m_xdgPopup = xdg_surface_get_popup(m_xdgSurface, parent->m_statusItem ? nullptr : parent->m_xdgSurface,
                                       positioner);
    xdg_positioner_destroy(positioner);
    xdg_popup_add_listener(m_xdgPopup, &ShellSurfaceListeners::xdgPopup, this);
    if (parent->m_statusItem)
        cutefish_status_item_v1_get_popup(parent->m_statusItem, m_xdgPopup);
    if (grab)
        xdg_popup_grab(m_xdgPopup, connection.seat(), connection.inputSerial());
}

SizeF ShellSurface::outputSize() const
{
    for (wl_output* output : m_outputs) {
        const SizeF size = m_connection.outputSize(output);
        if (size.width > 0)
            return size;
    }
    if (m_parent)
        return m_parent->outputSize();
    return m_connection.primaryOutputSize();
}

void ShellSurface::createSurface()
{
    m_surface = wl_compositor_create_surface(m_connection.compositor());
    // Input events find their window through the surface.
    wl_surface_set_user_data(m_surface, this);
    wl_surface_add_listener(m_surface, &ShellSurfaceListeners::surface, this);

    m_viewport = wp_viewporter_get_viewport(m_connection.viewporter(), m_surface);
    m_fractionalScale = wp_fractional_scale_manager_v1_get_fractional_scale(
        m_connection.fractionalScaleManager(), m_surface);
    wp_fractional_scale_v1_add_listener(m_fractionalScale, &ShellSurfaceListeners::fractionalScale,
                                        this);
}

void ShellSurface::createXdgSurface()
{
    m_xdgSurface = xdg_wm_base_get_xdg_surface(m_connection.wmBase(), m_surface);
    xdg_surface_add_listener(m_xdgSurface, &ShellSurfaceListeners::xdgSurface, this);
}

ShellSurface::~ShellSurface()
{
    m_connection.forget(this);
    for (auto* feedback : m_feedbacks)
        wp_presentation_feedback_destroy(feedback);
    if (m_frameCallback)
        wl_callback_destroy(m_frameCallback);
    if (m_connection.surfaceBlur())
        cutefish_surface_blur_v1_unset(m_connection.surfaceBlur(), m_surface);
    if (m_dialog)
        xdg_dialog_v1_destroy(m_dialog);
    if (m_imported)
        zxdg_imported_v2_destroy(m_imported);
    if (m_shellSurface)
        cutefish_shell_surface_v1_destroy(m_shellSurface);
    if (m_xdgPopup)
        xdg_popup_destroy(m_xdgPopup);
    if (m_xdgToplevel)
        xdg_toplevel_destroy(m_xdgToplevel);
    if (m_statusItem)
        cutefish_status_item_v1_destroy(m_statusItem);
    if (m_xdgSurface)
        xdg_surface_destroy(m_xdgSurface);
    wp_fractional_scale_v1_destroy(m_fractionalScale);
    wp_viewport_destroy(m_viewport);
    wl_surface_destroy(m_surface);
    m_connection.flush();
}

void ShellSurface::setTitle(const std::string& title)
{
    if (m_xdgToplevel)
        xdg_toplevel_set_title(m_xdgToplevel, title.c_str());
}

void ShellSurface::setAppId(const std::string& appId)
{
    if (m_xdgToplevel)
        xdg_toplevel_set_app_id(m_xdgToplevel, appId.c_str());
}

void ShellSurface::setMinimumSize(int width, int height)
{
    if (m_xdgToplevel)
        xdg_toplevel_set_min_size(m_xdgToplevel, width, height);
}

void ShellSurface::setMaximumSize(int width, int height)
{
    if (m_xdgToplevel)
        xdg_toplevel_set_max_size(m_xdgToplevel, width, height);
}

void ShellSurface::setMaximized(bool maximized)
{
    if (!m_xdgToplevel)
        return;
    if (maximized)
        xdg_toplevel_set_maximized(m_xdgToplevel);
    else
        xdg_toplevel_unset_maximized(m_xdgToplevel);
}

void ShellSurface::setFullScreen(bool fullScreen)
{
    if (!m_xdgToplevel)
        return;
    if (fullScreen)
        xdg_toplevel_set_fullscreen(m_xdgToplevel, nullptr);
    else
        xdg_toplevel_unset_fullscreen(m_xdgToplevel);
}

void ShellSurface::minimize()
{
    if (m_xdgToplevel)
        xdg_toplevel_set_minimized(m_xdgToplevel);
}

void ShellSurface::setParent(ShellSurface* parent, bool modal)
{
    if (!m_xdgToplevel)
        return;
    xdg_toplevel_set_parent(m_xdgToplevel, parent ? parent->m_xdgToplevel : nullptr);
    if (!m_dialog && m_connection.dialogManager())
        m_dialog = xdg_wm_dialog_v1_get_xdg_dialog(m_connection.dialogManager(), m_xdgToplevel);
    if (m_dialog) {
        if (modal)
            xdg_dialog_v1_set_modal(m_dialog);
        else
            xdg_dialog_v1_unset_modal(m_dialog);
    }
}

void ShellSurface::setForeignParent(const std::string& handle, bool modal)
{
    if (!m_xdgToplevel)
        return;
    if (m_imported) {
        zxdg_imported_v2_destroy(m_imported);
        m_imported = nullptr;
    }
    std::string name = handle;
    if (name.rfind("wayland:", 0) == 0)
        name.erase(0, 8);
    if (name.empty() || !m_connection.importer()) {
        xdg_toplevel_set_parent(m_xdgToplevel, nullptr);
        return;
    }
    m_imported = zxdg_importer_v2_import_toplevel(m_connection.importer(), name.c_str());
    zxdg_imported_v2_set_parent_of(m_imported, m_surface);
    if (!m_dialog && m_connection.dialogManager())
        m_dialog = xdg_wm_dialog_v1_get_xdg_dialog(m_connection.dialogManager(), m_xdgToplevel);
    if (m_dialog) {
        if (modal)
            xdg_dialog_v1_set_modal(m_dialog);
        else
            xdg_dialog_v1_unset_modal(m_dialog);
    }
}

bool ShellSurface::setDesktop(const std::string& outputName)
{
    if (!m_connection.shell())
        return false;
    if (!m_shellSurface) {
        m_shellSurface = cutefish_shell_v1_get_shell_surface(m_connection.shell(), m_surface);
        cutefish_shell_surface_v1_add_listener(m_shellSurface, &ShellSurfaceListeners::shellSurface, this);
    }
    cutefish_shell_surface_v1_set_role(m_shellSurface, CUTEFISH_SHELL_SURFACE_V1_ROLE_DESKTOP);
    cutefish_shell_surface_v1_set_focus_policy(m_shellSurface, CUTEFISH_SHELL_SURFACE_V1_FOCUS_POLICY_NORMAL);
    if (!outputName.empty())
        cutefish_shell_surface_v1_set_output(m_shellSurface, outputName.c_str());
    // The desktop draws no frame, so the compositor owns no resize edges.
    if (m_connection.windowHints())
        cutefish_window_hints_v1_set_server_resize(m_connection.windowHints(), m_surface, 0);
    return true;
}

void ShellSurface::setShellOutput(const std::string& outputName)
{
    if (m_shellSurface && !outputName.empty())
        cutefish_shell_surface_v1_set_output(m_shellSurface, outputName.c_str());
}

void ShellSurface::activateShellSurface()
{
    if (m_shellSurface)
        cutefish_shell_surface_v1_activate(m_shellSurface);
}

void ShellSurface::setRestoreSize(int width, int height)
{
    if (m_xdgToplevel && m_connection.windowHints())
        cutefish_window_hints_v1_set_restore_size(m_connection.windowHints(), m_surface, width, height);
}

void ShellSurface::startMove()
{
    if (m_xdgToplevel)
        xdg_toplevel_move(m_xdgToplevel, m_connection.seat(), m_connection.buttonSerial());
}

void ShellSurface::setBlur(const RectF& rect, double radius)
{
    cutefish_surface_blur_v1* blur = m_connection.surfaceBlur();
    if (!blur)
        return;
    if (rect.width <= 0 || rect.height <= 0) {
        cutefish_surface_blur_v1_unset(blur, m_surface);
        return;
    }
    const int left = int(std::round(rect.x));
    const int top = int(std::round(rect.y));
    cutefish_surface_blur_v1_set(blur, m_surface, left, top, int(std::round(rect.right())) - left,
                                 int(std::round(rect.bottom())) - top, int(std::round(radius)));
}

void ShellSurface::setBlurShape(const Path& path)
{
    cutefish_surface_blur_v1* blur = m_connection.surfaceBlur();
    if (!blur)
        return;
    if (path.isEmpty()) {
        cutefish_surface_blur_v1_unset_shape(blur, m_surface);
        return;
    }
    const std::vector<int32_t> values = pathValues(path);
    // The protocol's limit; a longer outline would be a protocol error.
    if (values.size() > 4096) {
        cutefish_surface_blur_v1_unset_shape(blur, m_surface);
        return;
    }
    wl_array array;
    wl_array_init(&array);
    void* data = wl_array_add(&array, values.size() * sizeof(int32_t));
    if (data) {
        std::copy(values.begin(), values.end(), static_cast<int32_t*>(data));
        cutefish_surface_blur_v1_set_shape(blur, m_surface, &array);
    }
    wl_array_release(&array);
}

void ShellSurface::setServerResize(bool enabled)
{
    if (m_xdgToplevel && m_connection.windowHints())
        cutefish_window_hints_v1_set_server_resize(m_connection.windowHints(), m_surface, enabled ? 1 : 0);
}

void ShellSurface::setBodyShape(const Path& path)
{
    cutefish_window_hints_v1* hints = m_connection.windowHints();
    if (!hints)
        return;
    const std::vector<int32_t> values = pathValues(path);
    if (path.isEmpty() || values.size() > 4096) {
        cutefish_window_hints_v1_unset_body_shape(hints, m_surface);
        return;
    }
    wl_array array;
    wl_array_init(&array);
    void* data = wl_array_add(&array, values.size() * sizeof(int32_t));
    if (data) {
        std::copy(values.begin(), values.end(), static_cast<int32_t*>(data));
        cutefish_window_hints_v1_set_body_shape(hints, m_surface, &array);
    }
    wl_array_release(&array);
}

void ShellSurface::setCastsShadow(bool castsShadow)
{
    if (m_connection.windowHints())
        cutefish_window_hints_v1_set_shadow(m_connection.windowHints(), m_surface, castsShadow ? 1 : 0);
}

void ShellSurface::setOpacity(double opacity)
{
    if (m_connection.windowHints())
        cutefish_window_hints_v1_set_opacity(m_connection.windowHints(), m_surface,
                                             wl_fixed_from_double(std::clamp(opacity, 0.0, 1.0)));
}

bool ShellSurface::setScreenPosition(PointF position)
{
    cutefish_window_placement_v1* placement = m_connection.windowPlacement();
    if (!m_xdgToplevel || !placement)
        return false;
    cutefish_window_placement_v1_set_position(placement, m_surface, int(std::lround(position.x)),
                                              int(std::lround(position.y)));
    return true;
}

void ShellSurface::setGlass(const std::optional<GlassMaterial>& material)
{
    cutefish_surface_blur_v1* blur = m_connection.surfaceBlur();
    if (!blur)
        return;
    if (!material) {
        cutefish_surface_blur_v1_unset_glass(blur, m_surface);
        return;
    }
    const GlassMaterial& glass = *material;
    // Negative values are protocol errors.
    const auto fixed = [](double value) { return wl_fixed_from_double(std::max(0.0, value)); };
    cutefish_surface_blur_v1_set_glass_material(
        blur, m_surface, fixed(glass.blurSigma), fixed(glass.bevel), fixed(glass.depth),
        wl_fixed_from_double(std::max(1.0, glass.ior)), fixed(glass.profile),
        wl_fixed_from_double(glass.displacement), fixed(glass.chroma));
    cutefish_surface_blur_v1_set_glass_lighting(
        blur, m_surface, wl_fixed_from_double(glass.lightAngle), fixed(glass.specularIntensity),
        fixed(glass.shininess), fixed(glass.sheen), fixed(glass.ambientOcclusion),
        fixed(glass.ambientOcclusionRadius));
    cutefish_surface_blur_v1_set_glass_tint(blur, m_surface, fixed(glass.tintRed), fixed(glass.tintGreen),
                                            fixed(glass.tintBlue), fixed(glass.tintAlpha));
    cutefish_surface_blur_v1_set_glass_saturation(blur, m_surface, fixed(glass.saturation));
    cutefish_surface_blur_v1_set_glass_rim(blur, m_surface, fixed(glass.rim));
    cutefish_surface_blur_v1_set_glass(blur, m_surface);
}

void ShellSurface::setResizeAroundCenter(bool aroundCenter)
{
    if (m_xdgToplevel && m_connection.windowHints())
        cutefish_window_hints_v1_set_resize_anchor(m_connection.windowHints(), m_surface,
                                                   aroundCenter ? CUTEFISH_WINDOW_HINTS_V1_RESIZE_ANCHOR_CENTER
                                                                : CUTEFISH_WINDOW_HINTS_V1_RESIZE_ANCHOR_TOP_LEFT);
}

void ShellSurface::setSkipTaskbar(bool skip)
{
    if (m_connection.windowHints())
        cutefish_window_hints_v1_set_skip_taskbar(m_connection.windowHints(), m_surface, skip ? 1 : 0);
}

void ShellSurface::setAlwaysOnTop(bool onTop)
{
    if (m_connection.windowHints())
        cutefish_window_hints_v1_set_always_on_top(m_connection.windowHints(), m_surface, onTop ? 1 : 0);
}

void ShellSurface::hideApplication()
{
    if (m_connection.windowHints())
        cutefish_window_hints_v1_hide_application(m_connection.windowHints(), m_surface);
}

void ShellSurface::setRestoreId(const std::string& id)
{
    if (m_xdgToplevel && m_connection.windowHints())
        cutefish_window_hints_v1_set_restore_id(m_connection.windowHints(), m_surface, id.c_str());
}

void ShellSurface::setPlacementAnchor(const std::optional<PlacementAnchor>& anchor)
{
    if (!m_xdgToplevel || !m_connection.windowHints())
        return;
    if (!anchor) {
        cutefish_window_hints_v1_unset_placement_anchor(m_connection.windowHints(), m_surface);
        return;
    }
    const RectF& rect = anchor->rect;
    const int left = int(std::lround(rect.x));
    const int top = int(std::lround(rect.y));
    // The enums follow cutefish_window_hints_v1's placement edges.
    cutefish_window_hints_v1_set_placement_anchor(
            m_connection.windowHints(), m_surface, left, top,
            std::max(0, int(std::lround(rect.x + rect.width)) - left),
            std::max(0, int(std::lround(rect.y + rect.height)) - top), uint32_t(anchor->anchor),
            uint32_t(anchor->gravity));
}

void ShellSurface::setStatusItemIdentifier(const std::string& identifier)
{
    if (m_statusItem)
        cutefish_status_item_v1_set_identifier(m_statusItem, identifier.c_str());
}

void ShellSurface::setStatusItemHighlight(bool highlighted)
{
    if (m_statusItem)
        cutefish_status_item_v1_set_highlight(m_statusItem, highlighted ? 1 : 0);
}

void ShellSurface::map()
{
    wl_surface_commit(m_surface);
}

void ShellSurface::reposition(const PopupPlacement& placement, int width, int height)
{
    if (!m_xdgPopup || xdg_popup_get_version(m_xdgPopup) < XDG_POPUP_REPOSITION_SINCE_VERSION)
        return;
    xdg_positioner* positioner = createPositioner(m_connection, placement, width, height);
    xdg_popup_reposition(m_xdgPopup, positioner, 0);
    xdg_positioner_destroy(positioner);
}

void ShellSurface::prepareCommit(int width, int height, bool opaque)
{
    if (m_configurePending) {
        if (m_statusItem)
            cutefish_status_item_v1_ack_configure(m_statusItem, m_configureSerial);
        else
            xdg_surface_ack_configure(m_xdgSurface, m_configureSerial);
        m_configurePending = false;
    }

    const bool resized = width != m_committedWidth || height != m_committedHeight;
    if (resized)
        wp_viewport_set_destination(m_viewport, width, height);
    if (resized || opaque != m_committedOpaque) {
        if (opaque) {
            wl_region* region = wl_compositor_create_region(m_connection.compositor());
            wl_region_add(region, 0, 0, width, height);
            wl_surface_set_opaque_region(m_surface, region);
            wl_region_destroy(region);
        } else {
            wl_surface_set_opaque_region(m_surface, nullptr);
        }
    }
    m_committedWidth = width;
    m_committedHeight = height;
    m_committedOpaque = opaque;

    if (!m_frameCallback) {
        m_frameCallback = wl_surface_frame(m_surface);
        wl_callback_add_listener(m_frameCallback, &ShellSurfaceListeners::frame, this);
    }

    if (m_connection.presentation()) {
        struct wp_presentation_feedback* feedback =
            wp_presentation_feedback(m_connection.presentation(), m_surface);
        wp_presentation_feedback_add_listener(feedback, &ShellSurfaceListeners::feedback, this);
        m_feedbacks.insert(feedback);
    }
}

} // namespace cutefish::ui::wayland
