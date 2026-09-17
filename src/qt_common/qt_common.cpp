// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/literals.h"

#include "common/memory_detect.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "frontend_common/data_manager.h"
#include "hid_core/hid_core.h"
#include "network/network.h"
#include "qt_common.h"

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/cpu_features.h"
#include "core/memory.h"

#include <QGuiApplication>
#include <QStringLiteral>
#include "core/frontend/emu_window.h"
#include "qt_common/util/meta.h"

#include <QFile>

#include <QMessageBox>

#include <thread>
#include <JlCompress.h>
#include <QPainter>

#if !defined(WIN32) && !defined(__APPLE__)
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

#ifdef _WIN32
#include <QApplication>
#include <QSettings>
#include <windows.h>
#include "common/windows/timer_resolution.h"
#include "core/core_timing.h"
#endif

using namespace Common::Literals;

namespace QtCommon {

QWidget* rootObject = nullptr;

std::unique_ptr<Core::System> system = nullptr;
std::shared_ptr<FileSys::RealVfsFilesystem> vfs = nullptr;
std::unique_ptr<FileSys::ManualContentProvider> provider = nullptr;
std::unique_ptr<EmuThread> emu_thread = nullptr;

const QStringList supported_file_extensions = {QStringLiteral("nro"), QStringLiteral("nso"),
                                               QStringLiteral("nca"), QStringLiteral("xci"),
                                               QStringLiteral("nsp"), QStringLiteral("kip")};

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
    else if (platform_name == QStringLiteral("haiku"))
        return Core::Frontend::WindowSystemType::Xcb;

    LOG_CRITICAL(Frontend, "Unknown Qt platform {}!", platform_name.toStdString());
    return Core::Frontend::WindowSystemType::Windows;
} // namespace Core::Frontend::WindowSystemType

