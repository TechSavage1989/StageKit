#define PI 3.14159265359

uniform float strength<
    string label = "Strength";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 1.0;
    float step = 0.001;
> = 0.5;

uniform float scale<
    string label = "Scale";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 1.0;
    float step = 0.001;
> = 1.0;

uniform float4 curve_color = {0,0,0,0};

float4 mainImage(VertData v_in) : TARGET
{
    float2 uv = v_in.uv;
    const float ydiff = 1.0;
    uv -= float2(0.5,ydiff);
    uv.x *= ( uv_size.x / uv_size.y);
    uv /= scale;
    if(strength > 0.0){    
        float d = tan((1.0-strength)*PI/2.0);
        float r = length(float2(0.5*(uv_size.x / uv_size.y), d));
        float2 center = float2(0.0,(1.0-ydiff)+d);
        float pd = distance(uv, center);
        if(pd < r)
            return curve_color;
        float angle = atan2(center.x-uv.x,center.y-uv.y);
        uv = float2(uv.x + sin(angle)*(pd-r),(1.0-ydiff)-(pd-r));
    }
    uv.x /= ( uv_size.x / uv_size.y);
    uv += float2(0.5,ydiff);
    return image.Sample(textureSampler,uv);
}