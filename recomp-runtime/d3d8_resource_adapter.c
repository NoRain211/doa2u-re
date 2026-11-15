/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_resource_adapter.h"
#include "d3d8_adapter.h"
#include "kernel_abi.h"
#include "stop_report.h"

#include <string.h>

enum {
    FORMAT_TABLE = 0x003530b8u, DIRTY = 0x00355058u,
    STREAMS = 0x003579a8u, BIND_REF = 0x80000u,
    TYPE_MASK = 0x70000u, RETAIN_MASK = 0x780000u,
    OWNED_SLOTS = 4096u,
};

typedef struct OwnedResource {
    uint32_t header;
    uint32_t allocation;
} OwnedResource;
static OwnedResource owned[OWNED_SLOTS];
static bool inline_texture_active;
static RecompD3dTextureDesc inline_texture;

static uint32_t word(uint32_t address) { return *recomp_memory_u32(address); }
static void put(uint32_t address, uint32_t value) { *recomp_memory_u32(address) = value; }
static uint32_t arg(uint32_t index) { return kernel_arg(index + 1u); }

void recomp_d3d8_resource_reset(void)
{
    for (uint32_t i = 0u; i < OWNED_SLOTS; ++i) {
        if (owned[i].header != 0u) {
            if (owned[i].allocation != 0u) recomp_kernel_free_pool(owned[i].allocation);
            recomp_kernel_free_pool(owned[i].header);
        }
    }
    memset(owned, 0, sizeof owned);
    inline_texture_active = false;
}

/* Guest RAM holds the headers and the pool their storage. A rewind that kept
   this table as it was would stop on a resource whose release it undoes, and
   list twice one whose creation runs again. */
typedef struct ResourceState {
    OwnedResource owned[OWNED_SLOTS];
    RecompD3dTextureDesc inline_texture;
    bool inline_texture_active;
} ResourceState;

size_t recomp_d3d8_resource_state_size(void) { return sizeof(ResourceState); }

void recomp_d3d8_resource_state_save(void *state)
{
    ResourceState *out = state;
    /* Padding and the texture while unused stay zero, so stale bytes never reach SyncTest's compare. */
    memset(out, 0, sizeof *out);
    memcpy(out->owned, owned, sizeof owned);
    if (inline_texture_active) out->inline_texture = inline_texture;
    out->inline_texture_active = inline_texture_active;
}

void recomp_d3d8_resource_state_restore(const void *state)
{
    const ResourceState *in = state;
    memcpy(owned, in->owned, sizeof owned);
    inline_texture = in->inline_texture;
    inline_texture_active = in->inline_texture_active;
}

static OwnedResource *ownership(uint32_t header)
{
    for (uint32_t i = 0u; i < OWNED_SLOTS; ++i) {
        if (owned[i].header == header) return &owned[i];
    }
    return NULL;
}

static bool own(uint32_t header, uint32_t allocation)
{
    for (uint32_t i = 0u; i < OWNED_SLOTS; ++i) {
        if (owned[i].header == 0u) {
            owned[i] = (OwnedResource){header, allocation};
            return true;
        }
    }
    return false;
}

static uint32_t common(uint32_t resource)
{
    if (resource == 0u) recomp_stop(2, "d3d8:resource-null");
    return word(resource);
}

static void destroy(uint32_t resource)
{
    OwnedResource *entry = ownership(resource);
    /* shortcut: external resource headers need verified allocator ownership
       before their final Release can reclaim host-managed guest storage. */
    if (entry == NULL) recomp_stop(2, "d3d8:resource-destroy-unowned:%08x", resource);
    if (entry->allocation != 0u) recomp_kernel_free_pool(entry->allocation);
    recomp_kernel_free_pool(resource);
    *entry = (OwnedResource){0};
}

static uint32_t reference(uint32_t resource, bool release)
{
    uint32_t value = common(resource), count = value & 0xffffu;
    if ((!release && count == 0xffffu) || (release && count == 0u)) {
        recomp_stop(2, "d3d8:resource-reference-range");
    }
    if ((value & TYPE_MASK) == 0x50000u &&
        count == (release ? 1u : 0u) && word(resource + 20u) != 0u) {
        reference(word(resource + 20u), release);
    }
    if (release && count == 1u && (value & RETAIN_MASK) == 0u) {
        destroy(resource);
        return 0u;
    }
    put(resource, release ? value - 1u : value + 1u);
    return release ? count - 1u : count + 1u;
}

uint32_t recomp_d3d8_resource_add_ref(uint32_t resource)
{
    return reference(resource, false);
}

