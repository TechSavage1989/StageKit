#define PI  3.1415926535897932384626433832795
#define PI180 float(PI / 180.0)

uniform float threshold<
    string label = "Threshold";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 1.0;
    float step = 0.001;
> = 0.6;

float sind(float a)
{
	return sin(a * PI180);
}
 
float cosd(float a)
{
	return cos(a * PI180);
}
 
float added(float2 sh, float sa, float ca, float2 c, float d)
{
	return 0.5 + 0.25 * cos((sh.x * sa + sh.y * ca + c.x) * d) + 0.25 * cos((sh.x * ca - sh.y * sa + c.y) * d);
}
 
float4 mainImage(VertData v_in) : TARGET
{
	// Halftone dot matrix shader
	// @author Tomek Augustyn 2010
 
	// Ported from my old PixelBender experiment
	// https://github.com/og2t/HiSlope/blob/master/src/hislope/pbk/fx/halftone/Halftone.pbk
 
	float coordX = v_in.uv.x;
	float coordY = v_in.uv.y;
	float2 dstCoord = float2(coordX, coordY);
	float2 rotationCenter = float2(0.5, 0.5);
	float2 shift = dstCoord - rotationCenter;
 
	float dotSize = 3.0;
	float angle = 45.0;
 
	float rasterPattern = added(shift, sind(angle), cosd(angle), rotationCenter, PI / dotSize * 680.0);
	float4 srcPixel = image.Sample(textureSampler, dstCoord);
 
	float avg = 0.2125 * srcPixel.r + 0.7154 * srcPixel.g + 0.0721 * srcPixel.b;
    float gray = (rasterPattern * threshold + avg - threshold) / (1.0 - threshold);
 
	// uncomment to see how the raster pattern looks 
    // gray = rasterPattern;
 
	return float4(gray, gray, gray, 1.0);
}