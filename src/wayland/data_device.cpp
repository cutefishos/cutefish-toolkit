// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "wayland/data_device.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include <viewporter-client-protocol.h>
#include <xdg-toplevel-drag-v1-client-protocol.h>

#include "base/event_loop.h"
#include "ui/application.h"
#include "wayland/connection.h"
#include "wayland/shell_surface.h"

namespace cutefish::ui::wayland {

static ShellSurface* shellFor(wl_surface* surface)
{
    return surface ? static_cast<ShellSurface*>(wl_surface_get_user_data(surface)) : nullptr;
}

// A wl_data_offer of a drag, as views read it.
class WaylandDropOffer final : public DropOffer
{
public:
    WaylandDropOffer(DataDevice& device, wl_data_offer* offer, std::vector<std::string> types, uint32_t actions)
        : m_device(device)
        , m_offer(offer)
        , m_types(std::move(types))
        , m_actions(actions)
    {
    }

    ~WaylandDropOffer() override
    {
        if (m_offer)
            wl_data_offer_destroy(m_offer);
    }

    const std::vector<std::string>& mimeTypes() const override { return m_types; }
    uint32_t sourceActions() const override { return m_actions; }
    const DragSource* localSource() const override { return m_device.localSource(); }
    uint32_t action() const override { return m_action; }

    void read(const std::string& mimeType, std::function<void(std::string data)> done) override
    {
        int fds[2];
        if (!m_offer || std::find(m_types.begin(), m_types.end(), mimeType) == m_types.end()
            || pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) {
            m_device.m_loop.post([done = std::move(done)] { done({}); });
            return;
        }
        wl_data_offer_receive(m_offer, mimeType.c_str(), fds[1]);
        close(fds[1]);
        m_device.m_connection.flush();
        readFromPipe(m_device.m_loop, fds[0], [done = std::move(done)](std::optional<std::string> data) {
            done(data.value_or(std::string()));
        });
    }

    wl_data_offer* offer() const { return m_offer; }
    void setSourceActions(uint32_t actions) { m_actions = actions; }
    void setAction(uint32_t action) { m_action = action; }

private:
    DataDevice& m_device;
    wl_data_offer* m_offer;
    std::vector<std::string> m_types;
    uint32_t m_actions;
    uint32_t m_action = NoAction;
};

struct DataDeviceListeners
{
    static void offerMimeType(void* data, wl_data_offer* offer, const char* type)
    {
        static_cast<DataDevice*>(data)->offerMimeType(offer, type);
    }

    static void offerSourceActions(void* data, wl_data_offer* offer, uint32_t actions)
    {
        auto* self = static_cast<DataDevice*>(data);
        self->m_offerActions[offer] = actions;
        if (self->m_dropOffer && self->m_dropOffer->offer() == offer)
            self->m_dropOffer->setSourceActions(actions);
    }

    static void offerAction(void* data, wl_data_offer* offer, uint32_t action)
    {
        auto* self = static_cast<DataDevice*>(data);
        if (self->m_dropOffer && self->m_dropOffer->offer() == offer)
            self->m_dropOffer->setAction(action);
    }

    static void dataOffer(void* data, wl_data_device*, wl_data_offer* offer)
    {
        auto* self = static_cast<DataDevice*>(data);
        self->offerAnnounced(offer);
        wl_data_offer_add_listener(offer, &DataDeviceListeners::offer, self);
    }

    static void enter(void* data, wl_data_device*, uint32_t serial, wl_surface* surface, wl_fixed_t x,
                      wl_fixed_t y, wl_data_offer* offer)
    {
        auto* self = static_cast<DataDevice*>(data);
        self->endTarget();
        if (!offer)
            return;
        // The drag's offer leaves the selection's bookkeeping; the drop offer owns it now.
        std::vector<std::string> types;
        if (const std::vector<std::string>* announced = self->offerTypes(offer))
            types = *announced;
        self->forgetOffer(offer);
        uint32_t actions = 0;
        if (const auto it = self->m_offerActions.find(offer); it != self->m_offerActions.end()) {
            actions = it->second;
            self->m_offerActions.erase(it);
        }
        self->m_dropOffer = std::make_shared<WaylandDropOffer>(*self, offer, std::move(types), actions);
        self->m_dropSerial = serial;
        self->m_dropShell = shellFor(surface);
        self->m_dropAccepted = false;
        self->m_acceptedAction = ~0u;
        uint32_t action = 0;
        if (self->m_dropShell) {
            action = self->m_dropShell->delegate()->dragEnter(
                wl_fixed_to_double(x), wl_fixed_to_double(y), self->m_dropOffer.get(),
                self->m_connection.keyboardModifiers());
        }
        self->updateTarget(action);
    }

