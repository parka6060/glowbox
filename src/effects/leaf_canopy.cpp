#include "effects/leaf_canopy.hpp"
#include "compositor/compositor.hpp"
#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <util/platform.h>

namespace glowbox {
namespace {
using Key = std::array<uint32_t, 6>;
constexpr unsigned field_size = 1024, broad_size = 192;
constexpr const char *keys[] = {"canopy_coverage", "canopy_curve",     "canopy_forks",
                                "canopy_clumping", "canopy_leaf_size", "canopy_seed"};
Key key(float coverage, float curve, float forks, float clumping, float size, uint32_t seed) {
    const auto q = [](float v, float low, float high) {
        return (uint32_t)std::lround(std::clamp(std::isfinite(v) ? v : low, low, high) * 100.f);
    };
    return {q(coverage, 0, 1), q(curve, 0, 1), q(forks, 0, 1), q(clumping, 0, 1), q(size, .5f, 1.5f), seed};
}
Key key(const Compositor *f) {
    return key(f->lighting.canopy_coverage, f->lighting.canopy_curve, f->lighting.canopy_forks,
               f->lighting.canopy_clumping, f->lighting.canopy_leaf_size, f->lighting.canopy_seed);
}
struct Mesh {
    std::vector<vec3> points;
    std::vector<vec4> shape, motion;
    std::vector<uint32_t> indices;
    unsigned leaves = 0;
    double ms = 0;
};
struct Random {
    uint32_t a;
    double operator()() {
        uint32_t t = (a += 0x6d2b79f5u);
        t = (t ^ (t >> 15)) * (t | 1);
        t ^= t + ((t ^ (t >> 7)) * (t | 61));
        return double(t ^ (t >> 14)) / 4294967296.0;
    }
};
double mix(double a, double b, double t) {
    return a + (b - a) * t;
}
double smooth(double a, double b, double x) {
    double t = std::clamp((x - a) / (b - a), 0., 1.);
    return t * t * (3 - 2 * t);
}
struct Point {
    double x, y, a = 0;
};
struct Generator {
    Key config;
    Mesh mesh;
    double coverage, curve, forks, clumping, size;
    Random random(uint32_t id) {
        return {uint32_t(config[5]) * 92821u + id * 68917u};
    }
    void vertex(double x, double y, double u, double v, int layer, bool leaf, Point pivot, double phase) {
        vec3 p;
        vec3_set(&p, (float)(x / 1024), (float)(y / 1024), 0);
        mesh.points.push_back(p);
        mesh.shape.push_back({(float)u, (float)v, (float)layer, leaf ? 1.f : 0.f});
        mesh.motion.push_back({(float)(pivot.x / 1024), (float)(pivot.y / 1024), (float)phase, 0});
    }
    void quad(const std::array<Point, 4> &p, int layer, bool leaf, Point pivot, double phase) {
        double minx = p[0].x, maxx = p[0].x, miny = p[0].y, maxy = p[0].y;
        for (auto a : p) {
            minx = std::min(minx, a.x);
            maxx = std::max(maxx, a.x);
            miny = std::min(miny, a.y);
            maxy = std::max(maxy, a.y);
        }
        if (maxx < -1100 || maxy < -1100 || minx > 2124 || miny > 2124)
            return;
        constexpr int indices[] = {0, 1, 2, 0, 2, 3};
        constexpr double uv[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        uint32_t first = (uint32_t)mesh.points.size();
        for (int i = 0; i < 4; ++i)
            vertex(p[i].x, p[i].y, uv[i][0], uv[i][1], layer, leaf, pivot, phase);
        for (int i : indices)
            mesh.indices.push_back(first + i);
    }
    void stem(Point a, Point b, double wa, double wb, int layer) {
        double len = std::hypot(b.x - a.x, b.y - a.y);
        if (len < .01)
            return;
        double nx = -(b.y - a.y) / len * .5, ny = (b.x - a.x) / len * .5;
        quad({Point{a.x + nx * wa, a.y + ny * wa}, Point{b.x + nx * wb, b.y + ny * wb},
              Point{b.x - nx * wb, b.y - ny * wb}, Point{a.x - nx * wa, a.y - ny * wa}},
             layer, false, {}, 0);
    }
    void leaf(Point p, double angle, double length, double width, int layer, double phase) {
        if (length < .1)
            return;
        double ca = std::cos(angle), sa = std::sin(angle), w = length * width * .5;
        auto at = [&](double x, double y) { return Point{p.x + x * ca - y * sa, p.y + x * sa + y * ca}; };
        size_t before = mesh.points.size();
        quad({at(0, -w), at(length, -w), at(length, w), at(0, w)}, layer, true, p, phase);
        if (mesh.points.size() != before)
            ++mesh.leaves;
    }
    void branch(double x, double y, double angle, double length, double width, int order, uint32_t id,
                int layer) {
        if (length < 5 || order > 4)
            return;
        auto r = random(id);
        double bend = (r() - .5) * curve * 1.65;
        double cx = x + std::cos(angle) * length * .52, cy = y + std::sin(angle) * length * .52;
        double ex = x + std::cos(angle + bend) * length, ey = y + std::sin(angle + bend) * length;
        auto point = [&](double t) {
            double s = 1 - t;
            return Point{s * s * x + 2 * s * t * cx + t * t * ex, s * s * y + 2 * s * t * cy + t * t * ey,
                         std::atan2(s * (cy - y) + t * (ey - cy), s * (cx - x) + t * (ex - cx))};
        };
        Point previous{x, y};
        for (int i = 1; i <= 12; ++i) {
            auto p = point(i / 12.);
            stem(previous, p, std::max(.4, width * mix(1, .52, (i - 1) / 12.)),
                 std::max(.4, width * mix(1, .52, i / 12.)), order == 0 ? 3 : layer);
            previous = p;
        }
        if (order >= 1) {
            int count = std::max(3, (int)std::ceil(length / (9 + 5 * size)));
            double centre = .25 + r() * .6;
            for (int j = 0; j < count; ++j) {
                auto q = random(id * 131 + j + 1);
                double t = (j + .3 + q() * .4) / count;
                auto p = point(t);
                double cluster = std::exp(-std::pow((t - centre) / (.16 + .22 * (1 - clumping)), 2));
                double qa = q(), qb = q(), active = smooth(qa * .38, qb * .38 + .45, coverage);
                double a = p.a + (j % 2 ? 1 : -1) * (.65 + q() * .5);
                double shoot = (10 + q() * 25) * (order == 2 ? 1.2 : .8) * (1 + clumping * cluster) * active;
                Point tip{p.x + std::cos(a) * shoot, p.y + std::sin(a) * shoot};
                stem(p, tip, .55, .55, layer);
                int pairs = 2 + (int)std::floor(clumping * cluster * 3 + q() * 2);
                for (int k = 0; k < pairs; ++k) {
                    double u = (k + .6) / pairs;
                    Point base{mix(p.x, tip.x, u), mix(p.y, tip.y, u)};
                    for (int sign : {-1, 1}) {
                        double la = a + sign * (.5 + q() * .7),
                               len = (11 + q() * 15) * size * (.65 + active * .35) * (1 - .08 * order);
                        double petiole = 2 + q() * 3;
                        Point anchor{base.x + std::cos(la) * petiole, base.y + std::sin(la) * petiole};
                        stem(base, anchor, .4, .4, layer);
                        double w = .32 + q() * .23;
                        // Derive animation identity without consuming the layout's random stream.
                        double phase = double((id * 73 + j * 17 + k * 11 + (sign + 1)) % 997) * .031;
                        leaf(anchor, la, len * active, w, layer, phase);
                    }
                }
            }
        }
        if (order == 4)
            return;
        auto end = point(1);
        double ca = end.a + (r() - .5) * .5, cl = length * (.58 + r() * .19);
        branch(ex, ey, ca, cl, width * .64, order + 1, id * 7 + 1, layer);
        int n = 2 + (r() < forks ? 1 : 0);
        for (int k = 0; k < n; ++k) {
            double t = .26 + (k + .3 + r() * .45) / n * .67;
            auto p = point(t);
            int side = (k + id) % 2 ? 1 : -1;
            double activation = smooth(.05 + k * .11, .45 + k * .13, coverage);
            double len = length * (.35 + r() * .32) * (.4 + .6 * activation);
            double a = p.a + side * (.35 + r() * .75), w = width * (.36 + r() * .19);
            branch(p.x, p.y, a, len, w, order + 1, id * 7 + k + 2, layer);
        }
    }
    Mesh run(Key k) {
        const uint64_t start = os_gettime_ns();
        config = k;
        coverage = k[0] * .01;
        curve = k[1] * .01;
        forks = k[2] * .01;
        clumping = k[3] * .01;
        size = k[4] * .01;
        mesh.points.reserve(180000);
        mesh.shape.reserve(180000);
        mesh.motion.reserve(180000);
        constexpr double roots[][5] = {{-440, -340, .43, 580, 9},
                                       {-460, -20, .36, 620, 8},
                                       {-430, 330, .27, 550, 7},
                                       {-470, 650, .36, 590, 8},
                                       {-420, 900, .08, 490, 6}};
        for (int i = 0; i < 5; ++i) {
            auto r = random(100 + i);
            const auto &v = roots[i];
            double a = v[2] + (r() - .5) * .17;
            stem({v[0] - std::cos(a) * 2048, v[1] - std::sin(a) * 2048}, {v[0], v[1]}, v[4], v[4], 3);
            branch(v[0], v[1], a, v[3] * (.78 + coverage * .62), v[4], 0, 100 + i, i % 3);
        }
        mesh.points.shrink_to_fit();
        mesh.shape.shrink_to_fit();
        mesh.motion.shrink_to_fit();
        mesh.indices.shrink_to_fit();
        mesh.ms = (os_gettime_ns() - start) * 1e-6;
        return std::move(mesh);
    }
};
} // namespace
struct CanopyJob {
    Key config;
    std::atomic<bool> ready{false};
    Mesh mesh;
};
namespace {
struct Jobs {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::shared_ptr<CanopyJob>> cache, pending;
    std::thread worker;
    bool stopping = false;
    ~Jobs() {
        stop();
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            pending.clear();
        }
        cv.notify_one();
        if (worker.joinable())
            worker.join();
        cache.clear();
    }
    std::shared_ptr<CanopyJob> request(Key k) {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto &job : cache)
            if (job->config == k)
                return job;
        if (stopping)
            return {};
        auto job = std::make_shared<CanopyJob>();
        job->config = k;
        cache.push_back(job);
        pending.push_back(job);
        // In-use jobs survive eviction through their source/runtime owners.
        while (cache.size() > 3)
            cache.pop_front();
        if (!worker.joinable())
            worker = std::thread([this] {
                for (;;) {
                    std::shared_ptr<CanopyJob> job;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        cv.wait(lock, [&] { return stopping || !pending.empty(); });
                        if (stopping)
                            return;
                        job = pending.front();
                        pending.pop_front();
                    }
                    // Slider drags supersede jobs before they start. A pending
                    // job held only by this queue has no consumer anymore.
                    if (job.use_count() == 1)
                        continue;
                    try {
                        job->mesh = Generator{}.run(job->config);
                    } catch (...) {
                        job->mesh = {};
                    }
                    job->ready.store(true, std::memory_order_release);
                }
            });
        cv.notify_one();
        return job;
    }
};
Jobs jobs;
gs_vertbuffer_t *upload(const Mesh &mesh) {
    if (mesh.points.empty())
        return nullptr;
    auto *data = gs_vbdata_create();
    data->num = mesh.points.size();
    data->points = (vec3 *)bmemdup(mesh.points.data(), data->num * sizeof(vec3));
    data->num_tex = 2;
    data->tvarray = (gs_tvertarray *)bzalloc(2 * sizeof(gs_tvertarray));
    data->tvarray[0] = {4, bmemdup(mesh.shape.data(), data->num * sizeof(vec4))};
    data->tvarray[1] = {4, bmemdup(mesh.motion.data(), data->num * sizeof(vec4))};
    return gs_vertexbuffer_create(data, 0);
}
} // namespace
namespace {
// Fit the mask to every visible receiver plane, rather than clipping foliage
// at a fixed square when the user moves or rotates the Light source.
vec4 canopy_window(const Compositor *f) {
    float low_x = 0, low_y = 0, high_x = 1, high_y = 1;
    const float aspect = float(f->width) / std::max(1u, f->height);
    const float a = f->lighting.spotlight_direction * .01745329252f;
    const vec2 travel = {-std::sin(a), std::cos(a)};
    const auto emitter = spotlight_emitter_position(f, aspect);
    const float diameter = std::max(.01f, f->lighting.spotlight_size * 2);
    for (float plane : {1.f, spotlight_plane_scale(f)})
        for (float y : {0.f, 1.f})
            for (float x : {0.f, 1.f}) {
                const float reference_x = scene_view_active(f->view) ? x + f->view.x / (aspect * plane) : x;
                const float reference_y = scene_view_active(f->view) ? y + f->view.y / plane : y;
                float px = (reference_x - f->lighting.spotlight_x) * aspect - travel.x * (1 - plane) * .8f;
                float py = reference_y - f->lighting.spotlight_y - travel.y * (1 - plane) * .8f;
                if (f->lighting.light_type < .5f) {
                    px = (emitter.x + (reference_x - emitter.x) / plane - f->lighting.spotlight_x) * aspect;
                    py = emitter.y + (reference_y - emitter.y) / plane - f->lighting.spotlight_y;
                }
                const float qx = (px * travel.y - py * travel.x) / diameter * .5625f + .5f;
                const float qy = (px * travel.x + py * travel.y) / diameter + .5f;
                low_x = std::min(low_x, qx);
                high_x = std::max(high_x, qx);
                low_y = std::min(low_y, qy);
                high_y = std::max(high_y, qy);
            }
    float size = std::max(high_x - low_x, high_y - low_y);
    if (size < 1.00001f)
        return {0, 0, 1, 1};
    size *= 1.004f; // Filtering guard, outside the visible scene.
    return {(low_x + high_x - size) * .5f, (low_y + high_y - size) * .5f, size, size};
}
} // namespace
struct LeafCanopy {
    std::shared_ptr<CanopyJob> requested, current;
    gs_vertbuffer_t *mesh = nullptr;
    gs_indexbuffer_t *indices = nullptr;
    gs_texrender_t *mips[8] = {}, *fine = nullptr, *wide = nullptr;
    vec4 window = {0, 0, 1, 1}, history_window = {0, 0, 1, 1};
    float last_scale = -1, last_rotation = -999;
    float last_time = -999, last_sway = -1, last_flutter = -1, last_softness = -1, last_depth = -1;
    bool ready = false, blending = false;
    double change_time = 0;
    gs_texture_t *history[2] = {};
    gs_texrender_t *blended[2] = {};
};
std::shared_ptr<CanopyJob> request_canopy(obs_data_t *s) {
    float v[5];
    for (int i = 0; i < 5; ++i)
        v[i] = (float)obs_data_get_double(s, keys[i]);
    uint32_t seed = (uint32_t)std::clamp(obs_data_get_int(s, "canopy_seed"), 0LL, 4294967295LL);
    return jobs.request(key(v[0], v[1], v[2], v[3], v[4], seed));
}
bool same_canopy(const Compositor *a, const Compositor *b) {
    return key(a) == key(b);
}
void prepare_leaf_canopy(Compositor *f) {
    if (f->lighting.light_pattern != 1 || f->lighting.light_opacity <= 0 ||
        (f->lighting.spotlight_surface <= 0 && f->lighting.rays_amount <= 0))
        return;
    if (!f->leaf_canopy)
        f->leaf_canopy = new LeafCanopy;
    auto &c = *f->leaf_canopy;
    Key wanted = key(f);
    if (!c.requested || c.requested->config != wanted)
        c.requested = jobs.request(wanted);
    if (c.requested && c.requested != c.current && c.requested->ready.load(std::memory_order_acquire)) {
        if (auto *next = upload(c.requested->mesh)) {
            auto *indices =
                gs_indexbuffer_create(GS_UNSIGNED_LONG,
                                      bmemdup(c.requested->mesh.indices.data(),
                                              c.requested->mesh.indices.size() * sizeof(uint32_t)),
                                      c.requested->mesh.indices.size(), 0);
            if (!indices) {
                gs_vertexbuffer_destroy(next);
                return;
            }
            if (c.ready) {
                for (int i = 0; i < 2; ++i) {
                    const unsigned size = i ? broad_size : 768;
                    if (!c.history[i])
                        c.history[i] = gs_texture_create(size, size, GS_RGBA, 1, nullptr, 0);
                    if (c.history[i])
                        gs_copy_texture(c.history[i], gs_texrender_get_texture(
                                                          c.blending ? c.blended[i] : (i ? c.wide : c.fine)));
                }
                c.history_window = c.window;
                c.blending = c.history[0] && c.history[1];
                c.change_time = light_animation_seconds();
            }
            gs_vertexbuffer_destroy(c.mesh);
            gs_indexbuffer_destroy(c.indices);
            c.mesh = next;
            c.indices = indices;
            c.current = c.requested;
            c.last_time = -999;
            blog(LOG_DEBUG, "[Glowbox] Canopy: %u leaves, %zu vertices, %.2f ms CPU generation",
                 c.current->mesh.leaves, c.current->mesh.points.size(), c.current->mesh.ms);
        }
    }
    if (!c.mesh)
        return;
    const vec4 window = canopy_window(f);
    const bool same_window = window.x == c.window.x && window.y == c.window.y && window.z == c.window.z;
    const float time =
        (float)(light_animation_seconds() * f->lighting.leaf_speed + f->lighting.leaf_time_offset);
    if (c.ready && same_window && !c.blending && time == c.last_time &&
        c.last_sway == f->lighting.leaf_sway && c.last_flutter == f->lighting.leaf_shimmer &&
        c.last_softness == f->lighting.leaf_softness && c.last_depth == f->lighting.leaf_depth &&
        c.last_scale == f->lighting.canopy_scale && c.last_rotation == f->lighting.canopy_rotation)
        return;
    auto *e = f->effect;
    auto param = [&](const char *name) { return gs_effect_get_param_by_name(e, name); };
    if (!gs_effect_get_technique(e, "CanopyGeometry"))
        return;
    c.window = window;
    gs_effect_set_vec4(param("canopy_window"), &c.window);
    gs_effect_set_float(param("canopy_scale"), f->lighting.canopy_scale);
    gs_effect_set_float(param("canopy_angle"), f->lighting.canopy_rotation * .01745329252f);
    gs_effect_set_float(param("canopy_clock"), time);
    gs_effect_set_float(param("canopy_sway"), f->lighting.leaf_sway);
    gs_effect_set_float(param("canopy_flutter"), f->lighting.leaf_shimmer);
    gs_effect_set_float(param("canopy_depth"), f->lighting.leaf_depth);
    const bool srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(false);
    gs_blend_state_push();
    auto begin = [&](gs_texrender_t *&target, unsigned size) {
        if (!target)
            target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        gs_texrender_reset(target);
        if (!gs_texrender_begin(target, size, size))
            return false;
        vec4 zero{};
        gs_clear(GS_CLEAR_COLOR, &zero, 0, 0);
        gs_ortho(0, 1, 0, 1, -100, 100);
        return true;
    };
    bool ok = begin(c.mips[0], field_size);
    if (ok) {
        gs_blend_op(GS_BLEND_OP_MAX);
        gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
        gs_load_vertexbuffer(c.mesh);
        gs_load_indexbuffer(c.indices);
        while (gs_effect_loop(e, "CanopyGeometry"))
            gs_draw(GS_TRIS, 0, 0);
        gs_load_vertexbuffer(nullptr);
        gs_load_indexbuffer(nullptr);
        gs_texrender_end(c.mips[0]);
    }
    gs_blend_op(GS_BLEND_OP_ADD);
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    for (int i = 1; ok && i < 8; ++i) {
        gs_effect_set_texture(param("canopy_input"), gs_texrender_get_texture(c.mips[i - 1]));
        ok = begin(c.mips[i], field_size >> i);
        if (ok) {
            while (gs_effect_loop(e, "CanopyDownsample"))
                gs_draw_sprite(nullptr, 0, 1, 1);
            gs_texrender_end(c.mips[i]);
        }
    }
    for (int i = 0; ok && i < 8; ++i) {
        const auto name = std::string("canopy_mip") + std::to_string(i);
        gs_effect_set_texture(param(name.c_str()), gs_texrender_get_texture(c.mips[i]));
    }
    float s = f->lighting.leaf_softness, d = f->lighting.leaf_depth;
    const float base = .6f + s * 3.4f - std::log2(c.window.z);
    auto resolve = [&](gs_texrender_t *&target, unsigned size, float extra) {
        vec4 lod = {std::clamp(base + extra, 0.f, 7.f), std::clamp(base + extra + d * 1.1f, 0.f, 7.f),
                    std::clamp(base + extra + d * 2.1f, 0.f, 7.f),
                    std::clamp(base + extra + d * 2.9f, 0.f, 7.f)};
        gs_effect_set_vec4(param("canopy_lod"), &lod);
        if (!begin(target, size))
            return false;
        while (gs_effect_loop(e, "CanopyResolve"))
            gs_draw_sprite(nullptr, 0, 1, 1);
        gs_texrender_end(target);
        return true;
    };
    if (ok)
        ok = resolve(c.fine, 768, 0) && resolve(c.wide, broad_size, 1.7f);
    if (ok && c.blending) {
        float t = (float)std::clamp((light_animation_seconds() - c.change_time) / .18, 0., 1.);
        t = t * t * (3 - 2 * t);
        gs_effect_set_float(param("canopy_blend"), t);
        gs_effect_set_vec4(param("canopy_previous_window"), &c.history_window);
        for (int i = 0; ok && i < 2; ++i) {
            gs_effect_set_texture(param("canopy_input"), gs_texrender_get_texture(i ? c.wide : c.fine));
            gs_effect_set_texture(param("canopy_previous"), c.history[i]);
            ok = begin(c.blended[i], i ? broad_size : 768);
            if (ok) {
                while (gs_effect_loop(e, "CanopyBlend"))
                    gs_draw_sprite(nullptr, 0, 1, 1);
                gs_texrender_end(c.blended[i]);
            }
        }
        if (t >= 1)
            c.blending = false;
    }
    gs_blend_state_pop();
    gs_enable_framebuffer_srgb(srgb);
    c.ready = ok;
    if (ok) {
        c.last_time = time;
        c.last_sway = f->lighting.leaf_sway;
        c.last_flutter = f->lighting.leaf_shimmer;
        c.last_softness = s;
        c.last_depth = d;
        c.last_scale = f->lighting.canopy_scale;
        c.last_rotation = f->lighting.canopy_rotation;
    }
}
void bind_leaf_canopy(const Compositor *f, gs_effect_t *e, gs_texture_t *fallback) {
    const auto *c = f->leaf_canopy;
    bool ready = c && c->ready;
    gs_effect_set_texture(gs_effect_get_param_by_name(e, "leaf_canopy"),
                          ready ? gs_texrender_get_texture(c->blending ? c->blended[0] : c->fine) : fallback);
    gs_effect_set_texture(gs_effect_get_param_by_name(e, "leaf_canopy_soft"),
                          ready ? gs_texrender_get_texture(c->blending ? c->blended[1] : c->wide) : fallback);
    const vec4 window = ready ? c->window : vec4{0, 0, 1, 1};
    gs_effect_set_vec4(gs_effect_get_param_by_name(e, "leaf_window"), &window);
    gs_effect_set_float(gs_effect_get_param_by_name(e, "light_pattern"), f->lighting.light_pattern);
    if (f->lighting.light_pattern == 1 && !ready) {
        gs_effect_set_float(gs_effect_get_param_by_name(e, "spotlight_amount"), 0);
        gs_effect_set_float(gs_effect_get_param_by_name(e, "rays_amount"), 0);
        gs_effect_set_float(gs_effect_get_param_by_name(e, "light_opacity"), 0);
    }
}
void destroy_leaf_canopy(Compositor *f) {
    if (!f->leaf_canopy)
        return;
    auto &c = *f->leaf_canopy;
    gs_vertexbuffer_destroy(c.mesh);
    gs_indexbuffer_destroy(c.indices);
    for (auto *target : c.mips)
        gs_texrender_destroy(target);
    gs_texrender_destroy(c.fine);
    gs_texrender_destroy(c.wide);
    for (auto *target : c.blended)
        gs_texrender_destroy(target);
    for (auto *texture : c.history)
        gs_texture_destroy(texture);
    delete f->leaf_canopy;
    f->leaf_canopy = nullptr;
}
void stop_canopy_worker() {
    jobs.stop();
}
} // namespace glowbox
