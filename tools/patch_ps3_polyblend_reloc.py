#!/usr/bin/env python3
"""Retarget the inlined GL1 flash call in a preserved PS3 object.

GCC folded R_Flash into RI_RenderFrame and emitted its call through the local
R_PolyBlend.part.4 function descriptor.  That local relocation cannot be
interposed by a strong R_PolyBlend from another object.  This changes only that
one REL24 relocation to the public (weakened) R_PolyBlend symbol, allowing the
PS3 stereo-safe implementation to be selected at link time.
"""

from pathlib import Path
import struct
import sys


ELF_HEADER = struct.Struct(">16sHHIQQQIHHHHHH")
SECTION_HEADER = struct.Struct(">IIQQQQIIQQ")
SYMBOL = struct.Struct(">IBBHQQ")
RELA = struct.Struct(">QQq")
R_PPC64_REL24 = 10
STT_SECTION = 3


def cstring(data: bytes, offset: int) -> str:
    end = data.find(b"\0", offset)
    if end < 0:
        raise ValueError("unterminated ELF string")
    return data[offset:end].decode("ascii")


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} input.o output.o", file=sys.stderr)
        return 2

    source = Path(sys.argv[1])
    output = Path(sys.argv[2])
    if output.exists():
        raise FileExistsError(f"refusing to overwrite {output}")

    data = bytearray(source.read_bytes())
    header = ELF_HEADER.unpack_from(data, 0)
    ident = header[0]
    if ident[:4] != b"\x7fELF" or ident[4] != 2 or ident[5] != 2:
        raise ValueError("expected a big-endian ELF64 object")

    section_offset = header[6]
    section_entry_size = header[11]
    section_count = header[12]
    section_name_index = header[13]
    if section_entry_size != SECTION_HEADER.size:
        raise ValueError("unexpected ELF64 section-header size")

    sections = []
    for index in range(section_count):
        offset = section_offset + index * section_entry_size
        sections.append(SECTION_HEADER.unpack_from(data, offset))

    section_names_header = sections[section_name_index]
    section_names = data[
        section_names_header[4]:section_names_header[4] + section_names_header[5]
    ]
    names = [cstring(section_names, section[0]) for section in sections]

    try:
        relocation_index = names.index(".rela.text.RI_RenderFrame")
        symbol_index = names.index(".symtab")
        opd_index = names.index(".opd")
    except ValueError as exc:
        raise ValueError("required GL1 ELF section is missing") from exc

    symbol_header = sections[symbol_index]
    string_header = sections[symbol_header[6]]
    strings = data[string_header[4]:string_header[4] + string_header[5]]
    symbol_count = symbol_header[5] // symbol_header[9]
    symbols = []
    symbol_names = []
    for index in range(symbol_count):
        offset = symbol_header[4] + index * symbol_header[9]
        symbol = SYMBOL.unpack_from(data, offset)
        symbols.append(symbol)
        symbol_names.append(cstring(strings, symbol[0]))

    public_matches = [
        index for index, name in enumerate(symbol_names) if name == "R_PolyBlend"
    ]
    helper_matches = [
        index for index, name in enumerate(symbol_names)
        if name == "R_PolyBlend.part.4"
    ]
    if len(public_matches) != 1 or len(helper_matches) != 1:
        raise ValueError("unexpected R_PolyBlend symbol layout")

    public_index = public_matches[0]
    helper = symbols[helper_matches[0]]
    helper_opd_offset = helper[4]
    if helper[3] != opd_index:
        raise ValueError("R_PolyBlend helper is not in .opd")

    relocation_header = sections[relocation_index]
    relocation_count = relocation_header[5] // relocation_header[9]
    matches = []
    for index in range(relocation_count):
        offset = relocation_header[4] + index * relocation_header[9]
        relocation_offset, relocation_info, addend = RELA.unpack_from(data, offset)
        referenced_index = relocation_info >> 32
        relocation_type = relocation_info & 0xFFFFFFFF
        referenced = symbols[referenced_index]
        referenced_type = referenced[1] & 0x0F
        if (relocation_type == R_PPC64_REL24 and addend == helper_opd_offset and
                referenced_type == STT_SECTION and referenced[3] == opd_index):
            matches.append((offset, relocation_offset, relocation_type))

    if len(matches) != 1:
        raise ValueError(
            f"expected one inlined polyblend relocation, found {len(matches)}"
        )

    entry_offset, call_offset, relocation_type = matches[0]
    new_info = (public_index << 32) | relocation_type
    RELA.pack_into(data, entry_offset, call_offset, new_info, 0)
    output.write_bytes(data)
    print(
        f"patched .text.RI_RenderFrame+0x{call_offset:x}: "
        ".opd/R_PolyBlend.part.4 -> R_PolyBlend"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
