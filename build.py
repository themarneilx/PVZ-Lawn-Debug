#!/usr/bin/env python3
import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parent
ORIGINAL = "PlantsVsZombies.exe"
VARIANT = "PlantsVsZombies.Debug.exe"
MODULE = "PvzDebug.dll"
ORIGINAL_SHA256 = "f9669af338964787a3785a7895791297d599295b8bb669b0db49443f736a1322"
EXPORTS = (
    "PvzDebugInit",
    "PvzDebugUpdate",
    "PvzDebugEndBoard",
    "PvzDebugWalletForSave",
    "PvzDebugDrawBank",
    "PvzDebugPickerClick",
    "PvzDebugHidePlantHits",
    "PvzDebugDrawChooser",
    "PvzDebugChooserClick",
)
IMAGE_BASE = 0x400000
ALIGNMENT = 0x1000


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def set_u32(data, offset, value):
    struct.pack_into("<I", data, offset, value)


def aligned(number, alignment=ALIGNMENT):
    return (number + alignment - 1) & -alignment


def jump(at, target, opcode=b"\xe9"):
    return opcode + struct.pack("<i", target - at - 5)


def variant_bytes(original):
    if hashlib.sha256(original).hexdigest() != ORIGINAL_SHA256:
        raise ValueError("Expected the verified 32-bit 1.0.0.1051 executable.")

    file_header = u32(original, 0x3C) + 4
    opt = file_header + 20
    sections = opt + u16(original, file_header + 16)
    count = u16(original, file_header + 2)
    image_size = u32(original, opt + 56)
    dirs = opt + 96
    imports = u32(original, dirs + 8)
    cert_start = u32(original, dirs + 32)
    old_entry = u32(original, opt + 16)

    data_rva = image_size
    code_rva = data_rva + ALIGNMENT
    image = bytearray(original[:cert_start])
    code = bytearray()
    import_data = bytearray()

    cursor = imports
    while any(original[cursor:cursor + 20]):
        import_data.extend(original[cursor:cursor + 20])
        cursor += 20
    descriptor = len(import_data)
    import_data.extend(bytes(40))

    def add_data(blob, alignment=1):
        import_data.extend(bytes(aligned(len(import_data), alignment) - len(import_data)))
        rva = data_rva + len(import_data)
        import_data.extend(blob)
        return rva

    dll = add_data(b"PvzDebug.dll\0")
    hints = [add_data(b"\0\0" + name.encode("ascii") + b"\0", 2) for name in EXPORTS]
    thunk_table = struct.pack(f"<{len(hints) + 1}I", *hints, 0)
    ilt = add_data(thunk_table, 4)
    iat = add_data(thunk_table, 4)
    struct.pack_into("<5I", import_data, descriptor, ilt, 0, 0, dll, iat)

    def call_import(index):
        return b"\xff\x15" + struct.pack("<I", IMAGE_BASE + iat + index * 4)

    def append_stub(prefix, resume, suffix=b""):
        start = code_rva + len(code)
        code.extend(prefix + jump(start + len(prefix), resume) + suffix)
        return start

    def patch_site(address, expected, target, opcode=b"\xe9"):
        offset = address - IMAGE_BASE
        if image[offset:offset + len(expected)] != expected:
            raise ValueError(f"Unexpected instructions at {address:#x}.")
        patch = jump(offset, target, opcode)
        image[offset:offset + len(expected)] = patch + b"\x90" * (len(expected) - len(patch))

    def patch_vtable(address, expected, target):
        offset = address - IMAGE_BASE
        if u32(image, offset) != expected:
            raise ValueError(f"Unexpected vtable entry at {address:#x}.")
        set_u32(image, offset, IMAGE_BASE + target)

    saved = b"\x9c\x60"
    restored = b"\x61\x9d"
    entry = append_stub(saved + call_import(0) + restored, old_entry)

    update_address = 0x415D40
    update_rva = update_address - IMAGE_BASE
    update_original = bytes.fromhex("6aff 6835fa6400")
    skip = len(restored) + len(update_original) + 5
    prefix = saved + b"\x51" + call_import(1) + b"\x83\xc4\x04\x85\xc0\x75" + bytes([skip])
    update = append_stub(prefix + restored + update_original,
                         update_rva + len(update_original), restored + b"\xc3")
    patch_site(update_address, update_original, update)

    end_address = 0x44F380
    end_original = bytes.fromhex("8b8774070000")
    end = append_stub(saved + b"\x57" + call_import(2) + b"\x83\xc4\x04" + restored + end_original,
                      end_address - IMAGE_BASE + len(end_original))
    patch_site(end_address, end_original, end)

    wallet_address = 0x468425
    wallet = append_stub(saved + bytes.fromhex("ff742428 53") + call_import(3)
                         + bytes.fromhex("83c408 89442428") + restored, 0x443770 - IMAGE_BASE)
    patch_site(wallet_address, bytes.fromhex("e846b3fdff"), wallet, opcode=b"\xe8")

    draw = code_rva + len(code)
    code.extend(saved + bytes.fromhex("ff74242c ff74242c") + call_import(4)
                + bytes.fromhex("83c408") + restored + bytes.fromhex("c20800"))
    patch_site(0x41A215, bytes.fromhex("e816f40600"), draw, opcode=b"\xe8")

    mouse_address = 0x411F20
    mouse_original = bytes.fromhex("55 8bec 83e4f8")
    # The board is in ECX. Copy x, y and click count from the caller's stack.
    mouse_args = bytes.fromhex("ff742430 ff742430 ff742430 51")
    skip = len(restored) + len(mouse_original) + 5
    prefix = saved + mouse_args + call_import(5) + bytes.fromhex("83c410 85c0 75") + bytes([skip])
    mouse = append_stub(prefix + restored + mouse_original,
                        mouse_address - IMAGE_BASE + len(mouse_original),
                        restored + bytes.fromhex("c20c00"))
    patch_site(mouse_address, mouse_original, mouse)

    # The original hit test receives the result in EAX and the bank on the stack.
    hide = append_stub(saved + bytes.fromhex("50 ff74242c") + call_import(6)
                       + bytes.fromhex("83c408 85c0 75 07") + restored, 0x489970 - IMAGE_BASE,
                       restored + bytes.fromhex("31c0 c20c00"))
    patch_site(0x41182B, bytes.fromhex("e840810700"), hide, opcode=b"\xe8")

    chooser_draw = code_rva + len(code)
    code.extend(saved + bytes.fromhex("ff742428 51") + call_import(7)
                + bytes.fromhex("83c408") + restored + bytes.fromhex("c20400"))
    patch_vtable(0x66D5E0, 0x484690, chooser_draw)

    chooser_mouse = append_stub(saved + mouse_args + call_import(8)
                                + bytes.fromhex("83c410 85c0 75 07") + restored,
                                0x486770 - IMAGE_BASE, restored + bytes.fromhex("c20c00"))
    patch_vtable(0x66D650, 0x486770, chooser_mouse)

    data_raw = len(image)
    code_raw = data_raw + ALIGNMENT
    image.extend(import_data + bytes(ALIGNMENT - len(import_data)))
    image.extend(code + bytes(ALIGNMENT - len(code)))

    for index, name, rva, raw, payload, flags in (
        (count, b".pvzd", data_rva, data_raw, import_data, 0xC0000040),
        (count + 1, b".pvzc", code_rva, code_raw, code, 0x60000020),
    ):
        struct.pack_into("<8s6I2HI", image, sections + index * 40, name, len(payload), rva,
                         ALIGNMENT, raw, 0, 0, 0, 0, flags)

    struct.pack_into("<H", image, file_header + 2, count + 2)
    set_u32(image, opt + 4, u32(image, opt + 4) + ALIGNMENT)
    set_u32(image, opt + 8, u32(image, opt + 8) + ALIGNMENT)
    set_u32(image, opt + 16, entry)
    set_u32(image, opt + 56, code_rva + ALIGNMENT)
    set_u32(image, opt + 64, 0)
    struct.pack_into("<II", image, dirs + 8, data_rva, descriptor + 40)
    struct.pack_into("<II", image, dirs + 32, 0, 0)
    return image


