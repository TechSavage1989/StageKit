uniform texture2d previous_output;
uniform float strength<
    string label = "strength";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 1.0;
    float step = 0.001;
> = 0.0;

float4 mainImage(VertData v_in) : TARGET
{
	return lerp(image.Sample(textureSampler, v_in.uv), previous_output.Sample(textureSampler, v_in.uv), 1.0 - pow(2, -7 * strength));
}