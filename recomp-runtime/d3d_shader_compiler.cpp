// SPDX-License-Identifier: GPL-3.0-or-later
#include "d3d_shader_compiler.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <type_traits>

#ifdef RECOMP_SHADERC_LINKED
#include <shaderc/shaderc.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#ifdef __APPLE__
#include <spirv_cross/spirv_cross_c.h>
#endif

namespace {

/* shaderc's C API, which has been stable since its first release. Linux loads
   it at run time, so one build runs on distributions that name the library
   differently; Windows loads the shaderc.dll the build copies beside the
   runner; macOS links Homebrew's. */
using Compiler = void *;
using Options = void *;
using Result = void *;
enum { SourceHlsl = 1, StageVertex = 0, StageFragment = 1, StatusSuccess = 0, OptimizePerformance = 2 };

struct Shaderc {
    Compiler (*compiler_initialize)();
    Options (*options_initialize)();
    void (*options_release)(Options);
    void (*set_source_language)(Options, int);
    void (*set_auto_map_locations)(Options, bool);
    void (*set_hlsl_offsets)(Options, bool);
    void (*set_optimization_level)(Options, int);
    void (*add_macro_definition)(Options, const char *, size_t, const char *, size_t);
    Result (*compile_into_spv)(Compiler, const char *, size_t, int, const char *, const char *, Options);
    int (*result_status)(Result);
    const char *(*result_error)(Result);
    const char *(*result_bytes)(Result);
    size_t (*result_length)(Result);
    void (*result_release)(Result);
    Compiler compiler;
};

std::mutex shaderc_lock;
Shaderc shaderc;
bool shaderc_tried, shaderc_ready;

#ifdef RECOMP_SHADERC_LINKED
template <typename T, typename F>
void bind(T &slot, F function)
{
    slot = reinterpret_cast<T>(reinterpret_cast<void *>(function));
}
#endif

bool loadShaderc()
{
    if (shaderc_tried) return shaderc_ready;
    shaderc_tried = true;
#ifdef RECOMP_SHADERC_LINKED
    bind(shaderc.compiler_initialize, shaderc_compiler_initialize);
    bind(shaderc.options_initialize, shaderc_compile_options_initialize);
    bind(shaderc.options_release, shaderc_compile_options_release);
    bind(shaderc.set_source_language, shaderc_compile_options_set_source_language);
    bind(shaderc.set_auto_map_locations, shaderc_compile_options_set_auto_map_locations);
    bind(shaderc.set_hlsl_offsets, shaderc_compile_options_set_hlsl_offsets);
    bind(shaderc.set_optimization_level, shaderc_compile_options_set_optimization_level);
    bind(shaderc.add_macro_definition, shaderc_compile_options_add_macro_definition);
    bind(shaderc.compile_into_spv, shaderc_compile_into_spv);
    bind(shaderc.result_status, shaderc_result_get_compilation_status);
    bind(shaderc.result_error, shaderc_result_get_error_message);
    bind(shaderc.result_bytes, shaderc_result_get_bytes);
    bind(shaderc.result_length, shaderc_result_get_length);
    bind(shaderc.result_release, shaderc_result_release);
#else
#ifdef _WIN32
    // Only the pinned copy beside the runner; never the working directory or PATH.
    HMODULE library = LoadLibraryExA("shaderc.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    if (library == nullptr) {
        std::fprintf(stderr, "recomp shaders: the Vulkan renderer needs shaderc.dll beside the runner; "
            "build the game again\n");
        return false;
    }
    const auto symbol = [library](auto &slot, const char *name) {
        slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(
            reinterpret_cast<void *>(GetProcAddress(library, name)));
        return slot != nullptr;
    };
#else
    // SteamOS and Fedora ship libshaderc_shared; Debian and Ubuntu name it libshaderc.
    void *library = nullptr;
    for (const char *name : {"libshaderc_shared.so.1", "libshaderc_shared.so", "libshaderc.so.1"}) {
        library = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (library != nullptr) break;
    }
    if (library == nullptr) {
        std::fprintf(stderr, "recomp shaders: SDL_GPU shaders need shaderc (libshaderc_shared.so.1, "
            "or libshaderc.so.1 from the libshaderc1 package)\n");
        return false;
    }
    const auto symbol = [library](auto &slot, const char *name) {
        slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(dlsym(library, name));
        return slot != nullptr;
    };
#endif
    if (!symbol(shaderc.compiler_initialize, "shaderc_compiler_initialize") ||
        !symbol(shaderc.options_initialize, "shaderc_compile_options_initialize") ||
        !symbol(shaderc.options_release, "shaderc_compile_options_release") ||
        !symbol(shaderc.set_source_language, "shaderc_compile_options_set_source_language") ||
        !symbol(shaderc.set_auto_map_locations, "shaderc_compile_options_set_auto_map_locations") ||
        !symbol(shaderc.set_hlsl_offsets, "shaderc_compile_options_set_hlsl_offsets") ||
        !symbol(shaderc.set_optimization_level, "shaderc_compile_options_set_optimization_level") ||
        !symbol(shaderc.add_macro_definition, "shaderc_compile_options_add_macro_definition") ||
        !symbol(shaderc.compile_into_spv, "shaderc_compile_into_spv") ||
        !symbol(shaderc.result_status, "shaderc_result_get_compilation_status") ||
        !symbol(shaderc.result_error, "shaderc_result_get_error_message") ||
        !symbol(shaderc.result_bytes, "shaderc_result_get_bytes") ||
        !symbol(shaderc.result_length, "shaderc_result_get_length") ||
        !symbol(shaderc.result_release, "shaderc_result_release")) {
        std::fprintf(stderr, "recomp shaders: the loaded shaderc lacks a needed function\n");
        return false;
    }
#endif
    shaderc.compiler = shaderc.compiler_initialize();
    shaderc_ready = shaderc.compiler != nullptr;
    return shaderc_ready;
}

bool compileSpirv(const std::string &hlsl, const char *entry_point, bool vertex,
    const std::vector<std::string> &defines, std::vector<uint8_t> &spirv, std::string &error)
{
    std::lock_guard<std::mutex> lock(shaderc_lock);
    if (!loadShaderc()) {
        error = "shaderc unavailable";
        return false;
    }
    Options options = shaderc.options_initialize();
    if (options == nullptr) {
        error = "shaderc options";
        return false;
    }
    shaderc.set_source_language(options, SourceHlsl);
    // Constant buffers hold only float4 and float4x4, so HLSL and std140 offsets agree.
    shaderc.set_hlsl_offsets(options, true);
    shaderc.set_auto_map_locations(options, true);
    shaderc.set_optimization_level(options, OptimizePerformance);
    shaderc.add_macro_definition(options, "RECOMP_UNIFORM_SET", 18u, vertex ? "1" : "3", 1u);
    bool samples = false;
    for (const std::string &define : defines) samples |= define.rfind("RECOMP_SAMPLE_COUNT", 0) == 0;
    if (!samples) shaderc.add_macro_definition(options, "RECOMP_SAMPLE_COUNT", 19u, "4", 1u);
    for (const std::string &define : defines) {
        const size_t equals = define.find('=');
        const std::string name = define.substr(0, equals);
        const std::string value = equals == std::string::npos ? "1" : define.substr(equals + 1u);
        shaderc.add_macro_definition(options, name.data(), name.size(), value.data(), value.size());
    }
    Result result = shaderc.compile_into_spv(shaderc.compiler, hlsl.data(), hlsl.size(),
        vertex ? StageVertex : StageFragment, "presenter.hlsl", entry_point, options);
    shaderc.options_release(options);
    if (result == nullptr) {
        error = "shaderc returned no result";
        return false;
    }
    const bool compiled = shaderc.result_status(result) == StatusSuccess;
    if (compiled) {
        const auto *bytes = reinterpret_cast<const uint8_t *>(shaderc.result_bytes(result));
        spirv.assign(bytes, bytes + shaderc.result_length(result));
    } else {
        const char *message = shaderc.result_error ? shaderc.result_error(result) : nullptr;
        error = message != nullptr ? message : "shaderc failed";
    }
    shaderc.result_release(result);
    return compiled;
}

#ifdef __APPLE__
/* SDL_GPU's Metal slots: [[buffer(n)]] for uniform buffer n, and
   [[texture(n)]] with [[sampler(n)]] for sampler slot n. */
bool translateMsl(const std::vector<uint8_t> &spirv, bool vertex, RecompCompiledShader &out,
    std::string &error)
{
    spvc_context context = nullptr;
    if (spvc_context_create(&context) != SPVC_SUCCESS) {
        error = "spirv-cross context";
        return false;
    }
    const auto fail = [&](const char *what) {
        error = std::string(what) + ": " + spvc_context_get_last_error_string(context);
        spvc_context_destroy(context);
        return false;
    };
    spvc_parsed_ir ir = nullptr;
    if (spvc_context_parse_spirv(context, reinterpret_cast<const SpvId *>(spirv.data()),
            spirv.size() / sizeof(SpvId), &ir) != SPVC_SUCCESS) return fail("spirv-cross parse");
    spvc_compiler compiler = nullptr;
    if (spvc_context_create_compiler(context, SPVC_BACKEND_MSL, ir,
            SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) != SPVC_SUCCESS) return fail("spirv-cross compiler");
    spvc_compiler_options options = nullptr;
    if (spvc_compiler_create_compiler_options(compiler, &options) != SPVC_SUCCESS) return fail("spirv-cross options");
    spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_MSL_VERSION, SPVC_MAKE_MSL_VERSION(2, 1, 0));
    spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_MSL_PLATFORM, SPVC_MSL_PLATFORM_MACOS);
    if (spvc_compiler_install_compiler_options(compiler, options) != SPVC_SUCCESS) return fail("spirv-cross install");
    const SpvExecutionModel stage = vertex ? SpvExecutionModelVertex : SpvExecutionModelFragment;
    for (unsigned slot = 0u; slot < 8u; ++slot) {
        spvc_msl_resource_binding_2 binding;
        spvc_msl_resource_binding_init_2(&binding);
        binding.stage = stage;
        binding.desc_set = vertex ? 1u : 3u;
        binding.binding = slot;
        binding.count = 1u;
        binding.msl_buffer = slot;
        if (spvc_compiler_msl_add_resource_binding_2(compiler, &binding) != SPVC_SUCCESS)
            return fail("spirv-cross buffer binding");
        if (vertex) continue;
        spvc_msl_resource_binding_init_2(&binding);
        binding.stage = stage;
        binding.desc_set = 2u;
        binding.binding = slot;
        binding.count = 1u;
        binding.msl_texture = slot;
        binding.msl_sampler = slot;
        if (spvc_compiler_msl_add_resource_binding_2(compiler, &binding) != SPVC_SUCCESS)
            return fail("spirv-cross texture binding");
    }
    const spvc_entry_point *entry_points = nullptr;
    size_t entry_count = 0u;
    if (spvc_compiler_get_entry_points(compiler, &entry_points, &entry_count) != SPVC_SUCCESS ||
        entry_count == 0u) return fail("spirv-cross entry point");
    const char *source = nullptr;
    if (spvc_compiler_compile(compiler, &source) != SPVC_SUCCESS) return fail("spirv-cross compile");
    out.code.assign(source, source + std::strlen(source));
    out.entry_point = spvc_compiler_get_cleansed_entry_point_name(
        compiler, entry_points[0].name, entry_points[0].execution_model);
    spvc_context_destroy(context);
    return true;
}
#endif

} // namespace

