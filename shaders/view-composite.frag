#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Parameters {
    mat4 qt_Matrix;
    float qt_Opacity;
    int mode;     // 0 blur, 1 shadow, 2 glow, 3 bloom
    int colorMix; // 0 interpolate, 1 add, 2 subtract
    int onlyEffect;
    vec2 offsetUV;
    vec4 channels;
    vec4 tint;
    float strength;
};
layout(binding = 1) uniform sampler2D source;
layout(binding = 2) uniform sampler2D filtered;

vec3 straightRgb(vec4 pixel)
{
    return pixel.rgb / max(pixel.a, 1e-6);
}

vec4 sampleFiltered(vec2 uv)
{
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return vec4(0.0);

    return texture(filtered, uv);
}

void main()
{
    vec4 original = texture(source, qt_TexCoord0);
    vec4 blurred = sampleFiltered(qt_TexCoord0 - offsetUV);
    vec4 pixel;
    if (mode == 0) {
        vec3 rgb = mix(straightRgb(original), straightRgb(blurred), channels.rgb);
        float alpha = mix(original.a, blurred.a, channels.a);
        pixel = vec4(rgb * alpha, alpha);
    } else if (mode == 3) {
        float alpha = original.a + clamp(blurred.a * strength, 0.0, 1.0) * (1.0 - original.a);
        pixel = vec4(min(original.rgb + blurred.rgb * strength, vec3(alpha)), alpha);
    } else {
        vec3 rgb = straightRgb(blurred);
        float alpha = blurred.a;
        if (mode == 2) {
            rgb = tint.rgb;
            alpha *= tint.a;
        } else if (colorMix == 1) {
            rgb = clamp(rgb + tint.rgb * tint.a, 0.0, 1.0);
        } else if (colorMix == 2) {
            rgb = clamp(rgb - tint.rgb * tint.a, 0.0, 1.0);
        } else {
            rgb = mix(rgb, tint.rgb, tint.a);
        }
        alpha = clamp(alpha * strength, 0.0, 1.0);
        vec4 halo = vec4(rgb * alpha, alpha);
        pixel = onlyEffect != 0 ? halo : original + halo * (1.0 - original.a);
    }
    fragColor = pixel * qt_Opacity;
}
