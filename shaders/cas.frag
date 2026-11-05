#version 450
// Contrast Adaptive Sharpening, based on AMD FidelityFX CAS (MIT license, Copyright (c) AMD).
// Reads a copy of the game's frame (uSource) and writes the sharpened result back into the frame.
//
// UBO at binding 1 = NVN binding 0 (see fsr.vert). Texture at binding 0 = NVN binding 0.

layout(location = 0) in vec2 vFbCoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 1) uniform Params {
    vec4 uCrop;     // x, y, width, height of the visible area (texels)
    vec4 uEffect;   // sharpness 0..1, split (1 = only left half), unused, unused
};

layout(binding = 0) uniform sampler2D uSource;

vec3 fetchClamped(ivec2 p, ivec2 lo, ivec2 hi) {
    return texelFetch(uSource, clamp(p, lo, hi), 0).rgb;
}

void main() {
    ivec2 lo = ivec2(uCrop.xy);
    ivec2 hi = ivec2(uCrop.xy + uCrop.zw) - ivec2(1);
    ivec2 p = clamp(ivec2(vFbCoord), lo, hi);

    // 3x3 neighbourhood:  a b c / d e f / g h i
    vec3 a = fetchClamped(p + ivec2(-1, -1), lo, hi);
    vec3 b = fetchClamped(p + ivec2( 0, -1), lo, hi);
    vec3 c = fetchClamped(p + ivec2( 1, -1), lo, hi);
    vec3 d = fetchClamped(p + ivec2(-1,  0), lo, hi);
    vec3 e = texelFetch(uSource, p, 0).rgb;
    vec3 f = fetchClamped(p + ivec2( 1,  0), lo, hi);
    vec3 g = fetchClamped(p + ivec2(-1,  1), lo, hi);
    vec3 h = fetchClamped(p + ivec2( 0,  1), lo, hi);
    vec3 i = fetchClamped(p + ivec2( 1,  1), lo, hi);

    // Soft min/max: cross + corners.
    vec3 mnCross = min(min(min(d, e), min(f, b)), h);
    vec3 mxCross = max(max(max(d, e), max(f, b)), h);
    vec3 mn = mnCross + min(mnCross, min(min(a, c), min(g, i)));
    vec3 mx = mxCross + max(mxCross, max(max(a, c), max(g, i)));

    // Amount of sharpening depends on local contrast (less where it would clip).
    vec3 amp = clamp(min(mn, 2.0 - mx) / max(mx, vec3(1.0 / 65536.0)), 0.0, 1.0);
    amp = sqrt(amp);
    float sharpness = clamp(uEffect.x, 0.0, 1.0);
    float peak = -1.0 / mix(8.0, 5.0, sharpness);
    vec3 w = amp * peak;
    vec3 sharpened = clamp((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w), 0.0, 1.0);

    // Split view for testing: right half stays untouched, thin line at the border.
    float splitX = uCrop.x + uCrop.z * 0.5;
    float useSplit = step(0.5, uEffect.y);
    float rightSide = useSplit * step(splitX, vFbCoord.x);
    float border = useSplit * (1.0 - step(1.5, abs(vFbCoord.x - splitX)));
    vec3 result = mix(sharpened, e, rightSide);
    result = mix(result, vec3(1.0, 0.85, 0.0), border);

    fragColor = vec4(result, 1.0);
}
