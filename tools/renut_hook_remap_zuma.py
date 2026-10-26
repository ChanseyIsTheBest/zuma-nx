#!/usr/bin/env python3
"""zuma-nx (2026-10-26): Renut's hook table (app/src/renut_engine/gpu/d3d/hooks/*.cpp) at Zuma's addresses
(XDK 2.0.2135). One-off, run once on the tree copied from feedingfrenzy-nx (whose table is at Feeding Frenzy's XDK
2571 addresses); docs/zuma-notes.md, "Renut's renderer at Zuma's addresses", has the evidence.

Zuma's game code calls 82 functions of the Direct3D library (out/zuma_d3d_api.txt); a PC trace (`trace1`: 56 of them
called by frame 2,400) tells which run. Each of Feeding Frenzy's active
hooks was paired with Zuma's function of the same name:
  - the API functions by an anchor alignment of the two libraries (out/renut_map/anchor.txt: exact-sequence anchors,
    the functions between them aligned on their shape), by field signatures and by reading both side by side
    (tools/ppc_dis.py), with a PC trace of Zuma's calls (app/src/zuma_d3d_trace.cpp);
  - the 200 render and sampler state functions from an alignment of the two state blocks (out/renut_map/
    state_align.txt: the same D3DRS order, each setter followed by its getter, one to one).

Every hook line whose function is mapped gets this game's address; every other one is commented out: not linked in
this XDK, or not called by the game (Direct3D's internal functions never run while its entry points are hooked).

  python -P tools/renut_hook_remap_zuma.py [--dry-run]
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOOKS = os.path.join(ROOT, 'app', 'src', 'renut_engine', 'gpu', 'd3d', 'hooks')

# Renut hook name (the line's comment) -> Zuma address, with the evidence.
API = {
    'Direct3D_CreateDevice': ('82121838', 'called once from sub_820E1DE0 with (0, 1, 0, 0, params, renderer+52); '
                                          'Feeding Frenzy\'s shape (34 instructions, lis 0x8007, 128, the device '
                                          'size 20608)'),
    'D3DDevice_Clear': ('821287F8', 'anchor (0.98) of Feeding Frenzy\'s 820B2428'),
    'D3DDevice_Present': ('821221C0', 'copies the resolve rectangles, resolves, calls Swap sub_82121BE0 (the only '
                                      'VdSwap caller)'),
    'D3DDevice_SetPixelShader': ('821246E0', '(dev, shader) at +12944, then the literal constants (memcpy)'),
    'D3DDevice_SetVertexShader': ('82124A50', '(dev, shader): AddRef, the old one released, stored at +20456, '
                                              'then sub_821248F8 (Feeding Frenzy\'s 820B6008 body)'),
    'D3DDevice_SetVertexDeclaration': ('82125C90', '(dev, decl): AddRef, stored at +11408, the old one released, '
                                                   'dirty bits 0x80 / 0x400 of the qword at +48'),
    'D3DDevice_SetStreamSource': ('8212D4D0', '(dev, stream, vb, offset, stride): 40 instructions, the old buffer '
                                              'released through sub_82122418'),
    'D3DDevice_DrawVertices': ('8212AA88', 'Feeding Frenzy\'s 820B4118 shape (144 instructions against 138, the '
                                           'same immediates 26 / 25 / 8450)'),
    'D3DVertexBuffer_Unlock': ('82122A88', 'reads +12 (4 instructions, Feeding Frenzy\'s reads +20)'),
    'D3DDevice_CreateVertexBuffer': ('82122968', '(length, usage, pool): Feeding Frenzy\'s shape, fields 8 lower'),
    'D3DDevice_CreatePixelShader': ('82125AC8', '(container) -> shader (lis 0xB580)'),
    'D3DDevice_CreateVertexShader': ('82125BB8', '(container) -> shader; 592-byte header (lis 0xA580)'),
    'D3DDevice_CreateVertexDeclaration': ('82124AB8', '(elements) -> declaration (255 end mark, lis 0x2480)'),
    # Called by the game on the textures and surfaces Renut creates (the trace), each reading XDK 2135's header
    # layout: Renut owns these objects, so every function the game calls on them is Renut's.
    # The device's own count: XDK 2135 keeps it at +12 (Renut's device at +0x3C). The game's sub_8216D8C8 can take the
    # device from a resource and release it again; with the guest's code on Renut's device a Release that read 1 at +12
    # would shut the engines down and free it (sub_821217E0 calls VdShutdownEngines' caller sub_82130050).
    'D3DDevice_AddRef': ('82121720', '`lwz r10,12(r3); addi; stw` (the device\'s count at +12)'),
    'D3DDevice_Release': ('821217E0', 'the count at +12; at 1 sub_82130050 (engines shut down) and XMemFree'),
    'D3DResource_GetDevice': ('82122900', '(resource, out): *VdGlobalDevice (the import slot 0x8200056C, or 0x82000570 '
                                          'for process type 2), AddRef\'d'),
    'D3DDevice_SetVertexShaderConstantB': ('821243D0', 'aligned with Feeding Frenzy\'s 820B5C78'),
    'D3DDevice_SetPixelShaderConstantB': ('82124428', 'aligned with Feeding Frenzy\'s 820B5CD0'),
    'D3DDevice_SetVertexShaderConstantI': ('82124480', 'aligned with Feeding Frenzy\'s 820B5D28'),
    'D3DDevice_SetPixelShaderConstantI': ('821244D0', 'aligned with Feeding Frenzy\'s 820B5D78'),
}
# The vertex buffer's Lock returns base + offset (as Feeding Frenzy's): the same adapter.
ADAPTERS = {
    'D3DVertexBuffer_Lock': ('82122A18', 'ZumaVertexBufferLock'),
    # The game's textures and surfaces are its own XDK 2135 objects (resource.cpp, "Zuma's textures"): what touches the
    # device goes to Renut through each texture's shadow header; Renut's own objects keep Renut's AddRef / Release.
    'D3DDevice_SetTexture': ('8212A900', 'ZumaSetTexture'),
    'D3DLineTexture_LockRect': ('8212A280', 'ZumaTextureLockRect'),
    'D3DArrayTexture_UnlockRect': ('82128940', 'ZumaTextureUnlockRect'),
    'D3DSurface_LockRect': ('8212A870', 'ZumaSurfaceLockRect'),
    'D3DSurface_UnlockRect': ('82128958', 'ZumaSurfaceUnlockRect'),
    'D3DResource_AddRef': ('82122300', 'ZumaAddRef'),
    'D3DResource_Release': ('82122880', 'ZumaRelease'),
    'D3DResource_GetType': ('82122368', 'ZumaGetType'),
}
ADAPTER_WHY = {
    'D3DVertexBuffer_Lock': 'returns base + offset (adapter, as Feeding Frenzy\'s)',
    'D3DDevice_SetTexture': '(dev, sampler, texture): Renut\'s SetTexture on the texture\'s shadow (adapter)',
    'D3DLineTexture_LockRect': 'LockRect (texture, level, locked, rect, flags), on the shadow (adapter)',
    'D3DArrayTexture_UnlockRect': 'UnlockRect (texture, level), on the shadow (adapter)',
    'D3DSurface_LockRect': '(surface, locked, rect, flags): its texture (+44), face (+24) and level (+32), on the '
                           'texture\'s shadow (adapter)',
    'D3DSurface_UnlockRect': '(surface): its texture\'s (+44) shadow (adapter)',
    'D3DResource_AddRef': 'the game\'s (the count in Common\'s low byte) or Renut\'s for its own objects (adapter)',
    'D3DResource_Release': 'the game\'s, then its texture\'s shadow dropped at 0; or Renut\'s for its own objects '
                           '(adapter)',
    'D3DResource_GetType': 'the game\'s (the type in Common bits 16-18); Renut\'s objects in XDK 2135\'s numbers '
                           '(adapter)',
}
# Left as the game's own code: pure work on XDK 2135's headers, which the game's D3DX code also reads itself.
GUEST_KEPT = {
    'D3DDevice_CreateTexture': '8212A3F0',
    'D3DTexture_GetSurfaceLevel': '8212A1D8',
    'D3DSurface_GetDesc': '8212A818',
    'D3D_Get2DSurfaceDesc_Thunk': '8212A1D0',
    'D3DBaseTexture_GetLevelCount': '8212A1C0',
}
PORT_HOOKS = {'ZumaSetRenderState57', 'ZumaGetRenderState57'}


# The state functions come from the game's own state tables, which its D3D_InitializeApiState (sub_8212F960) copies
# into the device's dispatch tables: the game reaches every state function only through those (no direct call).
# Records of {getter, setter, default}: 97 render states at 0x823924B8, 20 sampler states at 0x82392948. Against N&B's
# order (the template index of Renut's hooks), render states 0-56 are at the same index, Zuma's 57 is a state N&B does
# not have (sub_8212C260: four words of the device; its own hook in state.cpp), N&B's 57-95 are Zuma's 58-96, and N&B's
# 96-100 (HiZEnable, HiZWriteEnable, LastPixel, LineWidth, Buffer2Frames) are not in XDK 2135. The samplers are in the
# same order. (The alignment of the state blocks, out/renut_map/state_align.txt, gave the same functions but for 18,
# where the table's order decides: out/renut_map/zuma_state_table.txt.)
IMAGE_BASE = 0x82000000
D3D_LO, D3D_HI = 0x82121000, 0x8213B000  # the Direct3D library (out/zuma_d3d_api.txt)
RENDER_TABLE, RENDER_COUNT = 0x823924B8, 97
SAMPLER_TABLE, SAMPLER_COUNT = 0x82392948, 20


def state_address(kind, index, setter):
    image = open(os.path.join(ROOT, 'out', 'default_image.bin'), 'rb').read()  # tools/codegen.ps1 writes it
    if kind == 'Render':
        z = index if index <= 56 else index + 1
        if z >= RENDER_COUNT:
            return None
        table = RENDER_TABLE
    else:
        z, table = index, SAMPLER_TABLE
    word = int.from_bytes(image[table - IMAGE_BASE + 12 * z + (4 if setter else 0):][:4], 'big')
    # The table's getters of Wrap10-Wrap15 (and of the unused entries 0-9) are the game's shared `li r3,0; blr`
    # (sub_821939F0), used all over the game: a word outside the Direct3D library is never hooked.
    if not D3D_LO <= word < D3D_HI:
        return None
    return f'{word:08X}'


def main():
    dry = '--dry-run' in sys.argv
    mapped = disabled = 0
    for name in sorted(os.listdir(HOOKS)):
        if not name.endswith('.cpp'):
            continue
        path = os.path.join(HOOKS, name)
        text = open(path, encoding='utf-8', newline='').read()
        nl = '\r\n' if '\r\n' in text else '\n'
        lines = text.split(nl)
        # A hook Zuma needs that Feeding Frenzy's table had commented out, or alone in an `#if 0` block (Bejeweled 3's
        # "not linked"), is made an active line again first.
        for i, line in enumerate(lines):
            m = re.match(r'(\s*)// feedingfrenzy-nx: not linked or not called in XDK 2571: '
                         r'(BANJO_RENUT_HOOK(?:_SHARED)?\(sub_XXXXXXXX, [^;]+\);\s*//\s*(\w+).*)$', line)
            if m and (m.group(3) in API or m.group(3) in ADAPTERS or re.search(r'(Set|Get)(Render|Sampler)StateHook<\d+>', m.group(2))):
                lines[i] = m.group(1) + m.group(2).replace('sub_XXXXXXXX', 'sub_00000000')
        for i in range(len(lines) - 2):
            m = re.match(r'\s*BANJO_RENUT_HOOK(?:_SHARED)?\(sub_[0-9A-F]{8}, [^;]+\);\s*//\s*(\w+)', lines[i + 1] or '')
            if (lines[i] or '').strip().startswith('#if 0') and (lines[i + 2] or '').strip().startswith('#endif') \
                    and m and (m.group(1) in API or m.group(1) in ADAPTERS):
                lines[i] = lines[i + 2] = None
        lines = [l for l in lines if l is not None]
        depth = 0  # inside #if 0
        for i, line in enumerate(lines):
            s = line.strip()
            if s.startswith('#if 0'):
                depth += 1
                continue
            if depth and s.startswith('#if'):
                depth += 1
                continue
            if depth and s.startswith('#endif'):
                depth -= 1
                continue
            if depth:
                continue
            m = re.match(r'(\s*)(BANJO_RENUT_HOOK(?:_SHARED)?)\(sub_([0-9A-F]{8}), ([^;]+)\);\s*//\s*(\w+)(.*)$', line)
            if not m:
                continue
            indent, macro, old, target, hname, rest = m.groups()
            if target in PORT_HOOKS:  # this port's own hooks, written by hand: left as they are
                continue
            addr, why = None, None
            if hname in GUEST_KEPT:
                lines[i] = (f'{indent}// zuma-nx: the game\'s own code (sub_{GUEST_KEPT[hname]}, XDK 2135\'s headers): '
                            f'{macro}(sub_XXXXXXXX, {target});  // {hname}')
                disabled += 1
                continue
            if hname in API:
                addr, why = API[hname]
            elif hname in ADAPTERS:
                addr, target = ADAPTERS[hname]
                why = ADAPTER_WHY[hname]
            elif (sm := re.match(r'(Set|Get)(Render|Sampler)StateHook<(\d+)>', target)):
                addr = state_address(sm.group(2), int(sm.group(3)), sm.group(1) == 'Set')
                why = 'the game\'s state table' if addr else None
            ff = f'Feeding Frenzy {old}' if old != '00000000' else 'not in Feeding Frenzy\'s table'
            if addr:
                lines[i] = f'{indent}{macro}(sub_{addr}, {target});  // {hname}  // zuma-nx: {why} ({ff})'
                mapped += 1
            else:
                lines[i] = (f'{indent}// zuma-nx: not linked or not called in XDK 2135: '
                            f'{macro}(sub_XXXXXXXX, {target});  // {hname} ({ff})')
                disabled += 1
        if not dry:
            open(path, 'w', encoding='utf-8', newline='').write(nl.join(lines))
    print(f'renut_hook_remap_zuma: {mapped} hooks at Zuma\'s addresses, {disabled} commented out'
          + (' (dry run)' if dry else ''))


if __name__ == '__main__':
    main()
