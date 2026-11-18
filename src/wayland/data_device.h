// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "ui/drag.h"
#include "wayland/selection.h"

struct wl_data_device;
struct wl_data_device_manager;
struct wl_data_offer;
struct wl_data_source;
struct wl_seat;
struct wl_buffer;
struct wl_surface;
struct xdg_toplevel_drag_v1;
struct wp_viewport;

namespace cutefish::ui::wayland {

struct DataDeviceListeners;
class ShellSurface;
class WaylandDropOffer;

// The seat's wl_data_device: the clipboard selection, and drag and drop
// with this client as the source, the target or both.
class DataDevice final : public Selection
{
public:
    DataDevice(Connection& connection, base::EventLoop& loop, wl_data_device_manager* manager,
               wl_seat* seat);
    ~DataDevice() override;

    void setSelection(const std::string& text);
    void requestSelection(std::function<void(std::string)> callback);

    // Starts a drag from the last button press on the surface; `attached`
    // is a toplevel the compositor moves with the pointer. False when a
    // drag is already running.
    bool startDrag(ShellSurface* origin, DragSource source, ShellSurface* attached, double scale);
    // The source of the drag this client runs, for its own targets.
    const DragSource* localSource() const { return m_dragSource ? &m_drag : nullptr; }
    // A window going away is no drop target any more.
    void forget(ShellSurface* shell);

protected:
    void* createSource(const std::vector<std::string>& mimeTypes) override;
    void destroySource(void* source) override;
    void setSource(void* source) override;
    void receive(void* offer, const char* mimeType, int fd) override;
    void destroyOffer(void* offer) override;

private:
    friend struct DataDeviceListeners;
    friend class WaylandDropOffer;

    // Tells the target window's view and answers the compositor.
    void updateTarget(uint32_t answer);
    // Drops the offer; an unfinished one tells the source the drag was cancelled.
    void endTarget();
    void endSource(DropAction action);
    void createIcon(const DragSource& source);

    wl_data_device_manager* m_manager;
    wl_data_device* m_device = nullptr;

    // The drag over one of our surfaces.
    std::unordered_map<wl_data_offer*, uint32_t> m_offerActions;
    std::shared_ptr<WaylandDropOffer> m_dropOffer;
    ShellSurface* m_dropShell = nullptr;
    uint32_t m_dropSerial = 0;
    uint32_t m_acceptedAction = 0;
    bool m_dropAccepted = false;

    // The drag this client runs.
    DragSource m_drag;
    wl_data_source* m_dragSource = nullptr;
    xdg_toplevel_drag_v1* m_toplevelDrag = nullptr;
    uint32_t m_dragAction = 0;
    bool m_dropPerformed = false;
    wl_surface* m_icon = nullptr;
    wp_viewport* m_iconViewport = nullptr;
    wl_buffer* m_iconBuffer = nullptr;
    void* m_iconMemory = nullptr;
    size_t m_iconSize = 0;
};

} // namespace cutefish::ui::wayland
