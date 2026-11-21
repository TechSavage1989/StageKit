// Invert shader 1.0 - for OBS Shaderfilter
// Copyright 2021 by SkeletonBow
// https://twitter.com/skeletonbowtv
// https://twitch.tv/skeletonbowtv

// Performs RGB color inversion or YUV luma inversion with optional sRGB gamma handling

uniform bool Invert_Color = false;
uniform bool Invert_Luma = true;
uniform bool Gamma_Correction = true;
uniform bool Test_Ramp = false;

float3 encodeSRGB(float3 linearRGB)
{
    float3 a = float3(12.92,12.92,12.92) * linearRGB;
    float3 b = float3(1.055,1.055,1.055) * pow(linearRGB, float3(1.0 / 2.4,1.0 / 2.4,1.0 / 2.4)) - float3(0.055,0.055,0.055);
    float3 c = step(float3(0.0031308,0.0031308,0.0031308), linearRGB);
    return float3(lerp(a, b, c));
}

float3 decodeSRGB(float3 screenRGB)
{
    float3 a = screenRGB / float3(12.92,12.92,12.92);
    float3 b = pow((screenRGB + float3(0.055,0.055,0.055)) / float3(1.055,1.055,1.055), float3(2.4,2.4,2.4));
    float3 c = step(float3(0.04045,0.04045,0.04045), screenRGB);
    return float3(lerp(a, b, c));
}

float3 HUEtoRGB(in float H)
{
	float R = abs(H * 6 - 3) - 1;
	float G = 2 - abs(H * 6 - 2);
	float B = 2 - abs(H * 6 - 4);
	return float3(clamp(float3(R,G,B), float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0)));
}

float3 RGBtoYUV(float3 color)
{
	// YUV matriz (BT709 luma coefficients)
#ifdef OPENGL
        float3x3 toYUV = float3x3(
			float3(0.2126, -0.09991,  0.615),
			float3(0.7152, -0.33609, -0.55861),
			float3(0.0722,  0.436,   -0.05639));
#else
        float3x3 toYUV = {
		{  0.2126, -0.09991,  0.615  },
		{  0.7152, -0.33609, -0.55861 },
		{  0.0722,  0.436,   -0.05639 },
	};
#endif
	return mul(color, toYUV);
}

float3 YUVtoRGB(float3 color)
{
	// YUV matriz (BT709)
#ifdef OPENGL
        float3x3 fromYUV = float3x3(
			float3(1.000,    1.000,   1.000),
			float3(0.0,     -0.21482, 2.12798),
			float3(1.28033, -0.38059, 0.0));
#else
	float3x3 fromYUV = { 
		{ 1.000,    1.000,   1.000 },
		{ 0.0,     -0.21482, 2.12798 },
		{ 1.28033, -0.38059, 0.0 },
	};
#endif
	return mul(color, fromYUV);
}

float3 generate_ramps(float3 color, float2 uv)
{
	float3 ramp = float3(0.0, 0.0, 0.0);
	if(uv.y < 0.2)
		ramp.r = uv.x;			// Red ramp
	else if(uv.y < 0.4)
		ramp.g = uv.x;			// Green ramp
	else if(uv.y < 0.6)
		ramp.b = uv.x;			// Blue ramp
	else if(uv.y < 0.8)
		ramp = float3(uv.x, uv.x, uv.x);			// Grey ramp
	else
		ramp = HUEtoRGB(uv.x);	// Hue rainbow
	
	return ramp;
}

float4 mainImage( VertData v_in ) : TARGET
{
	float2 uv = v_in.uv;
	float4 obstex = image.Sample( textureSampler, uv );
	float3 color = obstex.rgb;
	// Apply sRGB gamma transfer encode
	if(Gamma_Correction) color = encodeSRGB( color );
	// Override display with test patterns to visually see what is happening
	if( Test_Ramp )	color = generate_ramps( obstex.rgb, uv );
	// RGB color invert	
	if( Invert_Color ) {
		color = float3(1.0, 1.0, 1.0) - color;
	}
	// YUV luma invert
	if( Invert_Luma ) {
		float3 yuv = RGBtoYUV( color );
		yuv.x = 1.0 - yuv.x;
		color = YUVtoRGB(yuv);
	}
	// Apply sRGB gamma transfer decode
	if(Gamma_Correction) color = decodeSRGB( color );
	return float4(color, obstex.a);
}
