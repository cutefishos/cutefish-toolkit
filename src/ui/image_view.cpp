// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/image_view.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <list>
#include <unordered_map>
#include <utility>

#include <sys/stat.h>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"

#include "base/event_loop.h"
#include "base/thread_pool.h"
#include "ui/image_decoder.h"
#include "ui/masked_layer.h"
#include "ui/thumbnail_cache.h"
#include "ui/window.h"

namespace cutefish::ui {

struct ImageView::Picture
{
    sk_sp<SkImage> image;
    // The picture's own size in device pixels at the ratio below; its
    // aspect is the one drawn.
    double width = 0;
    double height = 0;
    double devicePixelRatio = 1;
    // MaskedLayer's textures, made with the picture off the UI thread,
    // for the item described by the key.
    std::string layerKey;
    MaskedLayerTextures layer;
};

// Pictures that finish within a refresh of one another are shown together,
// in one frame; one held back waits for the others at most that long.
static constexpr std::chrono::milliseconds kDeliveryWindow(16);

namespace {

struct Arrival
{
    std::weak_ptr<char> alive;
    ImageView* view;
    std::string key;
    std::shared_ptr<const ImageView::Picture> picture;
};

struct Deliveries
{
    int running = 0;
    std::vector<Arrival> arrived;
    int timer = 0;
};

} // namespace

static Deliveries& deliveries()
{
    static Deliveries deliveries;
    return deliveries;
}

struct ImageView::Composed
{
    std::string key;
    MaskedLayerTextures textures;
};

struct ImageView::Loading
{
    base::Job job;
    std::shared_ptr<std::atomic<bool>> abandoned = std::make_shared<std::atomic<bool>>(false);
    bool running = true;

    Loading() { ++deliveries().running; }
    ~Loading()
    {
        abandoned->store(true);
        if (!running)
            return;
        // A view gone or loading something else no longer holds the others back.
        base::EventLoop* loop = base::EventLoop::current();
        if (--deliveries().running == 0 && !deliveries().arrived.empty() && loop)
            loop->post([] { deliver(); });
    }

    static void arrive(ImageView* view, std::string key, std::shared_ptr<const Picture> picture)
    {
        Deliveries& d = deliveries();
        view->m_loading->running = false;
        --d.running;
        view->m_loading.reset();
        d.arrived.push_back({view->m_alive, view, std::move(key), std::move(picture)});
        if (d.running == 0) {
            deliver();
        } else if (!d.timer) {
            d.timer = base::EventLoop::current()->startTimer(kDeliveryWindow, false, [] {
                deliveries().timer = 0;
                deliver();
            });
        }
    }

