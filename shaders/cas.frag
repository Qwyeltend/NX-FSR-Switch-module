#version 450
// One pass: anti-aliasing (FXAA, after Timothy Lottes' FXAA) on edges + contrast adaptive sharpening
// (AMD FidelityFX CAS, MIT license, Copyright (c) AMD) elsewhere. Reads a copy of the game's frame
// (uSource) and writes the result back into the frame.
//
// UBO at binding 1 = NVN binding 0 (see fsr.vert). Texture at binding 0 = NVN binding 0.
// No loops / early returns: keeps the shader free of scratch memory (checked by tools/dksh2nvn.py).

layout(location = 0) in vec2 vFbCoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 1) uniform Params {
    vec4 uCrop;     // x, y, width, height of the visible area (texels)
    vec4 uEffect;   // sharpness 0..1, split (1 = only left half), flip rows (vertex shader), anti-aliasing 0..1
};

layout(binding = 0) uniform sampler2D uSource;

const vec3 LUMA = vec3(0.299, 0.587, 0.114);

vec3 fetchClamped(ivec2 p, ivec2 lo, ivec2 hi) {
    return texelFetch(uSource, clamp(p, lo, hi), 0).rgb;
}

// Bilinear sample at a texel-space position, kept inside the visible area.
vec3 sampleAt(vec2 pos, vec2 lo, vec2 hi, vec2 invSize) {
    return textureLod(uSource, clamp(pos, lo, hi) * invSize, 0.0).rgb;
}

void main() {
    ivec2 lo = ivec2(uCrop.xy);
    ivec2 hi = ivec2(uCrop.xy + uCrop.zw) - ivec2(1);
    ivec2 p = clamp(ivec2(vFbCoord), lo, hi);
    vec2 center = vec2(p) + 0.5;
    vec2 invSize = 1.0 / vec2(textureSize(uSource, 0));
    vec2 loF = vec2(lo) + 0.5;
    vec2 hiF = vec2(hi) + 0.5;

    // ---- CAS: 3x3 neighbourhood  a b c / d e f / g h i
    vec3 a = fetchClamped(p + ivec2(-1, -1), lo, hi);
    vec3 b = fetchClamped(p + ivec2( 0, -1), lo, hi);
    vec3 c = fetchClamped(p + ivec2( 1, -1), lo, hi);
    vec3 d = fetchClamped(p + ivec2(-1,  0), lo, hi);
    vec3 e = texelFetch(uSource, p, 0).rgb;
    vec3 f = fetchClamped(p + ivec2( 1,  0), lo, hi);
    vec3 g = fetchClamped(p + ivec2(-1,  1), lo, hi);
    vec3 h = fetchClamped(p + ivec2( 0,  1), lo, hi);
    vec3 i = fetchClamped(p + ivec2( 1,  1), lo, hi);

    vec3 mnCross = min(min(min(d, e), min(f, b)), h);
    vec3 mxCross = max(max(max(d, e), max(f, b)), h);
    vec3 mn = mnCross + min(mnCross, min(min(a, c), min(g, i)));
    vec3 mx = mxCross + max(mxCross, max(max(a, c), max(g, i)));
    vec3 amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, vec3(1.0 / 65536.0)), 0.0, 1.0));
    float sharpness = clamp(uEffect.x, 0.0, 1.0);
    vec3 w = amp * (-1.0 / mix(8.0, 5.0, sharpness));
    vec3 sharpened = clamp((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w), 0.0, 1.0);
    vec3 base = mix(e, sharpened, step(0.001, sharpness));

    // ---- FXAA on the original frame
    float lumaM  = dot(e, LUMA);
    float lumaNW = dot(sampleAt(center + vec2(-0.5, -0.5), loF, hiF, invSize), LUMA);
    float lumaNE = dot(sampleAt(center + vec2( 0.5, -0.5), loF, hiF, invSize), LUMA);
    float lumaSW = dot(sampleAt(center + vec2(-0.5,  0.5), loF, hiF, invSize), LUMA);
    float lumaSE = dot(sampleAt(center + vec2( 0.5,  0.5), loF, hiF, invSize), LUMA);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    vec2 dir = vec2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)), (lumaNW + lumaSW) - (lumaNE + lumaSE));
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 / 8.0), 1.0 / 128.0);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-8.0), vec2(8.0));

    vec3 rgbA = 0.5 * (sampleAt(center + dir * (1.0 / 3.0 - 0.5), loF, hiF, invSize) +
                       sampleAt(center + dir * (2.0 / 3.0 - 0.5), loF, hiF, invSize));
    vec3 rgbB = rgbA * 0.5 + 0.25 * (sampleAt(center - dir * 0.5, loF, hiF, invSize) +
                                     sampleAt(center + dir * 0.5, loF, hiF, invSize));
    float lumaB = dot(rgbB, LUMA);
    float useA = float(lumaB < lumaMin || lumaB > lumaMax);
    vec3 smoothed = mix(rgbB, rgbA, useA);

    // Only where there is a real edge (local contrast above threshold), soft transition.
    float contrast = lumaMax - lumaMin;
    float threshold = max(0.0625, lumaMax * 0.125);
    float edge = smoothstep(threshold, threshold * 2.0, contrast);
    float aa = clamp(uEffect.w, 0.0, 1.0);
    vec3 result = mix(base, smoothed, edge * aa);

    // ---- comparison view: right half untouched, thin line at the border
    float splitX = uCrop.x + uCrop.z * 0.5;
    float useSplit = step(0.5, uEffect.y);
    float rightSide = useSplit * step(splitX, vFbCoord.x);
    float border = useSplit * (1.0 - step(1.5, abs(vFbCoord.x - splitX)));
    result = mix(result, e, rightSide);
    result = mix(result, vec3(1.0, 0.85, 0.0), border);

    fragColor = vec4(result, 1.0);
}
