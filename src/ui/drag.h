// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ui/bitmap.h"
#include "ui/geometry.h"

namespace cutefish::ui {

class Window;

// wl_data_device_manager's dnd_action values, so they pass through as they are.
enum DropAction : uint32_t {
    NoAction = 0,
    CopyAction = 1,
    MoveAction = 2,
    AskAction = 4,
};

// What View::dragUpdate() answers when the target takes any of `accepted`:
// the compositor picks one by the keys held, `preferred` when none is, and
// the target hears its pick through DropOffer::action().
constexpr uint32_t dropActions(uint32_t accepted, DropAction preferred)
{
    return accepted << 8 | preferred;
}

// The parts of a dragUpdate() answer; a single action is both.
constexpr uint32_t acceptedDropActions(uint32_t answer)
{
    return answer >> 8 | (answer & 0xff);
}

constexpr DropAction preferredDropAction(uint32_t answer)
{
    return DropAction(answer & 0xff);
}

// The action a drop with that answer takes: the compositor's pick when the
// target accepts it, else the preferred one.
constexpr DropAction takenDropAction(uint32_t answer, uint32_t picked)
{
    return (picked & acceptedDropActions(answer)) ? DropAction(picked) : preferredDropAction(answer);
}

// What a view hands out when it starts a drag: the data in each MIME type,
// made when a target asks for it, and the icon under the pointer.
struct DragSource
{
    std::vector<std::string> mimeTypes;
    // The bytes in one of mimeTypes; called on the UI thread, possibly
    // several times, until onFinished.
    std::function<std::string(const std::string& mimeType)> data;
    // The actions the source allows, a mask of DropAction.
    uint32_t actions = CopyAction;
    // Drawn at the pointer, its hot spot under it; null for none. Device
    // pixels at iconScale; the hot spot is in logical pixels of the icon.
    Bitmap icon;
    double iconScale = 1;
    PointF hotSpot;
    // Once, with the action the target took, or NoAction when the drag was
    // cancelled or nothing took it.
    std::function<void(DropAction action)> onFinished;
    // In place of onFinished when set: also whether the pointer was let go
    // (`dropped`) rather than the drag cancelled, as Escape does; a window
    // drag dropped where nothing takes it is dropped with NoAction.
    std::function<void(DropAction action, bool dropped)> onEnded;
    // A toplevel of the application the compositor moves with the pointer
    // during the drag (xdg_toplevel_drag_v1), as for a tab pulled out into
    // a new window; `attachedOffset` is where the pointer sits in it, in its
    // logical coordinates. Null for none.
    Window* attachedWindow = nullptr;
    PointF attachedOffset;
    // Whatever the application wants a target in its own process to see
    // through DropOffer::localSource().
    const void* context = nullptr;
};

// A drag over a window, as the host offers it: wl_data_offer's view.
class DropOffer
{
public:
    virtual ~DropOffer() = default;

    virtual const std::vector<std::string>& mimeTypes() const = 0;
    // The actions the source allows, a mask of DropAction.
    virtual uint32_t sourceActions() const = 0;
    // Reads the data in one of mimeTypes(); `done` runs later on the UI
    // thread, with nothing when the source failed or went. Readable from the
    // first enter until the drop has been handled.
    virtual void read(const std::string& mimeType, std::function<void(std::string data)> done) = 0;

    // The drag's source when this process started it, for a target in
    // another of its windows (or the same one); null for other clients'.
    virtual const DragSource* localSource() const { return nullptr; }

    // The action the compositor picked out of those the target accepts and
    // the source allows; NoAction before it has.
    virtual uint32_t action() const { return NoAction; }

    bool hasMimeType(const std::string& mimeType) const
    {
        return std::find(mimeTypes().begin(), mimeTypes().end(), mimeType) != mimeTypes().end();
    }
};

struct DragEvent
{
    // In the receiving view's coordinates.
    PointF position;
    DropOffer* offer = nullptr;
    uint32_t modifiers = 0;
};

} // namespace cutefish::ui
