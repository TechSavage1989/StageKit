uniform texture2d other_image;

float4 mainImage(VertData v_in) : TARGET
{
	float4 other = other_image.Sample(textureSampler, v_in.uv);
	float4 base = image.Sample(textureSampler, v_in.uv);
	return clamp(base + other, 0.0, 1.0);
}
