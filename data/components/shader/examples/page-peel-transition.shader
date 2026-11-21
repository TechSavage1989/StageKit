uniform texture2d image_a;
uniform texture2d image_b;
uniform float transition_time<
    string label = "Transittion Time";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 1.0;
    float step = 0.001;
> = 0.5;
uniform bool convert_linear = true;
uniform float4 page_color = {1.0, 1.0, 1.0, 1.0};
uniform float page_transparency<
    string label = "Page Transparency";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 1.0;
    float step = 0.001;
> = 0.5;

float4 mainImage(VertData v_in) : TARGET
{
    // Normalized pixel coordinates (from 0 to 1)
    float2 aspect = float2( uv_size.x / uv_size.y, 1.0 );
	float2 uv = v_in.uv;

    float t = transition_time * 12.0 + 11.0;
    // Define the fold.
    float2 origin = float2( 0.6 + 0.6 * sin( t * 0.2 ), 0.5 + 0.5 * cos( t * 0.13 ) ) * aspect;
    float2 normal = normalize( float2( 1.0, 2.0 * sin( t * 0.3 ) ) * aspect );

    // Sample texture.
    float3 col = float3(1.0,0.0,0.0);
    
    // Check on which side the pixel lies.
    float2 pt = uv * aspect - origin;
    float side = dot( pt, normal );
    if( side > 0.0 ) {
        // Next page
        col = image_b.Sample( textureSampler, uv ).rgb;
            
        float shadow = smoothstep( 0.0, 0.05, side );
        col = lerp( col * 0.6, col, shadow );
    }
    else {
        

        // Find the mirror pixel.
        pt = ( uv * aspect - 2.0 * side * normal ) / aspect;
        
        // Check if we're still inside the image bounds.
        if( pt.x >= 0.0 && pt.x < 1.0 && pt.y >= 0.0 && pt.y < 1.0 ) {
            col = image_a.Sample( textureSampler, pt ).rgb; // Back color.
            col = lerp(page_color.rgb, col, page_transparency);
            
            float shadow = smoothstep( 0.0, 0.2, -side );
            col = lerp( col * 0.2, col, shadow );
        }else{
            col = image_a.Sample( textureSampler, uv ).rgb;
        }
    }
    
    // Output to screen
    if(convert_linear)
        col = srgb_nonlinear_to_linear(col);
	return float4(col,1.0);
}
