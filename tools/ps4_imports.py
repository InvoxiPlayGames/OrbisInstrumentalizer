"""Lists the PLT stub address of each libSceUsbd function a PS4 eboot imports.

usage: python -I ps4_imports.py <eboot.elf> <OrbisUsbd.h>
"""
import base64
import hashlib
import re
import struct
import sys

NID_SUFFIX = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")

PT_DYNAMIC = 2
PT_SCE_DYNLIBDATA = 0x61000000
DT_SCE_JMPREL = 0x61000029
DT_SCE_PLTRELSZ = 0x6100002D
DT_SCE_STRTAB = 0x61000035
DT_SCE_SYMTAB = 0x61000039
DT_SCE_SYMTABSZ = 0x6100003F


def nid(name: str) -> str:
    digest = hashlib.sha1(name.encode() + NID_SUFFIX).digest()
    return base64.b64encode(digest[:8][::-1]).decode().replace("/", "-")[:11]


def main(elf_path: str, header_path: str) -> None:
    data = open(elf_path, "rb").read()
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    segments = []
    for i in range(phnum):
        p_type, _, p_offset, p_vaddr, _, p_filesz, _, _ = struct.unpack_from("<IIQQQQQQ", data, phoff + i * phentsize)
        segments.append((p_type, p_offset, p_vaddr, p_filesz))

    dynamic = next(s for s in segments if s[0] == PT_DYNAMIC)
    dynlib = next(s for s in segments if s[0] == PT_SCE_DYNLIBDATA)
    tags = {}
    for off in range(dynamic[1], dynamic[1] + dynamic[3], 16):
        tag, val = struct.unpack_from("<qQ", data, off)
        tags.setdefault(tag, val)

    # these tags are offsets into the dynlib data segment
    base = dynlib[1]
    strtab = base + tags[DT_SCE_STRTAB]
    symtab = base + tags[DT_SCE_SYMTAB]
    jmprel = base + tags[DT_SCE_JMPREL]
    pltrelsz = tags[DT_SCE_PLTRELSZ]

    def vaddr_to_offset(vaddr: int) -> int:
        for p_type, p_offset, p_vaddr, p_filesz in segments:
            if p_type in (1, 0x61000010) and p_vaddr <= vaddr < p_vaddr + p_filesz:
                return p_offset + (vaddr - p_vaddr)
        raise ValueError(hex(vaddr))

    wanted = {}
    for name in re.findall(r"\b(sceUsbd\w+)\s*\(", open(header_path, encoding="utf-8").read()):
        wanted[nid(name)] = name

    found = []
    for off in range(jmprel, jmprel + pltrelsz, 24):
        r_offset, r_info, _ = struct.unpack_from("<QQq", data, off)
        sym = r_info >> 32
        st_name = struct.unpack_from("<I", data, symtab + sym * 24)[0]
        end = data.index(b"\0", strtab + st_name)
        symbol = data[strtab + st_name:end].decode()
        name = wanted.get(symbol.split("#")[0])
        if name is None:
            continue
        # lazy binding: the GOT slot initially points 6 bytes into the PLT stub (after its jmp)
        got_value = struct.unpack_from("<Q", data, vaddr_to_offset(r_offset))[0]
        found.append((got_value - 6, name, r_offset))

    for stub, name, got in sorted(found):
        print(f"0x{stub:08x}  GOT 0x{got:08x}  {name}")
    print(f"{len(found)} sceUsbd imports", file=sys.stderr)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
