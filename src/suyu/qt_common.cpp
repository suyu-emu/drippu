// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QGuiApplication>
#include <QStringLiteral>
#include <QWindow>
#include "common/logging/log.h"
#include "core/frontend/emu_window.h"
#include "suyu/qt_common.h"

#if !defined(_WIN32) && !defined(__APPLE__)
#include <qpa/qplatformnativeinterface.h>
#elif defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

#if defined(__APPLE__)
namespace {
id FindMetalLayerInTree(id layer, Class metal_layer_class) {
    // Qt may expose a QContainerLayer here; MoltenVK needs the actual CAMetalLayer.
    if (!layer || !metal_layer_class) {
        return nullptr;
    }

    const SEL is_kind_of_class = sel_registerName("isKindOfClass:");
    if (reinterpret_cast<bool (*)(id, SEL, Class)>(objc_msgSend)(layer, is_kind_of_class,
                                                                 metal_layer_class)) {
        return layer;
    }

    // Search descendants because the Metal layer may be nested below Qt's wrappers.
    const SEL sublayers_selector = sel_registerName("sublayers");
    id sublayers = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(layer, sublayers_selector);
    if (!sublayers) {
        return nullptr;
    }

    const SEL count_selector = sel_registerName("count");
    const SEL object_at_index_selector = sel_registerName("objectAtIndex:");
    const auto count =
        reinterpret_cast<unsigned long (*)(id, SEL)>(objc_msgSend)(sublayers, count_selector);
    for (unsigned long i = 0; i < count; ++i) {
        id metal_layer = FindMetalLayerInTree(
            reinterpret_cast<id (*)(id, SEL, unsigned long)>(objc_msgSend)(
                sublayers, object_at_index_selector, i),
            metal_layer_class);
        if (metal_layer) {
            return metal_layer;
        }
    }

    return nullptr;
}

// Qt 6 VulkanSurface often leaves a QContainerLayer (or no layer) on the NSView.
// VK_EXT_metal_surface / MoltenVK require a CAMetalLayer, so create and attach
// one the same way SDL's Metal path does when the walk finds nothing.
id CreateAndAttachMetalLayer(id view) {
    Class metal_layer_class = objc_getClass("CAMetalLayer");
    if (!metal_layer_class) {
        LOG_CRITICAL(Frontend, "CAMetalLayer is unavailable");
        return nullptr;
    }

    id metal_layer =
        reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend)(metal_layer_class, sel_registerName("alloc"));
    metal_layer =
        reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(metal_layer, sel_registerName("init"));
    if (!metal_layer) {
        LOG_CRITICAL(Frontend, "Failed to allocate CAMetalLayer for the Qt window");
        return nullptr;
    }

    reinterpret_cast<void (*)(id, SEL, bool)>(objc_msgSend)(view, sel_registerName("setWantsLayer:"),
                                                            true);
    reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(view, sel_registerName("setLayer:"),
                                                          metal_layer);
    // NSView retains the layer; drop our alloc/init +1.
    reinterpret_cast<void (*)(id, SEL)>(objc_msgSend)(metal_layer, sel_registerName("release"));

    id attached = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(view, sel_registerName("layer"));
    return FindMetalLayerInTree(attached, metal_layer_class);
}

id GetOrCreateMetalLayer(id view, QWindow* window) {
    if (!view) {
        LOG_CRITICAL(Frontend, "Failed to get the NSView for the Qt window");
        return nullptr;
    }

    Class metal_layer_class = objc_getClass("CAMetalLayer");
    id layer = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(view, sel_registerName("layer"));
    id metal_layer = FindMetalLayerInTree(layer, metal_layer_class);
    if (!metal_layer) {
        metal_layer = CreateAndAttachMetalLayer(view);
    }
    if (!metal_layer) {
        LOG_CRITICAL(Frontend,
                     "Failed to get or create a CAMetalLayer for the Qt window; "
                     "VK_EXT_metal_surface cannot proceed");
        return nullptr;
    }

    if (window) {
        reinterpret_cast<void (*)(id, SEL, double)>(objc_msgSend)(
            metal_layer, sel_registerName("setContentsScale:"),
            static_cast<double>(window->devicePixelRatio()));
    }
    return metal_layer;
}
} // namespace
#endif

namespace QtCommon {
Core::Frontend::WindowSystemType GetWindowSystemType() {
    // Determine WSI type based on Qt platform.
    QString platform_name = QGuiApplication::platformName();
    if (platform_name == QStringLiteral("windows"))
        return Core::Frontend::WindowSystemType::Windows;
    else if (platform_name == QStringLiteral("xcb"))
        return Core::Frontend::WindowSystemType::X11;
    else if (platform_name == QStringLiteral("wayland"))
        return Core::Frontend::WindowSystemType::Wayland;
    else if (platform_name == QStringLiteral("wayland-egl"))
        return Core::Frontend::WindowSystemType::Wayland;
    else if (platform_name == QStringLiteral("cocoa"))
        return Core::Frontend::WindowSystemType::Cocoa;
    else if (platform_name == QStringLiteral("android"))
        return Core::Frontend::WindowSystemType::Android;

    LOG_CRITICAL(Frontend, "Unknown Qt platform {}!", platform_name.toStdString());
    return Core::Frontend::WindowSystemType::Windows;
} // namespace Core::Frontend::WindowSystemType

Core::Frontend::EmuWindow::WindowSystemInfo GetWindowSystemInfo(QWindow* window) {
    Core::Frontend::EmuWindow::WindowSystemInfo wsi;
    wsi.type = GetWindowSystemType();

#if defined(_WIN32)
    // Our Win32 Qt external doesn't have the private API.
    wsi.render_surface = reinterpret_cast<void*>(window->winId());
#elif defined(__APPLE__)
    if (!window) {
        LOG_CRITICAL(Frontend, "Failed to get a CAMetalLayer: QWindow is null");
        wsi.render_surface = nullptr;
    } else {
        // Walk first; if Qt left only a QContainerLayer (typical with VulkanSurface),
        // create and attach a CAMetalLayer on the NSView. Null here means creation
        // failed (already logged); vulkan_surface will refuse it.
        wsi.render_surface = reinterpret_cast<void*>(
            GetOrCreateMetalLayer(reinterpret_cast<id>(window->winId()), window));
    }
#else
    QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
    wsi.display_connection = pni->nativeResourceForWindow("display", window);
    if (wsi.type == Core::Frontend::WindowSystemType::Wayland)
        wsi.render_surface = window ? pni->nativeResourceForWindow("surface", window) : nullptr;
    else
        wsi.render_surface = window ? reinterpret_cast<void*>(window->winId()) : nullptr;
#endif
    wsi.render_surface_scale = window ? static_cast<float>(window->devicePixelRatio()) : 1.0f;

    return wsi;
}
} // namespace QtCommon
