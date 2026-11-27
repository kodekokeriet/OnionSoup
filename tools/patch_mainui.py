#!/usr/bin/env python3
"""Patch Miyoo MainUI so game names in the list are not drawn under the box art.

MainUI reserves room for the box art (TextMenuDrawer field +4 = 250 px when a
game has an image; the favorite mark is moved left by it), but it still draws
the game name in full width, so the image covers the end of the name. This
patch makes the list clip each name at the left edge of the box art area:

1. TextMenuDrawer::draw: the text rect width becomes list width minus the
   reserved box art width (0 when there is no image).
2. The list item text helper asks drawText for a new alignment, 5 = "left,
   clipped at x = rect.w - 20" (rect.w is used as the right edge).
3. drawText (generic text blitter) is rewritten more compactly with the same
   behaviour for alignments 0-4, plus alignment 5. Its dead shadow render
   (the shadow blit was already removed) is dropped.

Usage: tools/patch_mainui.py <MainUI> [more MainUI files]   (patches in place,
skips files that are already patched). Needs: pip install pyelftools keystone-engine
"""
import struct
import sys

from elftools.elf.elffile import ELFFile
from keystone import KS_ARCH_ARM, KS_MODE_ARM, Ks

MARK = b"OSOUPCLP"  # written into the dead tail of the old drawText


def plt_stubs(elf, data):
    dynsym = elf.get_section_by_name(".dynsym")
    got = {r["r_offset"]: dynsym.get_symbol(r["r_info_sym"]).name
           for r in elf.get_section_by_name(".rel.plt").iter_relocations()}
    plt = elf.get_section_by_name(".plt")
    base, off, size = plt["sh_addr"], plt["sh_offset"], plt["sh_size"]
    stubs = {}
    # entries after the 20-byte header: add ip,pc,#A ; add ip,ip,#B ; ldr pc,[ip,#C]!
    for a in range(base + 20, base + size - 8, 12):
        w = struct.unpack_from("<3I", data, off + a - base)
        if (w[0] & 0xFFFFF000) != 0xE28FC000 or (w[1] & 0xFFFFF000) != 0xE28CC000:
            continue

        def imm(x):
            rot, v = (x >> 8) & 0xF, x & 0xFF
            return ((v >> (2 * rot)) | (v << (32 - 2 * rot))) & 0xFFFFFFFF

        target = (a + 8 + imm(w[0]) + imm(w[1]) + (w[2] & 0xFFF)) & 0xFFFFFFFF
        if target in got:
            stubs[got[target]] = a
    return stubs


def find_one(data, pattern, mask=None):
    hits = []
    start = 0
    n = len(pattern)
    while True:
        i = data.find(pattern[:8], start)
        if i < 0:
            break
        ok = all((data[i + k] & (mask[k] if mask else 0xFF)) == (pattern[k] & (mask[k] if mask else 0xFF))
                 for k in range(n))
        if ok:
            hits.append(i)
        start = i + 4
    if len(hits) != 1:
        raise SystemExit("pattern found %d times" % len(hits))
    return hits[0]


def words(*ws):
    return b"".join(struct.pack("<I", w) for w in ws)


DRAWTEXT_ASM = """
    push {r4, r5, r6, r7, r8, lr}
    sub sp, sp, #16
    mov r4, r0
    mov r5, r2
    mov r6, r3
    movs r0, r1
    beq ret0
    ldr r1, [sp, #40]
    ldr r2, [sp, #44]
    bl %(render)d
    movs r7, r0
    beq ret0
    ldr r0, [r7, #8]
    ldr r1, [r7, #12]
    sxth r2, r5
    asr r3, r5, #16
    uxth ip, r6
    lsr lr, r6, #16
    sub ip, ip, r0
    sub lr, lr, r1
    ldr r8, [sp, #48]
    cmp r8, #0
    beq done
    cmp r8, #1
    beq center
    cmp r8, #5
    beq clip
    cmp r8, #3
    beq vcenter
    add r2, r2, ip
    cmp r8, #4
    bne done
vcenter:
    add lr, lr, lr, lsr #31
    add r3, r3, lr, asr #1
    b done
center:
    add ip, ip, ip, lsr #31
    add r2, r2, ip, asr #1
    b vcenter
clip:
    uxth ip, r6
    sub ip, ip, r2
    sub ip, ip, #20
    cmp r0, ip
    movgt r0, ip
    cmp r0, #0
    movlt r0, #0
done:
    strh r2, [sp, #8]
    strh r3, [sp, #10]
    mov r2, #0
    str r2, [sp]
    strh r0, [sp, #4]
    strh r1, [sp, #6]
    mov r0, r7
    mov r1, sp
    mov r2, r4
    add r3, sp, #8
    bl %(blit)d
    ldr r4, [r7, #8]
    mov r0, r7
    bl %(free)d
    mov r0, r4
    b out
ret0:
    mov r0, #0
out:
    add sp, sp, #16
    pop {r4, r5, r6, r7, r8, pc}
"""