namespace {

bool replaceOnce(std::string &text, const std::string &from, const std::string &to)
{
    const size_t at = text.find(from);
    if (at == std::string::npos) return false;
    text.replace(at, from.size(), to);
    return true;
}

/* Prefixes each member of struct name that has a non-system semantic with
   the next [[vk::location]], so both stages and the vertex layout agree
   whatever glslang keeps or drops. */
void locateMembers(std::string &text, const char *name)
{
    const std::string head = std::string("struct ") + name + " {";
    const size_t start = text.find(head);
    if (start == std::string::npos) return;
    const size_t open = start + head.size();
    const size_t close = text.find("};", open);
    if (close == std::string::npos) return;
    std::string body = text.substr(open, close - open), located;
    unsigned location = 0u;
    size_t member = 0u;
    for (size_t end; (end = body.find(';', member)) != std::string::npos; member = end + 1u) {
        std::string declaration = body.substr(member, end - member);
        const size_t first = declaration.find_first_not_of(" \t\r\n");
        const size_t colon = declaration.find(':');
        if (first != std::string::npos && colon != std::string::npos &&
            declaration.find("SV_", colon) == std::string::npos) {
            declaration.insert(first, "[[vk::location(" + std::to_string(location++) + ")]] ");
        }
        located += declaration + ";";
    }
    located += body.substr(member);
    text.replace(open, close - open, located);
}

} // namespace