static void bind(uint32_t resource)
{
    if (resource == 0u) return;
    uint32_t value = common(resource);
    if ((value & RETAIN_MASK) == RETAIN_MASK) recomp_stop(2, "d3d8:resource-binding-overflow");
    if ((value & RETAIN_MASK) == 0u && (value & TYPE_MASK) == 0x50000u && word(resource + 20u) != 0u) {
        const uint32_t parent = word(resource + 20u);
        if ((common(parent) & TYPE_MASK) != 0x40000u) recomp_stop(2, "d3d8:surface-parent-type");
        bind(parent);
    }
    put(resource, value + BIND_REF);
}

static void unbind(uint32_t resource, uint32_t device, bool fence)
{
    if (resource == 0u) return;
    uint32_t value = common(resource);
    if ((value & RETAIN_MASK) == 0u) recomp_stop(2, "d3d8:resource-binding-underflow");
    const uint32_t parent = (value & RETAIN_MASK) == BIND_REF && (value & TYPE_MASK) == 0x50000u
        ? word(resource + 20u) : 0u;
    value -= BIND_REF;
    put(resource, value);
    if (fence) put(resource + 8u, word(device + 0x2cu));
    if (parent != 0u) unbind(parent, device, false);
    if ((value & 0x78ffffu) == 0u) destroy(resource);
}

void recomp_d3d8_resource_set_render_targets(uint32_t color, uint32_t depth)
{
    const uint32_t device = recomp_d3d8_require_device();
    const uint32_t old_color = word(device + 0x1a04u), old_depth = word(device + 0x1a08u);
    bind(color);
    bind(depth);
    unbind(old_color, device, true);
    unbind(old_depth, device, true);
    put(device + 0x1a04u, color);
    put(device + 0x1a08u, depth);
}

bool recomp_d3d8_resource_describe(uint32_t resource, RecompD3dTextureDesc *out)
{
    if (resource == 0u || out == NULL) return false;
    uint32_t value = common(resource), type = value & TYPE_MASK;
    if (type != 0x40000u && type != 0x50000u) return false;
    uint32_t format = word(resource + 12u);
    const bool volume = (format & 0xf0u) == 0x30u;
    if (((format & 0xf0u) != 0x20u && !volume) || (format & 4u) != 0u) return false;
    if (volume && (((format >> 8u) & 255u) != 6u ||
        ((format >> 16u) & 15u) > 1u || word(resource + 16u) != 0u)) return false;
    uint8_t descriptor = *recomp_memory(FORMAT_TABLE + ((format >> 8u) & 255u), 1u);
    return recomp_d3d_texture_describe(format, word(resource + 16u), word(resource + 4u), descriptor, out);
}

typedef struct LevelView {
    RecompD3dTextureDesc desc;
    uint32_t format, size, pitch, bytes;
} LevelView;

static LevelView level_view(uint32_t resource, uint32_t level)
{
    LevelView view = {0};
    if (!recomp_d3d8_resource_describe(resource, &view.desc) ||
        view.desc.volume_depth != 0u || level >= view.desc.mip_levels ||
        (view.desc.linear && level != 0u)) {
        recomp_stop(2, "d3d8:resource-level:%u:%08x:%08x:%08x:ret=%08x", level, common(resource),
            word(resource + 12u), word(resource + 16u), word(recomp_runtime.registers.esp));
    }
    uint32_t width = view.desc.width, height = view.desc.height;
    uint32_t format = view.desc.format_byte, bits = view.desc.bits_per_pixel;
    if (bits == 0u) recomp_stop(2, "d3d8:resource-format:%u", format);
    for (uint32_t i = 0u; i <= level; ++i) {
        uint32_t stored_width = width, stored_height = height;
        if (format >= 0xcu && format <= 0xfu && format != 0xdu) {
            if (stored_width < 4u) stored_width = 4u;
            if (stored_height < 4u) stored_height = 4u;
        }
        uint64_t bytes = (uint64_t)stored_width * stored_height * bits / 8u;
        if (bytes == 0u || bytes > 0x04000000u) recomp_stop(2, "d3d8:resource-size");
        if (i == level) {
            view.bytes = (uint32_t)bytes;
            view.pitch = format == 0xcu ? stored_width * 2u :
                (format == 0xeu || format == 0xfu) ? stored_width * 4u : stored_width * bits / 8u;
            break;
        }
        if (view.desc.data > 0x04000000u - bytes) recomp_stop(2, "d3d8:resource-size");
        view.desc.data += (uint32_t)bytes;
        if (width > 1u) width >>= 1u;
        if (height > 1u) height >>= 1u;
    }
    view.desc.width = width;
    view.desc.height = height;
    view.format = word(resource + 12u);
    view.size = word(resource + 16u);
    if (view.desc.linear) {
        view.pitch = view.desc.pitch;
        uint64_t bytes = (uint64_t)view.pitch * height;
        if (bytes > 0x04000000u) recomp_stop(2, "d3d8:resource-size");
        view.bytes = (uint32_t)bytes;
    } else {
        uint32_t log_width = 0u, log_height = 0u;
        while ((1u << log_width) < width) ++log_width;
        while ((1u << log_height) < height) ++log_height;
        view.format = (view.format & 0xfffffu) | log_width << 20u | log_height << 24u;
    }
    (void)recomp_memory(view.desc.data, view.bytes);
    return view;
}

