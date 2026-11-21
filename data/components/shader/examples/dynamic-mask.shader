uniform texture2d input_source<
    string label = "Input Source";
>;

uniform float red_base_value<
    string label = "Base Value";
    string widget_type = "slider";
    string group = "Red Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 1.0;
uniform float red_red_input_value<
    string label = "Red Input Value";
    string widget_type = "slider";
    string group = "Red Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 0.0;
uniform float red_green_input_value<
    string label = "Green Input Value";
    string widget_type = "slider";
    string group = "Red Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 0.0;
uniform float red_blue_input_value<
    string label = "Blue Input Value";
    string widget_type = "slider";
    string group = "Red Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 0.0;
uniform float red_alpha_input_value<
    string label = "Alpha Input Value";
    string widget_type = "slider";
    string group = "Red Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 0.0;
uniform float red_multiplier<
    string label = "Multiplier";
    string widget_type = "slider";
    string group = "Red Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 1.0;

uniform float green_base_value<
    string label = "Base Value";
    string widget_type = "slider";
    string group = "Green Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 1.0;
uniform float green_red_input_value<
    string label = "Red Input Value";
    string widget_type = "slider";
    string group = "Green Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 0.0;
uniform float green_green_input_value<
    string label = "Green Input Value";
    string widget_type = "slider";
    string group = "Green Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 0.0;
uniform float green_blue_input_value<
    string label = "Blue Input Value";
    string widget_type = "slider";
    string group = "Green Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 0.0;
uniform float green_alpha_input_value<
    string label = "Alpha Input Value";
    string widget_type = "slider";
    string group = "Green Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 0.0;
uniform float green_multiplier<
    string label = "Multiplier";
    string widget_type = "slider";
    string group = "Green Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 1.0;

uniform float blue_base_value<
    string label = "Base Value";
    string widget_type = "slider";
    string group = "Blue Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 1.0;
uniform float blue_red_input_value<
    string label = "Red Input Value";
    string widget_type = "slider";
    string group = "Blue Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 0.0;
uniform float blue_green_input_value<
    string label = "Green Input Value";
    string widget_type = "slider";
    string group = "Blue Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 0.0;
uniform float blue_blue_input_value<
    string label = "Blue Input Value";
    string widget_type = "slider";
    string group = "Blue Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 0.0;
uniform float blue_alpha_input_value<
    string label = "Alpha Input Value";
    string widget_type = "slider";
    string group = "Blue Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 0.0;
uniform float blue_multiplier<
    string label = "Multiplier";
    string widget_type = "slider";
    string group = "Blue Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
>  = 1.0;

uniform float alpha_base_value<
    string label = "Base Value";
    string widget_type = "slider";
    string group = "Alpha Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 1.0;
uniform float alpha_red_input_value<
    string label = "Red Input Value";
    string widget_type = "slider";
    string group = "Alpha Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 0.0;
uniform float alpha_green_input_value<
    string label = "Green Input Value";
    string widget_type = "slider";
    string group = "Alpha Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 0.0;
uniform float alpha_blue_input_value<
    string label = "Blue Input Value";
    string widget_type = "slider";
    string group = "Alpha Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 0.0;
uniform float alpha_alpha_input_value<
    string label = "Alpha Input Value";
    string widget_type = "slider";
    string group = "Alpha Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 0.0;
uniform float alpha_multiplier<
    string label = "Multiplier";
    string widget_type = "slider";
    string group = "Alpha Channel";
    float minimum = -100.0;
    float maximum = 100.0;
    float step = 0.01;
> = 1.0;

float4 mainImage(VertData v_in) : TARGET
{
	float4 input_color = input_source.Sample(textureSampler, v_in.uv);
    float4 mask;
    mask.r = (red_base_value + red_red_input_value * input_color.r + red_green_input_value * input_color.g + red_blue_input_value * input_color.b + red_alpha_input_value * input_color.a) * red_multiplier;
    mask.g = (green_base_value + green_red_input_value * input_color.r + green_green_input_value * input_color.g + green_blue_input_value * input_color.b + green_alpha_input_value * input_color.a) * green_multiplier;
    mask.b = (blue_base_value + blue_red_input_value * input_color.r + blue_green_input_value * input_color.g + blue_blue_input_value * input_color.b + blue_alpha_input_value * input_color.a) * blue_multiplier;
    mask.a = (alpha_base_value + alpha_red_input_value * input_color.r + alpha_green_input_value * input_color.g + alpha_blue_input_value * input_color.b + alpha_alpha_input_value * input_color.a) * alpha_multiplier;
	float4 base = image.Sample(textureSampler, v_in.uv);
	return base * mask;
}
