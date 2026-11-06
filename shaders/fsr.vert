#version 450
// Full screen triangle over the crop area of the game's frame.
// vFbCoord = texel position inside the frame texture.
//
// Row order of the frame texture depends on the game's NVN window origin mode:
// upper left origin -> rows run top to bottom (uEffect.z = 0), lower left -> bottom to top (uEffect.z = 1).
//
// UBO is declared at binding 1 on purpose: UAM maps binding N to hardware constbuf c[N+2],
// NVN maps binding N to c[N+3]. Binding 1 here = binding 0 for NVN at runtime.

layout(std140, binding = 1) uniform Params {
    vec4 uCrop;     // x, y, width, height of the visible area (texels)
    vec4 uEffect;   // sharpness 0..1, split (1 = only left half), flip rows (0/1), unused
};

layout(location = 0) out vec2 vFbCoord;

void main() {
    // Vertices: (-1,-1), (3,-1), (-1,3) cover the whole viewport with one triangle.
    float x = gl_VertexID == 1 ? 3.0 : -1.0;
    float y = gl_VertexID == 2 ? 3.0 : -1.0;
    float v = mix(0.5 - y * 0.5, 0.5 + y * 0.5, step(0.5, uEffect.z));
    vec2 uv = vec2(x * 0.5 + 0.5, v);
    vFbCoord = uv * uCrop.zw + uCrop.xy;
    gl_Position = vec4(x, y, 0.0, 1.0);
}