static uint32_t resource_type(uint32_t resource)
{
    uint32_t type = common(resource) & TYPE_MASK;
    if (type == 0u) return 6u;
    if (type <= 0x30000u) return 6u + (type >> 16u);
    if (type == 0x40000u) {
        uint32_t format = word(resource + 12u);
        return (format & 4u) != 0u ? 5u : (format & 0xf0u) > 0x20u ? 4u : 3u;
    }
    if (type == 0x50000u) return (word(resource + 12u) & 0xf0u) > 0x20u ? 2u : 1u;
    return 10u;
}

static void add_ref(void) { kernel_return(1u, reference(arg(0u), false)); }
static void release(void) { kernel_return(1u, reference(arg(0u), true)); }
static void get_type(void) { kernel_return(1u, resource_type(arg(0u))); }
static void resource_register(void)
{
    uint32_t resource = arg(0u), value = common(resource);
    uint32_t data = word(resource + 4u) + arg(1u);
    if ((value & TYPE_MASK) != 0x20000u) data &= 0x0fffffffu;
    put(resource + 4u, data);
    const uint32_t format = word(resource + 12u);
    if ((value & TYPE_MASK) == 0x40000u && (format & 0xf4u) == 0x30u) {
        /* XPR volume textures (FUN_00289f80) only need their span released;
           sampling one still stops in describe. */
        RecompD3dTextureDesc desc;
        const uint32_t descriptor = *recomp_memory(FORMAT_TABLE + ((format >> 8u) & 255u), 1u);
        const uint32_t byte = (format >> 8u) & 255u;
        if (word(resource + 16u) != 0u || (byte >= 0xcu && byte <= 0xfu) ||
            !recomp_d3d_texture_describe(format, 0u, data, descriptor, &desc) || desc.bits_per_pixel == 0u)
            recomp_stop(2, "d3d8:volume-register:%08x:%08x", format, word(resource + 16u));
        uint32_t width = desc.width, height = desc.height, depth = 1u << ((format >> 28u) & 15u);
        uint64_t bytes = 0u;
        for (uint32_t i = 0u; i < desc.mip_levels; ++i) {
            bytes += (uint64_t)width * height * depth * desc.bits_per_pixel / 8u;
            width = width > 1u ? width >> 1u : 1u;
            height = height > 1u ? height >> 1u : 1u;
            depth = depth > 1u ? depth >> 1u : 1u;
        }
        if (bytes > 0x04000000u - desc.data) recomp_stop(2, "d3d8:resource-size");
        recomp_d3d8_invalidate_textures(desc.data, (uint32_t)bytes);
    } else if ((value & TYPE_MASK) == 0x40000u || (value & TYPE_MASK) == 0x50000u) {
        /* The game re-registers render-target textures between rendering and
           sampling them (0x2813f9), so Register must keep rendered pixels. */
        LevelView view = level_view(resource, 0u);
        recomp_d3d8_invalidate_textures(view.desc.data, view.bytes);
    }
    recomp_runtime.registers.esp += 12u;
}

static void create_vertex_buffer(void)
{
    uint32_t length = arg(0u), header = 0u, allocation = 0u;
    if (length == 0u || length > 0x04000000u - 255u) goto returned;
    header = recomp_kernel_allocate_pool(12u);
    if (header == 0u) goto returned;
    allocation = recomp_kernel_allocate_pool(length + 255u);
    if (allocation == 0u || !own(header, allocation)) {
        if (allocation != 0u) recomp_kernel_free_pool(allocation);
        recomp_kernel_free_pool(header);
        header = 0u;
        goto returned;
    }
    put(header, 0x01000001u);
    put(header + 4u, (allocation + 255u) & ~255u);
    put(header + 8u, 0u);
returned:
    kernel_return(1u, header);
}