std::string recomp_shader_draw_bindings(const std::string &hlsl)
{
    std::string text = hlsl;
    replaceOnce(text, "cbuffer Transform : register(b0) {",
        "[[vk::binding(0, RECOMP_UNIFORM_SET)]] cbuffer Transform : register(b0) {");
    if (replaceOnce(text, "    float4 vc[192];\n", "")) {
        const size_t transform = text.find("cbuffer Transform");
        const size_t close = text.find("}\n", transform);
        if (close != std::string::npos) {
            text.insert(close + 2u, "[[vk::binding(1, RECOMP_UNIFORM_SET)]] cbuffer ProgramConstants : register(b1) {\n"
                "    float4 vc[192];\n}\n");
        }
    }
    static const char *const resources[][2] = {
        {"Texture2D guest_texture", "[[vk::binding(0, 2)]] Texture2D guest_texture"},
        {"SamplerState guest_sampler", "[[vk::binding(0, 2)]] SamplerState guest_sampler"},
        {"Texture2D alpha_mask", "[[vk::binding(1, 2)]] Texture2D alpha_mask"},
        {"SamplerState mask_sampler", "[[vk::binding(1, 2)]] SamplerState mask_sampler"},
        {"Texture2D bump_map", "[[vk::binding(2, 2)]] Texture2D bump_map"},
        {"SamplerState bump_sampler", "[[vk::binding(2, 2)]] SamplerState bump_sampler"},
        {"Texture3D guest_volume : register(t3);", "[[vk::binding(3, 2)]] Texture3D guest_volume : register(t3);\n"
            "[[vk::binding(3, 2)]] SamplerState volume_sampler : register(s3);"},
    };
    for (const auto &resource : resources) replaceOnce(text, resource[0], resource[1]);
    for (size_t at; (at = text.find("guest_volume.Sample(guest_sampler")) != std::string::npos;) {
        text.replace(at, 33u, "guest_volume.Sample(volume_sampler");
    }
    /* glslang has no GetRenderTargetSamplePosition. The standard D3D patterns
       are also Vulkan's standard locations and Metal's defaults; the caller
       defines RECOMP_SAMPLE_COUNT (SDL_GPU has 2, 4 or 8 samples). */
    if (text.find("GetRenderTargetSamplePosition(") != std::string::npos) {
        const size_t depth = text.find("struct DepthOutput");
        if (depth != std::string::npos) {
            text.insert(depth,
                "float2 recomp_sample_position(uint i) {\n"
                "#if RECOMP_SAMPLE_COUNT == 2\n"
                "    const float2 p[2] = {float2(4, 4), float2(-4, -4)};\n"
                "#elif RECOMP_SAMPLE_COUNT == 8\n"
                "    const float2 p[8] = {float2(1, -3), float2(-1, 3), float2(5, 1), float2(-3, -5),\n"
                "        float2(-5, 5), float2(-7, -1), float2(3, 7), float2(7, -7)};\n"
                "#else\n"
                "    const float2 p[4] = {float2(-2, -6), float2(6, -2), float2(-6, 2), float2(2, 6)};\n"
                "#endif\n"
                "    return p[i % RECOMP_SAMPLE_COUNT] / 16.0f;\n"
                "}\n");
        }
        for (size_t at; (at = text.find("GetRenderTargetSamplePosition(")) != std::string::npos;) {
            text.replace(at, 30u, "recomp_sample_position(");
        }
    }
    /* Xbox diffuse is packed A8R8G8B8. Direct3D 11 reads it as B8G8R8A8 and
       hands the shader RGBA; SDL_GPU's UBYTE4 keeps memory order, BGRA. */
    for (size_t at = 0u; (at = text.find("input.diffuse", at)) != std::string::npos; at += 18u) {
        const char next = at + 13u < text.size() ? text[at + 13u] : '\0';
        if (std::isalnum(static_cast<unsigned char>(next)) || next == '_') continue;
        text.insert(at + 13u, ".zyxw");
    }
    locateMembers(text, "VSIn");
    locateMembers(text, "VSOut");
    return text;
}

