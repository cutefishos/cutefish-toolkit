// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/image_decoder.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include <jxl/decode.h>
#include <jxl/resizable_parallel_runner.h>
#include <libheif/heif.h>
#include <tiffio.h>
#include <webp/decode.h>

#include "include/codec/SkBmpDecoder.h"
#include "include/codec/SkCodec.h"
#include "include/codec/SkEncodedOrigin.h"
#include "include/codec/SkIcoDecoder.h"
#include "include/codec/SkJpegDecoder.h"
#include "include/codec/SkPngDecoder.h"
#include "include/codec/SkWbmpDecoder.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "modules/svg/include/SkSVGDOM.h"
#include "modules/svg/include/SkSVGRenderContext.h"
#include "modules/svg/include/SkSVGSVG.h"

#include "base/file.h"
#include "ui/color_profile.h"
#include "ui/icon_loader.h"
#include "ui/image_formats.h"

namespace cutefish::ui {

// The allocation limit, 256 MiB.
static constexpr int64_t kAllocationLimit = int64_t(256) * 1024 * 1024;

namespace {

// Pixels as a decoder left them, before scaling and turning.
struct Source
{
    sk_sp<SkImage> image;
    // The file's full size, before turning; the image may be smaller.
    int width = 0;
    int height = 0;
    SkEncodedOrigin origin = kTopLeft_SkEncodedOrigin;
};

} // namespace

void scaledSize(int width, int height, int boxWidth, int boxHeight, ImageScaling scaling, int* outWidth,
                int* outHeight)
{
    if (scaling == ImageScaling::Stretch || width <= 0 || height <= 0) {
        *outWidth = boxWidth;
        *outHeight = boxHeight;
        return;
    }
    const int64_t rw = int64_t(boxHeight) * width / height;
    const bool useHeight = scaling == ImageScaling::Fit ? rw <= boxWidth : rw >= boxWidth;
    if (useHeight) {
        *outWidth = int(rw);
        *outHeight = boxHeight;
    } else {
        *outWidth = boxWidth;
        *outHeight = int(int64_t(boxWidth) * height / width);
    }
}

static bool withinLimit(int64_t width, int64_t height)
{
    return width > 0 && height > 0 && width * height * 4 <= kAllocationLimit;
}

static sk_sp<SkImage> imageFromRgba(const uint8_t* pixels, int width, int height, size_t stride, bool alpha)
{
    SkBitmap bitmap;
    const SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType,
                                               alpha ? kUnpremul_SkAlphaType : kOpaque_SkAlphaType);
    // Copied into premultiplied native pixels, which draw fastest.
    if (!bitmap.tryAllocPixels(SkImageInfo::MakeN32(width, height, alpha ? kPremul_SkAlphaType : kOpaque_SkAlphaType)))
        return nullptr;
    if (!bitmap.writePixels(SkPixmap(info, pixels, stride)))
        return nullptr;
    bitmap.setImmutable();
    return bitmap.asImage();
}