# start of the original drawText (identical in all MainUI builds)
DRAWTEXT_SIG = words(0xE92D4800, 0xE28DB004, 0xE24DD030, 0xE50B0028, 0xE50B102C,
                     0xE24B1034, 0xE881000C, 0xE51B302C, 0xE3530000)
# TextMenuDrawer::draw, text rect width (K = 0x28 plain item, 0x6f / 0x14 item
# with / without its own icon): ldr r3,[fp,#-0x50]; ldrh r3,[r3,#0x34];
# sub r3,r3,#K; uxth r3,r3; strh r3,[fp,#-0x38]
RECT_SIGS = [words(0xE51B3050, 0xE1D333B4, 0xE2433000 | k, 0xE6FF3073, 0xE14B33B8)
             for k in (0x28, 0x6F, 0x14)]
# list item text helper, simple path: ldrb r3,[fp,#0x1c]; str r3,[sp,#0xc];
# ldr r3,[fp,#0x18]; str r3,[sp,#8]; ldr r3,[fp,#0x14]; str r3,[sp,#4];
# ldr r3,[fp,#0x10]; str r3,[sp]; add r3,fp,#8; ldm r3,{r2,r3};
# ldr r1,[fp,#-0xc8]; ldr r0,[fp,#-0xc4]
ITEM_SIG = words(0xE5DB301C, 0xE58D300C, 0xE59B3018, 0xE58D3008, 0xE59B3014,
                 0xE58D3004, 0xE59B3010, 0xE58D3000, 0xE28B3008, 0xE893000C,
                 0xE51B10C8, 0xE51B00C4)


def patch(path):
    data = bytearray(open(path, "rb").read())
    if MARK in data:
        print("%s: already patched" % path)
        return
    elf = ELFFile(open(path, "rb"))
    text = elf.get_section_by_name(".text")
    va = lambda off: text["sh_addr"] + off - text["sh_offset"]
    stubs = plt_stubs(elf, data)
    ks = Ks(KS_ARCH_ARM, KS_MODE_ARM)

    # 1. drawText
    dt = find_one(data, DRAWTEXT_SIG)
    end = data.find(words(0xE24BD004, 0xE8BD8800), dt)  # sub sp,fp,#4 ; pop {fp,pc}
    asm = DRAWTEXT_ASM % {"render": stubs["TTF_RenderUTF8_Blended"],
                          "blit": stubs["SDL_UpperBlit"], "free": stubs["SDL_FreeSurface"]}
    code = bytes(ks.asm(asm, va(dt))[0])
    room = end + 8 - dt
    if len(code) + len(MARK) > room:
        raise SystemExit("new drawText does not fit")
    nop = words(0xE320F000)
    tail = room - len(code) - len(MARK)
    data[dt:dt + room] = code + nop * (tail // 4) + MARK

    # 2. text rect width = list width - reserved box art width (drawText's
    #    alignment 5 turns it into the right edge for clipping)
    for sig in RECT_SIGS:
        r = find_one(data, sig)
        data[r:r + 20] = bytes(ks.asm("""
            ldr r3, [fp, #-0x50]
            ldrh r2, [r3, #0x34]
            ldr r3, [r3, #4]
            sub r3, r2, r3
            strh r3, [fp, #-0x38]""", va(r))[0])

    # 3. both simple-path drawText calls in the item helper use alignment 5
    start = 0
    n = 0
    sig = bytes(ITEM_SIG)
    while True:
        i = data.find(sig, start)
        if i < 0:
            break
        data[i + 8:i + 12] = words(0xE3A03005)  # mov r3, #5
        n += 1
        start = i + 4
    if n != 2:
        raise SystemExit("item helper calls found: %d" % n)

    open(path, "wb").write(data)
    print("%s: patched (drawText at 0x%x)" % (path, va(dt)))


if __name__ == "__main__":
    for p in sys.argv[1:]:
        patch(p)
