uniform float redx<
    string label = "Red X";
    string widget_type = "slider";
    float minimum = -10.0;
    float maximum = 10.0;
    float step = 0.01;
> = 2.00;
uniform float redy<
    string label = "Red Y";
    string widget_type = "slider";
    float minimum = -10.0;
    float maximum = 10.0;
    float step = 0.01;
> = 0.00;
uniform float greenx<
    string label = "Green X";
    string widget_type = "slider";
    float minimum = -10.0;
    float maximum = 10.0;
    float step = 0.01;
> = 0.00;
uniform float greeny<
    string label = "Green Y";
    string widget_type = "slider";
    float minimum = -10.0;
    float maximum = 10.0;
    float step = 0.01;
> = 0.00;
uniform float bluex<
    string label = "Blue X";
    string widget_type = "slider";
    float minimum = -10.0;
    float maximum = 10.0;
    float step = 0.01;
> = -2.00;
uniform float bluey<
    string label = "Blue Y";
    string widget_type = "slider";
    float minimum = -10.0;
    float maximum = 10.0;
    float step = 0.01;
> = 0.00;


float4 mainImage(VertData v_in) : TARGET
{
    float4 c = image.Sample(textureSampler, v_in.uv);
    if(redx != 0.0 || redy != 0.0)
        c.r = image.Sample(textureSampler, v_in.uv + float2(redx/100.0, redy/100.0)).r;
    if(greenx != 0.0 || greeny != 0.0)
        c.g = image.Sample(textureSampler, v_in.uv + float2(greenx/100.0, greeny/100.0)).g;
    if(bluex != 0.0 || bluey != 0.0)
        c.b = image.Sample(textureSampler, v_in.uv + float2(bluex/100.0, bluey/100.0)).b;
    return c;
}