    static void leave(void* data, wl_data_device*)
    {
        auto* self = static_cast<DataDevice*>(data);
        if (self->m_dropShell)
            self->m_dropShell->delegate()->dragLeave();
        self->m_dropShell = nullptr;
        self->endTarget();
    }

    static void motion(void* data, wl_data_device*, uint32_t, wl_fixed_t x, wl_fixed_t y)
    {
        auto* self = static_cast<DataDevice*>(data);
        if (!self->m_dropOffer || !self->m_dropShell)
            return;
        const uint32_t action = self->m_dropShell->delegate()->dragMotion(
            wl_fixed_to_double(x), wl_fixed_to_double(y), self->m_connection.keyboardModifiers());
        self->updateTarget(action);
    }

    static void drop(void* data, wl_data_device*)
    {
        auto* self = static_cast<DataDevice*>(data);
        if (!self->m_dropOffer)
            return;
        ShellSurface* shell = self->m_dropShell;
        self->m_dropShell = nullptr;
        // Keeps the offer alive through the handler, which may read from it.
        const std::shared_ptr<WaylandDropOffer> offer = self->m_dropOffer;
        uint32_t action = shell && self->m_dropAccepted ? shell->delegate()->drop() : 0;
        // A drop is finished with one action, never ask: a target that left
        // the choice open gets the source's copy, else its move.
        if (action == AskAction) {
            const uint32_t allowed = offer->sourceActions();
            action = allowed & CopyAction ? CopyAction : allowed & MoveAction ? MoveAction : NoAction;
        }
        wl_data_offer* wire = offer->offer();
        if (action != 0 && wire && wl_proxy_get_version(reinterpret_cast<wl_proxy*>(wire)) >= 3) {
            // The source hears the final action, the one the user chose
            // after an ask included.
            if (action != offer->action())
                wl_data_offer_set_actions(wire, action, action);
            wl_data_offer_finish(wire);
        }
        self->endTarget();
    }

    static void selection(void* data, wl_data_device*, wl_data_offer* offer)
    {
        static_cast<DataDevice*>(data)->selectionChanged(offer);
    }

    static void sourceTarget(void*, wl_data_source*, const char*)
    {
    }

    static void sourceSend(void* data, wl_data_source* source, const char* type, int32_t fd)
    {
        auto* self = static_cast<DataDevice*>(data);
        if (source == self->m_dragSource) {
            const std::string mimeType = type;
            const bool offered = std::find(self->m_drag.mimeTypes.begin(), self->m_drag.mimeTypes.end(),
                                           mimeType) != self->m_drag.mimeTypes.end();
            if (!offered || !self->m_drag.data) {
                close(fd);
                return;
            }
            writeToPipe(self->m_loop, fd, self->m_drag.data(mimeType));
            return;
        }
        self->Selection::sourceSend(source, type, fd);
    }

    static void sourceCancelled(void* data, wl_data_source* source)
    {
        auto* self = static_cast<DataDevice*>(data);
        if (source == self->m_dragSource) {
            self->endSource(NoAction);
            return;
        }
        self->Selection::sourceCancelled(source);
    }

    static void sourceDropPerformed(void* data, wl_data_source* source)
    {
        auto* self = static_cast<DataDevice*>(data);
        if (source == self->m_dragSource)
            self->m_dropPerformed = true;
    }

    static void sourceFinished(void* data, wl_data_source* source)
    {
        auto* self = static_cast<DataDevice*>(data);
        if (source == self->m_dragSource)
            self->endSource(DropAction(self->m_dragAction));
    }

    static void sourceAction(void* data, wl_data_source* source, uint32_t action)
    {
        auto* self = static_cast<DataDevice*>(data);
        if (source == self->m_dragSource)
            self->m_dragAction = action;
    }

