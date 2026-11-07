#version 460
layout(location=0) in vec4 previousPosition;
layout(location=0) out vec4 geometryMotion;
layout(push_constant) uniform Projection {
    vec2 oldSign;
    vec2 nowSign;
    vec2 size;
    uint negativeOneToOne;
    uint coverageTag;
};
void main() {
    vec2 previousPixel = (previousPosition.xy / previousPosition.w * 0.5 + 0.5) * size;
    vec2 velocity = previousPixel - gl_FragCoord.xy;
    geometryMotion = vec4(velocity, log2(max(1.0-gl_FragCoord.z, 1e-7)), float(coverageTag));
}
