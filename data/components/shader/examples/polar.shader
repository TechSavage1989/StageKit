#define PI 3.14159265359
#define PI_2 6.2831
#define mod(x,y) (x - y * floor(x / y))

uniform float center_x<
    string label = "Center x";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 1.0;
    float step = 0.001;
> = 0.5;
uniform float center_y<
    string label = "Center y";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 1.0;
    float step = 0.001;
> = 0.5;

uniform float point_y<
    string label = "Point y";
    string widget_type = "slider";
    float minimum = -1.0;
    float maximum = 1.0;
    float step = 0.001;
> = 0.0;

uniform bool flip;

uniform float rotate<
    string label = "Rotate";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 1.0;
    float step = 0.001;
> = 0.0;

uniform float repeat<
    string label = "Repeat";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 20.0;
    float step = 0.001;
> = 1.0;

uniform float scale<
    string label = "Scale";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 2.0;
    float step = 0.001;
> = 0.5;

float4 mainImage(VertData v_in) : TARGET
{
    float2 uv = v_in.uv;
    uv.x -= center_x ;
    uv.y -= center_y ;
    uv.x = uv.x * ( uv_size.x / uv_size.y);
    float pixel_angle = atan2(uv.x,uv.y)/PI_2+0.5;
    if(repeat < 1.0){
        pixel_angle = mod(pixel_angle+rotate,1.0);
        if(pixel_angle > repeat)
            return float4(0,0,0,0);
        pixel_angle = mod(pixel_angle/repeat,1.0);
    } else {
        pixel_angle = mod(pixel_angle*repeat+rotate, 1.0);
    }
    float pixel_distance = length(uv)/ scale - point_y;
    float2 uv2 = float2(pixel_angle , pixel_distance);
    if(flip)
        uv2 = float2(1.0,1.0) - uv2;
    return image.Sample(textureSampler,uv2);
}