static Source decodeWithSkia(sk_sp<SkData> data, int boxWidth, int boxHeight)
{
    static const SkCodecs::Decoder decoders[] = {SkPngDecoder::Decoder(), SkJpegDecoder::Decoder(),
                                                 SkBmpDecoder::Decoder(), SkIcoDecoder::Decoder(),
                                                 SkWbmpDecoder::Decoder()};
    std::unique_ptr<SkCodec> codec = SkCodec::MakeFromData(std::move(data), decoders);
    if (!codec)
        return {};
    Source source;
    source.width = codec->dimensions().width();
    source.height = codec->dimensions().height();
    source.origin = codec->getOrigin();
    if (!withinLimit(source.width, source.height))
        return {};

    // JPEG decodes at an eighth to the whole cheaply; take the smallest that
    // still covers the box.
    SkISize size = codec->dimensions();
    if (boxWidth > 0 && boxHeight > 0) {
        const bool swaps = SkEncodedOriginSwapsWidthHeight(source.origin);
        const int needWidth = swaps ? boxHeight : boxWidth;
        const int needHeight = swaps ? boxWidth : boxHeight;
        const float scale = std::max(float(needWidth) / float(source.width), float(needHeight) / float(source.height));
        if (scale < 1) {
            const SkISize scaled = codec->getScaledDimensions(scale);
            if (scaled.width() >= needWidth && scaled.height() >= needHeight)
                size = scaled;
        }
    }
    const SkImageInfo info = codec->getInfo().makeDimensions(size).makeColorType(kN32_SkColorType)
            .makeAlphaType(codec->getInfo().isOpaque() ? kOpaque_SkAlphaType : kPremul_SkAlphaType)
            .makeColorSpace(nullptr);
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(info))
        return {};
    // An sRGB destination makes the codec convert from the file's profile
    // unless that is sRGB or missing; the pixels stay untagged, as
    // everything drawn is.
    const SkCodec::Result result = codec->getPixels(info.makeColorSpace(SkColorSpace::MakeSRGB()), bitmap.getPixels(),
                                                    bitmap.rowBytes());
    if (result != SkCodec::kSuccess && result != SkCodec::kIncompleteInput && result != SkCodec::kErrorInInput)
        return {};
    bitmap.setImmutable();
    source.image = bitmap.asImage();
    return source;
}

static Source decodeWebp(const std::string& data)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
    int width = 0;
    int height = 0;
    if (!WebPGetInfo(bytes, data.size(), &width, &height) || !withinLimit(width, height))
        return {};
    WebPBitstreamFeatures features;
    if (WebPGetFeatures(bytes, data.size(), &features) != VP8_STATUS_OK)
        return {};
    std::vector<uint8_t> pixels(size_t(width) * size_t(height) * 4);
    if (!WebPDecodeRGBAInto(bytes, data.size(), pixels.data(), pixels.size(), width * 4))
        return {};
    if (const std::unique_ptr<SourceProfile> profile = SourceProfile::fromWebp(data))
        profile->toSrgb(pixels.data(), width, height, size_t(width) * 4, false, false);
    Source source;
    source.width = width;
    source.height = height;
    source.image = imageFromRgba(pixels.data(), width, height, size_t(width) * 4, features.has_alpha);
    return source;
}

// The file's bytes as libtiff reads a file.
struct TiffMemory
{
    std::string_view data;
    toff_t offset = 0;
};

static tmsize_t tiffRead(thandle_t handle, void* buffer, tmsize_t size)
{
    auto* memory = static_cast<TiffMemory*>(handle);
    if (size < 0 || memory->offset >= memory->data.size())
        return 0;
    const size_t count = std::min<size_t>(size_t(size), memory->data.size() - memory->offset);
    std::memcpy(buffer, memory->data.data() + memory->offset, count);
    memory->offset += count;
    return tmsize_t(count);
}

static tmsize_t tiffWrite(thandle_t, void*, tmsize_t)
{
    return 0;
}

static toff_t tiffSeek(thandle_t handle, toff_t offset, int whence)
{
    auto* memory = static_cast<TiffMemory*>(handle);
    const toff_t base = whence == SEEK_CUR ? memory->offset : whence == SEEK_END ? memory->data.size() : 0;
    memory->offset = base + offset;
    return memory->offset;
}

static int tiffClose(thandle_t)
{
    return 0;
}

static toff_t tiffSize(thandle_t handle)
{
    return static_cast<TiffMemory*>(handle)->data.size();
}

