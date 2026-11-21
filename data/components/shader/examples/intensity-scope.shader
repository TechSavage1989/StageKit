// Robin Green, Dec 2016
// Creative Commons Attribution-NonCommercial-ShareAlike 3.0 Unported License.
// https://www.shadertoy.com/view/XtcSRs adopted for OBS by Exeldro
uniform float gain<
    string label = "Gain";
    string widget_type = "slider";
    float minimum = 0.01;
    float maximum = 1.00;
    float step = 0.01;
> = 0.3;
uniform float blend<
    string label = "Blend";
    string widget_type = "slider";
    float minimum = 0.00;
    float maximum = 1.00;
    float step = 0.01;
> = 0.6;

float4 mainImage(VertData v_in) : TARGET
{
	float2 uv = v_in.uv;
    uv.y = 1.0 - uv.y;
       
    // calculate the intensity bucket for this pixel based on column height (padded at the top)
    const float max_value = 270.0;
    const float buckets = 512.0;
    float bucket_min = log( max_value * floor(uv.y * buckets) / buckets );
    float bucket_max = log( max_value * floor((uv.y * buckets) + 1.0) / buckets );
    
    // count the count the r,g,b and luma in this column that match the bucket
    float4 count = float4(0.0, 0.0, 0.0, 0.0);
    for( int i=0; i < 512; ++i ) {
        float j = float(i) / buckets;
        float4 pixel = image.Sample(textureSampler, float2(uv.x, j )) * 256.0;
        
        // calculate the Rec.709 luma for this pixel
        pixel.a = pixel.r * 0.2126 + pixel.g * 0.7152 + pixel.b * 0.0722;

        float4 logpixel = log(pixel);
        if( logpixel.r >= bucket_min && logpixel.r < bucket_max) count.r += 1.0;
        if( logpixel.g >= bucket_min && logpixel.g < bucket_max) count.g += 1.0;
        if( logpixel.b >= bucket_min && logpixel.b < bucket_max) count.b += 1.0;
        if( logpixel.a >= bucket_min && logpixel.a < bucket_max) count.a += 1.0;
    }
    
    // sum luma into RGB, tweak log intensity for readability
    count.rgb = log(count.rgb * (1.0-blend) + count.aaa * blend) * gain;
          
    // output
    return count;
}