    static constexpr wl_data_offer_listener offer = {
        offerMimeType,
        offerSourceActions,
        offerAction,
    };
    static constexpr wl_data_device_listener device = {
        dataOffer, enter, leave, motion, drop, selection,
    };
    static constexpr wl_data_source_listener source = {
        sourceTarget, sourceSend, sourceCancelled, sourceDropPerformed, sourceFinished, sourceAction,
    };
};

DataDevice::DataDevice(Connection& connection, base::EventLoop& loop, wl_data_device_manager* manager,
                       wl_seat* seat)
    : Selection(connection, loop, ClipboardMode::Clipboard)
    , m_manager(manager)
    , m_device(wl_data_device_manager_get_data_device(manager, seat))
{
    wl_data_device_add_listener(m_device, &DataDeviceListeners::device, this);
}

DataDevice::~DataDevice()
{
    m_dropShell = nullptr;
    m_dropOffer.reset();
    if (m_dragSource) {
        // No callback: the application is going.
        m_drag.onFinished = nullptr;
        m_drag.onEnded = nullptr;
        endSource(NoAction);
    }
    releaseAll();
    wl_data_device_release(m_device);
}

void DataDevice::updateTarget(uint32_t answer)
{
    if (!m_dropOffer || !m_dropOffer->offer())
        return;
    // Out of what the source allows; the compositor picks among the
    // accepted ones by the keys held.
    const uint32_t allowed = m_dropOffer->sourceActions() ? m_dropOffer->sourceActions() : ~0u;
    uint32_t preferred = preferredDropAction(answer) & allowed;
    preferred &= ~preferred + 1;
    const uint32_t accepted = (acceptedDropActions(answer) & allowed) | preferred;
    const uint32_t target = dropActions(accepted, DropAction(preferred));
    if (target == m_acceptedAction)
        return;
    m_acceptedAction = target;
    m_dropAccepted = accepted != 0;
    wl_data_offer* offer = m_dropOffer->offer();
    const std::vector<std::string>& types = m_dropOffer->mimeTypes();
    wl_data_offer_accept(offer, m_dropSerial, m_dropAccepted && !types.empty() ? types.front().c_str() : nullptr);
    if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(offer)) >= 3)
        wl_data_offer_set_actions(offer, accepted, preferred);
    m_connection.flush();
}

void DataDevice::forget(ShellSurface* shell)
{
    if (m_dropShell == shell)
        m_dropShell = nullptr;
}

void DataDevice::endTarget()
{
    m_dropOffer.reset();
    m_dropAccepted = false;
    m_acceptedAction = 0;
}

void DataDevice::createIcon(const DragSource& source)
{
    if (source.icon.isNull() || !m_connection.shm())
        return;
    const Bitmap pixels = source.icon.converted(Bitmap::Format::Argb32Premultiplied);
    const int width = pixels.width();
    const int height = pixels.height();
    const size_t stride = size_t(width) * 4;
    m_iconSize = stride * size_t(height);
    const int fd = memfd_create("cutefish-drag-icon", MFD_CLOEXEC);
    if (fd < 0)
        return;
    if (ftruncate(fd, off_t(m_iconSize)) < 0) {
        close(fd);
        return;
    }
    m_iconMemory = mmap(nullptr, m_iconSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m_iconMemory == MAP_FAILED) {
        m_iconMemory = nullptr;
        close(fd);
        return;
    }
    for (int y = 0; y < height; ++y)
        std::memcpy(static_cast<uint8_t*>(m_iconMemory) + size_t(y) * stride, pixels.constScanLine(y), stride);
    wl_shm_pool* pool = wl_shm_create_pool(m_connection.shm(), fd, int32_t(m_iconSize));
    m_iconBuffer = wl_shm_pool_create_buffer(pool, 0, width, height, int32_t(stride), WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    m_icon = wl_compositor_create_surface(m_connection.compositor());
}

bool DataDevice::startDrag(ShellSurface* origin, DragSource source, ShellSurface* attached, double)
{
    if (m_dragSource || !origin)
        return false;
    m_drag = std::move(source);
    m_dragAction = 0;
    m_dropPerformed = false;
    m_dragSource = wl_data_device_manager_create_data_source(m_manager);
    wl_data_source_add_listener(m_dragSource, &DataDeviceListeners::source, this);
    for (const std::string& type : m_drag.mimeTypes)
        wl_data_source_offer(m_dragSource, type.c_str());
    if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(m_dragSource)) >= 3)
        wl_data_source_set_actions(m_dragSource, m_drag.actions);

    if (attached && attached->toplevel() && m_connection.toplevelDragManager()) {
        m_toplevelDrag = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(m_connection.toplevelDragManager(),
                                                                            m_dragSource);
        xdg_toplevel_drag_v1_attach(m_toplevelDrag, attached->toplevel(),
                                    int32_t(std::lround(m_drag.attachedOffset.x)),
                                    int32_t(std::lround(m_drag.attachedOffset.y)));
    }

    createIcon(m_drag);
    wl_data_device_start_drag(m_device, m_dragSource, origin->surface(), m_icon, m_connection.buttonSerial());
    if (m_icon && m_iconBuffer) {
        const double scale = m_drag.iconScale > 0 ? m_drag.iconScale : 1.0;
        const int width = m_drag.icon.width();
        const int height = m_drag.icon.height();
        // Kept with the surface: destroying it would drop the destination.
        m_iconViewport = wp_viewporter_get_viewport(m_connection.viewporter(), m_icon);
        wp_viewport_set_destination(m_iconViewport, std::max(1, int(std::lround(width / scale))),
                                    std::max(1, int(std::lround(height / scale))));
        // The hot spot sits under the pointer.
        wl_surface_attach(m_icon, m_iconBuffer, -int32_t(std::lround(m_drag.hotSpot.x)),
                          -int32_t(std::lround(m_drag.hotSpot.y)));
        wl_surface_damage_buffer(m_icon, 0, 0, width, height);
        wl_surface_commit(m_icon);
    }
    m_connection.flush();
    return true;
}

