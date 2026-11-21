uniform bool horizontal;
uniform bool vertical;
float4 mainImage(VertData v_in) : TARGET
{
    float2 uv = v_in.uv;
    if(horizontal)
        uv.x = 1.0 - uv.x;
    if(vertical)
        uv.y = 1.0 - uv.y;
    return image.Sample(textureSampler, uv);
}