    static void deliver()
    {
        Deliveries& d = deliveries();
        if (d.timer) {
            base::EventLoop::current()->stopTimer(d.timer);
            d.timer = 0;
        }
        const std::vector<Arrival> arrived = std::move(d.arrived);
        d.arrived.clear();
        for (const Arrival& arrival : arrived) {
            if (arrival.alive.lock())
                arrival.view->apply(arrival.key, arrival.picture);
        }
    }
};

// Decoded pictures by request, least recently used first, within a budget.
static constexpr size_t kCacheBytes = size_t(64) * 1024 * 1024;

namespace {

struct Cache
{
    std::list<std::pair<std::string, std::shared_ptr<const ImageView::Picture>>> entries;
    std::unordered_map<std::string, decltype(entries)::iterator> index;
    size_t bytes = 0;
};

} // namespace

static Cache& cache()
{
    static Cache cache;
    return cache;
}

static size_t bytesOf(const SkImage* image)
{
    return image ? size_t(image->width()) * size_t(image->height()) * 4 : 0;
}

static size_t bytesOf(const ImageView::Picture& picture)
{
    return bytesOf(picture.image.get()) + bytesOf(picture.layer.layer.get()) + bytesOf(picture.layer.mask.get());
}

static std::shared_ptr<const ImageView::Picture> cached(const std::string& key)
{
    Cache& c = cache();
    const auto it = c.index.find(key);
    if (it == c.index.end())
        return nullptr;
    c.entries.splice(c.entries.end(), c.entries, it->second);
    return it->second->second;
}

static void store(const std::string& key, std::shared_ptr<const ImageView::Picture> picture)
{
    Cache& c = cache();
    if (c.index.count(key))
        return;
    c.bytes += bytesOf(*picture);
    c.entries.emplace_back(key, std::move(picture));
    c.index[key] = std::prev(c.entries.end());
    while (c.bytes > kCacheBytes && c.entries.size() > 1) {
        c.bytes -= bytesOf(*c.entries.front().second);
        c.index.erase(c.entries.front().first);
        c.entries.pop_front();
    }
}

// Changes when the file is written over or replaced under the same name.
static std::string fileStamp(const std::string& path)
{
    struct stat info;
    if (path.empty() || stat(path.c_str(), &info) != 0)
        return {};
    return std::to_string(info.st_ino) + ":" + std::to_string(info.st_size) + ":"
            + std::to_string(info.st_mtim.tv_sec) + "." + std::to_string(info.st_mtim.tv_nsec);
}

static base::ThreadPool& decodePool()
{
    // Two threads: scrolling a grid queues many.
    static base::ThreadPool pool(2);
    return pool;
}

ImageView::ImageView(std::string path)
{
    setSource(std::move(path));
}

ImageView::~ImageView() = default;

void ImageView::reset()
{
    m_loading.reset();
    m_picture.reset();
    m_requested.clear();
}

void ImageView::setSource(std::string path)
{
    std::string stamp = fileStamp(path);
    if (path == m_source && m_status != Status::Null) {
        // Written over: decoded again, the picture it has showing meanwhile.
        if (stamp != m_stamp) {
            m_stamp = std::move(stamp);
            m_loading.reset();
            m_requested.clear();
            requestLayout();
        }
        return;
    }
    reset();
    m_source = std::move(path);
    m_stamp = std::move(stamp);
    setStatus(m_source.empty() ? Status::Null : Status::Loading);
    requestLayout();
}

void ImageView::setThumbnail(bool thumbnail)
{
    if (thumbnail == m_thumbnail)
        return;
    m_thumbnail = thumbnail;
    m_requested.clear();
    requestLayout();
}

void ImageView::setImage(const Image& image, double devicePixelRatio)
{
    setPixels(image.width(), image.height(), image.bits(), size_t(image.width()) * 4, false, devicePixelRatio);
}

void ImageView::setPixels(int width, int height, const uint32_t* argb, size_t stride, bool premultiplied,
                          double devicePixelRatio)
{
    reset();
    m_source.clear();
    if (width <= 0 || height <= 0 || !argb) {
        setStatus(Status::Null);
        requestLayout();
        return;
    }
    // 0xAARRGGBB words are BGRA bytes on a little-endian machine.
    const SkImageInfo source = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType,
                                                 premultiplied ? kPremul_SkAlphaType : kUnpremul_SkAlphaType);
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(SkImageInfo::MakeN32Premul(width, height))
        || !bitmap.writePixels(SkPixmap(source, argb, stride))) {
        setStatus(Status::Error);
        return;
    }
    bitmap.setImmutable();
    auto picture = std::make_shared<Picture>();
    picture->image = bitmap.asImage();
    picture->width = width;
    picture->height = height;
    picture->devicePixelRatio = devicePixelRatio > 0 ? devicePixelRatio : 1;
    m_picture = std::move(picture);
    setStatus(Status::Ready);
    requestLayout();
}

void ImageView::setFillMode(FillMode mode)
{
    if (mode == m_fillMode)
        return;
    m_fillMode = mode;
    requestLayout();
}

void ImageView::setSourceSize(SizeF size)
{
    if (size == m_sourceSize)
        return;
    m_sourceSize = size;
    requestLayout();
}

void ImageView::setAutoTransform(bool autoTransform)
{
    if (autoTransform == m_autoTransform)
        return;
    m_autoTransform = autoTransform;
    requestLayout();
}

void ImageView::setCornerRadius(double radius, Corners corners)
{
    m_radius = radius;
    m_corners = corners;
    requestPaint();
}

void ImageView::setSmooth(bool smooth)
{
    m_smooth = smooth;
    requestPaint();
}

void ImageView::setMipmap(bool mipmap)
{
    m_mipmap = mipmap;
    requestPaint();
}

void ImageView::setRendering(Rendering rendering)
{
    m_rendering = rendering;
    requestPaint();
}

void ImageView::setStatus(Status status)
{
    if (status == m_status)
        return;
    m_status = status;
    if (onStatusChanged)
        onStatusChanged(status);
}

SizeF ImageView::sizeHint() const
{
    if (!m_picture)
        return {};
    return {m_picture->width / m_picture->devicePixelRatio, m_picture->height / m_picture->devicePixelRatio};
}

