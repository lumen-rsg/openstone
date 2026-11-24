#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Parameters {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 stepUV;
    float radius;
    float sigma;
    float threshold;
};
layout(binding = 1) uniform sampler2D source;

vec4 sampleLayer(vec2 uv)
{
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return vec4(0.0);

    vec4 pixel = texture(source, uv);
    if (threshold >= 0.0) {
        // Extract bright contributions from straight RGB and keep premultiplied output.
        vec3 rgb = pixel.rgb / max(pixel.a, 1e-6);
        float brightness = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
        float bright = clamp((brightness - threshold) / max(1.0 - threshold, 1e-6), 0.0, 1.0);
        pixel *= bright;
    }
    return pixel;
}

void main()
{
    vec4 sum = sampleLayer(qt_TexCoord0);
    float total = 1.0;
    for (int i = 1; i <= 32; ++i) {
        if (float(i) > radius)
            break;
        float distance = float(i) / max(sigma, 1e-6);
        float weight = exp(-0.5 * distance * distance);
        vec2 offset = stepUV * float(i);
        sum += (sampleLayer(qt_TexCoord0 + offset) + sampleLayer(qt_TexCoord0 - offset)) * weight;
        total += 2.0 * weight;
    }
    fragColor = sum * (qt_Opacity / total);
}
