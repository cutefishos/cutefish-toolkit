// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "ui/image.h"
#include "ui/painter.h"
#include "ui/view.h"

namespace cutefish::ui {

struct MaskedLayerItem;

// A picture from a file, decoded on a thread at the device size it is shown at;
// or pixels given to it. PNG, JPEG, WebP, TIFF, HEIF, AVIF, JPEG XL, BMP, ICO
// and SVG files. Decoded pictures are shared between views through a cache.
class ImageView : public View
{
public:
    enum class FillMode {
        // The view's whole rect, the aspect ignored; the default.
        Stretch,
        // Inside the rect, centred.
        PreserveAspectFit,
        // Covering the rect, centred and cut to it.
        PreserveAspectCrop,
        // At its own size, centred.
        Pad,
    };

    enum class Status {
        Null,
        Loading,
        Ready,
        Error,
    };

    explicit ImageView(std::string path = {});
    ~ImageView() override;

    // A file; empty shows nothing. The same file again is decoded again
    // when it has been written over since.
    void setSource(std::string path);
    const std::string& source() const { return m_source; }
    // Through the freedesktop thumbnail cache, as Cutefish's
    // "image://thumbnailer/" provider: a thumbnail big enough for the source
    // size is read from ~/.cache/thumbnails, or made and stored there.
    void setThumbnail(bool thumbnail);
    // Pixels instead of a file: 0xAARRGGBB words, rows stride bytes apart,
    // devicePixelRatio of them to a logical pixel.
    void setImage(const Image& image, double devicePixelRatio = 1);
    void setPixels(int width, int height, const uint32_t* argb, size_t stride, bool premultiplied,
                   double devicePixelRatio = 1);

    void setFillMode(FillMode mode);
    FillMode fillMode() const { return m_fillMode; }
    // The size to decode at, in logical pixels; the view's own size when empty.
    // A fixed one keeps a resize from decoding again.
    void setSourceSize(SizeF size);
    // Turns the pixels as the file's EXIF orientation says; off by
    // default. Thumbnails are always turned.
    void setAutoTransform(bool autoTransform);
    // Cuts the picture to a rounded rect of the view.
    void setCornerRadius(double radius, Corners corners = Corners::Circular);
    // Off draws pixels unfiltered.
    void setSmooth(bool smooth);
    // Trilinear filtering when shrinking; used by MaskedLayer, which
    // otherwise filters bilinearly.
    void setMipmap(bool mipmap);
    // Turns the picture this many quarters clockwise as it is drawn, without
    // decoding it again; the view's rect holds the turned picture. Direct
    // rendering only.
    void setQuarterTurns(int turns);

    enum class Rendering {
        // On device pixels, cut to the corner radius with antialiasing.
        Direct,
        // Masked by a rounded rectangle of the corner radius through layers
        // (MaskedLayerItem): drawn into a layer of the view's size and placed
        // at the view's exact, unsnapped position, both filtered.
        MaskedLayer,
    };
    void setRendering(Rendering rendering);

    Status status() const { return m_status; }
    std::function<void(Status)> onStatusChanged;

    // Once ready: the picture's size in logical pixels.
    SizeF sizeHint() const override;
    // An unsnapped MaskedLayer reaches into the pixels around the view.
    RectF visualRect() const override;

    struct Picture;

protected:
    void layout() override;
    void paint(Painter& painter) override;

private:
    void reset();
    void load(double scale);
    void apply(const std::string& key, std::shared_ptr<const Picture> picture);
    void paintMaskedLayer(Painter& painter);
    MaskedLayerItem layerItem(double scale) const;
    void setStatus(Status status);

    std::string m_source;
    // The file as it was when set, so a picture written over is not
    // taken from the cache.
    std::string m_stamp;
    bool m_thumbnail = false;
    FillMode m_fillMode = FillMode::Stretch;
    SizeF m_sourceSize;
    bool m_autoTransform = false;
    double m_radius = 0;
    Corners m_corners = Corners::Circular;
    bool m_smooth = true;
    bool m_mipmap = false;
    uint8_t m_quarterTurns = 0;
    Rendering m_rendering = Rendering::Direct;
    Status m_status = Status::Null;
    std::shared_ptr<const Picture> m_picture;
    // The request the picture or the decode under way is for.
    std::string m_requested;
    struct Loading;
    std::unique_ptr<Loading> m_loading;
    // Gone with the view, so a result held for the frame skips it.
    std::shared_ptr<char> m_alive = std::make_shared<char>();
    // MaskedLayer's pixels and what they were made for.
    struct Composed;
    std::unique_ptr<Composed> m_composed;
};

} // namespace cutefish::ui
