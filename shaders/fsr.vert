#version 450
// Full screen triangle over the crop area of the game's frame.
// vFbCoord = texel position inside the frame texture (same convention as SaltyNX's logo shader:
// texture rows run top to bottom on NVN).
//
// UBO is declared at binding 1 on purpose: UAM maps binding N to hardware constbuf c[N+2],
// NVN maps binding N to c[N+3]. Binding 1 here = binding 0 for NVN at runtime.

layout(std140, binding = 1) uniform Params {
    vec4 uCrop;     // x, y, width, height of the visible area (texels)
    vec4 uEffect;   // sharpness 0..1, split (1 = only left half), unused, unused
};

layout(location = 0) out vec2 vFbCoord;

void main() {
    // Vertices: (-1,-1), (3,-1), (-1,3) cover the whole viewport with one triangle.
    float x = gl_VertexID == 1 ? 3.0 : -1.0;
    float y = gl_VertexID == 2 ? 3.0 : -1.0;
    vec2 uv = vec2(x * 0.5 + 0.5, 0.5 - y * 0.5);
    vFbCoord = uv * uCrop.zw + uCrop.xy;
    gl_Position = vec4(x, y, 0.0, 1.0);
}
