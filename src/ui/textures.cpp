#include "textures.h"

#include <d3d11.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "../app/host.h"
#include "../core/fsx.h"
#include "../os/win.h"

namespace vs::ui {

namespace {

struct Entry {
    ID3D11ShaderResourceView* srv = nullptr;
    bool pending = false;
    bool failed = false;
    int64_t mtime = 0;
};

struct Job {
    std::string key;
    std::string path;
    int px;
    float round;
    bool exe;
    unsigned seed;
    bool placeholder;
    bool dark = false;
};

struct Done {
    std::string key;
    img::Image im;
};

std::unordered_map<std::string, Entry> g_cache;
std::mutex g_mtx;
std::condition_variable g_cv;
std::deque<Job> g_jobs;
std::vector<Done> g_done;
std::thread g_worker;
bool g_stop = false;
bool g_started = false;

void worker() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [] { return g_stop || !g_jobs.empty(); });
            if (g_stop) return;
            j = g_jobs.front();
            g_jobs.pop_front();
        }
        img::Image im;
        if (j.placeholder) {
            im = img::placeholder(j.seed, j.px, j.dark);
        } else if (j.exe) {
            int w = 0, h = 0;
            std::vector<uint8_t> rgba;
            if (os::extract_icon_rgba(j.path, j.px, w, h, rgba)) { im.w = w; im.h = h; im.rgba = std::move(rgba); }
        } else {
            img::load_file(j.path, im);
        }
        if (im.ok()) {
            im = img::crop_square(im);
            if (im.w != j.px) im = img::resize(im, j.px, j.px);
            if (j.round > 0) img::round_corners(im, j.round);
        }
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_done.push_back({j.key, std::move(im)});
        }
        ui_wake();
    }
}

void ensure_worker() {
    if (g_started) return;
    g_started = true;
    g_worker = std::thread(worker);
}

ID3D11ShaderResourceView* upload(const img::Image& im) {
    ID3D11Device* dev = d3d_device();
    if (!dev || !im.ok()) return nullptr;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = im.w;
    d.Height = im.h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = im.rgba.data();
    sd.SysMemPitch = im.w * 4;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, &sd, &tex)) || !tex) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = d.Format;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    dev->CreateShaderResourceView(tex, &sv, &srv);
    tex->Release();
    return srv;
}

ImTextureID request(const std::string& key, Job job, int64_t mtime) {
    auto it = g_cache.find(key);
    if (it != g_cache.end()) {
        if (it->second.mtime == mtime || it->second.pending) return (ImTextureID)it->second.srv;
        // file changed: reload, keep showing the old one meanwhile
        it->second.mtime = mtime;
    } else {
        Entry e;
        e.mtime = mtime;
        it = g_cache.emplace(key, e).first;
    }
    it->second.pending = true;
    ensure_worker();
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_jobs.push_back(std::move(job));
    }
    g_cv.notify_one();
    return (ImTextureID)it->second.srv;
}

}  // namespace

ImTextureID tex_file(const std::string& path, int px, float round) {
    if (path.empty()) return (ImTextureID)0;
    std::string key = "f|" + path + "|" + std::to_string(px) + "|" + std::to_string((int)(round * 100));
    auto it = g_cache.find(key);
    if (it != g_cache.end() && (it->second.pending || it->second.failed)) return (ImTextureID)it->second.srv;
    int64_t mt = it != g_cache.end() ? it->second.mtime : fsx::mtime_unix(path);
    return request(key, Job{key, path, px, round, false, 0, false}, mt);
}

ImTextureID tex_exe_icon(const std::string& exe, int px) {
    if (exe.empty()) return (ImTextureID)0;
    std::string key = "e|" + exe + "|" + std::to_string(px);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return (ImTextureID)it->second.srv;
    if (!fsx::exists(exe)) {
        Entry e;
        e.failed = true;
        g_cache.emplace(key, e);
        return (ImTextureID)0;
    }
    return request(key, Job{key, exe, px, 0.0f, true, 0, false}, 0);
}

ImTextureID tex_placeholder(unsigned seed, int px, float round, bool dark) {
    std::string key = std::string("p|") + (dark ? "d" : "l") + std::to_string(seed % 8) + "|" + std::to_string(px) + "|" + std::to_string((int)(round * 100));
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return (ImTextureID)it->second.srv;
    Job j{key, "", px, round, false, seed, true};
    j.dark = dark;
    return request(key, j, 0);
}

void tex_pump() {
    std::vector<Done> done;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        done.swap(g_done);
    }
    for (auto& d : done) {
        auto it = g_cache.find(d.key);
        if (it == g_cache.end()) continue;
        it->second.pending = false;
        if (!d.im.ok()) { it->second.failed = true; continue; }
        ID3D11ShaderResourceView* srv = upload(d.im);
        if (srv) {
            if (it->second.srv) it->second.srv->Release();
            it->second.srv = srv;
            it->second.failed = false;
        }
    }
}

void tex_forget(const std::string& path) {
    for (auto it = g_cache.begin(); it != g_cache.end();) {
        if (it->first.find("|" + path + "|") != std::string::npos && !it->second.pending) {
            if (it->second.srv) it->second.srv->Release();
            it = g_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void tex_clear() {
    for (auto it = g_cache.begin(); it != g_cache.end();) {
        if (!it->second.pending && it->first[0] == 'f') {
            if (it->second.srv) it->second.srv->Release();
            it = g_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void tex_shutdown() {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_stop = true;
    }
    g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();
    for (auto& [k, e] : g_cache)
        if (e.srv) e.srv->Release();
    g_cache.clear();
}

}  // namespace vs::ui