static Source decodeTiff(const std::string& data)
{
    // Unknown tags and the like are not worth a line on stderr.
    TIFFSetWarningHandler(nullptr);
    TIFFSetErrorHandler(nullptr);
    TiffMemory memory{data};
    std::unique_ptr<TIFF, decltype(&TIFFClose)> tiff(
            TIFFClientOpen("image", "rm", &memory, tiffRead, tiffWrite, tiffSeek, tiffClose, tiffSize, nullptr,
                           nullptr),
            TIFFClose);
    if (!tiff)
        return {};
    uint32_t width = 0;
    uint32_t height = 0;
    TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &height);
    if (!withinLimit(width, height))
        return {};
    uint16_t samples = 0;
    uint16_t extra = 0;
    uint16_t* extraTypes = nullptr;
    TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &samples);
    TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_EXTRASAMPLES, &extra, &extraTypes);
    std::vector<uint32_t> raster(size_t(width) * size_t(height));
    if (!TIFFReadRGBAImageOriented(tiff.get(), width, height, raster.data(), ORIENTATION_TOPLEFT, 0))
        return {};
    // ABGR words with red in the low byte: RGBA bytes on a little-endian machine,
    // premultiplied when the file's extra sample is associated alpha.
    const bool alpha = extra > 0;
    Source source;
    source.width = int(width);
    source.height = int(height);
    const bool associated = alpha && extraTypes && extraTypes[0] == EXTRASAMPLE_ASSOCALPHA;
    uint32_t iccSize = 0;
    void* icc = nullptr;
    if (TIFFGetField(tiff.get(), TIFFTAG_ICCPROFILE, &iccSize, &icc)) {
        if (const std::unique_ptr<SourceProfile> profile = SourceProfile::fromIcc(icc, iccSize))
            profile->toSrgb(reinterpret_cast<uint8_t*>(raster.data()), int(width), int(height), size_t(width) * 4,
                            false, associated);
    }
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(SkImageInfo::MakeN32(int(width), int(height),
                                                    alpha ? kPremul_SkAlphaType : kOpaque_SkAlphaType)))
        return {};
    const SkImageInfo info = SkImageInfo::Make(int(width), int(height), kRGBA_8888_SkColorType,
                                               !alpha ? kOpaque_SkAlphaType
                                                      : associated ? kPremul_SkAlphaType : kUnpremul_SkAlphaType);
    if (!bitmap.writePixels(SkPixmap(info, raster.data(), size_t(width) * 4)))
        return {};
    bitmap.setImmutable();
    source.image = bitmap.asImage();
    return source;
}

static std::unique_ptr<SourceProfile> heifProfile(const heif_image_handle* handle)
{
    const heif_color_profile_type type = heif_image_handle_get_color_profile_type(handle);
    if (type == heif_color_profile_type_prof || type == heif_color_profile_type_rICC) {
        std::vector<uint8_t> icc(heif_image_handle_get_raw_color_profile_size(handle));
        if (icc.empty() || heif_image_handle_get_raw_color_profile(handle, icc.data()).code != heif_error_Ok)
            return nullptr;
        return SourceProfile::fromIcc(icc.data(), icc.size());
    }
    heif_color_profile_nclx* rawNclx = nullptr;
    if (heif_image_handle_get_nclx_color_profile(handle, &rawNclx).code != heif_error_Ok || !rawNclx)
        return nullptr;
    std::unique_ptr<heif_color_profile_nclx, decltype(&heif_nclx_color_profile_free)> nclx(
            rawNclx, heif_nclx_color_profile_free);
    if (nclx->color_primaries == heif_color_primaries_ITU_R_BT_709_5
        || nclx->color_primaries == heif_color_primaries_unspecified)
        return nullptr;
    const float primaries[8] = {nclx->color_primary_red_x,   nclx->color_primary_red_y,
                                nclx->color_primary_green_x, nclx->color_primary_green_y,
                                nclx->color_primary_blue_x,  nclx->color_primary_blue_y,
                                nclx->color_primary_white_x, nclx->color_primary_white_y};
    return SourceProfile::fromCicp(nclx->transfer_characteristics, primaries);
}

