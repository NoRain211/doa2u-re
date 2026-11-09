/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "runner_load.h"
#include "runtime.h"
#include "xbox_memory_layout.h"

#include <algorithm>
#include <cstring>

namespace {

void writeU32(std::vector<std::uint8_t>& memory, std::uint32_t address,
    std::uint32_t value)
{
    std::memcpy(memory.data() + address, &value, sizeof value);
}

std::uint32_t kernelDataAddress(std::uint32_t ordinal)
{
    switch (ordinal) {
    case 322u: return XBOX_KERNEL_DATA_BASE;
    case 324u: return XBOX_KERNEL_DATA_BASE + 0x10u;
    case 16u: return XBOX_KERNEL_DATA_BASE + 0x20u;
    case 40u: return XBOX_KERNEL_DATA_BASE + 0x30u;
    case 156u: return XBOX_KERNEL_DATA_BASE + 0x40u;
    case 164u: return XBOX_KERNEL_DATA_BASE + 0x50u;
    case 259u: return XBOX_KERNEL_DATA_BASE + 0x60u;
    case 323u: return XBOX_KERNEL_DATA_BASE + 0x70u;
    case 325u: return XBOX_KERNEL_DATA_BASE + 0x80u;
    case 354u: return XBOX_KERNEL_DATA_BASE + 0x90u;
    case 356u: return XBOX_KERNEL_DATA_BASE + 0x190u;
    default: return 0u;
    }
}

} // namespace