Core::Frontend::EmuWindow::WindowSystemInfo GetWindowSystemInfo(QWindow* window) {
    Core::Frontend::EmuWindow::WindowSystemInfo wsi;
    wsi.type = GetWindowSystemType();

#if defined(WIN32)
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

const QString tr(const char* str) {
    return rootObject->tr(str);
}

const QString tr(const std::string& str) {
    return rootObject->tr(str.c_str());
}

static void LogRuntimes() {
#ifdef _MSC_VER
    // It is possible that the name of the dll will change.
    // vcruntime140.dll is for 2015 and onwards
    static constexpr char runtime_dll_name[] = "vcruntime140.dll";
    UINT sz = GetFileVersionInfoSizeA(runtime_dll_name, nullptr);
    bool runtime_version_inspection_worked = false;
    if (sz > 0) {
        std::vector<u8> buf(sz);
        if (GetFileVersionInfoA(runtime_dll_name, 0, sz, buf.data())) {
            VS_FIXEDFILEINFO* pvi;
            sz = sizeof(VS_FIXEDFILEINFO);
            if (VerQueryValueA(buf.data(), "\\", reinterpret_cast<LPVOID*>(&pvi), &sz)) {
                if (pvi->dwSignature == VS_FFI_SIGNATURE) {
                    runtime_version_inspection_worked = true;
                    LOG_INFO(Frontend, "MSVC Compiler: {} Runtime: {}.{}.{}.{}", _MSC_VER,
                             pvi->dwProductVersionMS >> 16, pvi->dwProductVersionMS & 0xFFFF,
                             pvi->dwProductVersionLS >> 16, pvi->dwProductVersionLS & 0xFFFF);
                }
            }
        }
    }
    if (!runtime_version_inspection_worked) {
        LOG_INFO(Frontend, "Unable to inspect {}", runtime_dll_name);
    }
#endif
    LOG_INFO(Frontend, "Qt Compile: {} Runtime: {}", QT_VERSION_STR, qVersion());
}

static QString PrettyProductName() {
#ifdef _WIN32
    // After Windows 10 Version 2004, Microsoft decided to switch to a different notation: 20H2
    // With that notation change they changed the registry key used to denote the current version
    QSettings windows_registry(
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"),
        QSettings::NativeFormat);
    const QString release_id = windows_registry.value(QStringLiteral("ReleaseId")).toString();
    if (release_id == QStringLiteral("2009")) {
        const u32 current_build = windows_registry.value(QStringLiteral("CurrentBuild")).toUInt();
        const QString display_version =
            windows_registry.value(QStringLiteral("DisplayVersion")).toString();
        const u32 ubr = windows_registry.value(QStringLiteral("UBR")).toUInt();
        u32 version = 10;
        if (current_build >= 22000) {
            version = 11;
        }
        return QStringLiteral("Windows %1 Version %2 (Build %3.%4)")
            .arg(QString::number(version), display_version, QString::number(current_build),
                 QString::number(ubr));
    }
#endif
    return QSysInfo::prettyProductName();
}

static void RemoveCachedContents() {
    const auto cache_dir = Common::FS::GetSuyuPath(Common::FS::SuyuPath::CacheDir);
    const auto offline_fonts = cache_dir / "fonts";
    const auto offline_manual = cache_dir / "offline_web_applet_manual";
    const auto offline_legal_information = cache_dir / "offline_web_applet_legal_information";
    const auto offline_system_data = cache_dir / "offline_web_applet_system_data";

    Common::FS::RemoveDirRecursively(offline_fonts);
    Common::FS::RemoveDirRecursively(offline_manual);
    Common::FS::RemoveDirRecursively(offline_legal_information);
    Common::FS::RemoveDirRecursively(offline_system_data);
}

void Init(QWidget* root) {
    rootObject = root;

    system = std::make_unique<Core::System>();
    vfs = std::make_unique<FileSys::RealVfsFilesystem>();
    provider = std::make_unique<FileSys::ManualContentProvider>();

    // initialization stuff
    Common::FS::CreateSuyuPaths();

    system->Initialize();

    Common::Log::Initialize();
    Common::Log::Start();

    Network::Init();

    QtCommon::Meta::RegisterMetaTypes();

    // build version
    const auto branch_name = std::string(Common::g_scm_branch);
    const auto description = std::string(Common::g_scm_desc);
    const auto build_id = std::string(Common::g_build_id);

    const auto yuzu_build = fmt::format("suyu Development Build | {}-{}", branch_name, description);
    const auto override_build =
        fmt::format(fmt::runtime(std::string(Common::g_title_bar_format_idle)), build_id);
    const auto yuzu_build_version = override_build.empty() ? yuzu_build : override_build;
    const auto processor_count = std::thread::hardware_concurrency();

    // info logging
    LOG_INFO(Frontend, "drippu Version: {}", yuzu_build_version);
    LogRuntimes();
#ifdef ARCHITECTURE_x86_64
    const auto& caps = Common::g_cpu_caps;
    std::string cpu_string = caps.cpu_string;
    if (caps.avx || caps.avx2 || caps.avx512f) {
        cpu_string += " | AVX";
        if (caps.avx512f) {
            cpu_string += "512";
        } else if (caps.avx2) {
            cpu_string += '2';
        }
        if (caps.fma) {
            cpu_string += " | FMA";
        }
    }
    LOG_INFO(Frontend, "Host CPU: {}", cpu_string);
    if (std::optional<int> processor_core = Common::GetProcessorCount()) {
        LOG_INFO(Frontend, "Host CPU Cores: {}", *processor_core);
    }
#endif
    LOG_INFO(Frontend, "Host CPU Threads: {}", processor_count);
    LOG_INFO(Frontend, "Host OS: {}", PrettyProductName().toStdString());
    LOG_INFO(Frontend, "Host RAM: {:.2f} GiB",
             Common::GetMemInfo().TotalPhysicalMemory / f64{1_GiB});
    LOG_INFO(Frontend, "Host Swap: {:.2f} GiB", Common::GetMemInfo().TotalSwapMemory / f64{1_GiB});
#ifdef _WIN32
    LOG_INFO(Frontend, "Host Timer Resolution: {:.4f} ms",
             std::chrono::duration_cast<std::chrono::duration<f64, std::milli>>(
                 Common::Windows::SetCurrentTimerResolutionToMaximum())
                 .count());
    QtCommon::system->CoreTiming().SetTimerResolutionNs(
        Common::Windows::GetCurrentTimerResolution());
#endif

    // Remove cached contents generated during the previous session
    RemoveCachedContents();
}

std::filesystem::path GetSuyuCommand() {
    std::filesystem::path command;

    // TODO: flatpak?
    QString appimage = QString::fromLocal8Bit(getenv("APPIMAGE"));
    if (!appimage.isEmpty()) {
        command = std::filesystem::path{appimage.toStdString()};
    } else {
        const QStringList args = QGuiApplication::arguments();
        command = args[0].toStdString();
    }

    // If relative path, make it an absolute path
    if (command.c_str()[0] == '.') {
        command = Common::FS::GetCurrentDir() / command;
    }

    return command;
}

void SetupContentProviders() {
    system->SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system->RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual,
                                    provider.get());
    system->GetFileSystemController().CreateFactories(*vfs);
}

void SetupHID() {
    system->HIDCore().ReloadInputDevices();
}

QString ReadableByteSize(qulonglong size) {
    return QString::fromStdString(FrontendCommon::DataManager::ReadableBytesSize(size));
}

QPixmap CreateCirclePixmapFromColor(const QColor& color) {
    QPixmap circle_pixmap(16, 16);
    circle_pixmap.fill(Qt::transparent);
    QPainter painter(&circle_pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(color);
    painter.setBrush(color);
    painter.drawEllipse({circle_pixmap.width() / 2.0, circle_pixmap.height() / 2.0}, 7.0, 7.0);
    return circle_pixmap;
}

} // namespace QtCommon