static void vertex_buffer_lock(void)
{
    uint32_t buffer = arg(0u), flags = arg(1u);
    if ((common(buffer) & TYPE_MASK) != 0u || (flags & ~0xf0u) != 0u) {
        recomp_stop(2, "d3d8:vertex-lock");
    }
    uint32_t data = word(buffer + 4u) & 0x0fffffffu;
    (void)recomp_memory(data, 1u);
    /* Return an address in the runtime's mapped CPU RAM rather than an Xbox alias. */
    kernel_return(2u, data);
}

static void lock_rect(bool surface)
{
    uint32_t resource = arg(0u), level = surface ? 0u : arg(1u);
    uint32_t output = arg(surface ? 1u : 2u), rect = arg(surface ? 2u : 3u);
    uint32_t flags = arg(surface ? 3u : 4u);
    if (output == 0u || (flags & ~0xf0u) != 0u) recomp_stop(2, "d3d8:lock-rect-arguments");
    LevelView view = level_view(resource, level);
    uint32_t data = view.desc.data;
    if (rect != 0u) {
        uint32_t left = word(rect), top = word(rect + 4u);
        uint32_t right = word(rect + 8u), bottom = word(rect + 12u);
        if (left >= right || top >= bottom || right > view.desc.width || bottom > view.desc.height ||
            (!view.desc.linear && (left != 0u || top != 0u))) {
            recomp_stop(2, "d3d8:lock-rect-range");
        }
        data += top * view.pitch + left * view.desc.bits_per_pixel / 8u;
    }
    if ((flags & 0x80u) == 0u) {
        recomp_d3d8_release_memory(view.desc.data, view.bytes);
    }
    put(output, view.pitch);
    put(output + 4u, data);
    recomp_runtime.registers.esp += surface ? 20u : 24u;
}
static void texture_lock_rect(void) { lock_rect(false); }
static void surface_lock_rect(void) { lock_rect(true); }

static void get_surface_level(void)
{
    uint32_t texture = arg(0u), level = arg(1u);
    LevelView view = level_view(texture, level);
    uint32_t header = recomp_kernel_allocate_pool(24u);
    if (header != 0u && !own(header, 0u)) {
        recomp_kernel_free_pool(header);
        header = 0u;
    }
    if (header != 0u) {
        reference(texture, false);
        const uint32_t words[6] = {0x01050001u, view.desc.data, 0u, view.format, view.size, texture};
        memcpy(recomp_memory(header, sizeof words), words, sizeof words);
    }
    kernel_return(2u, header);
}

static void surface_get_desc(void)
{
    uint32_t resource = arg(0u), output = arg(1u);
    if (output == 0u) recomp_stop(2, "d3d8:surface-desc-output");
    LevelView view = level_view(resource, 0u);
    uint32_t usage = view.desc.render_target ? 1u : view.desc.depth ? 2u : 0u;
    uint32_t device = recomp_d3d8_require_device();
    uint32_t first_buffer = word(device + 0x1a14u);
    uint32_t multisample = first_buffer != 0u && word(resource + 4u) == word(first_buffer + 4u)
        ? word(device + 0x196cu) : 0x11u;
    const uint32_t words[7] = {view.desc.format_byte, resource_type(resource), usage,
        view.bytes, multisample, view.desc.width, view.desc.height};
    memcpy(recomp_memory(output, sizeof words), words, sizeof words);
    recomp_runtime.registers.esp += 12u;
}

static void set_texture(void)
{
    uint32_t device = recomp_d3d8_require_device(), stage = arg(0u), texture = arg(1u);
    if (stage >= 4u) recomp_stop(2, "d3d8:texture-stage:%u", stage);
    if (texture != 0u && (common(texture) & TYPE_MASK) != 0x40000u) recomp_stop(2, "d3d8:texture-type");
    uint32_t slot = device + 0xf88u + stage * 4u, previous = word(slot);
    bind(texture);
    unbind(previous, device, true);
    put(slot, texture);
    if (stage == 3u) inline_texture_active = false;
    uint32_t shadow_address = device + 12u + stage * 4u;
    if (texture == 0u) {
        put(shadow_address, 0x80000000u);
        put(DIRTY, word(DIRTY) | 0x4800u);
    } else {
        uint32_t source = word(texture + 12u), shadow = source & 0x20f4u;
        if ((shadow & 0x2000u) != 0u) {
            uint32_t format = source & 0xff00u;
            shadow &= ~0x2000u;
            if (format >= 0x2a00u && format <= 0x3100u) shadow |= 0x40000000u;
        }
        if (word(shadow_address) != shadow) {
            put(shadow_address, shadow);
            put(DIRTY, word(DIRTY) | 0x4000u | (previous == 0u ? 0x800u : 0u));
        }
    }
    kernel_return(2u, 0u);
}