std::string recomp_shader_emulate_sampler_bias(const std::string &hlsl)
{
    std::string text = hlsl;
    static const char *const textures[] = {"guest_texture", "alpha_mask", "bump_map", "guest_volume"};
    static const char *const slots[] = {"x", "y", "z", "x"};  // the volume shares slot 0's sampler
    for (unsigned i = 0u; i < 4u; ++i) {
        const std::string sample = std::string(textures[i]) + ".Sample";
        for (size_t at = 0u; (at = text.find(sample, at)) != std::string::npos; at += sample.size()) {
            const size_t name_end = at + sample.size();
            const bool plain = text.compare(name_end, 1u, "(") == 0;
            const bool biased = text.compare(name_end, 5u, "Bias(") == 0;
            if (!plain && !biased) continue;  // SampleLevel names its own level
            size_t close = text.find('(', name_end), depth = 0u;
            for (; close < text.size(); ++close) {
                if (text[close] == '(') ++depth;
                else if (text[close] == ')' && --depth == 0u) break;
            }
            if (close >= text.size()) continue;
            const std::string bias = std::string("recomp_sampler_bias.") + slots[i];
            text.insert(close, plain ? ", " + bias : " + " + bias);
            if (plain) text.insert(name_end, "Bias");
        }
    }
    const size_t prologue = text.find("struct VSIn");
    if (prologue != std::string::npos) {
        /* Both stages parse ps_main; only the fragment stage uses the buffer,
           so the vertex declaration never reaches its interface. */
        text.insert(prologue, "#if RECOMP_UNIFORM_SET == 3\n[[vk::binding(1, 3)]]\n#else\n"
            "[[vk::binding(2, 1)]]\n#endif\n"
            "cbuffer SamplerBias : register(b2) { float4 recomp_sampler_bias; }\n");
    }
    return text;
}

