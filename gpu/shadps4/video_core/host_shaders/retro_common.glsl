// bbport: shared inputs and helpers of the port's own picture filters (Filters tab).
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0) uniform sampler2D source_tex;   // the finished scene, before the UI
layout(binding = 1, rgba8) uniform writeonly image2D result_img;
layout(binding = 2) uniform texture2D depth_tex;    // D32, 0..1 (D3D projection), scene pixels
layout(push_constant) uniform Params {
    vec2 output_size;
    vec2 texture_size; // the scene's image (the scene fills its top-left input_size)
    vec2 input_size;
    int frame_count;
    int frame_direction;
    float knobs[24];       // the filter's settings, in BbFilters order
    vec4 view_to_world[3]; // rows, xyz rotation
    vec4 projection;       // x scale, y scale, z scale, z offset
    vec4 flags;            // x: camera valid, y: history reset (datamosh), z: scene is blue-first
};

ivec2 out_pixel;
vec2 out_uv;     // 0..1 over the output
vec2 scene_pos;  // continuous scene pixel position
ivec2 scene_px;  // the scene pixel under this output pixel

bool begin_pixel() {
    out_pixel = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(out_pixel, ivec2(output_size)))) return false;
    out_uv = (vec2(out_pixel) + 0.5) / output_size;
    scene_pos = out_uv * input_size;
    scene_px = ivec2(scene_pos);
    return true;
}

// The game may store its picture blue-first (and swap back when displaying it).
vec3 swap_order(vec3 c) {
    return flags.z > 0.5 ? c.bgr : c;
}

void finish(vec3 color) {
    imageStore(result_img, out_pixel, vec4(swap_order(clamp(color, 0.0, 1.0)), 1.0));
}

// Output pixels per scene pixel.
float pixel_scale() {
    return output_size.y / input_size.y;
}

ivec2 clamp_scene(ivec2 sp) {
    return clamp(sp, ivec2(0), ivec2(input_size) - 1);
}

vec3 scene_color(ivec2 sp) {
    return swap_order(texture(source_tex, (vec2(clamp_scene(sp)) + 0.5) / texture_size).rgb);
}

// Bilinear scene color at a continuous scene position.
vec3 scene_sample(vec2 pos) {
    return swap_order(texture(source_tex, clamp(pos, vec2(0.5), input_size - 0.5) / texture_size).rgb);
}

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

bool has_camera() {
    return flags.x > 0.5;
}

// View distance of a scene pixel; the sky (depth 1) is far away.
float scene_distance(ivec2 sp) {
    float depth = texelFetch(depth_tex, clamp_scene(sp), 0).r;
    if (!has_camera() || depth >= 1.0) return 1e6;
    return projection.w / min(depth - projection.z, -1e-8);
}

vec3 view_position(ivec2 sp) {
    float z = scene_distance(sp);
    vec2 ndc = vec2((float(sp.x) + 0.5) / input_size.x * 2.0 - 1.0,
                    1.0 - (float(sp.y) + 0.5) / input_size.y * 2.0);
    return vec3(ndc.x * z / projection.x, ndc.y * z / projection.y, z);
}

// View-space normal from the depth of the neighbours (zero without a camera or on the sky).
vec3 view_normal(ivec2 sp) {
    if (!has_camera() || scene_distance(sp) > 1e5) return vec3(0.0);
    vec3 c = view_position(sp);
    vec3 dx = view_position(sp + ivec2(1, 0)) - c, dx2 = c - view_position(sp - ivec2(1, 0));
    vec3 dy = view_position(sp + ivec2(0, 1)) - c, dy2 = c - view_position(sp - ivec2(0, 1));
    if (dot(dx2, dx2) < dot(dx, dx)) dx = dx2;
    if (dot(dy2, dy2) < dot(dy, dy)) dy = dy2;
    vec3 n = cross(dy, dx);
    return dot(n, n) > 0.0 ? normalize(n) : vec3(0.0);
}

// 1 on a silhouette or crease: a neighbour at `width` pixels much nearer or farther (relative,
// so near and far objects get the same line), or a sharp bend; `sensitivity` scales both.
float depth_edge(ivec2 sp, float sensitivity, int width) {
    if (!has_camera()) return 0.0;
    float c = scene_distance(sp);
    float l = scene_distance(sp + ivec2(-width, 0)), r = scene_distance(sp + ivec2(width, 0));
    float t = scene_distance(sp + ivec2(0, -width)), b = scene_distance(sp + ivec2(0, width));
    float jump = max(max(abs(l - c), abs(r - c)), max(abs(t - c), abs(b - c))) / max(c, 1e-3);
    float bend = (abs(l + r - 2.0 * c) + abs(t + b - 2.0 * c)) / max(c, 1e-3);
    float s = max(sensitivity, 0.01);
    return (jump * s > 0.06 || bend * s > 0.05) ? 1.0 : 0.0;
}

