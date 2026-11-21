// https://www.shadertoy.com/view/sd3yzn adopted by Exeldro

uniform float pixH<
    string label = "Size";
    string widget_type = "slider";
    float minimum = 4.0;
    float maximum = 500.0;
    float step = 0.01;
> = 100.0;
uniform bool alternative;

float2 fract2(float2 v){
    return float2(v.x - floor(v.x), v.y - floor(v.y));
}

float2 random2( float2 p ) {
    return fract2(sin(float2(dot(p,float2(127.1,311.7)),dot(p,float2(269.5,183.3))))*43758.5453);
}
float2 randomSpin(float2 p, float f){
    return 1.0 * float2(
    cos( f * elapsed_time * 3.14159 * sign(random2(p).y - 0.5) + random2(p).y * 3.14159), 
    sin( f * elapsed_time * 3.14159 * sign(random2(p).x - 0.5) + random2(p).x * 3.14159));
}
float4 VoronoiPixelation(float2 uv, float pixH ){
    float2 pixInt = fract2(uv * pixH);
    float2 pixExt = floor(uv * pixH);
    float m_dist = 10.0;
    float2 relClos = float2(0.0, 0.0);
    float2 relRot = 0.5 * float2(cos(elapsed_time), sin(elapsed_time));


    for (int y= -3; y <= 3; y++) {
        for (int x= -3; x <= 3; x++) {
            float2 neighbor = float2(float(x),float(y));

            float2 point1 = random2(pixExt + neighbor);
            float2 relRot = randomSpin(pixExt + neighbor, 0.5);
            float2 diff = neighbor + relRot + point1 - pixInt;
            float dist = length(diff);
            if(dist < m_dist){
                m_dist = dist;
                relClos = neighbor;
            }
        }
    }
    float2 nPoint = pixExt + relClos + randomSpin(pixExt + relClos, 0.5) + random2(pixExt + relClos);
    nPoint = nPoint / pixH;
    nPoint.x = nPoint.x * uv_scale.x ;
    
    return image.Sample(textureSampler, nPoint);
}
float4 VoronoiPixelation2(float2 uv, float pixH ){
    float2 pixInt = fract2(uv * pixH);
    float2 pixExt = floor(uv * pixH);
    float m_dist = 10.0;
    float2 relClos = float2(0.0, 0.0);
    float2 relRot = 0.5 * float2(cos(elapsed_time), sin(elapsed_time));


    for (int y= -3; y <= 3; y++) {
        for (int x= -3; x <= 3; x++) {
            float2 neighbor = float2(float(x),float(y));

            float2 point2 = random2(pixExt + neighbor);
            float2 relRot = randomSpin(pixExt + neighbor, 0.5);
            float2 diff = neighbor + relRot + point2 - pixInt;
            float dist = length(diff);
            if(dist < m_dist){
                m_dist = dist;
                relClos = neighbor;
            }
        }
    }
    float2 nPoint = pixExt + relClos + random2(pixExt + relClos);
    nPoint = nPoint / pixH;
    nPoint.x = nPoint.x * uv_scale.x;
    
    return image.Sample(textureSampler, nPoint);
}


float4 mainImage(VertData v_in) : TARGET
{
    if (alternative) {
        return VoronoiPixelation2(v_in.uv, pixH);
    } else {
        return VoronoiPixelation(v_in.uv, pixH);
    }
}