bool recomp_d3d8_resource_inline_texture(uint32_t stage, RecompD3dTextureDesc *out)
{
    if (stage != 3u || !inline_texture_active || out == NULL) return false;
    /* The pair changes offset/format, not the sampler enable written by SetTexture. */
    if (word(recomp_d3d8_require_device() + 0xf94u) == 0u) return false;
    *out = inline_texture;
    return true;
}

/* SDK 0x3443f0 is fastcall(method, data, format), ret 4. The reached game
   callers use SetTexture's stage-3 pair without changing its resource shadow. */
static void inline_texture_pair(void)
{
    (void)recomp_d3d8_require_device();
    const uint32_t method = recomp_runtime.registers.ecx;
    const uint32_t format = arg(0u), pixel = (format >> 8u) & 255u;
    if (method != 0x81bc0u) recomp_stop(2, "d3d8:inline-texture-method:%08x", method);
    /* The pair has no rectangle size; only verified swizzled formats apply. */
    if ((format & 0xf4u) != 0x20u ||
        (pixel != 6u && pixel != 0xbu && pixel != 0xcu && pixel != 0xeu && pixel != 0xfu && pixel != 0x19u))
        recomp_stop(2, "d3d8:inline-texture-format:%08x", format);
    recomp_d3d_texture_describe(format, 0u, recomp_runtime.registers.edx,
        *recomp_memory(FORMAT_TABLE + pixel, 1u), &inline_texture);
    inline_texture_active = true;
    recomp_runtime.registers.ecx = format;
    kernel_return(1u, recomp_runtime.registers.eax);
}

static void get_texture(void)
{
    uint32_t device = recomp_d3d8_require_device(), stage = arg(0u);
    if (stage >= 4u) recomp_stop(2, "d3d8:texture-stage:%u", stage);
    uint32_t texture = word(device + 0xf88u + stage * 4u);
    if (texture != 0u) reference(texture, false);
    kernel_return(1u, texture);
}

static void set_palette(void)
{
    uint32_t device = recomp_d3d8_require_device(), stage = arg(0u), palette = arg(1u);
    if (stage >= 4u) recomp_stop(2, "d3d8:palette-stage:%u", stage);
    if (palette != 0u && (common(palette) & TYPE_MASK) != 0x30000u) recomp_stop(2, "d3d8:palette-type");
    uint32_t slot = device + 0xf98u + stage * 4u;
    bind(palette);
    unbind(word(slot), device, true);
    put(slot, palette);
    recomp_runtime.registers.esp += 12u;
}

static void set_stream_source(void)
{
    uint32_t device = recomp_d3d8_require_device(), stream = arg(0u), buffer = arg(1u), stride = arg(2u);
    if (stream >= 16u || (buffer != 0u && (common(buffer) & TYPE_MASK) != 0u)) recomp_stop(2, "d3d8:stream-source");
    uint32_t slot = STREAMS + stream * 12u;
    bind(buffer);
    unbind(word(slot + 8u), device, true);
    put(DIRTY, word(DIRTY) | (word(slot) == stride ? 0x40u : 0x70u));
    put(slot, stride);
    put(slot + 8u, buffer);
    recomp_runtime.registers.esp += 16u;
}

static void set_indices(void)
{
    uint32_t device = recomp_d3d8_require_device(), buffer = arg(0u), base = arg(1u);
    if (buffer != 0u && (common(buffer) & TYPE_MASK) != 0x10000u) recomp_stop(2, "d3d8:index-buffer-type");
    bind(buffer);
    unbind(word(device + 0x7a0u), device, false);
    put(0x00355054u, buffer == 0u ? 0u : word(buffer + 4u));
    put(device + 0x7a0u, buffer);
    put(device + 0x1cu, base);
    recomp_runtime.registers.esp += 12u;
}

RecompFunction recomp_d3d8_resource_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x003443f0u: return inline_texture_pair;
    case 0x00347e80u: return add_ref;
    case 0x00347ec0u: return release;
    case 0x00347f10u: return get_type;
    case 0x00347fa0u: return resource_register;
    case 0x00346140u: return create_vertex_buffer;
    case 0x00346190u: return vertex_buffer_lock;
    case 0x00348060u: return get_surface_level;
    case 0x003480b0u: return texture_lock_rect;
    case 0x00348170u: return surface_get_desc;
    case 0x00348190u: return surface_lock_rect;
    case 0x00345460u: return get_texture;
    case 0x00345490u: return set_texture;
    case 0x00345670u: return set_palette;
    case 0x00345700u: return set_indices;
    case 0x00348f20u: return set_stream_source;
    default: return NULL;
    }
}
