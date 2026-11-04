#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D frame;
layout(set = 0, binding = 1) uniform sampler2D luma;
layout(set = 0, binding = 2) uniform sampler2D chroma;

layout(push_constant) uniform Params {
    // x, y, width, height of the video box in framebuffer pixels.
    vec4 rect;
    // x, y, width, height of the letterboxed picture inside rect.
    vec4 draw;
    // x: video active, y: full range, z/w: reserved.
    vec4 flags;
} params;

vec4 video_pixel(vec2 pixel) {
    vec2 uv = (pixel - params.draw.xy) / params.draw.zw;
    float y = texture(luma, uv).r;
    vec2 c = texture(chroma, uv).rg;
    if (params.flags.y > 0.5) {
        float u = c.r - 0.5;
        float v = c.g - 0.5;
        vec3 rgb = vec3(y + 1.402 * v, y - 0.344 * u - 0.714 * v, y + 1.772 * u);
        return vec4(clamp(rgb, 0.0, 1.0), 1.0);
    }
    float yy = 1.164 * (y - 16.0 / 255.0);
    float u = c.r - 128.0 / 255.0;
    float v = c.g - 128.0 / 255.0;
    vec3 rgb = vec3(yy + 1.596 * v, yy - 0.391 * u - 0.813 * v, yy + 2.018 * u);
    return vec4(clamp(rgb, 0.0, 1.0), 1.0);
}

bool inside(vec4 box, vec2 pixel) {
    return pixel.x >= box.x && pixel.y >= box.y &&
           pixel.x < box.x + box.z && pixel.y < box.y + box.w;
}

void main() {
    color = vec4(texture(frame, v_uv).rgb, 1.0);
    if (params.flags.x > 0.5) {
        vec2 pixel = gl_FragCoord.xy;
        if (inside(params.rect, pixel)) {
            color = inside(params.draw, pixel)
                        ? video_pixel(pixel)
                        : vec4(0.0, 0.0, 0.0, 1.0);
        }
    }
}
