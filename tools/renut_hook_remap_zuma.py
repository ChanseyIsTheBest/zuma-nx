#!/usr/bin/env python3
"""zuma-nx (2026-10-26): Renut's hook table (app/src/renut_engine/gpu/d3d/hooks/*.cpp) at Zuma's addresses
(XDK 2.0.2135). One-off, run once on the tree copied from feedingfrenzy-nx (whose table is at Feeding Frenzy's XDK
2571 addresses); docs/zuma-notes.md, "Renut's renderer at Zuma's addresses", has the evidence.

Zuma's game code calls 82 functions of the Direct3D library (out/zuma_d3d_api.txt). Each of Feeding Frenzy's active
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
    'D3DDevice_SetTexture': ('8212A900', '(dev, sampler, texture): AddRef of the new texture (+4), the old one '
                                         'released through sub_82122418; 98 instructions'),
    'D3DDevice_SetPixelShader': ('821246E0', '(dev, shader) at +12944, then the literal constants (memcpy)'),
    'D3DDevice_SetVertexShader': ('82124A50', '(dev, shader): AddRef, the old one released, stored at +20456, '
                                              'then sub_821248F8 (Feeding Frenzy\'s 820B6008 body)'),
    'D3DDevice_SetVertexDeclaration': ('82125C90', '(dev, decl): AddRef, stored at +11408, the old one released, '
                                                   'dirty bits 0x80 / 0x400 of the qword at +48'),
    'D3DDevice_SetStreamSource': ('8212D4D0', '(dev, stream, vb, offset, stride): 40 instructions, the old buffer '
                                              'released through sub_82122418'),
    'D3DDevice_DrawVertices': ('8212AA88', 'Feeding Frenzy\'s 820B4118 shape (144 instructions against 138, the '
                                           'same immediates 26 / 25 / 8450)'),
    'D3DResource_Release': ('82122880', 'decrements Common (lis 0x0004), destroys at 0 (calls itself for the '
                                        'parent)'),
    'D3DVertexBuffer_Unlock': ('82122A88', 'reads +12 (4 instructions, Feeding Frenzy\'s reads +20)'),
    'D3DDevice_CreateTexture': ('8212A3F0', '(width, height, depth, levels, usage, format, pool, type): '
                                            'Feeding Frenzy\'s shape, fields 8 bytes lower'),
    'D3DDevice_CreateVertexBuffer': ('82122968', '(length, usage, pool): Feeding Frenzy\'s shape, fields 8 lower'),
    'D3DDevice_CreatePixelShader': ('82125AC8', '(container) -> shader (lis 0xB580)'),
    'D3DDevice_CreateVertexShader': ('82125BB8', '(container) -> shader; 592-byte header (lis 0xA580)'),
    'D3DDevice_CreateVertexDeclaration': ('82124AB8', '(elements) -> declaration (255 end mark, lis 0x2480)'),
    'D3D_Get2DSurfaceDesc_Thunk': ('8212A340', '`b 0x82129790` (Feeding Frenzy\'s 820B3160 body, field '
                                               'signature 1.00)'),
    'D3DLineTexture_LockRect': ('8212A280', 'LockRect (texture, level, locked, rect, flags): 6 instructions, '
                                            'field signature 1.00'),
    'D3DArrayTexture_UnlockRect': ('82128940', 'aligned with Feeding Frenzy\'s 820B2570'),
    'D3DSurface_UnlockRect': ('82128958', 'aligned with Feeding Frenzy\'s 820B2588'),
    'D3DDevice_SetVertexShaderConstantB': ('821243D0', 'aligned with Feeding Frenzy\'s 820B5C78'),
    'D3DDevice_SetPixelShaderConstantB': ('82124428', 'aligned with Feeding Frenzy\'s 820B5CD0'),
    'D3DDevice_SetVertexShaderConstantI': ('82124480', 'aligned with Feeding Frenzy\'s 820B5D28'),
    'D3DDevice_SetPixelShaderConstantI': ('821244D0', 'aligned with Feeding Frenzy\'s 820B5D78'),
}
# The vertex buffer's Lock returns base + offset (as Feeding Frenzy's): the same adapter.
ADAPTERS = {
    'D3DVertexBuffer_Lock': ('82122A18', 'ZumaVertexBufferLock'),
}


def state_map():
    out = {}
    for line in open(os.path.join(ROOT, 'out', 'renut_map', 'state_align.txt')):
        m = re.match(r'([0-9A-F]{8})\s+\d+\s+\S+\s+([-\d.]+)\s+(D3DDevice_[GS]et(?:Render|Sampler)State_\w+)', line)
        if m:
            out[m.group(3)] = m.group(1)
    return out


def main():
    dry = '--dry-run' in sys.argv
    states = state_map()
    mapped = disabled = 0
    for name in sorted(os.listdir(HOOKS)):
        if not name.endswith('.cpp'):
            continue
        path = os.path.join(HOOKS, name)
        text = open(path, encoding='utf-8', newline='').read()
        nl = '\r\n' if '\r\n' in text else '\n'
        lines = text.split(nl)
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
            addr, why = None, None
            if hname in API:
                addr, why = API[hname]
            elif hname in ADAPTERS:
                addr, target = ADAPTERS[hname]
                why = 'returns base + offset (adapter, as Feeding Frenzy\'s)'
            elif hname in states:
                addr, why = states[hname], 'state block alignment'
            if addr:
                lines[i] = f'{indent}{macro}(sub_{addr}, {target});  // {hname}  // zuma-nx: {why} (Feeding Frenzy {old})'
                mapped += 1
            else:
                lines[i] = (f'{indent}// zuma-nx: not linked or not called in XDK 2135: '
                            f'{macro}(sub_XXXXXXXX, {target});  // {hname} (Feeding Frenzy {old})')
                disabled += 1
        if not dry:
            open(path, 'w', encoding='utf-8', newline='').write(nl.join(lines))
    print(f'renut_hook_remap_zuma: {mapped} hooks at Zuma\'s addresses, {disabled} commented out'
          + (' (dry run)' if dry else ''))


if __name__ == '__main__':
    main()
