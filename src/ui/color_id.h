// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <concepts>
#include <cstdint>
#include <type_traits>

namespace cutefish::ui {

// A colour named by what it is for; ColorProvider gives its value in the
// current theme. An application names its own colours in an enumeration of
// its own, on uint16_t, numbered from ApplicationFirst, and adds a mixer
// with their recipes (ColorProvider::addMixer).
enum class ColorId : uint16_t {
    // The appearance's colours, as Theme holds them.
    Background,
    SecondBackground,
    AlternateBackground,
    Text,
    DisabledText,
    Highlight,
    HighlightedText,

    // A destructive action, such as deleting.
    DestructiveAction,
    // A hairline between rows or parts of a panel.
    Separator,
    // Under a header, and between a list's header and its rows.
    HeaderSeparator,
    // A pressed row or item of a list or a sidebar.
    ItemPressed,
    // An application window over the compositor's blur.
    TranslucentWindowBackground,
    TranslucentWindowBackgroundInactive,

    ButtonBackground,
    ButtonBackgroundHovered,
    ButtonBackgroundPressed,
    ButtonFocusRing,

    ComboBoxBackground,
    ComboBoxBackgroundPressed,
    ComboBoxFocusBorder,

    // Check box and radio button indicators.
    ToggleBackground,
    ToggleBackgroundChecked,
    ToggleBorder,
    ToggleBorderChecked,
    ToggleFocusRing,
    CheckBoxInnerEdge,
    CheckBoxInnerEdgeChecked,
    // The check, or the radio button's dot.
    ToggleMark,

    SwitchTrack,
    SwitchTrackOn,
    SwitchHandle,

    SliderTrack,
    SliderTrackFilled,
    SliderTickMark,
    SliderHandle,
    SliderHandlePressed,
    SliderHandleBorder,
    SliderHandleShadow,
    SliderHandleShadowPressed,

    SegmentedControlTrack,
    SegmentedControlPlate,
    SegmentedControlPlateShadow,
    SegmentedControlTextPressed,
    // The tab style.
    SegmentedTabBackground,
    SegmentedTabPlateShadow,

    ScrollBarThumb,
    ScrollBarThumbDragging,

    TextFieldPlaceholder,
    TextFieldFocusRing,

    RoundButtonHovered,
    RoundButtonPressed,

    HeaderButtonTint,

    DialogButtonBackground,
    DialogButtonBackgroundHovered,
    DialogButtonBorder,
    DialogButtonInnerEdge,
    DialogButtonProminentText,
    DialogButtonProminentInnerEdge,
    DialogButtonFocusRing,

    MenuHighlight,
    MenuSeparator,
    MenuDisabledText,
    MenuShortcutText,
    MenuArrow,
    MenuArrowDisabled,
    MenuScrollIndicator,

    WindowTabBarTrack,
    WindowTabBarSelected,
    WindowTabBarSelectedEdge,

    ToolTipBackground,
    ToolTipShadow,
    ToastShadow,

    ToolkitEnd,

    ApplicationFirst = 1024,
};

// ColorId, or an application's enumeration of colours.
template<typename Id>
concept ColorIdType = std::is_enum_v<Id> && std::same_as<std::underlying_type_t<Id>, uint16_t>;

} // namespace cutefish::ui
