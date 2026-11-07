// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#version 460 core
layout (location = 0) in vec2 frag_tex_coord;
layout (location = 0) out vec4 color;
layout (binding = 0) uniform sampler2D color_texture;
vec4 cubic(float x) {
    float x2 = x * x;
    float x3 = x2 * x;
    return vec4(1.0, x, x2, x3) * transpose(mat4x4(
         0.0,   2.0,  0.0,  0.0,
        -1.0,   0.0,  1.0,  0.0,
         2.0,  -5.0,  4.0, -1.0,
        -1.0,   3.0, -3.0,  1.0
    ) * (1.0 / 2.0));
}
vec4 textureBicubic(sampler2D samp, vec2 uv) {
    vec2 tex_size = vec2(textureSize(samp, 0));
    vec2 cc_tex = uv * tex_size - 0.5f;
    vec2 fex = cc_tex - floor(cc_tex);
    vec4 xcubic = cubic(fex.x);
    vec4 ycubic = cubic(fex.y);
    // A bilinear lookup can combine the two positive middle weights. The negative
    // outer lobes need separate taps or high-contrast edges get the wrong weights.
    vec2 center = floor(cc_tex) + 0.5;
    vec3 wx = vec3(xcubic.x, xcubic.y + xcubic.z, xcubic.w);
    vec3 wy = vec3(ycubic.x, ycubic.y + ycubic.z, ycubic.w);
    vec3 px = (center.x + vec3(-1.0, xcubic.z / wx.y, 2.0)) / tex_size.x;
    vec3 py = (center.y + vec3(-1.0, ycubic.z / wy.y, 2.0)) / tex_size.y;
    vec4 result = vec4(0.0);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            result += texture(samp, vec2(px[x], py[y])) * (wx[x] * wy[y]);
        }
    }
    return result;
}
// Average the source texels covered by a destination pixel. A bilinear lookup
// can integrate each adjacent pair when positioned at their coverage-weighted center.
vec2 coveredPair(float first, float lo, float hi) {
    float a = max(0.0, min(first + 1.0, hi) - max(first, lo));
    float b = max(0.0, min(first + 2.0, hi) - max(first + 1.0, lo));
    return vec2(first + 0.5 + b / (a + b), a + b);
}

vec4 areaDownsample(vec2 center, vec2 span, vec2 size) {
    vec2 lo = center - 0.5 * span;
    vec2 hi = center + 0.5 * span;
    vec4 sum = vec4(0.0);
    // Bound the work when the window is minimized or reduced to a thumbnail.
    if (max(span.x, span.y) > 16.0) {
        const int grid = 8;
        for (int y = 0; y < grid; ++y) {
            for (int x = 0; x < grid; ++x) {
                vec2 pos = lo + (vec2(x, y) + 0.5) * span / float(grid);
                sum += textureLod(color_texture, pos / size, 0.0);
            }
        }
        return sum / float(grid * grid);
    }
    for (float y = floor(lo.y); y < hi.y; y += 2.0) {
        vec2 row = coveredPair(y, lo.y, hi.y);
        for (float x = floor(lo.x); x < hi.x; x += 2.0) {
            vec2 col = coveredPair(x, lo.x, hi.x);
            sum += textureLod(color_texture, vec2(col.x, row.x) / size, 0.0) * (col.y * row.y);
        }
    }
    return sum / (span.x * span.y);
}

void main() {
    vec2 size = vec2(textureSize(color_texture, 0));
    vec2 center = frag_tex_coord * size;
    vec2 span = fwidth(center);
    // At two or more input pixels per axis, a fixed cubic interpolation kernel
    // can sample just one phase of a repeating pattern. Integrate its area instead.
    if (min(span.x, span.y) >= 1.999) {
        color = areaDownsample(center, span, size);
    } else {
        color = textureBicubic(color_texture, frag_tex_coord);
    }
}
