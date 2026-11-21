float4 mainImage(VertData v_in) : TARGET
{
    const float pi = 3.14159265358979323846;
    float2 center = float2(0.5, 0.5);
    float2 factor;
    if(uv_size.x < uv_size.y){
        factor = float2(1.0, uv_size.x/uv_size.y);
    }else{
        factor = float2(uv_size.x/uv_size.y, 1.0);
    }
    center = center * factor;
    float d = distance(center, v_in.uv * factor);
    if( d > 0.5)
        return float4(0,0,0,0); // outside
    if( d > 0.45)
        return float4(1,1,1,1); // white border
    
    float fade_factor = d/2.5;
    float4 black_color = float4(fade_factor, fade_factor, fade_factor, 1.0);
    float4 white_color = float4(0.75+fade_factor, 0.75+fade_factor, 0.75+fade_factor, 1.0) ;
    float2 toCenter = center - v_in.uv*factor;
    float angle = atan2(toCenter.y ,toCenter.x);
    float angle2 = angle + pi * 2.5;
    angle2 = angle2 / (pi*2.0) * 3.0;
    angle2 = angle2 - floor(angle2);
    angle2 -= 0.5;
    angle2 *= pi / 1.5;

    float2 center2 = float2(0.0, 0.5);
    float2 pos = float2(sin(angle2)*d*2.0, cos(angle2)*d*2.0);
    float d2 = distance(center2, pos);
    if(d2 < 0.41)
        return black_color; // black circle * 3
    if(d < 0.15)
        return white_color; // center white

    float angle4 = angle + pi * 3.5;
    angle4 = angle4 / (pi*2.0) * 3.0;
    angle4 -= floor(angle4);
    angle4 = 1.0 - angle4;
    angle4 *= pi /1.5;

    float2 center3 = float2(0.0, 0.4);
    float2 pos3 = float2(sin(angle4)*d*2.0, cos(angle4)*d*2.0);
    float d3 = distance(center3, pos3);
    if(d3 < 0.49)
        return white_color; // white circle * 3
    return black_color;
}