RectF ImageView::visualRect() const
{
    return m_rendering == Rendering::MaskedLayer ? localRect().adjusted(-1, -1, 1, 1) : localRect();
}

void ImageView::layout()
{
    if (Window* window = this->window())
        load(window->scale());
}

static std::string layerKey(const MaskedLayerItem& item);

// Starts decoding for the size the view now needs, unless that is what it has
// or is getting; meanwhile the picture it has stays.
void ImageView::load(double scale)
{
    if (m_source.empty())
        return;
    int width = 0;
    int height = 0;
    ImageScaling scaling = ImageScaling::Fit;
    if (m_fillMode != FillMode::Pad) {
        const SizeF size = m_sourceSize.width > 0 && m_sourceSize.height > 0 ? m_sourceSize : geometry().size();
        width = int(std::lround(size.width * scale));
        height = int(std::lround(size.height * scale));
        if (width <= 0 || height <= 0)
            return;
        if (m_fillMode == FillMode::PreserveAspectCrop)
            scaling = ImageScaling::Cover;
        // A source size bounds the picture; without one it is drawn
        // at the view's exact size.
        else if (m_fillMode == FillMode::Stretch && !(m_sourceSize.width > 0 && m_sourceSize.height > 0))
            scaling = ImageScaling::Stretch;
    }

    const std::string key = std::string(m_thumbnail ? "thumbnail:" : "file:") + m_source + "|" + m_stamp + "|"
            + std::to_string(width) + "x" + std::to_string(height) + "|" + std::to_string(int(scaling))
            + (m_autoTransform ? "|turned" : "");
    if (key == m_requested)
        return;
    m_requested = key;
    if (std::shared_ptr<const Picture> picture = cached(key)) {
        m_loading.reset();
        m_picture = std::move(picture);
        setStatus(Status::Ready);
        requestLayout();
        return;
    }

    ImageRequest request;
    request.path = m_source;
    request.width = width;
    request.height = height;
    request.scaling = scaling;
    request.autoTransform = m_autoTransform;
    const bool thumbnail = m_thumbnail;
    const double ratio = m_sourceSize.width > 0 && m_sourceSize.height > 0 ? scale : 1;
    const bool layered = m_rendering == Rendering::MaskedLayer && m_fillMode != FillMode::Pad
                         && geometry().width > 0 && geometry().height > 0;
    const MaskedLayerItem item = layerItem(scale);
    m_loading = std::make_unique<Loading>();
    std::shared_ptr<std::atomic<bool>> abandoned = m_loading->abandoned;
    m_loading->job = decodePool().run(
            [request, thumbnail, ratio, abandoned, layered, item]() -> std::shared_ptr<const Picture> {
                // Scrolled away before its turn came.
                if (abandoned->load())
                    return nullptr;
                auto picture = std::make_shared<Picture>();
                if (thumbnail) {
                    picture->image = thumbnails::request(request.path, request.width, request.height);
                    if (picture->image) {
                        picture->width = picture->image->width();
                        picture->height = picture->image->height();
                    }
                    picture->devicePixelRatio = ratio;
                } else {
                    const DecodedImage decoded = decodeImage(request);
                    picture->image = decoded.image;
                    // Without a source size the picture keeps its own size in
                    // logical pixels.
                    picture->width = ratio == 1 ? decoded.originalWidth : (decoded.image ? decoded.image->width() : 0);
                    picture->height = ratio == 1 ? decoded.originalHeight : (decoded.image ? decoded.image->height() : 0);
                    picture->devicePixelRatio = ratio;
                }
                if (picture->image && layered) {
                    picture->layer = renderMaskedLayerTextures(*picture->image, item);
                    picture->layerKey = layerKey(item);
                }
                return picture->image ? picture : nullptr;
            },
            [this, key](std::shared_ptr<const Picture> picture) { Loading::arrive(this, key, std::move(picture)); });
}

void ImageView::apply(const std::string& key, std::shared_ptr<const Picture> picture)
{
    // Loading something else since.
    if (key != m_requested)
        return;
    if (!picture) {
        m_picture.reset();
        setStatus(Status::Error);
        requestLayout();
        return;
    }
    store(key, picture);
    m_picture = std::move(picture);
    setStatus(Status::Ready);
    requestLayout();
}

