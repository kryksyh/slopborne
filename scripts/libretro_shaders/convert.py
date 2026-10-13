#!/usr/bin/env python3
# usage: convert.py <libretro .glsl> <out .comp> <source path for the header> [--no-params]
# Writes <out .comp>.json with the shader's #pragma parameters that became menu knobs.
# Turns a single-pass libretro GLSL shader (VERTEX/FRAGMENT in one file) into a Vulkan compute
# shader: fragment part only, uniforms -> push constants, Texture -> binding 0, output -> binding 1.
import re, sys
import json
src, out, origin = sys.argv[1], sys.argv[2], sys.argv[3]
with_params = '--no-params' not in sys.argv
s = open(src, errors='replace').read()
head = s[:s.index('#if defined(VERTEX)')] if '#if defined(VERTEX)' in s else ''
frag = s[s.index('defined(FRAGMENT)'):]
frag = frag[frag.index('\n') + 1:]
vert = ''
if '#if defined(VERTEX)' in s:
    vert = s[s.index('#if defined(VERTEX)'):s.index('defined(FRAGMENT)')]
    vert = vert[vert.index('\n') + 1:]
    vert = vert[:vert.rfind('#elif')] if '#elif' in vert else vert
# drop the closing #endif of the VERTEX/FRAGMENT block
frag = frag[:frag.rstrip().rfind('#endif')]
drop = [r'^\s*#\s*version.*$',
        r'^\s*(uniform\s+)?(COMPAT_PRECISION\s+)?(int|float|vec2)\s+(FrameDirection|FrameCount|OutputSize|TextureSize|InputSize)\s*;.*$',
        r'^\s*uniform\s+sampler2D\s+Texture\s*;.*$',
        r'^\s*COMPAT_VARYING\s+.*;.*$',
        r'^\s*out\s+(COMPAT_PRECISION\s+)?vec4\s+FragColor\s*;.*$',
        r'^\s*#\s*define\s+FragColor\s+gl_FragColor.*$',
        r'^\s*precision\s+\w+\s+float\s*;.*$',
        r'^\s*uniform\s+(COMPAT_PRECISION\s+)?(int|float)\s+(OrigInputSize|OrigTextureSize).*$',
        r'^\s*#\s*define\s+COMPAT_(TEXTURE|VARYING|PRECISION|ATTRIBUTE)\b.*$',
        r'^\s*#\s*define\s+texture2D\b.*$']
for d in drop:
    frag = re.sub(d, '', frag, flags=re.M)
frag = frag.replace('gl_FragColor', 'FragColor').replace('gl_FragCoord', 'FragCoordC')
# Every sample goes through bbport_texture (channel order of the game's picture).
frag = re.sub(r'\btexture\s*\(', 'bbport_texture(', frag)
# The vertex stage runs per pixel: its varyings become globals the fragment code reads.
for d in drop:
    vert = re.sub(d.replace(r'^\s*COMPAT_VARYING\s+.*;.*$', r'^\s*COMPAT_VARYING\s+(COMPAT_PRECISION\s+)?vec4\s+TEX0\s*;.*$'), '', vert, flags=re.M)
vert = re.sub(r'^\s*COMPAT_ATTRIBUTE\s+.*;.*$', '', vert, flags=re.M)
vert = re.sub(r'^\s*(uniform\s+)?(COMPAT_PRECISION\s+)?mat4\s+MVPMatrix\s*;.*$', '', vert, flags=re.M)
vert = re.sub(r'^(\s*)COMPAT_VARYING\s+', r'\1', vert, flags=re.M)
vert = re.sub(r'^(\s*)(in|attribute|varying|out)\s+(?=(COMPAT_PRECISION\s+|highp\s+|mediump\s+|lowp\s+)?(vec|float|mat))', r'\1', vert, flags=re.M)
vert = vert.replace('gl_Position', 'bbport_position')
vert = re.sub(r'\bvoid\s+main\s*\(\s*(void)?\s*\)', 'void vertex_main()', vert)
def functions(text):
    names = {}
    for m in re.finditer(r'^[ \t]*(?:\w+[ \t]+)+(\w+)[ \t]*\([^;{)]*\)\s*\{', text, re.M):
        depth, i = 0, m.end() - 1
        while i < len(text):
            depth += text[i] == '{'
            depth -= text[i] == '}'
            i += 1
            if depth == 0:
                break
        names.setdefault(m.group(1), []).append((m.start(), i))
    return names
vert_functions = functions(vert)
spans = [span for name, list_ in functions(frag).items() if name in vert_functions and name != 'shader_main'
         for span in list_]
for start, end in sorted(spans, reverse=True):
    frag = frag[:start] + frag[end:]
frag = re.sub(r'^\s*(varying|in)\s+(COMPAT_PRECISION\s+|highp\s+|mediump\s+|lowp\s+)?(float|vec[234]|mat[234])\s+\w+\s*;.*$', '', frag, flags=re.M)
frag = re.sub(r'^\s*out\s+(COMPAT_PRECISION\s+|highp\s+|mediump\s+|lowp\s+)?vec4\s+\w+\s*;.*$', '', frag, flags=re.M)
# Code before the stages (shared #defines, helpers) is kept too.
head_code = '\n'.join(l for l in head.split('\n') if not l.strip().startswith('#version'))
frag = head_code + '\n' + vert + '\n' + frag
# Declarations the wrapper provides, in any spelling.
frag = re.sub(r'^\s*(uniform\s+|in\s+|attribute\s+|varying\s+|out\s+|COMPAT_ATTRIBUTE\s+|COMPAT_VARYING\s+)?'
              r'(COMPAT_PRECISION\s+|highp\s+|mediump\s+|lowp\s+)?(vec[234]|mat4|float|int)\s+(VertexCoord|TexCoord|COLOR|MVPMatrix|TEX0|FrameDirection|FrameCount|OutputSize|TextureSize|InputSize|OrigInputSize|OrigTextureSize)\s*;.*$',
              '', frag, flags=re.M)