void DataDevice::endSource(DropAction action)
{
    if (!m_dragSource)
        return;
    if (m_toplevelDrag) {
        xdg_toplevel_drag_v1_destroy(m_toplevelDrag);
        m_toplevelDrag = nullptr;
    }
    wl_data_source_destroy(m_dragSource);
    m_dragSource = nullptr;
    if (m_iconViewport) {
        wp_viewport_destroy(m_iconViewport);
        m_iconViewport = nullptr;
    }
    if (m_icon) {
        wl_surface_destroy(m_icon);
        m_icon = nullptr;
    }
    if (m_iconBuffer) {
        wl_buffer_destroy(m_iconBuffer);
        m_iconBuffer = nullptr;
    }
    if (m_iconMemory) {
        munmap(m_iconMemory, m_iconSize);
        m_iconMemory = nullptr;
    }
    DragSource finished = std::move(m_drag);
    m_drag = DragSource();
    m_connection.flush();
    // A cancel after the release is a drop nothing took.
    if (finished.onEnded)
        finished.onEnded(action, m_dropPerformed);
    else if (finished.onFinished)
        finished.onFinished(action);
}

void* DataDevice::createSource(const std::vector<std::string>& mimeTypes)
{
    wl_data_source* source = wl_data_device_manager_create_data_source(m_manager);
    wl_data_source_add_listener(source, &DataDeviceListeners::source, this);
    for (const std::string& type : mimeTypes)
        wl_data_source_offer(source, type.c_str());
    return source;
}

void DataDevice::destroySource(void* source)
{
    wl_data_source_destroy(static_cast<wl_data_source*>(source));
}

void DataDevice::setSource(void* source)
{
    wl_data_device_set_selection(m_device, static_cast<wl_data_source*>(source), m_connection.inputSerial());
}

void DataDevice::receive(void* offer, const char* mimeType, int fd)
{
    wl_data_offer_receive(static_cast<wl_data_offer*>(offer), mimeType, fd);
}

void DataDevice::destroyOffer(void* offer)
{
    m_offerActions.erase(static_cast<wl_data_offer*>(offer));
    wl_data_offer_destroy(static_cast<wl_data_offer*>(offer));
}

void DataDevice::setSelection(const std::string& text)
{
    MimeData data;
    data.setText(text);
    set(data);
}

void DataDevice::requestSelection(std::function<void(std::string)> callback)
{
    const std::string type = textMimeType();
    if (type.empty()) {
        m_loop.post([callback = std::move(callback)] { callback({}); });
        return;
    }
    request(type, [callback = std::move(callback)](std::optional<std::string> text) {
        callback(text.value_or(std::string()));
    });
}

} // namespace cutefish::ui::wayland
