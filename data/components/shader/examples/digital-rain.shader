// based on https://www.shadertoy.com/view/ldccW4 by WillKirkby


#ifndef OPENGL
#define fract frac
#define mix lerp
#endif

uniform texture2d font = "font.png";
uniform texture2d noise = "noise.png";
uniform float4 base_color = {.1, 1.0, .35, 1.0};
uniform float rain_speed<
    string label = "Rain Speed";
    string widget_type = "slider";
    float minimum = 0.001;
    float maximum = 2.0;
    float step = .001;
> = 1.0;

uniform float char_speed<
    string label = "Character Speed";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 2.0;
    float step = .001;
> = 1.0;

uniform float glow_contrast<
    string label = "Glow contrast";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 2.5;
    float step = .001;
> = 1.0;


float mod(float x, float y)
{
  return x - y * floor(x/y);
}

float2 mod2(float2 x, float2 y)
{
  return x - y * floor(x/y);
}

float text(float2 fragCoord)
{
    float2 uv = mod2(fragCoord, float2(16.0, 16.0) )/16.0;
    float2 block = (fragCoord*.0625 - uv)/uv_size*16.0;
    uv = uv*.8+.1; // scale the letters up a bit
    block += elapsed_time*.002*char_speed;
    uv += floor(noise.Sample(textureSampler, fract(block)).xy * 16.); // randomize letters
    uv *= .0625; // bring back into 0-1 range
    return font.Sample(textureSampler, uv).r;
}

float3 rain(float2 fragCoord)
{
	fragCoord.x -= mod(fragCoord.x, 16.);   
    float offset=sin(fragCoord.x*15.);
    float speed=(cos(fragCoord.x*3.)*.3+.7)*rain_speed;
   
    float y = fract(fragCoord.y/uv_size.y + elapsed_time*speed + offset);
    return base_color.rgb / pow(y*20.0, glow_contrast);
}

float4 mainImage(VertData v_in) : TARGET
{
    return mix(image.Sample(textureSampler, v_in.uv),float4(rain(float2(v_in.uv.x,1.0-v_in.uv.y)*uv_size),1.0), text(v_in.uv*uv_size));
}