/*
    stub_finder.c
    Finds the game's PLT stubs for its libSceUsbd imports at runtime, so the hooks don't depend
    on one version of the game.

    Each import is called through a stub, `jmp [rip+x]`, where x points at the import's GOT slot.
    Once the game is loaded, the GOT slots of bound imports hold the library's own addresses,
    which we can look up with sceKernelDlsym. So: find the GOT slots holding those addresses in
    the game's data, then the stubs jumping through them in the game's code.

    Imports the game hasn't bound yet (if binding is lazy) are filled in from the ones that were
    found: the linker lays out stubs (16 bytes each) and GOT slots (8 bytes each) for a library's
    imports next to each other, sorted by name, which is checked before trusting it.
    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <orbis/libkernel.h>

#include "oi_log.h"
#include "stub_finder.h"

#define MAX_MODULES 256
#define PLT_STUB_SIZE 16

typedef struct {
    uint64_t start;
    uint64_t size;
} Segment;

static bool IsJumpStub(const uint8_t *code, uint64_t address, uint64_t *got_out) {
    // jmp qword ptr [rip + disp32]
    if (code[0] != 0xFF || code[1] != 0x25)
        return false;
    int32_t disp;
    memcpy(&disp, code + 2, sizeof(disp));
    *got_out = address + 6 + (int64_t)disp;
    return true;
}

static bool InSegment(const Segment *segment, uint64_t address, uint64_t length) {
    return address >= segment->start && address + length <= segment->start + segment->size;
}

uint64_t ResolveOwnImport(const void *function) {
    // our own calls to the library go through a stub of ours just like the game's
    uint64_t got;
    if (function == NULL)
        return 0;
    const uint8_t *code = (const uint8_t *)function;
    OILogLimited(3, "Stub finder: own import at %p starts %02X %02X %02X %02X %02X %02X\n", function,
        code[0], code[1], code[2], code[3], code[4], code[5]);
    // taking the address of an import can also give the library's address straight away
    if (!IsJumpStub(code, (uint64_t)function, &got))
        return (uint64_t)function;
    uint64_t target = *(const uint64_t *)got;
    // not bound yet: still points back into the stub
    if (target >= (uint64_t)function && target < (uint64_t)function + PLT_STUB_SIZE)
        return 0;
    return target;
}

int FindImportStubs(const char *library_name, const char *const *names, int count, uint64_t base_address,
                    const uint64_t *known_targets, uint64_t *stubs_out, uint64_t *gots_out) {
    for (int i = 0; i < count; i++)
        stubs_out[i] = gots_out[i] = 0;

    // the game's code and data, and the library
    OrbisKernelModule modules[MAX_MODULES];
    size_t module_count = 0;
    if (sceKernelGetModuleList(modules, MAX_MODULES, &module_count) != 0) {
        OILog("Stub finder: no module list\n");
        return 0;
    }
    Segment code = { 0 }, data[4];
    int data_count = 0;
    int library = -1;
    for (size_t m = 0; m < module_count; m++) {
        OrbisKernelModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(modules[m], &info) != 0)
            continue;
        if (strstr(info.name, library_name) != NULL)
            library = modules[m];
        OILogLimited(80, "Stub finder: module %s\n", info.name);
        bool is_game = false;
        for (uint32_t s = 0; s < info.segmentCount && s < 4; s++) {
            if ((uint64_t)info.segmentInfo[s].address == base_address)
                is_game = true;
        }
        if (!is_game)
            continue;
        OILog("Stub finder: game module %s, %u segments\n", info.name, info.segmentCount);
        for (uint32_t s = 0; s < info.segmentCount && s < 4; s++) {
            Segment segment = { (uint64_t)info.segmentInfo[s].address, info.segmentInfo[s].size };
            int prot = info.segmentInfo[s].prot;
            OILog("Stub finder:   segment +0x%lx size 0x%lx prot 0x%x\n",
                (unsigned long)(segment.start - base_address), (unsigned long)segment.size, prot);
            if ((prot & ORBIS_KERNEL_PROT_CPU_EXEC) != 0) {
                if (segment.size > code.size)
                    code = segment;
            } else if ((prot & ORBIS_KERNEL_PROT_CPU_READ) != 0 && data_count < 4) {
                data[data_count++] = segment;
            }
        }
    }
    if (code.size == 0 || data_count == 0) {
        OILog("Stub finder: game code %s, data %d, library %s\n", code.size ? "found" : "missing",
            data_count, library >= 0 ? "found" : "missing");
        return 0;
    }

    // where the library's functions really are
    uint64_t targets[64];
    if (count > 64)
        count = 64;
    for (int i = 0; i < count; i++) {
        void *address = NULL;
        targets[i] = known_targets != NULL ? known_targets[i] : 0;
        if (targets[i] == 0 && library >= 0 && sceKernelDlsym(library, names[i], &address) == 0)
            targets[i] = (uint64_t)address;
    }

    // GOT slots holding them
    int got_found = 0;
    for (int d = 0; d < data_count; d++) {
        const uint64_t *slots = (const uint64_t *)data[d].start;
        uint64_t slot_count = data[d].size / 8;
        for (uint64_t j = 0; j < slot_count; j++) {
            uint64_t value = slots[j];
            if (value == 0)
                continue;
            // (targets that aren't known are 0, which never matches here)
            for (int i = 0; i < count; i++) {
                if (value == targets[i] && gots_out[i] == 0) {
                    gots_out[i] = (uint64_t)&slots[j];
                    got_found++;
                }
            }
        }
    }

    // stubs jumping through them
    int stub_found = 0;
    const uint8_t *text = (const uint8_t *)code.start;
    for (uint64_t offset = 0; offset + PLT_STUB_SIZE <= code.size; offset += PLT_STUB_SIZE) {
        uint64_t got;
        if (!IsJumpStub(text + offset, code.start + offset, &got))
            continue;
        for (int i = 0; i < count; i++) {
            if (gots_out[i] != 0 && got == gots_out[i] && stubs_out[i] == 0) {
                stubs_out[i] = code.start + offset;
                stub_found++;
            }
        }
    }
    OILog("Stub finder: %d of %d imports bound, %d stubs found\n", got_found, count, stub_found);

    // fill in the rest from the layout, using the first stub found as the anchor
    int anchor = -1;
    for (int i = 0; i < count && anchor < 0; i++) {
        if (stubs_out[i] != 0)
            anchor = i;
    }
    if (anchor < 0)
        return 0;
    int total = 0;
    for (int i = 0; i < count; i++) {
        uint64_t stub = stubs_out[anchor] + (int64_t)(i - anchor) * PLT_STUB_SIZE;
        uint64_t expected_got = gots_out[anchor] + (int64_t)(i - anchor) * 8;
        uint64_t got;
        if (!InSegment(&code, stub, PLT_STUB_SIZE) || !IsJumpStub((const uint8_t *)stub, stub, &got) || got != expected_got) {
            OILog("Stub finder: layout doesn't hold for %s\n", names[i]);
            continue;
        }
        if (stubs_out[i] != 0 && stubs_out[i] != stub) {
            OILog("Stub finder: %s found at +0x%lx, layout says +0x%lx\n", names[i],
                (unsigned long)(stubs_out[i] - base_address), (unsigned long)(stub - base_address));
            return 0; // something's off, trust nothing
        }
        stubs_out[i] = stub;
        gots_out[i] = got;
        total++;
    }
    return total;
}