bool recomp_load_xbe(const doaxbv::XbeParseResult& parsed,
    std::vector<std::uint8_t>& memory, std::string& error)
{
    const auto& metadata = parsed.metadata;
    const auto& header = metadata.header;
    const std::uint64_t imageEnd = static_cast<std::uint64_t>(header.baseAddress) +
        header.sizeImage;
    const std::uint64_t headerEnd = static_cast<std::uint64_t>(header.baseAddress) +
        header.sizeHeaders;
    if (!parsed.ok() || memory.size() != RECOMP_XBOX_RAM_SIZE ||
        header.baseAddress < 0x1000u || header.sizeImage == 0u ||
        imageEnd >= XBOX_GUEST_ALLOC_LIMIT || headerEnd > imageEnd ||
        header.sizeHeaders > parsed.fileBytes.size()) {
        error = "XBE image or headers overlap reserved memory or leave their source";
        return false;
    }
    if (metadata.selectedEntryPoint < header.baseAddress ||
        metadata.selectedEntryPoint >= imageEnd ||
        header.stackCommit > XBOX_STARTUP_STACK_TOP - XBOX_STARTUP_STACK_BASE) {
        error = "XBE entry or startup stack is outside its reserved range";
        return false;
    }
    std::uint64_t tlsSize = 0u;
    if (metadata.tls) {
        const auto& tls = *metadata.tls;
        if (tls.dataEndAddress < tls.dataStartAddress ||
            (tls.dataEndAddress != tls.dataStartAddress &&
             (tls.dataStartAddress < header.baseAddress || tls.dataEndAddress > imageEnd))) {
            error = "TLS initialized-data range is outside the image";
            return false;
        }
        if (tls.tlsIndexAddress != 0u && (tls.tlsIndexAddress < header.baseAddress ||
            static_cast<std::uint64_t>(tls.tlsIndexAddress) + 4u > imageEnd)) {
            error = "TLS index leaves the XBE image";
            return false;
        }
        tlsSize = static_cast<std::uint64_t>(tls.dataEndAddress) -
            tls.dataStartAddress + tls.sizeOfZeroFill;
        // XAPI uses a negative index from StackBase to the pointer preceding aligned TLS.
        tlsSize = (tlsSize + 15u) & ~std::uint64_t{15u};
        if (tlsSize + 4u > memory.size() - XBOX_STARTUP_THREAD_STACK_SLOT) {
            error = "startup TLS leaves reserved memory";
            return false;
        }
    }
    for (const auto& section : metadata.sections) {
        const std::uint64_t virtualEnd = static_cast<std::uint64_t>(section.virtualAddress) +
            std::max(section.virtualSize, section.rawSize);
        const std::uint64_t rawEnd = static_cast<std::uint64_t>(section.rawAddress) +
            section.rawSize;
        if (section.virtualAddress < header.baseAddress || virtualEnd > imageEnd ||
            rawEnd > parsed.fileBytes.size() || !section.digestMatches) {
            error = "XBE section leaves its range or has a digest mismatch";
            return false;
        }
    }
    for (const auto& thunk : metadata.kernelThunks) {
        const std::uint64_t slot = static_cast<std::uint64_t>(metadata.selectedKernelThunkAddress) +
            static_cast<std::uint64_t>(thunk.index) * 4u;
        if (slot < header.baseAddress || slot + 4u > imageEnd) {
            error = "kernel thunk slot leaves the XBE image";
            return false;
        }
    }

    std::memcpy(memory.data() + header.baseAddress, parsed.fileBytes.data(), header.sizeHeaders);
    for (const auto& section : metadata.sections) {
        std::memset(memory.data() + section.virtualAddress, 0,
            std::max(section.virtualSize, section.rawSize));
        std::memcpy(memory.data() + section.virtualAddress,
            parsed.fileBytes.data() + section.rawAddress, section.rawSize);
    }
    // Synthetic kernel version and dashboard defaults; keys remain zero.
    memory[XBOX_KERNEL_DATA_BASE + 4u] = 0xa1u;
    memory[XBOX_KERNEL_DATA_BASE + 5u] = 0xb1u;
    const std::uint16_t version[] = {1u, 0u, 5849u, 0u};
    std::memcpy(memory.data() + XBOX_KERNEL_DATA_BASE + 0x10u, version, sizeof version);
    writeU32(memory, XBOX_KERNEL_DATA_BASE + 0x30u, 3u);
    for (const auto& thunk : metadata.kernelThunks) {
        const auto dataAddress = kernelDataAddress(thunk.ordinal);
        writeU32(memory, metadata.selectedKernelThunkAddress + thunk.index * 4u,
            dataAddress != 0u ? dataAddress : thunk.value);
    }

    // The lifter adds the same reserved PCR base to every FS-relative operand.
    std::memset(memory.data() + XBOX_PCR_BASE, 0, XBOX_PCR_SIZE);
    const auto tlsBlock = XBOX_STARTUP_THREAD_STACK_SLOT + 4u;
    if (metadata.tls && metadata.tls->dataEndAddress != metadata.tls->dataStartAddress) {
        std::memcpy(memory.data() + tlsBlock, memory.data() + metadata.tls->dataStartAddress,
            metadata.tls->dataEndAddress - metadata.tls->dataStartAddress);
    }
    writeU32(memory, XBOX_STARTUP_THREAD_STACK_SLOT, tlsBlock);
    writeU32(memory, XBOX_PCR_BASE, 0xffffffffu);
    writeU32(memory, XBOX_PCR_BASE + 4u, static_cast<std::uint32_t>(tlsBlock + tlsSize));
    writeU32(memory, XBOX_PCR_BASE + 8u, XBOX_STARTUP_STACK_BASE);
    writeU32(memory, XBOX_PCR_BASE + 0x1cu, XBOX_PCR_BASE);
    writeU32(memory, XBOX_PCR_BASE + 0x20u, XBOX_PCR_BASE + 0x28u);
    writeU32(memory, XBOX_PCR_BASE + 0x28u, XBOX_STARTUP_THREAD_OBJECT);
    writeU32(memory, XBOX_STARTUP_THREAD_OBJECT, 6u);
    writeU32(memory, XBOX_STARTUP_THREAD_OBJECT + 0x1cu,
        static_cast<std::uint32_t>(tlsBlock + tlsSize));
    writeU32(memory, XBOX_STARTUP_THREAD_OBJECT + 0x20u, XBOX_STARTUP_STACK_BASE);
    writeU32(memory, XBOX_STARTUP_THREAD_OBJECT + 0x28u, XBOX_STARTUP_THREAD_STACK_SLOT);
    writeU32(memory, XBOX_STARTUP_THREAD_OBJECT + 0x12cu, 1u);
    return true;
}