def build_module(scratch):
    include = Path(os.environ.get("PVZ_WINE_INCLUDE", "/usr/include/wine"))
    libs = Path(os.environ.get("PVZ_WINE_LIBS", "/usr/lib/wine/i386-windows"))
    generated = scratch / MODULE
    objects = []
    for source in ("game", "menu", "picker"):
        obj = scratch / f"{source}.obj"
        subprocess.run([
            "clang", "--target=i686-w64-windows-gnu", "-std=c17", "-ffreestanding",
            "-fno-stack-protector", "-Wall", "-Wextra", "-Werror", "-O2",
            "-isystem", str(include / "msvcrt"), "-isystem", str(include / "windows"),
            "-c", str(SOURCE / f"{source}.c"), "-o", str(obj),
        ], check=True)
        objects.append(str(obj))

    subprocess.run([
        "lld-link", "/dll", "/entry:DllMain@12", "/machine:x86", "/safeseh:no",
        f"/out:{generated}", *(f"/export:{name}" for name in EXPORTS), *objects,
        *(str(libs / f"lib{name}.a") for name in ("kernel32", "user32", "gdi32")),
    ], check=True)
    return generated.read_bytes()


def install_files(game_folder, image, module):
    with tempfile.TemporaryDirectory(prefix=".pvz-install-", dir=game_folder) as directory:
        exe_temp = Path(directory) / VARIANT
        dll_temp = Path(directory) / MODULE
        exe_temp.write_bytes(image)
        if os.name != "nt":
            exe_temp.chmod(0o755)
        dll_temp.write_bytes(module)
        dll_temp.replace(game_folder / MODULE)
        exe_temp.replace(game_folder / VARIANT)


def main():
    parser = argparse.ArgumentParser(description="Build or install Lawn Debug for Plants vs. Zombies.")
    parser.add_argument("--install", type=Path, metavar="GAME_FOLDER",
                        help="Use the release DLL to install into an existing game folder.")
    args = parser.parse_args()
    game_folder = SOURCE
    if args.install is not None:
        game_folder = args.install.resolve()
    elif not (SOURCE / ORIGINAL).is_file() and (SOURCE.parent / ORIGINAL).is_file():
        game_folder = SOURCE.parent

    try:
        original = game_folder / ORIGINAL
        if not original.is_file():
            parser.error(f"Could not find {original}. Select the folder containing the original game executable.")
        image = variant_bytes(original.read_bytes())
        if args.install is None:
            with tempfile.TemporaryDirectory(prefix=".pvz-build-", dir=SOURCE) as directory:
                module = build_module(Path(directory))
        else:
            release_dll = SOURCE / MODULE
            if not release_dll.is_file():
                parser.error(f"Missing {release_dll}. Extract the complete release ZIP before installing.")
            module = release_dll.read_bytes()
        install_files(game_folder, image, module)
    except PermissionError as error:
        parser.error(f"{error}. Close the game and make sure you can write to its folder.")
    except (OSError, ValueError) as error:
        parser.error(str(error))

    print(f"Created {VARIANT} and {MODULE} in {game_folder}")


if __name__ == "__main__":
    main()