bool recomp_shader_compiler_available()
{
    std::lock_guard<std::mutex> lock(shaderc_lock);
    return loadShaderc();
}

static bool compileUncached(const std::string &hlsl, const char *entry_point, bool vertex,
    RecompShaderTarget target, const std::vector<std::string> &defines,
    RecompCompiledShader &out, std::string &error);

namespace {

/* RECOMP_SHADER_CACHE names a folder for compiled shaders, so a later launch
   skips shaderc and SPIRV-Cross. Entries are keyed by everything that goes
   into a compile; a missing or unreadable entry is simply compiled again. */
std::string cachePath(const std::string &hlsl, const char *entry_point, bool vertex,
    RecompShaderTarget target, const std::vector<std::string> &defines)
{
    const char *folder = std::getenv("RECOMP_SHADER_CACHE");
    if (folder == nullptr || folder[0] == '\0') return {};
    uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&hash](const std::string &text) {
        for (const unsigned char c : text) hash = (hash ^ c) * 0x100000001b3ull;
        hash = (hash ^ 0xffu) * 0x100000001b3ull;
    };
    mix("doa2u-shader-cache-1");
    mix(hlsl);
    mix(entry_point);
    mix(vertex ? "vertex" : "fragment");
    for (const std::string &define : defines) mix(define);
    char name[48];
    std::snprintf(name, sizeof name, "/%016llx.%s", static_cast<unsigned long long>(hash),
        target == RecompShaderTarget::Msl ? "msl" : "spv");
    return std::string(folder) + name;
}

/* An entry is the entry point name, a newline, then the code. */
bool readCache(const std::string &path, RecompCompiledShader &out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file || !std::getline(file, out.entry_point) || out.entry_point.empty()) return false;
    out.code.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !out.code.empty();
}

void writeCache(const std::string &path, const RecompCompiledShader &shader)
{
    // Written beside the entry and renamed, so a reader never sees half of one.
    const std::string partial = path + ".tmp" + std::to_string(reinterpret_cast<uintptr_t>(&shader));
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        if (!file) return;
        file << shader.entry_point << '\n';
        file.write(reinterpret_cast<const char *>(shader.code.data()),
            static_cast<std::streamsize>(shader.code.size()));
        if (!file) return;
    }
    std::rename(partial.c_str(), path.c_str());
}

} // namespace

bool recomp_shader_compile(const std::string &hlsl, const char *entry_point, bool vertex,
    RecompShaderTarget target, const std::vector<std::string> &defines,
    RecompCompiledShader &out, std::string &error)
{
    const std::string cached = cachePath(hlsl, entry_point, vertex, target, defines);
    if (!cached.empty() && readCache(cached, out)) return true;
    if (!compileUncached(hlsl, entry_point, vertex, target, defines, out, error)) return false;
    if (!cached.empty()) writeCache(cached, out);
    return true;
}

static bool compileUncached(const std::string &hlsl, const char *entry_point, bool vertex,
    RecompShaderTarget target, const std::vector<std::string> &defines,
    RecompCompiledShader &out, std::string &error)
{
    std::vector<uint8_t> spirv;
    if (!compileSpirv(hlsl, entry_point, vertex, defines, spirv, error)) return false;
    if (target == RecompShaderTarget::Spirv) {
        out.code = std::move(spirv);
        // shaderc names the SPIR-V entry point after the HLSL function.
        out.entry_point = entry_point;
        return true;
    }
#ifdef __APPLE__
    return translateMsl(spirv, vertex, out, error);
#else
    error = "MSL output needs SPIRV-Cross, built only for Apple hosts";
    return false;
#endif
}
