// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "base/application_bundle.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <string_view>
#include <vector>

#include "base/file.h"
#include "base/mime_database.h"
#include "base/paths.h"
#include "base/property_list.h"

namespace cutefish::base::bundle {

namespace fs = std::filesystem;

static constexpr std::string_view kSuffix = ".app";

namespace {

struct UniformType
{
    std::string_view identifier;
    std::vector<std::string_view> mimeTypes;
};

} // namespace

// The system's uniform type identifiers that bundles name in their document
// types. A type stands for its descendants as well; "group/*" for every type
// in the group.
static const std::vector<UniformType>& uniformTypes()
{
    static const std::vector<UniformType> types = {
            {"public.text", {"text/plain"}},
            {"public.plain-text", {"text/plain"}},
            {"public.utf8-plain-text", {"text/plain"}},
            {"public.utf16-plain-text", {"text/plain"}},
            {"public.utf16-external-plain-text", {"text/plain"}},
            {"public.delimited-values-text", {"text/csv", "text/tab-separated-values"}},
            {"public.comma-separated-values-text", {"text/csv"}},
            {"public.tab-separated-values-text", {"text/tab-separated-values"}},
            {"public.utf8-tab-separated-values-text", {"text/tab-separated-values"}},
            {"public.rtf", {"application/rtf"}},
            {"public.html", {"text/html"}},
            {"public.xhtml", {"application/xhtml+xml"}},
            {"public.xml", {"application/xml"}},
            {"public.json", {"application/json"}},
            {"public.yaml", {"application/yaml"}},
            {"public.css", {"text/css"}},
            {"public.log", {"text/x-log"}},
            {"public.patch-file", {"text/x-patch"}},
            {"net.daringfireball.markdown", {"text/markdown"}},
            {"public.source-code",
             {"text/x-csrc", "text/x-chdr", "text/x-c++src", "text/x-c++hdr", "text/x-objcsrc", "text/x-objc++src",
              "text/x-swift", "text/x-java", "text/x-asm", "text/x-makefile", "application/x-shellscript",
              "text/x-python3", "text/x-python", "application/x-ruby", "application/x-perl", "application/x-php",
              "application/javascript"}},
            {"public.script",
             {"application/x-shellscript", "text/x-python3", "text/x-python", "application/x-ruby",
              "application/x-perl", "application/x-php", "application/javascript"}},
            {"public.c-source", {"text/x-csrc"}},
            {"public.c-header", {"text/x-chdr"}},
            {"public.c-plus-plus-source", {"text/x-c++src"}},
            {"public.c-plus-plus-header", {"text/x-c++hdr"}},
            {"public.objective-c-source", {"text/x-objcsrc"}},
            {"public.objective-c-plus-plus-source", {"text/x-objc++src"}},
            {"public.swift-source", {"text/x-swift"}},
            {"com.sun.java-source", {"text/x-java"}},
            {"public.assembly-source", {"text/x-asm"}},
            {"public.make-source", {"text/x-makefile"}},
            {"public.shell-script", {"application/x-shellscript"}},
            {"public.python-script", {"text/x-python3", "text/x-python"}},
            {"public.ruby-script", {"application/x-ruby"}},
            {"public.perl-script", {"application/x-perl"}},
            {"public.php-script", {"application/x-php"}},
            {"com.netscape.javascript-source", {"application/javascript"}},
            {"public.image", {"image/*"}},
            {"public.jpeg", {"image/jpeg"}},
            {"public.png", {"image/png"}},
            {"public.tiff", {"image/tiff"}},
            {"com.compuserve.gif", {"image/gif"}},
            {"com.microsoft.bmp", {"image/bmp"}},
            {"com.microsoft.ico", {"image/vnd.microsoft.icon"}},
            {"com.apple.icns", {"image/x-icns"}},
            {"public.heic", {"image/heic"}},
            {"public.heif", {"image/heif"}},
            {"public.avif", {"image/avif"}},
            {"org.webmproject.webp", {"image/webp"}},
            {"public.jpeg-2000", {"image/jp2"}},
            {"public.svg-image", {"image/svg+xml"}},
            {"com.truevision.tga-image", {"image/x-tga"}},
            {"com.adobe.photoshop-image", {"image/vnd.adobe.photoshop"}},
            {"com.adobe.pdf", {"application/pdf"}},
            {"com.adobe.postscript", {"application/postscript"}},
            {"com.adobe.encapsulated-postscript", {"image/x-eps"}},
            {"public.audiovisual-content", {"video/*", "audio/*"}},
            {"public.movie", {"video/*"}},
            {"public.video", {"video/*"}},
            {"public.mpeg-4", {"video/mp4"}},
            {"com.apple.m4v-video", {"video/x-m4v"}},
            {"com.apple.quicktime-movie", {"video/quicktime"}},
            {"public.mpeg", {"video/mpeg"}},
            {"public.mpeg-2-video", {"video/mpeg"}},
            {"public.mpeg-2-transport-stream", {"video/mp2t"}},
            {"public.avi", {"video/x-msvideo"}},
            {"public.3gpp", {"video/3gpp"}},
            {"org.webmproject.webm", {"video/webm"}},
            {"org.matroska.mkv", {"video/x-matroska"}},
            {"public.audio", {"audio/*"}},
            {"public.mp3", {"audio/mpeg"}},
            {"public.mpeg-4-audio", {"audio/mp4"}},
            {"com.apple.m4a-audio", {"audio/mp4"}},
            {"public.aac-audio", {"audio/aac"}},
            {"public.aiff-audio", {"audio/x-aiff"}},
            {"public.aifc-audio", {"audio/x-aifc"}},
            {"com.microsoft.waveform-audio", {"audio/x-wav"}},
            {"org.xiph.flac", {"audio/flac"}},
            {"public.midi-audio", {"audio/midi"}},
            {"com.apple.coreaudio-format", {"audio/x-caf"}},
            {"public.archive",
             {"application/zip", "application/x-tar", "application/gzip", "application/x-bzip2", "application/x-xz",
              "application/x-7z-compressed", "application/vnd.rar", "application/x-compressed-tar",
              "application/x-bzip2-compressed-tar", "application/x-xz-compressed-tar"}},
            {"public.zip-archive", {"application/zip"}},
            {"com.pkware.zip-archive", {"application/zip"}},
            {"public.tar-archive", {"application/x-tar"}},
            {"org.gnu.gnu-zip-archive", {"application/gzip"}},
            {"public.bzip2-archive", {"application/x-bzip2"}},
            {"org.tukaani.xz-archive", {"application/x-xz"}},
            {"org.7-zip.7-zip-archive", {"application/x-7z-compressed"}},
            {"com.rarlab.rar-archive", {"application/vnd.rar"}},
            {"com.apple.disk-image", {"application/x-apple-diskimage"}},
            {"com.apple.disk-image-udif", {"application/x-apple-diskimage"}},
            {"public.font", {"font/*"}},
            {"public.truetype-ttf-font", {"font/ttf"}},
            {"public.opentype-font", {"font/otf"}},
            {"public.truetype-collection-font", {"font/collection"}},
            {"org.openxmlformats.wordprocessingml.document",
             {"application/vnd.openxmlformats-officedocument.wordprocessingml.document"}},
            {"org.openxmlformats.spreadsheetml.sheet",
             {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"}},
            {"org.openxmlformats.presentationml.presentation",
             {"application/vnd.openxmlformats-officedocument.presentationml.presentation"}},
            {"com.microsoft.word.doc", {"application/msword"}},
            {"com.microsoft.excel.xls", {"application/vnd.ms-excel"}},
            {"com.microsoft.powerpoint.ppt", {"application/vnd.ms-powerpoint"}},
            {"org.oasis-open.opendocument.text", {"application/vnd.oasis.opendocument.text"}},
            {"org.oasis-open.opendocument.spreadsheet", {"application/vnd.oasis.opendocument.spreadsheet"}},
            {"org.oasis-open.opendocument.presentation", {"application/vnd.oasis.opendocument.presentation"}},
            {"org.idpf.epub-container", {"application/epub+zip"}},
            {"public.vcard", {"text/vcard"}},
            {"com.apple.ical.ics", {"text/calendar"}},
            {"com.apple.property-list", {"application/x-plist"}},
    };
    return types;
}

// The names of a type in the database, or the type of a file name ending in
// the extension; nothing for a name the database does not know or one that
// stands for any file.
static std::string knownType(std::string_view name)
{
    if (name.ends_with("/*"))
        return std::string(name);
    if (name.find('*') != std::string_view::npos)
        return {};
    const std::string type = mime::canonicalName(name);
    return type == "application/octet-stream" ? std::string() : type;
}

static std::string typeForExtension(const std::string& extension)
{
    if (extension.empty() || extension.find_first_of("*/") != std::string::npos)
        return {};
    return knownType(mime::typeForFileName("file." + extension));
}

static const UniformType* uniformType(std::string_view identifier)
{
    const auto found = std::find_if(uniformTypes().begin(), uniformTypes().end(),
                                    [identifier](const UniformType& type) { return type.identifier == identifier; });
    return found != uniformTypes().end() ? &*found : nullptr;
}

// Whether a system type is text, so a type conforming to it is too.
static bool isText(std::string_view identifier)
{
    const UniformType* type = uniformType(identifier);
    return type && std::all_of(type->mimeTypes.begin(), type->mimeTypes.end(), [](std::string_view mimeType) {
        return mime::inherits(mimeType, "text/plain");
    });
}

// The types the bundle declares itself, by identifier: their MIME types and
// the types of their extensions. An extension also names other files, such
// as .vhd both VHDL source and disk images, so a text type keeps only text.
static std::map<std::string, std::vector<std::string>> declaredTypes(const plist::Value& info)
{
    std::map<std::string, std::vector<std::string>> result;
    for (const char* key : {"UTExportedTypeDeclarations", "UTImportedTypeDeclarations"}) {
        const plist::Value* declarations = info.find(key);
        if (!declarations)
            continue;
        for (const plist::Value& declaration : declarations->array) {
            const std::string identifier = declaration.string("UTTypeIdentifier");
            const plist::Value* tags = declaration.find("UTTypeTagSpecification");
            if (identifier.empty() || !tags || result.contains(identifier))
                continue;
            std::vector<std::string>& types = result[identifier];
            if (const plist::Value* mimeTypes = tags->find("public.mime-type")) {
                for (const std::string& type : mimeTypes->strings())
                    types.push_back(knownType(type));
            }
            const plist::Value* conformsTo = declaration.find("UTTypeConformsTo");
            const std::vector<std::string> parents = conformsTo ? conformsTo->strings() : std::vector<std::string>();
            const bool text = std::any_of(parents.begin(), parents.end(), isText);
            if (const plist::Value* extensions = tags->find("public.filename-extension")) {
                for (const std::string& extension : extensions->strings()) {
                    const std::string type = typeForExtension(extension);
                    if (!text || mime::inherits(type, "text/plain"))
                        types.push_back(type);
                }
            }
        }
    }
    return result;
}

// What CFBundleDocumentTypes says the bundle opens: the types of its content
// type identifiers, its extensions and its MIME types, all of them, since
// identifiers this list does not know are common. A document type the bundle
// only describes, or one for any file such as public.data, opens nothing.
static std::vector<std::string> documentTypes(const plist::Value& info)
{
    const plist::Value* documents = info.find("CFBundleDocumentTypes");
    if (!documents)
        return {};
    const std::map<std::string, std::vector<std::string>> declared = declaredTypes(info);

    std::vector<std::string> result;
    const auto add = [&result](const std::string& type) {
        if (!type.empty() && std::find(result.begin(), result.end(), type) == result.end())
            result.push_back(type);
    };
    for (const plist::Value& document : documents->array) {
        if (document.string("CFBundleTypeRole") == "None" || document.string("LSHandlerRank") == "None")
            continue;

        if (const plist::Value* contentTypes = document.find("LSItemContentTypes")) {
            for (const std::string& identifier : contentTypes->strings()) {
                if (const UniformType* known = uniformType(identifier)) {
                    for (const std::string_view type : known->mimeTypes)
                        add(knownType(type));
                } else if (const auto own = declared.find(identifier); own != declared.end()) {
                    for (const std::string& type : own->second)
                        add(type);
                }
            }
        }

        if (const plist::Value* mimeTypes = document.find("CFBundleTypeMIMETypes")) {
            for (const std::string& type : mimeTypes->strings())
                add(knownType(type));
        }
        if (const plist::Value* extensions = document.find("CFBundleTypeExtensions")) {
            for (const std::string& extension : extensions->strings())
                add(typeForExtension(extension));
        }
    }
    return result;
}

std::string applicationsDirectory()
{
    return paths::home() + "/Applications";
}

std::string bundlePath(const std::string& path)
{
    if (path.empty())
        return {};
    std::error_code error;
    std::string result = fs::absolute(path, error).lexically_normal().string();
    while (result.size() > 1 && result.back() == '/')
        result.pop_back();
    const std::string name = fs::path(result).filename().string();
    if (name.size() <= kSuffix.size() || !name.ends_with(kSuffix))
        return {};
    return result;
}

std::string bundleOfProgram(const std::string& program)
{
    if (program.empty() || program.back() == '/')
        return {};
    std::error_code error;
    const fs::path macos = fs::absolute(program, error).lexically_normal().parent_path();
    const fs::path contents = macos.parent_path();
    if (macos.filename() != "MacOS" || contents.filename() != "Contents")
        return {};
    return bundlePath(contents.parent_path().string());
}

// As Exec quotes it: one argument, and a '%' that is no field code.
static std::string execArgument(const std::string& argument, bool literal)
{
    std::string result;
    for (const char c : argument) {
        if (literal && c == '%')
            result += '%';
        else if (c == ' ' || c == '\t' || c == '\n' || c == '"' || c == '\'' || c == '\\')
            result += '\\';
        result += c;
    }
    return result.empty() ? std::string("\"\"") : result;
}

// The handler's Exec with the bundle in its file or URL argument, or after
// the others when it takes none, and the documents to open last.
static std::string execFor(const std::string& path, const DesktopEntry& handler)
{
    // Field codes have to survive, so the handler's arguments are split with
    // every '%' escaped.
    DesktopEntry raw;
    raw.exec = handler.exec;
    for (size_t i = 0; i < raw.exec.size(); ++i) {
        if (raw.exec[i] == '%')
            raw.exec.replace(i++, 1, "%%");
    }

    std::string result;
    bool placed = false;
    for (const std::string& token : raw.command()) {
        if (!result.empty())
            result += ' ';
        if (!placed && (token == "%f" || token == "%F" || token == "%u" || token == "%U")) {
            result += execArgument(path, true);
            placed = true;
            continue;
        }
        result += execArgument(token, false);
    }
    if (!placed && !result.empty())
        result += ' ' + execArgument(path, true);
    return result + " %F";
}

std::optional<DesktopEntry> read(const std::string& path, const DesktopEntry& handler)
{
    const std::string bundle = bundlePath(path);
    if (bundle.empty())
        return std::nullopt;
    const std::optional<std::string> contents = file::read(bundle + "/Contents/Info.plist");
    if (!contents)
        return std::nullopt;
    const std::optional<plist::Value> info = plist::read(*contents);
    if (!info || info->type != plist::Value::Type::Dictionary)
        return std::nullopt;
    const auto value = [&info](const char* key) { return info->string(key); };

    DesktopEntry entry;
    entry.id = bundle;
    entry.path = bundle;
    entry.type = "Application";
    entry.bundle = true;
    entry.name = value("CFBundleDisplayName");
    if (entry.name.empty())
        entry.name = value("CFBundleName");
    if (entry.name.empty()) {
        entry.name = fs::path(bundle).filename().string();
        entry.name.resize(entry.name.size() - kSuffix.size());
    }

    std::string icon = value("CFBundleIconFile");
    if (!icon.empty() && icon.find('/') == std::string::npos) {
        if (fs::path(icon).extension().empty())
            icon += ".icns";
        icon = bundle + "/Contents/Resources/" + icon;
        std::error_code error;
        if (fs::is_regular_file(icon, error))
            entry.icon = icon;
    }

    // An agent or background application has no dock icon of its own to
    // show the launch on.
    entry.startupNotify = !info->flag("LSUIElement") && !info->flag("LSBackgroundOnly");
    entry.mimeTypes = documentTypes(*info);
    entry.exec = execFor(bundle, handler);
    entry.tryExec = handler.tryExec;
    return entry;
}

bool opensType(const DesktopEntry& entry, const std::string& type)
{
    return std::any_of(entry.mimeTypes.begin(), entry.mimeTypes.end(), [&type](const std::string& opened) {
        if (opened.ends_with("/*"))
            return type.starts_with(std::string_view(opened).substr(0, opened.size() - 1));
        return mime::inherits(type, opened);
    });
}

} // namespace cutefish::base::bundle
