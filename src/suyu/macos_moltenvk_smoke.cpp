// SPDX-FileCopyrightText: 2026 drippu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Apple Silicon smoke of the live Qt Vulkan path:
// QApplication + VulkanRenderWidget / InitRenderTarget WSI snapshot before
// addWidget, then RendererVulkan's CreateDevice + swapchain on that surface.
// MoltenVK is loaded from suyu.app Contents/Frameworks (no LIBVULKAN_PATH).
// Does not boot a game.

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

#include <QApplication>
#include <QHBoxLayout>
#include <QPaintEngine>
#include <QStringLiteral>
#include <QWidget>
#include <QWindow>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/framebuffer_layout.h"
#include "suyu/qt_common.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace {

void Fail(const std::string& why) {
    std::cerr << "macos_moltenvk_smoke: FAIL " << why << '\n';
    std::exit(1);
}

// Mirrors bootmanager.cpp RenderWidget / VulkanRenderWidget.
class RenderWidget : public QWidget {
public:
    explicit RenderWidget(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_PaintOnScreen);
        if (QtCommon::GetWindowSystemType() == Core::Frontend::WindowSystemType::Wayland) {
            setAttribute(Qt::WA_DontCreateNativeAncestors);
        }
    }

    QPaintEngine* paintEngine() const override {
        return nullptr;
    }
};

class VulkanRenderWidget : public RenderWidget {
public:
    explicit VulkanRenderWidget(QWidget* parent) : RenderWidget(parent) {
        windowHandle()->setSurfaceType(QWindow::VulkanSurface);
    }
};

} // namespace

int main(int argc, char** argv) {
#ifdef __APPLE__
    unsetenv("LIBVULKAN_PATH");
#endif

    Common::Log::Initialize();
    Common::Log::Start();
    Common::Log::SetColorConsoleBackendEnabled(true);

    const auto bundle = Common::FS::GetBundleDirectory();
    const auto moltenvk = bundle / "Contents/Frameworks/libMoltenVK.dylib";
    std::cout << "macos_moltenvk_smoke: bundle=" << bundle.string() << '\n';
    if (!std::filesystem::exists(moltenvk)) {
        Fail("MoltenVK missing from app Frameworks at " + moltenvk.string());
    }
    std::cout << "macos_moltenvk_smoke: moltenvk=" << moltenvk.string() << '\n';

    QApplication app(argc, argv);

    QWidget host;
    host.setWindowTitle(QStringLiteral("macos_moltenvk_smoke"));
    auto* layout = new QHBoxLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0);
    host.show();
    QApplication::processEvents();

    // InitRenderTarget: dummy widget so Qt places the render window, then Vulkan.
    {
        const RenderWidget dummy_widget{&host};
    }

    auto* child = new VulkanRenderWidget(&host);
    if (!child->windowHandle()) {
        Fail("VulkanRenderWidget has no QWindow");
    }
    // Qt 6: QWindow::create() is void (same as bootmanager InitializeVulkan).
    child->windowHandle()->create();
    QApplication::processEvents();

    // Live WSI snapshot is taken before addWidget (InitRenderTarget order).
    const auto wsi = QtCommon::GetWindowSystemInfo(child->windowHandle());
    child->resize(Layout::ScreenUndocked::Width, Layout::ScreenUndocked::Height);
    layout->addWidget(child);
    host.resize(Layout::ScreenUndocked::Width, Layout::ScreenUndocked::Height);
    QApplication::processEvents();

    if (wsi.type != Core::Frontend::WindowSystemType::Cocoa) {
        Fail("window system is not Cocoa");
    }
    if (wsi.render_surface == nullptr) {
        Fail("GetWindowSystemInfo returned a null CAMetalLayer (before addWidget)");
    }
    std::cout << "macos_moltenvk_smoke: CAMetalLayer ok\n";

    try {
        Vulkan::vk::InstanceDispatch dld;
        const auto library = Vulkan::OpenLibrary();
        if (!library || !library->IsOpen()) {
            Fail("OpenLibrary did not load MoltenVK from suyu.app Frameworks");
        }
        const Vulkan::vk::Instance instance =
            Vulkan::CreateInstance(*library, dld, VK_API_VERSION_1_1, wsi.type);
        const Vulkan::vk::SurfaceKHR surface = Vulkan::CreateSurface(instance, wsi);
        std::cout << "macos_moltenvk_smoke: Vulkan surface ok\n";

        Vulkan::Device device = Vulkan::CreateDevice(instance, dld, *surface);
        if (!device.IsMoltenVK()) {
            Fail("CreateDevice did not select MoltenVK (driver=" + device.GetDriverName() + ")");
        }
        std::cout << "macos_moltenvk_smoke: driver=MoltenVK\n";
        std::cout << "macos_moltenvk_smoke: device ok\n";

        Vulkan::StateTracker state_tracker;
        Vulkan::Scheduler scheduler(device, state_tracker);
        Vulkan::Swapchain swapchain(*surface, device, scheduler, Layout::ScreenUndocked::Width,
                                    Layout::ScreenUndocked::Height);
        std::cout << "macos_moltenvk_smoke: swapchain ok\n";
        void(device.GetLogical().WaitIdle());
    } catch (const Vulkan::vk::Exception& exception) {
        Fail(std::string("Vulkan: ") + exception.what());
    } catch (const std::exception& exception) {
        Fail(std::string("exception: ") + exception.what());
    }

    std::cout << "macos_moltenvk_smoke: OK\n";
    Common::Log::Stop();
    return 0;
}
