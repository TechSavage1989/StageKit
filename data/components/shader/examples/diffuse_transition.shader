//based on https://www.shadertoy.com/view/4cK3z1
uniform texture2d image_a;
uniform texture2d image_b;
uniform float transition_time = 0.5;
uniform bool convert_linear = true;

// Diffuse (move pixels) between two 2D images
// Demo inspired by Iterative-(de)Blending (see Figure 9 in https://arxiv.org/pdf/2305.03486.pdf)
// Note: the approach in this demo is different - rather than randomising paths we use means

// increase for greater precision - this is O(n^2) :(
uniform int num_samples<
    string label = "Number of samples (10)";
    string widget_type = "slider";
    int minimum = 2;
    int maximum = 100;
    int step = 1;
> = 10;

float4 mainImage(VertData v_in) : TARGET
{
    float2 uv = v_in.uv;

    if (transition_time < 0.00001) {
        return image_a.Sample(textureSampler, uv);
    }
    
    // we need to normalise the distributions so just sum the samples for a division later
    // note: could calculate this once per image in a buffer or something
    float3 from_total = float3(0.0,0.0,0.0);
    float3 to_total = float3(0.0,0.0,0.0);

    for (int i=0; i<num_samples; i++) {
        float sample_x = float(i) / float(num_samples);

        for (int j=0; j<num_samples; j++) {
            float sample_y = float(j) / float(num_samples);
            float2 sample_pos = float2(sample_x, sample_y);

            from_total += image_a.Sample(textureSampler, sample_pos).rgb;
            to_total += image_b.Sample(textureSampler, sample_pos).rgb;
        }
    }

    // only a subset of the inputs and outputs would cross our 3d coord, we can compute the ranges
    // maths: https://www.desmos.com/3d/60b155c9e9
    float from_alpha = -transition_time/(1.0-transition_time);
    
    float2 from_start = clamp(((1.0 - uv) *from_alpha) + uv, float2(0.0,0.0), float2(1.0,1.0));
    float2 from_end = clamp(-uv * from_alpha + uv, float2(0.0,0.0), float2(1.0,1.0));
    
    float to_alpha = (1.0-transition_time) / -transition_time;

    float2 to_start = clamp(-uv * to_alpha + uv, float2(0.0,0.0), float2(1.0,1.0));
    float2 to_end = clamp((1.0 - uv) * to_alpha + uv, float2(0.0,0.0), float2(1.0,1.0));

    //all we need to do is figure out how many points from the original distribution will go through this coord on their way to the target
    float3 sum = float3(0.0,0.0,0.0);

    for (int i=0; i<num_samples; i++) {
        float sample_x = float(i) / float(num_samples);

        for (int j=0; j<num_samples; j++) {
            float sample_y = float(j) / float(num_samples);
            float2 sample_pos = float2(sample_x, sample_y);

            float2 from_pos = lerp(from_start, from_end, sample_pos);
            float2 to_pos = lerp(to_start, to_end, sample_pos);

            sum += image_a.Sample(textureSampler, from_pos).rgb  * (image_b.Sample(textureSampler, to_pos).rgb / to_total);
        }
    }
    
    //the two distributions may have a different sum so scale to blend between the two
    float3 target_total = lerp(from_total, to_total, transition_time);
    float3 total_multiplier = target_total / from_total;

    sum *= total_multiplier;
    if (convert_linear)
		sum = srgb_nonlinear_to_linear(sum);

    return  float4(sum,1.0);
}
