#version 450
// Lab test: simple contrast-adaptive sharpen (CAS-like) of the frame copy in uSource.
// Same interface as SaltyNX's logo shader: UBO binding 0, combined sampler binding 0.

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Params {
    float uSharpness;   // 0..1
    float uPad0;
    float uPad1;
    float uPad2;
};

layout(binding = 0) uniform sampler2D uSource;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 maxp = textureSize(uSource, 0) - ivec2(1);
    vec3 c = texelFetch(uSource, p, 0).rgb;
    vec3 n = texelFetch(uSource, clamp(p + ivec2(0, -1), ivec2(0), maxp), 0).rgb;
    vec3 s = texelFetch(uSource, clamp(p + ivec2(0,  1), ivec2(0), maxp), 0).rgb;
    vec3 w = texelFetch(uSource, clamp(p + ivec2(-1, 0), ivec2(0), maxp), 0).rgb;
    vec3 e = texelFetch(uSource, clamp(p + ivec2( 1, 0), ivec2(0), maxp), 0).rgb;

    vec3 mn = min(c, min(min(n, s), min(w, e)));
    vec3 mx = max(c, max(max(n, s), max(w, e)));
    vec3 amp = clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-5)), 0.0, 1.0);
    amp = sqrt(amp);
    float peak = -1.0 / mix(8.0, 5.0, clamp(uSharpness, 0.0, 1.0));
    vec3 wgt = amp * peak;
    vec3 outc = (c + (n + s + w + e) * wgt) / (1.0 + 4.0 * wgt);
    fragColor = vec4(clamp(outc, 0.0, 1.0), 1.0);
}