static Source decodeHeif(const std::string& data)
{
    std::unique_ptr<heif_context, decltype(&heif_context_free)> context(heif_context_alloc(), heif_context_free);
    if (!context)
        return {};
    heif_error error = heif_context_read_from_memory_without_copy(context.get(), data.data(), data.size(), nullptr);
    if (error.code != heif_error_Ok)
        return {};
    heif_image_handle* rawHandle = nullptr;
    error = heif_context_get_primary_image_handle(context.get(), &rawHandle);
    std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> handle(rawHandle,
                                                                                    heif_image_handle_release);
    if (error.code != heif_error_Ok || !handle)
        return {};
    if (!withinLimit(heif_image_handle_get_width(handle.get()), heif_image_handle_get_height(handle.get())))
        return {};
    // The decoder turns and mirrors the pixels as the file says.
    const bool alpha = heif_image_handle_has_alpha_channel(handle.get());
    heif_image* rawImage = nullptr;
    error = heif_decode_image(handle.get(), &rawImage, heif_colorspace_RGB,
                              alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB, nullptr);
    std::unique_ptr<heif_image, decltype(&heif_image_release)> image(rawImage, heif_image_release);
    if (error.code != heif_error_Ok || !image)
        return {};
    const int width = heif_image_get_width(image.get(), heif_channel_interleaved);
    const int height = heif_image_get_height(image.get(), heif_channel_interleaved);
    int stride = 0;
    uint8_t* pixels = heif_image_get_plane(image.get(), heif_channel_interleaved, &stride);
    if (!pixels || width <= 0 || height <= 0)
        return {};
    const std::unique_ptr<SourceProfile> profile = heifProfile(handle.get());
    Source source;
    source.width = width;
    source.height = height;
    if (alpha) {
        if (profile)
            profile->toSrgb(pixels, width, height, size_t(stride), false, false);
        source.image = imageFromRgba(pixels, width, height, size_t(stride), true);
    } else {
        std::vector<uint8_t> rgba(size_t(width) * size_t(height) * 4);
        for (int y = 0; y < height; ++y) {
            const uint8_t* in = pixels + size_t(y) * size_t(stride);
            uint8_t* out = rgba.data() + size_t(y) * size_t(width) * 4;
            for (int x = 0; x < width; ++x, in += 3, out += 4) {
                out[0] = in[0];
                out[1] = in[1];
                out[2] = in[2];
                out[3] = 255;
            }
        }
        if (profile)
            profile->toSrgb(rgba.data(), width, height, size_t(width) * 4, false, false);
        source.image = imageFromRgba(rgba.data(), width, height, size_t(width) * 4, false);
    }
    return source;
}

static Source decodeJxl(const std::string& data)
{
    std::unique_ptr<JxlDecoder, decltype(&JxlDecoderDestroy)> decoder(JxlDecoderCreate(nullptr), JxlDecoderDestroy);
    std::unique_ptr<void, decltype(&JxlResizableParallelRunnerDestroy)> runner(
            JxlResizableParallelRunnerCreate(nullptr), JxlResizableParallelRunnerDestroy);
    if (!decoder || !runner
        || JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE)
                   != JXL_DEC_SUCCESS
        || JxlDecoderSetParallelRunner(decoder.get(), JxlResizableParallelRunner, runner.get()) != JXL_DEC_SUCCESS
        || JxlDecoderSetUnpremultiplyAlpha(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS)
        return {};
    JxlDecoderSetInput(decoder.get(), reinterpret_cast<const uint8_t*>(data.data()), data.size());
    JxlDecoderCloseInput(decoder.get());
    const JxlPixelFormat format{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    bool alpha = false;
    std::unique_ptr<SourceProfile> profile;
    while (true) {
        const JxlDecoderStatus status = JxlDecoderProcessInput(decoder.get());
        if (status == JXL_DEC_BASIC_INFO) {
            JxlBasicInfo info;
            if (JxlDecoderGetBasicInfo(decoder.get(), &info) != JXL_DEC_SUCCESS)
                return {};
            // The decoder turns the pixels; orientations 5 to 8 swap the sides.
            const bool swaps = info.orientation >= JXL_ORIENT_TRANSPOSE;
            width = int(swaps ? info.ysize : info.xsize);
            height = int(swaps ? info.xsize : info.ysize);
            alpha = info.alpha_bits > 0;
            if (!withinLimit(width, height))
                return {};
            JxlResizableParallelRunnerSetThreads(runner.get(),
                                                 JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));
        } else if (status == JXL_DEC_COLOR_ENCODING) {
            // Taken only by images in the codec's own colour space; others come
            // in their file's colours, which the target profile then names.
            JxlColorEncoding srgb = {};
            srgb.color_space = JXL_COLOR_SPACE_RGB;
            srgb.white_point = JXL_WHITE_POINT_D65;
            srgb.primaries = JXL_PRIMARIES_SRGB;
            srgb.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
            srgb.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;
            JxlDecoderSetPreferredColorProfile(decoder.get(), &srgb);
            size_t iccSize = 0;
            if (JxlDecoderGetICCProfileSize(decoder.get(), JXL_COLOR_PROFILE_TARGET_DATA, &iccSize)
                == JXL_DEC_SUCCESS) {
                std::vector<uint8_t> icc(iccSize);
                if (JxlDecoderGetColorAsICCProfile(decoder.get(), JXL_COLOR_PROFILE_TARGET_DATA, icc.data(),
                                                   icc.size())
                    == JXL_DEC_SUCCESS)
                    profile = SourceProfile::fromIcc(icc.data(), icc.size());
            }
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            size_t needed = 0;
            if (JxlDecoderImageOutBufferSize(decoder.get(), &format, &needed) != JXL_DEC_SUCCESS
                || needed != size_t(width) * size_t(height) * 4)
                return {};
            pixels.resize(needed);
            if (JxlDecoderSetImageOutBuffer(decoder.get(), &format, pixels.data(), needed) != JXL_DEC_SUCCESS)
                return {};
        } else if (status == JXL_DEC_FULL_IMAGE || status == JXL_DEC_SUCCESS) {
            // Only the first frame of an animation.
            break;
        } else {
            return {};
        }
    }
    if (pixels.empty())
        return {};
    if (profile)
        profile->toSrgb(pixels.data(), width, height, size_t(width) * 4, false, false);
    Source source;
    source.width = width;
    source.height = height;
    source.image = imageFromRgba(pixels.data(), width, height, size_t(width) * 4, alpha);
    return source;
}

