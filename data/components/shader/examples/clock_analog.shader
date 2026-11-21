//Based on https://www.shadertoy.com/view/XdKXzy
uniform int current_time_ms;
uniform int current_time_sec;
uniform int current_time_min;
uniform int current_time_hour;
uniform float3 hour_handle_color = {1.0,1.0,1.0};
uniform float3 minute_handle_color = {1.0,1.0,1.0};
uniform float3 second_handle_color = {1.0,0.0,0.0};
uniform float3 outline_color = {1.0,1.0,1.0};
uniform float3 top_line_color = {1.0,0.0,0.0};
uniform float3 background_color = {.5,.5,.5};
uniform int time_offset_hours = 0;

#ifndef OPENGL
#define mod(x,y) (x - y * floor(x / y))
#endif
// this is my first try to actually use glsl almost from scratch
// so far all i've done is learning by doing / reading glsl docs.
// this is inspired by my non glsl â€želapsed_timeâ€œ projects
// especially this one: https://www.gottz.de/analoguhr.htm

// i will most likely use a buffer in future to calculate the elapsed_time
// aswell as to draw the background of the clock only once.
// tell me if thats a bad idea.

// update:
// screenshot: http://i.imgur.com/dF0nHDk.png
// as soon as i think its in a usefull state i'll release the source
// of that particular c++ application on github.
// i hope sommeone might find it usefull :D

#define PI 3.141592653589793238462643383

// from https://www.shadertoy.com/view/4s3XDn <3
float ln(float2 p, float2 a, float2 b)
{
	float2 pa = p - a;
	float2 ba = b - a;
	float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h);
}

// i think i should spend some elapsed_time reading docs in order to minimize this.
// hints apreciated
// (Rotated LiNe)
float rln(float2 uv, float start, float end, float perc) {
    float inp = perc * PI * 2.0;
	float2 coord = float2(sin(inp), cos(inp));
    return ln(uv, coord * start, coord * end);
}

// i need this to have an alphachannel in the output
// i intend to use an optimized version of this shader for a transparent desktop widget experiment
float4 mixer(float4 c1, float4 c2) {
    // please tell me if you think this would boost performance.
    // the elapsed_time i implemented mix myself it sure did reduce
    // the amount of operations but i'm not sure now
    // if (c2.a <= 0.0) return c1;
    // if (c2.a >= 1.0) return c2;
    return float4(lerp(c1.rgb, c2.rgb, c2.a), c1.a + c2.a);
    // in case you are curious how you could implement mix yourself:
    // return float4(c2.rgb * c2.a + c1.rgb * (1.0-c2.a), c1.a+c2.a);
}
    
float4 styleHandle(float4 color, float px, float dist, float3 handleColor, float width, float shadow) {
    if (dist <= width + shadow) {
        // lets draw the shadow
        color = mixer(color, float4(0.0, 0.0, 0.0,
                                (1.0-pow(smoothstep(width, width + shadow, dist),0.2))*0.2));
        // now lets draw the antialiased handle
        color = mixer(color, float4(handleColor, smoothstep(width, max(width - 3.0 * px, 0.0), dist)));
    }
    return color;
}

float4 mainImage(VertData v_in) : TARGET
{
    float2 R = uv_size;
    // calculate the size of a pixel
    float px = 1.0 / R.y;
    // create percentages of the coordinate system
    float2 p = (v_in.uv * uv_size).xy / R;
    // center the scene and add perspective
    float2 uv = (2.0 * (float2(v_in.uv.x,1.0-v_in.uv.y) * uv_size) - R) / min(R.x, R.y);
    
    /*float2 uv = -1.0 + 2.0 * p.xy;
    // lets add perspective for mobile device support
    if (uv_size.x > uv_size.y)
    	uv.x *= uv_size.x / uv_size.y;
    else
        uv.y *= uv_size.y / uv_size.x;*/
    
    // lets scale the scene a bit down:
    uv *= 1.1;
    px *= 0.9;
    
    float width = 0.015;
    float dist = 1.0;
    float centerdist = length(uv);
    
    float4 color = image.Sample(textureSampler, v_in.uv);
    
    // background of the clock
    if (centerdist < 1.0 - width) color = mixer(color, float4(background_color, 0.4*(1.8-length(uv))));
    
    float isRed = 1.0;
 
    if (centerdist > 1.0 - 12.0 * width && centerdist <= 1.1) {
        // minute bars
        for (float i = 0.0; i <= 15.0; i += 1.0) {
            if (mod(i, 5.0) == 0.0) {
                dist = min(dist, rln(abs(uv), 1.0 - 10.0 * width, 1.0 - 2.0 * width, i / 60.0));
                // draw first bar red
                if (i == 0.0 && uv.y > 0.0) {
                    isRed = dist;
                    dist = smoothstep(width, max(width - 3.0 * px, 0.0), dist);
                    color = mixer(color, float4(top_line_color, dist));
                    dist = 1.0;
                }
            }
            else {
                dist = min(dist, rln(abs(uv), 1.0 - 10.0 * width, 1.0 - 7.0 * width, i / 60.0));
            }
        }

        // outline circle
        dist = min(dist, abs(1.0-width-length(uv)));
        // draw clock shadow
        if (centerdist > 1.0)
            color = mixer(color, float4(0.0,0.0,0.0, 0.3*smoothstep(1.0 + width*2.0, 1.0, centerdist)));

        // draw outline + minute bars in white
		color = mixer(color, float4(0.0, 0.0, 0.0,
			(1.0 - pow(smoothstep(width, width + 0.02, min(isRed, dist)), 0.4))*0.2));
		color = mixer(color, float4(outline_color, smoothstep(width, max(width - 3.0 * px, 0.0), dist)));
    }
    
    if (centerdist < 1.0) {
        float elapsed_time = float((time_offset_hours+current_time_hour)*3600+current_time_min*60+current_time_sec) + pow(float(current_time_ms)/1000.0,16.0);
        // hour
        color = styleHandle(color, px,
                            rln(uv, -0.05, 0.5, elapsed_time / 3600.0 / 12.0),
                            hour_handle_color, 0.03, 0.02);

        // minute
        color = styleHandle(color, px,
                            rln(uv, -0.075, 0.7, elapsed_time / 3600.0),
                            minute_handle_color, 0.02, 0.02);

        // second
        color = styleHandle(color, px,
                            min(rln(uv, -0.1, 0.9, elapsed_time / 60.0), length(uv)-0.01),
                            second_handle_color, 0.01, 0.02);
    }
    
    
    return  color;
}