frag = re.sub(r'^(\s*)(attribute|varying)\s+(?=(highp\s+|mediump\s+|lowp\s+)?(vec|float|mat))', r'\1', frag, flags=re.M)
# Runtime parameters: loose uniforms become constants at their #pragma parameter default.
pragmas = re.findall(r'#pragma\s+parameter\s+(\w+)\s+"([^"]*)"\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)\s*([-+0-9.eE]+)?', s)
defaults = {p[0]: p[2] for p in pragmas}
info = {p[0]: p for p in pragmas}
knobs = {}
def to_const(m):
    ty, name = m.group(3), m.group(4)
    if with_params and ty in ('float', 'int') and name in info and (name in knobs or len(knobs) < 24):
        k = knobs.setdefault(name, len(knobs))
        return f'#define {name} {"int(" if ty == "int" else "("}bbport_param[{k}])'
    if ty in ('float', 'int'):
        value = defaults.get(name, '0')
        if ty == 'float' and '.' not in value and 'e' not in value.lower():
            value += '.0'
        if ty == 'int':
            value = str(int(float(value)))
        return f'const {ty} {name} = {value};'
    return f'{ty} {name} = {ty}(0.0);'
frag = re.sub(r'^\s*uniform\s+(COMPAT_PRECISION\s+|highp\s+|mediump\s+|lowp\s+)?()(float|int|vec2|vec3|vec4)\s+(\w+)\s*;.*$',
              to_const, frag, flags=re.M)
frag = re.sub(r'\bvoid\s+main\s*\(\s*(void)?\s*\)', 'void shader_main()', frag)
# head comments (author, license) are kept verbatim
comments = '\n'.join(l for l in head.split('\n') if l.strip().startswith(('//', '/*', '*', '*/')) or l.strip() == '')
prelude = f'''#version 450
// bbport: compute port of the libretro shader {origin}
// (https://github.com/libretro/glsl-shaders), unchanged except for the wrapper below.
// Original header:
{comments.strip()}

BBPORT_PARAMETER_UNIFORM
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0) uniform sampler2D Texture;
layout(binding = 1, rgba8) uniform writeonly image2D bbport_result;
layout(push_constant) uniform BbportParams {{
    vec2 OutputSize;
    vec2 TextureSize;
    vec2 InputSize;
    int FrameCount;
    int FrameDirection;
    float bbport_param[24]; // the menu's knobs: the shader's #pragma parameters
    vec4 bbport_view_to_world[3];
    vec4 bbport_projection;
    vec4 bbport_flags; // z: the game stores this picture blue-first (read and write swapped)
}};
vec4 bbport_texture(sampler2D s, vec2 uv) {{
    vec4 c = texture(s, uv);
    return bbport_flags.z > 0.5 ? c.bgra : c;
}}
vec4 bbport_texture(sampler2D s, vec2 uv, float bias) {{
    vec4 c = texture(s, uv, bias);
    return bbport_flags.z > 0.5 ? c.bgra : c;
}}
#define COMPAT_TEXTURE bbport_texture
#define texture2D bbport_texture
#define OrigInputSize InputSize
#define OrigTextureSize TextureSize
#define COMPAT_VARYING
#define COMPAT_PRECISION
#define COMPAT_ATTRIBUTE
vec4 TEX0;
vec4 TexCoord;
vec4 VertexCoord;
vec4 COLOR = vec4(1.0);
mat4 MVPMatrix = mat4(1.0);
vec4 bbport_position;
vec4 FragColor;
vec4 FragCoordC;
vec2 bbport_output_size() {{ return OutputSize; }}
vec2 bbport_texture_size() {{ return TextureSize; }}
vec2 bbport_input_size() {{ return InputSize; }}
'''
epilogue = '''
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(p, ivec2(bbport_output_size())))) return;
    vec2 uv = (vec2(p) + 0.5) / bbport_output_size();
    TEX0 = vec4(uv * bbport_input_size() / bbport_texture_size(), 0.0, 0.0);
    TexCoord = TEX0;
    VertexCoord = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    FragCoordC = vec4(vec2(p) + 0.5, 0.0, 1.0);
    FragColor = vec4(0.0);
    BBPORT_VERTEX_CALL
    shader_main();
    vec3 bbport_color = clamp(FragColor.rgb, 0.0, 1.0);
    imageStore(bbport_result, p, vec4(bbport_flags.z > 0.5 ? bbport_color.bgr : bbport_color, 1.0));
}
'''
epilogue = epilogue.replace('BBPORT_VERTEX_CALL', 'vertex_main();' if 'vertex_main' in frag else '')
prelude = prelude.replace('BBPORT_PARAMETER_UNIFORM', '#define PARAMETER_UNIFORM' if with_params else '')
open(out, 'w').write(prelude + frag + epilogue)
params = [{'name': n, 'label': info[n][1], 'default': float(info[n][2]), 'min': float(info[n][3]),
           'max': float(info[n][4]), 'step': float(info[n][5] or 0), 'index': k}
          for n, k in sorted(knobs.items(), key=lambda kv: kv[1])]
json.dump(params, open(out + '.json', 'w'), indent=1)
