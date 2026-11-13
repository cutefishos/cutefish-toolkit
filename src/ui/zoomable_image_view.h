// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <string>

#include "base/timer.h"
#include "ui/image_info.h"
#include "ui/scroll_view.h"

namespace cutefish::ui {

class Animation;

// An image fitted to the view, never enlarged past its own pixels until the
// user zooms. Zoom 1 is one image pixel per device pixel. Ctrl+wheel and
// pinch zoom about the pointer, a double click toggles fit and actual size,
// and a zoomed image pans by dragging and scrolling. The fitted decode is
// joined by a full-resolution one from the first zoom past fit, and decoded
// again when the view grows; an SVG is drawn again at the zoom it comes to
// rest at, and an animated image plays.
class ZoomableImageView : public ScrollView
{
public:
    ZoomableImageView();
    ~ZoomableImageView() override;

    // The file and what its header says; an empty path shows nothing.
    // `prepared` is the size in device pixels that an ImageView fitting the
    // file with its EXIF orientation applied decoded it at, so it comes from
    // the image cache; empty decodes at the view's size. `ratio` is the
    // device pixels per logical pixel the image is shown at.
    void setImage(const std::string& path, const ImageInfo& info, SizeF prepared, double ratio);
    bool isLoading() const;
    bool hasFailed() const;
    // Past the fitted zoom.
    bool isZoomed() const { return m_zoom > fitZoom() + 0.0001; }
    // Device pixels per image pixel: 1 is the actual size.
    double zoom() const { return m_zoom; }
    // The point of the image, in its pixels, under a point of the view.
    PointF imagePoint(PointF at) const;
    // Turns the image as shown this many quarters clockwise, fitted again,
    // without decoding it again; it then shows its fitted layer alone, zoomed
    // or not, until setImage() turns it back.
    void setQuarterTurns(int turns);
    int quarterTurns() const { return m_quarterTurns; }

    // Shows the image at this zoom with the content at this scroll position,
    // as the user left another view of the same picture.
    void setZoomAndPosition(double zoom, PointF position);
    void zoomBy(double factor, PointF at, bool animate);
    void zoomTo(double target, PointF at, bool animate);
    void zoomToFit() { zoomTo(fitZoom(), center(), true); }
    void zoomToActualSize() { zoomTo(1, center(), true); }
    // The middle of the area below the top inset.
    PointF center() const { return {geometry().width / 2, m_topInset + (geometry().height - m_topInset) / 2}; }

    // The top this far down lies under a floating header: the image is
    // fitted and centred below it, and a zoomed image pans under it.
    void setTopInset(double inset);
    double topInset() const { return m_topInset; }

    std::function<void()> onStatusChanged;
    // A right press, at its position in the window, instead of a pan.
    std::function<void(PointF windowPosition)> onContextMenuRequested;

protected:
    void layout() override;
    bool scroll(const ScrollEvent& event) override;
    bool gesture(const GestureEvent& event) override;
    void hoverMove(const PointerEvent& event) override;

private:
    class Canvas;

    double naturalWidth() const;
    double naturalHeight() const;
    double fitZoom() const;
    double maxZoom() const { return std::max(fitZoom(), 8.0); }
    double frameX(double zoom) const;
    double frameY(double zoom) const;
    SizeF upright(SizeF frame) const;
    double snap(double value) const;
    void setZoom(double zoom);
    void setPan(double x, double y);
    void resetZoom();
    void toggleActualSize(PointF at);
    void startDetail();

    Canvas* m_canvas;
    std::string m_path;
    ImageInfo m_info;
    double m_ratio = 1;
    int m_quarterTurns = 0;
    double m_zoom = 1;
    bool m_fitted = true;
    bool m_anchored = false;
    PointF m_anchorView;
    PointF m_anchorImage;
    PointF m_pointer;
    double m_animationFrom = 1;
    double m_animationTo = 1;
    std::unique_ptr<Animation> m_animation;
    base::Timer m_vectorTimer;
    // The size the fitted layer decodes at.
    SizeF m_fitSource;
    base::Timer m_refitTimer;
    SizeF m_lastSize;
    double m_pinchScale = 1;
    double m_topInset = 0;
};

} // namespace cutefish::ui
