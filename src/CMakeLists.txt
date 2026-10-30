# Generated protocol marshalling; the sources live in the build tree.
set(CUTEFISH_PROTOCOL_DIR ${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols)
file(MAKE_DIRECTORY ${CUTEFISH_PROTOCOL_DIR})

function(cutefish_wayland_protocol name xml)
    set(code ${CUTEFISH_PROTOCOL_DIR}/${name}-protocol.c)
    set(client ${CUTEFISH_PROTOCOL_DIR}/${name}-client-protocol.h)
    add_custom_command(OUTPUT ${code}
        COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${code}
        DEPENDS ${xml} VERBATIM)
    add_custom_command(OUTPUT ${client}
        COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${client}
        DEPENDS ${xml} VERBATIM)
    set(${name}_SOURCES ${code} ${client} PARENT_SCOPE)
endfunction()

cutefish_wayland_protocol(xdg-shell
    ${WAYLAND_PROTOCOLS_DATADIR}/stable/xdg-shell/xdg-shell.xml)
cutefish_wayland_protocol(viewporter
    ${WAYLAND_PROTOCOLS_DATADIR}/stable/viewporter/viewporter.xml)
cutefish_wayland_protocol(presentation-time
    ${WAYLAND_PROTOCOLS_DATADIR}/stable/presentation-time/presentation-time.xml)
cutefish_wayland_protocol(fractional-scale-v1
    ${WAYLAND_PROTOCOLS_DATADIR}/staging/fractional-scale/fractional-scale-v1.xml)
cutefish_wayland_protocol(cursor-shape-v1
    ${WAYLAND_PROTOCOLS_DATADIR}/staging/cursor-shape/cursor-shape-v1.xml)
cutefish_wayland_protocol(xdg-dialog-v1
    ${WAYLAND_PROTOCOLS_DATADIR}/staging/xdg-dialog/xdg-dialog-v1.xml)
cutefish_wayland_protocol(xdg-activation-v1
    ${WAYLAND_PROTOCOLS_DATADIR}/staging/xdg-activation/xdg-activation-v1.xml)
cutefish_wayland_protocol(xdg-foreign-unstable-v2
    ${WAYLAND_PROTOCOLS_DATADIR}/unstable/xdg-foreign/xdg-foreign-unstable-v2.xml)
cutefish_wayland_protocol(text-input-unstable-v3
    ${WAYLAND_PROTOCOLS_DATADIR}/unstable/text-input/text-input-unstable-v3.xml)
cutefish_wayland_protocol(xdg-toplevel-drag-v1
    ${WAYLAND_PROTOCOLS_DATADIR}/staging/xdg-toplevel-drag/xdg-toplevel-drag-v1.xml)
cutefish_wayland_protocol(primary-selection-unstable-v1
    ${WAYLAND_PROTOCOLS_DATADIR}/unstable/primary-selection/primary-selection-unstable-v1.xml)
cutefish_wayland_protocol(pointer-gestures-unstable-v1
    ${WAYLAND_PROTOCOLS_DATADIR}/unstable/pointer-gestures/pointer-gestures-unstable-v1.xml)
cutefish_wayland_protocol(xdg-output-unstable-v1
    ${WAYLAND_PROTOCOLS_DATADIR}/unstable/xdg-output/xdg-output-unstable-v1.xml)
# cursor-shape-v1 refers to the tablet tool interface.
cutefish_wayland_protocol(tablet-unstable-v2
    ${WAYLAND_PROTOCOLS_DATADIR}/unstable/tablet/tablet-unstable-v2.xml)
# Copies of cutefish-compositor's protocols; the copies must stay identical.
cutefish_wayland_protocol(cutefish-window-hints-v1
    ${PROJECT_SOURCE_DIR}/protocols/cutefish-window-hints-v1.xml)
cutefish_wayland_protocol(cutefish-surface-blur-v1
    ${PROJECT_SOURCE_DIR}/protocols/cutefish-surface-blur-v1.xml)
cutefish_wayland_protocol(cutefish-shell-v1
    ${PROJECT_SOURCE_DIR}/protocols/cutefish-shell-v1.xml)
cutefish_wayland_protocol(cutefish-main-menu-v1
    ${PROJECT_SOURCE_DIR}/protocols/cutefish-main-menu-v1.xml)
cutefish_wayland_protocol(cutefish-status-item-v1
    ${PROJECT_SOURCE_DIR}/protocols/cutefish-status-item-v1.xml)
cutefish_wayland_protocol(cutefish-window-placement-v1
    ${PROJECT_SOURCE_DIR}/protocols/cutefish-window-placement-v1.xml)

add_subdirectory(base)

set(CUTEFISH_TOOLKIT_PUBLIC_HEADERS
    ui/about_dialog.h
    ui/animated_image.h
    ui/animation.h
    ui/application.h
    ui/bitmap.h
    ui/bitmap_painter.h
    ui/canvas_2d.h
    ui/blurred_copy.h
    ui/fast_blur.h
    ui/bubble_shape.h
    ui/button.h
    ui/check_box.h
    ui/clipboard.h
    ui/color_id.h
    ui/color_mixer.h
    ui/color_provider.h
    ui/combo_box.h
    ui/dialog_window.h
    ui/drag.h
    ui/drop_down_popup.h
    ui/file_drop_area.h
    ui/embedded_gpu.h
    ui/embedded_host.h
    ui/embedded_window.h
    ui/event.h
    ui/frame_client.h
    ui/geometry.h
    ui/geometry_conversions.h
    ui/glyph_run.h
    ui/file_chooser.h
    ui/header_backdrop.h
    ui/header_button.h
    ui/icon.h
    ui/icon_image.h
    ui/image.h
    ui/image_view.h
    ui/keys.h
    ui/label.h
    ui/layout.h
    ui/list_view.h
    ui/menu.h
    ui/painter.h
    ui/path.h
    ui/popup_glass.h
    ui/progress_bar.h
    ui/cell_layout.h
    ui/round_image_button.h
    ui/scroll_bar.h
    ui/scroll_view.h
    ui/scroller.h
    ui/search_field.h
    ui/segmented_control.h
    ui/slider.h
    ui/status_item.h
    ui/styled_label.h
    ui/subsurface.h
    ui/switch.h
    ui/test_input.h
    ui/text_area.h
    ui/text_field.h
    ui/text_layout.h
    ui/theme.h
    ui/toast.h
    ui/trusted_connection.h
    ui/toggle_control.h
    ui/view.h
    ui/window.h
    ui/window_controls.h
    ui/window_header.h
    ui/window_tab_bar.h
)

add_library(cutefish-toolkit SHARED
    gpu/egl_context.cpp
    gpu/program_cache.cpp
    wayland/connection.cpp
    wayland/data_device.cpp
    wayland/primary_selection.cpp
    wayland/selection.cpp
    wayland/keyboard.cpp
    wayland/main_menu.cpp
    wayland/text_input.cpp
    wayland/shell_surface.cpp
    wayland/wayland_window.cpp
    ui/about_dialog.cpp
    ui/animated_image.cpp
    ui/animation.cpp
    ui/appearance.cpp
    ui/application.cpp
    ui/bubble_shape.cpp
    ui/button.cpp
    ui/check_box.cpp
    ui/clipboard.cpp
    ui/color_mixer.cpp
    ui/color_profile.cpp
    ui/color_provider.cpp
    ui/combo_box.cpp
    ui/dialog_window.cpp
    ui/drop_down_popup.cpp
    ui/editable_text.cpp
    ui/embedded_gpu.cpp
    ui/embedded_window.cpp
    ui/font_fallback.cpp
    ui/frame_client.cpp
    ui/geometry.cpp
    ui/glyph_run.cpp
    ui/file_chooser.cpp
    ui/file_drop_area.cpp
    ui/header_backdrop.cpp
    ui/header_button.cpp
    ui/icon.cpp
    ui/icon_image.cpp
    ui/icon_loader.cpp
    ui/image.cpp
    ui/gif_decoder.cpp
    ui/image_decoder.cpp
    ui/jxl_library.cpp
    ui/image_formats.cpp
    ui/image_view.cpp
    ui/label.cpp
    ui/line_breaks.cpp
    ui/layout.cpp
    ui/list_view.cpp
    ui/menu.cpp
    ui/multi_effect_blur.cpp
    ui/painter.cpp
    ui/path.cpp
    ui/bitmap.cpp
    ui/bitmap_painter.cpp
    ui/canvas_2d.cpp
    ui/blurred_copy.cpp
    ui/fast_blur.cpp
    ui/platform_window.cpp
    ui/popup_glass.cpp
    ui/progress_bar.cpp
    ui/masked_layer.cpp
    ui/cell_layout.cpp
    ui/smooth_rectangle.cpp
    ui/round_image_button.cpp
    ui/rounded_path.cpp
    ui/rounded_shape.cpp
    ui/scroll_bar.cpp
    ui/scroll_view.cpp
    ui/scroller.cpp
    ui/search_field.cpp
    ui/segmented_control.cpp
    ui/slider.cpp
    ui/status_item.cpp
    ui/styled_label.cpp
    ui/subsurface.cpp
    ui/switch.cpp
    ui/test_input.cpp
    ui/text_area.cpp
    ui/text_field.cpp
    ui/text_layout.cpp
    ui/text_context.cpp
    ui/text_lines.cpp
    ui/theme.cpp
    ui/toolkit_colors.cpp
    ui/thumbnail_cache.cpp
    ui/toast.cpp
    ui/trusted_connection.cpp
    ui/toggle_control.cpp
    ui/view.cpp
    ui/window.cpp
    ui/window_compositing.cpp
    ui/window_controls.cpp
    ui/window_restore.cpp
    ui/window_header.cpp
    ui/window_tab_bar.cpp
    ${xdg-shell_SOURCES}
    ${viewporter_SOURCES}
    ${presentation-time_SOURCES}
    ${fractional-scale-v1_SOURCES}
    ${cursor-shape-v1_SOURCES}
    ${pointer-gestures-unstable-v1_SOURCES}
    ${primary-selection-unstable-v1_SOURCES}
    ${xdg-toplevel-drag-v1_SOURCES}
    ${text-input-unstable-v3_SOURCES}
    ${xdg-dialog-v1_SOURCES}
    ${xdg-activation-v1_SOURCES}
    ${xdg-foreign-unstable-v2_SOURCES}
    ${xdg-output-unstable-v1_SOURCES}
    ${tablet-unstable-v2_SOURCES}
    ${cutefish-window-hints-v1_SOURCES}
    ${cutefish-surface-blur-v1_SOURCES}
    ${cutefish-shell-v1_SOURCES}
    ${cutefish-main-menu-v1_SOURCES}
    ${cutefish-status-item-v1_SOURCES}
    ${cutefish-window-placement-v1_SOURCES}
)

# The glyph icons, built in so they need no install location.
file(GLOB CUTEFISH_TOOLKIT_ICONS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/data/icons/*.svg)
set(CUTEFISH_TOOLKIT_EMBEDDED ${CMAKE_CURRENT_BINARY_DIR}/embedded_files.cpp)
# A list would be split into separate arguments on the way to the script.
string(REPLACE ";" "|" CUTEFISH_TOOLKIT_ICON_LIST "${CUTEFISH_TOOLKIT_ICONS}")
add_custom_command(OUTPUT ${CUTEFISH_TOOLKIT_EMBEDDED}
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${CUTEFISH_TOOLKIT_EMBEDDED}
            -DFILES=${CUTEFISH_TOOLKIT_ICON_LIST} -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
    DEPENDS ${CUTEFISH_TOOLKIT_ICONS} ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
    VERBATIM)
target_sources(cutefish-toolkit PRIVATE ${CUTEFISH_TOOLKIT_EMBEDDED})

add_library(Cutefish::Toolkit ALIAS cutefish-toolkit)

# Only the toolkit's own symbols: Skia's inline functions and the
# generated protocol code stay out of the processes that load it, the
# compositor's included.
target_link_options(cutefish-toolkit PRIVATE
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cutefish-toolkit.map)
set_property(TARGET cutefish-toolkit APPEND PROPERTY
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cutefish-toolkit.map)

set_target_properties(cutefish-toolkit PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME Toolkit
)

target_include_directories(cutefish-toolkit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/cutefish-toolkit>
    PRIVATE ${CUTEFISH_PROTOCOL_DIR}
)

target_link_libraries(cutefish-toolkit
    PUBLIC
        cutefish-toolkit-base
    PRIVATE
        skia
        PkgConfig::WaylandClient
        PkgConfig::Egl
        PkgConfig::GlesV2
        PkgConfig::Fontconfig
        PkgConfig::Xkbcommon
        PkgConfig::Systemd
        PkgConfig::ImageCodecs
        PkgConfig::Icu
        ${CMAKE_DL_LIBS}
)
target_include_directories(cutefish-toolkit PRIVATE ${Jxl_INCLUDE_DIRS})
target_compile_definitions(cutefish-toolkit PRIVATE
    CUTEFISH_JXL_LIBRARY="${CUTEFISH_jxl_SONAME}"
    CUTEFISH_JXL_THREADS_LIBRARY="${CUTEFISH_jxl_threads_SONAME}"
)

install(TARGETS cutefish-toolkit EXPORT CutefishToolkitTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

foreach(header ${CUTEFISH_TOOLKIT_PUBLIC_HEADERS})
    get_filename_component(directory ${header} DIRECTORY)
    install(FILES ${header} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/cutefish-toolkit/${directory})
endforeach()

install(EXPORT CutefishToolkitTargets
    NAMESPACE Cutefish::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CutefishToolkit
)
