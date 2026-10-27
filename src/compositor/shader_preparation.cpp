#define NOMINMAX
#include "compositor/shader_preparation.hpp"
#include <stdexcept>
#include <obs-module.h>
#include <util/platform.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#endif
#include "renderer_shader.hpp"
namespace glowbox {
namespace {
std::atomic<ShaderPreparation> state{ShaderPreparation::Idle};
std::atomic<bool> stopping{false};
std::atomic<uint64_t> started{0};
std::thread worker;
std::mutex start_mutex;
} // namespace
std::string native_effect_source() {
    std::string source;
    for (const char *part : kRendererShaderParts)
        source += part;
    return source;
}
ShaderPreparation shader_preparation_state() {
    return state.load(std::memory_order_acquire);
}
ShaderPreparation request_shader_preparation([[maybe_unused]] const std::string &support) {
    std::lock_guard<std::mutex> lock(start_mutex);
    if (state.load() != ShaderPreparation::Idle)
        return state.load();
#ifndef _WIN32
    state = ShaderPreparation::Unsupported;
#else
    // Background preparation requires OBS's shared D3D shader cache.

    if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11) {
        state = ShaderPreparation::Unsupported;
        return state.load();
    }
    const auto native = native_effect_source();
    if (native.empty() || support.empty()) {
        state = ShaderPreparation::Failed;
        return state.load();
    }
    obs_video_info video{};
    obs_get_video_info(&video);
    started = os_gettime_ns();
    state = ShaderPreparation::Pending;
    try {
        worker = std::thread([support, native, adapter = video.adapter] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            graphics_t *context = nullptr;
            bool ok = false;
            try {
                // If the cache cannot persist bytecode, main-context creation would
                // compile again and freeze OBS. Fail visibly instead of doing that.
                char *cache_path = os_get_program_data_path_ptr("obs-studio/shader-cache");
                if (!cache_path)
                    throw std::runtime_error("shader cache path unavailable");
                auto cache = std::filesystem::u8path(cache_path);
                bfree(cache_path);
                std::filesystem::create_directories(cache);
                auto probe = cache / ("glowbox-write-" + std::to_string(GetCurrentProcessId()) + ".tmp");
                {
                    std::ofstream file(probe, std::ios::binary);
                    file << "probe";
                    file.close();
                    if (!file)
                        throw std::runtime_error("shader cache is not writable");
                }
                std::filesystem::remove(probe);
                if (stopping.load())
                    return;
                if (gs_create(&context, "libobs-d3d11", adapter) != GS_SUCCESS)
                    throw std::runtime_error("preparation context unavailable");
                gs_enter_context(context);
                for (const auto *source : {&support, &native}) {
                    if (stopping.load())
                        break;
                    char *errors = nullptr;
                    auto *effect = gs_effect_create(source->c_str(), nullptr, &errors);
                    if (errors) {
                        blog(LOG_WARNING, "[Glowbox] shader preparation: %s", errors);
                        bfree(errors);
                    }
                    if (!effect)
                        throw std::runtime_error("shader compilation failed");
                    gs_effect_destroy(effect);
                }
                gs_leave_context();
                ok = !stopping.load();
            } catch (const std::exception &e) {
                blog(LOG_ERROR, "[Glowbox] shader preparation failed: %s", e.what());
            }
            // Destroy only the worker's resources. It never uses OBS's render context
            // or retains a source pointer.
            if (context)
                gs_destroy(context);
            state.store(ok ? ShaderPreparation::Ready : ShaderPreparation::Failed, std::memory_order_release);
            blog(ok ? LOG_INFO : LOG_ERROR, "[Glowbox] background shader preparation %s in %.1f ms",
                 ok ? "complete" : "failed", (os_gettime_ns() - started.load()) / 1000000.0);
        });
    } catch (const std::exception &e) {
        state = ShaderPreparation::Failed;
        blog(LOG_ERROR, "[Glowbox] could not start shader preparation: %s", e.what());
    }
#endif
    return state.load();
}
void stop_shader_preparation() {
    stopping = true;
    if (worker.joinable())
        worker.join();
    obs_enter_graphics();
    release_shader_preparation_badge();
    obs_leave_graphics();
}

namespace {
struct LoadingImage {
    uint8_t *pixels = nullptr;
    uint32_t width = 0, height = 0;
    gs_color_format format = GS_RGBA;
    gs_texture_t *texture = nullptr;
};
LoadingImage loading_images[3];

gs_texture_t *loading_texture(unsigned index) {
    auto &image = loading_images[index];
    if (image.pixels) {
        const uint8_t *data = image.pixels;
        image.texture = gs_texture_create(image.width, image.height, image.format, 1, &data, 0);
        bfree(image.pixels);
        image.pixels = nullptr;
    }
    return image.texture;
}
} // namespace
void load_loading_images() {
    const char *names[] = {"loading.png", "loading-failed.png", "spinner.png"};
    for (unsigned i = 0; i < 3; ++i) {
        auto &image = loading_images[i];
        char *path = obs_module_file(names[i]);
        if (path)
            image.pixels = gs_create_texture_file_data2(path, GS_IMAGE_ALPHA_PREMULTIPLY, &image.format, &image.width, &image.height);
        bfree(path);
    }
}
void release_shader_preparation_badge() {
    for (auto &image : loading_images) {
        gs_texture_destroy(image.texture);
        bfree(image.pixels);
        image = {};
    }
}

void draw_shader_preparation(uint32_t width, uint32_t height, bool attachment_failed) {
    const auto status = attachment_failed ? ShaderPreparation::Failed : shader_preparation_state();
    if (status != ShaderPreparation::Pending && status != ShaderPreparation::Failed)
        return;
    const double seconds = (os_gettime_ns() - started.load()) / 1e9;
    if (status == ShaderPreparation::Pending && seconds < .25)
        return;

    const bool failed = status == ShaderPreparation::Failed;
    auto *badge = loading_texture(failed ? 1 : 0);
    auto *spinner = failed ? nullptr : loading_texture(2);
    if (!badge)
        return;

    // Artwork is baked at twice its display size. Fit small canvases too.
    const float desired = std::clamp(std::min(width / 1280.f, height / 720.f), .5f, 1.5f);
    const float scale = std::min(desired, width / (gs_texture_get_width(badge) * .5f + 32.f));
    const bool srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(false);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    gs_matrix_push();
    gs_matrix_translate3f(16 * scale, 16 * scale, 0);
    gs_matrix_scale3f(scale, scale, 1);

    auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    const auto sprite = [&](gs_texture_t *texture, unsigned w, unsigned h) {
        gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), texture);
        while (gs_effect_loop(effect, "Draw"))
            gs_draw_sprite(texture, 0, w, h);
    };
    sprite(badge, gs_texture_get_width(badge) / 2, gs_texture_get_height(badge) / 2);
    if (spinner) {
        gs_matrix_translate3f(20, 20, 0);
        gs_matrix_rotaa4f(0, 0, 1, (float)(seconds * .7 * 6.283185307));
        gs_matrix_translate3f(-9, -9, 0);
        sprite(spinner, 18, 18);
    }

    gs_matrix_pop();
    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(srgb);
}
} // namespace glowbox