// Brightness smoothed over a 5x5 window within the same surface (similar depth): texture
// detail (grass, stone grain) flattens, lighting and silhouettes stay.
float surface_luma(ivec2 sp) {
    float center = scene_distance(sp);
    float sum = 0.0, weight = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            ivec2 q = sp + ivec2(x, y);
            float w = abs(scene_distance(q) - center) < 0.03 * center ? 1.0 : 0.0;
            sum += luma(scene_color(q)) * w;
            weight += w;
        }
    }
    return weight > 0.0 ? sum / weight : luma(scene_color(sp));
}

float lift(float l) {
    return pow(clamp(l * 1.6, 0.0, 1.0), 0.7);
}

// Tone 0..1: the lifted brightness, `adapt` of it relative to the average of a ring around the
// pixel, so a dark scene (Hunter's Dream at night) still spreads over the whole range.
float local_tone(ivec2 sp, float adapt, bool smooth_surface) {
    float l = lift(smooth_surface ? surface_luma(sp) : luma(scene_color(sp)));
    float sum = 0.0;
    for (int i = 0; i < 16; ++i) {
        float a = float(i) * 0.3927;
        float r = (i & 1) == 0 ? 6.0 : 14.0;
        sum += lift(luma(scene_color(sp + ivec2(round(vec2(cos(a), sin(a)) * r)))));
    }
    float relative = 0.5 * l / max(sum / 16.0, 0.04);
    return clamp(mix(l, relative, adapt), 0.0, 1.0);
}

float bayer8(ivec2 p) {
    const int m[64] = int[64](0, 32, 8, 40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26,
                              12, 44, 4, 36, 14, 46, 6, 38, 60, 28, 52, 20, 62, 30, 54, 22,
                              3, 35, 11, 43, 1, 33, 9, 41, 51, 19, 59, 27, 49, 17, 57, 25,
                              15, 47, 7, 39, 13, 45, 5, 37, 63, 31, 55, 23, 61, 29, 53, 21);
    p &= 7;
    return (float(m[p.y * 8 + p.x]) + 0.5) / 64.0;
}

float bayer4(ivec2 p) {
    const int m[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
    p &= 3;
    return (float(m[p.y * 4 + p.x]) + 0.5) / 16.0;
}

// Interleaved gradient noise: a stable, blue-ish threshold pattern.
float ign(ivec2 p) {
    return fract(52.9829189 * fract(0.06711056 * float(p.x) + 0.00583715 * float(p.y)));
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// The camera's yaw and pitch as a shift in scene pixels: patterns placed at (scene pixel +
// offset) slide with the scene when the camera turns instead of swimming over it.
vec2 camera_offset() {
    if (!has_camera()) return vec2(0.0);
    vec3 forward = normalize(vec3(view_to_world[0].z, view_to_world[1].z, view_to_world[2].z));
    vec3 right = vec3(view_to_world[0].x, view_to_world[1].x, view_to_world[2].x);
    float yaw = atan(forward.x, forward.z);
    float side = sign(dot(right.xz, vec2(forward.z, -forward.x)) + 1e-6);
    float pitch = asin(clamp(forward.y, -1.0, 1.0));
    vec2 pixels = vec2(projection.x * input_size.x, projection.y * input_size.y) * 0.5;
    return vec2(side * yaw * pixels.x, -pitch * pixels.y);
}

// Two-tone palettes: x = ink, y = paper.
void ink_palette(int index, out vec3 ink, out vec3 paper) {
    if (index == 1) { ink = vec3(0.0, 0.16, 0.05); paper = vec3(0.2, 1.0, 0.4); }        // IBM 5151
    else if (index == 2) { ink = vec3(0.16, 0.06, 0.0); paper = vec3(1.0, 0.7, 0.15); } // Zenith
    else if (index == 3) { ink = vec3(0.16, 0.13, 0.45); paper = vec3(0.6, 0.7, 1.0); } // C1084
    else if (index == 4) { ink = vec3(0.2, 0.25, 0.1); paper = vec3(0.65, 0.73, 0.45); } // LCD
    else if (index == 5) { ink = vec3(0.03); paper = vec3(0.97); }
    else if (index == 6) { ink = vec3(0.24, 0.15, 0.08); paper = vec3(0.93, 0.86, 0.7); } // sepia
    else { ink = vec3(0.2, 0.2, 0.1); paper = vec3(0.9, 1.0, 1.0); }                    // Macintosh
}