void ImageView::paint(Painter& painter)
{
    if (!m_picture || !m_picture->image)
        return;
    if (m_rendering == Rendering::MaskedLayer && m_fillMode != FillMode::Pad) {
        paintMaskedLayer(painter);
        return;
    }
    const SkImage* image = m_picture->image.get();
    const Rect box = painter.deviceRect(localRect());
    if (box.width <= 0 || box.height <= 0)
        return;

    // The picture's aspect, from its own size: a decoded copy may be rounded.
    const double aspectWidth = m_picture->width > 0 ? m_picture->width : image->width();
    const double aspectHeight = m_picture->height > 0 ? m_picture->height : image->height();
    double width = box.width;
    double height = box.height;
    switch (m_fillMode) {
    case FillMode::Stretch:
        break;
    case FillMode::PreserveAspectFit:
    case FillMode::PreserveAspectCrop: {
        const double fit = std::min(box.width / aspectWidth, box.height / aspectHeight);
        const double cover = std::max(box.width / aspectWidth, box.height / aspectHeight);
        const double factor = m_fillMode == FillMode::PreserveAspectFit ? fit : cover;
        width = std::round(aspectWidth * factor);
        height = std::round(aspectHeight * factor);
        break;
    }
    case FillMode::Pad:
        width = std::round(aspectWidth / m_picture->devicePixelRatio * painter.scale());
        height = std::round(aspectHeight / m_picture->devicePixelRatio * painter.scale());
        break;
    }
    // Centred on whole device pixels.
    const double x = box.x + std::floor((box.width - width) / 2);
    const double y = box.y + std::floor((box.height - height) / 2);
    const SkRect target = SkRect::MakeXYWH(SkScalar(x), SkScalar(y), SkScalar(width), SkScalar(height));

    painter.save();
    if (m_radius > 0)
        painter.clipRoundedRect(localRect(), m_radius, m_corners);
    else
        painter.clip(localRect());
    SkPaint paint;
    paint.setAlphaf(float(painter.opacity()));
    const bool shrinks = width < image->width() || height < image->height();
    const SkSamplingOptions sampling = !m_smooth ? SkSamplingOptions(SkFilterMode::kNearest)
                                       : shrinks ? SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear)
                                                 : SkSamplingOptions(SkFilterMode::kLinear);
    painter.canvas()->drawImageRect(m_picture->image, target, sampling, &paint);
    painter.restore();
}

MaskedLayerItem ImageView::layerItem(double scale) const
{
    MaskedLayerItem item;
    item.width = geometry().width;
    item.height = geometry().height;
    item.scale = scale;
    item.fillMode = m_fillMode == FillMode::PreserveAspectFit ? MaskedLayerItem::FillMode::PreserveAspectFit
                    : m_fillMode == FillMode::PreserveAspectCrop ? MaskedLayerItem::FillMode::PreserveAspectCrop
                                                                 : MaskedLayerItem::FillMode::Stretch;
    item.smooth = m_smooth;
    item.mipmap = m_mipmap;
    item.radius = m_radius;
    item.continuousCorners = m_corners == Corners::Continuous;
    return item;
}

static std::string layerKey(const MaskedLayerItem& item)
{
    char key[160];
    std::snprintf(key, sizeof(key), "%.6f %.6f %.4f %d %d %d %.4f %d", item.width, item.height, item.scale,
                  int(item.fillMode), item.smooth, item.mipmap, item.radius, item.continuousCorners);
    return key;
}

// The textures come with the picture, or are made here when the size or the
// scale changed since; moving the view only draws them elsewhere.
void ImageView::paintMaskedLayer(Painter& painter)
{
    const MaskedLayerItem item = layerItem(painter.scale());
    const std::string key = layerKey(item);
    const MaskedLayerTextures* textures = nullptr;
    if (m_picture->layerKey == key) {
        textures = &m_picture->layer;
    } else {
        char picture[32];
        std::snprintf(picture, sizeof(picture), "%p ", static_cast<const void*>(m_picture->image.get()));
        const std::string composedKey = picture + key;
        if (!m_composed || m_composed->key != composedKey)
            m_composed = std::make_unique<Composed>(Composed{composedKey, renderMaskedLayerTextures(*m_picture->image, item)});
        textures = &m_composed->textures;
    }
    drawMaskedLayer(painter.canvas(), *textures, painter.offset().x, painter.offset().y, float(painter.opacity()));
}

} // namespace cutefish::ui
