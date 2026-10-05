// Fill rate: one shaded colour per pixel, with a little arithmetic so the
// fragment shader is not entirely trivial.
#version 450

layout(location = 0) out vec4 color;

layout(push_constant) uniform PushConstants { uint frame; } pc;

void main()
{
	vec2 uv = gl_FragCoord.xy * (1.0 / 1024.0);
	color = vec4(fract(uv.x + float(pc.frame) * 0.01), fract(uv.y), 0.75, 1.0);
}
