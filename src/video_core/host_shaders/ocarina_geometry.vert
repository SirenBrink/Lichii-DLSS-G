#version 460
layout(location=0) in vec4 nowClip[3];
layout(location=3) in vec4 oldClip[3];
layout(location=6) in uint nowID[3];
layout(location=9) in uint oldID[3];
layout(location=0) out vec4 previousPosition;
layout(push_constant) uniform Projection {
    vec2 oldSign;
    vec2 nowSign;
    vec2 size;
    uint negativeOneToOne;
    uint coverageTag;
};
void main() {
    int oldVertex = -1;
    bool valid = true;
    // One instance per triangle. Compare all vertex IDs before emitting any of
    // it. The driver may rotate a triangle's starting vertex in the XFB stream.
    for (int i=0; i<3; ++i) {
        int match = -1;
        for (int j=0; j<3; ++j) if (nowID[i] == oldID[j]) match=j;
        if (i == gl_VertexIndex) oldVertex=match;
        valid = valid && match >= 0 && nowClip[i].w > 0.0 && oldClip[i].w > 0.0;
        if (coverageTag == 3u && match >= 0) {
            // Mesh storage can be recycled between actors at a spawn or teleport.
            // Leave discontinuities to the existing reconstruction for that frame.
            vec2 current = nowClip[i].xy * nowSign / nowClip[i].w;
            vec2 previous = oldClip[match].xy * oldSign / oldClip[match].w;
            valid = valid && all(lessThan(abs(current - previous), vec2(0.5)));
        }
    }
    if (!valid) {
        gl_Position=vec4(2,2,2,1);
        previousPosition=vec4(0);
        return;
    }
    vec4 p = nowClip[gl_VertexIndex];
    gl_Position = vec4(p.xy * nowSign, negativeOneToOne != 0u ? 0.5*(p.z+p.w) : p.z, p.w);
    previousPosition = oldClip[oldVertex];
    previousPosition.xy *= oldSign;
}