static Source decodeSvg(const std::string& data, int boxWidth, int boxHeight, ImageScaling scaling)
{
    // Read for its size only; the drawing is the icon loader's, which knows
    // the style sheets and data URIs themes use.
    SkMemoryStream stream(data.data(), data.size());
    sk_sp<SkSVGDOM> svg = SkSVGDOM::Builder().make(stream);
    if (!svg || !svg->getRoot())
        return {};
    SkSize intrinsic = svg->getRoot()->intrinsicSize(SkSVGLengthContext(SkSize::Make(0, 0)));
    if ((intrinsic.width() <= 0 || intrinsic.height() <= 0) && svg->getRoot()->getViewBox().has_value())
        intrinsic = SkSize::Make(svg->getRoot()->getViewBox()->width(), svg->getRoot()->getViewBox()->height());
    int width = std::max(1, int(intrinsic.width()));
    int height = std::max(1, int(intrinsic.height()));
    Source source;
    source.width = width;
    source.height = height;
    // Drawn at the size it is shown at, as a scalable image is.
    if (boxWidth > 0 && boxHeight > 0)
        scaledSize(width, height, boxWidth, boxHeight, scaling, &width, &height);
    if (!withinLimit(width, height))
        return {};
    source.image = IconLoader::renderSvgData(SkData::MakeWithCopy(data.data(), data.size()), width, height);
    return source;
}

static bool startsWith(const std::string& data, size_t offset, std::string_view magic)
{
    return data.size() >= offset + magic.size() && std::memcmp(data.data() + offset, magic.data(), magic.size()) == 0;
}

static Source fromRaw(const image_formats::RawImage& raw)
{
    Source source;
    if (raw.isNull() || !withinLimit(raw.width, raw.height))
        return source;
    const SkImageInfo info = SkImageInfo::MakeN32Premul(raw.width, raw.height);
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(info))
        return source;
    bitmap.writePixels(SkPixmap(info, raw.pixels.data(), size_t(raw.width) * 4));
    bitmap.setImmutable();
    source.image = bitmap.asImage();
    source.width = raw.width;
    source.height = raw.height;
    return source;
}

