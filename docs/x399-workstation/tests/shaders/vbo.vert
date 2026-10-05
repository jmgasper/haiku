// The same full screen triangle, but fed from a vertex buffer.
#version 450

layout(location = 0) in vec3 position;

void main()
{
	gl_Position = vec4(position, 1.0);
}