static bool looksLikeSvg(const std::string& data, const std::string& path)
{
    if (path.ends_with(".svg") || path.ends_with(".svgz"))
        return true;
    const std::string head = data.substr(0, 512);
    return head.find("<svg") != std::string::npos;
}

DecodedImage decodeImage(const ImageRequest& request)
{
    const std::optional<std::string> data =
            request.data.empty() ? base::file::read(request.path) : std::string(request.data);
    if (!data || data->empty())
        return {};

    Source source;
    if (startsWith(*data, 0, "RIFF") && startsWith(*data, 8, "WEBP")) {
        source = decodeWebp(*data);
    } else if (startsWith(*data, 0, std::string_view("II*\0", 4)) || startsWith(*data, 0, std::string_view("MM\0*", 4))) {
        source = decodeTiff(*data);
    } else if (heif_check_filetype(reinterpret_cast<const uint8_t*>(data->data()), int(std::min<size_t>(data->size(), 64)))
               == heif_filetype_yes_supported) {
        source = decodeHeif(*data);
    } else if (startsWith(*data, 0, "\xff\x0a")
               || startsWith(*data, 0, std::string_view("\0\0\0\x0cJXL \x0d\x0a\x87\x0a", 12))) {
        source = decodeJxl(*data);
    } else if (image_formats::isGif(*data)) {
        source = fromRaw(image_formats::decodeGif(*data));
    } else if (image_formats::isPnm(*data)) {
        source = fromRaw(image_formats::decodePnm(*data));
    } else if (image_formats::isIcns(*data)) {
        const std::string_view png = image_formats::icnsPng(*data, std::max(request.width, request.height));
        if (!png.empty())
            source = decodeWithSkia(SkData::MakeWithCopy(png.data(), png.size()), request.width, request.height);
    } else if (image_formats::isXbm(*data)) {
        source = fromRaw(image_formats::decodeXbm(*data));
    } else if (image_formats::isTga(*data, request.path)) {
        source = fromRaw(image_formats::decodeTga(*data));
    } else if (looksLikeSvg(*data, request.path)) {
        source = decodeSvg(*data, request.width, request.height, request.scaling);
    } else {
        source = decodeWithSkia(SkData::MakeWithCopy(data->data(), data->size()), request.width, request.height);
    }
    if (!source.image)
        return {};

    const SkEncodedOrigin origin = request.autoTransform ? source.origin : kTopLeft_SkEncodedOrigin;
    const bool swaps = SkEncodedOriginSwapsWidthHeight(origin);
    DecodedImage result;
    result.originalWidth = swaps ? source.height : source.width;
    result.originalHeight = swaps ? source.width : source.height;

    int width = result.originalWidth;
    int height = result.originalHeight;
    if (request.width > 0 && request.height > 0) {
        scaledSize(width, height, request.width, request.height, request.scaling, &width, &height);
        if (!request.enlarge && (width > result.originalWidth || height > result.originalHeight)) {
            width = std::min(width, result.originalWidth);
            height = std::min(height, result.originalHeight);
        }
    }
    width = std::max(1, width);
    height = std::max(1, height);
    if (origin == kTopLeft_SkEncodedOrigin && width == source.image->width() && height == source.image->height()) {
        result.image = source.image;
        return result;
    }

    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32(
            width, height, source.image->isOpaque() ? kOpaque_SkAlphaType : kPremul_SkAlphaType));
    if (!surface)
        return {};
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorTRANSPARENT);
    canvas->concat(SkEncodedOriginToMatrix(origin, width, height));
    const int drawWidth = swaps ? height : width;
    const int drawHeight = swaps ? width : height;
    // Mipmaps average what a shrink drops, for smooth scaling.
    const bool shrinks = drawWidth < source.image->width() || drawHeight < source.image->height();
    const SkSamplingOptions sampling = shrinks ? SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear)
                                               : SkSamplingOptions(SkFilterMode::kLinear);
    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kSrc);
    canvas->drawImageRect(source.image, SkRect::MakeWH(SkScalar(drawWidth), SkScalar(drawHeight)), sampling, &paint);
    result.image = surface->makeImageSnapshot();
    return result;
}

} // namespace cutefish::ui
