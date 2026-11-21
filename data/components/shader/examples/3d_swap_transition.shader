//based on https://www.shadertoy.com/view/MlXGzf

uniform texture2d image_a;
uniform texture2d image_b;
uniform float transition_time = 0.5;
uniform bool convert_linear = true;

uniform float reflection<
    string label = "Reflection (0.4)";
    string widget_type = "slider";
    float minimum = 0.00;
    float maximum = 1.00;
    float step = 0.01;
> = 0.4;
uniform float perspective<
    string label = "Perspective (0.2)";
    string widget_type = "slider";
    float minimum = 0.00;
    float maximum = 1.00;
    float step = 0.01;
> = .2;
uniform float depth<
    string label = "Depth (3.0)";
    string widget_type = "slider";
    float minimum = 1.00;
    float maximum = 10.00;
    float step = 0.1;
> = 3.;

#ifndef OPENGL
#define lessThan(a,b) (a < b)
#endif


uniform float4 background_color = {0.0, 0.0, 0.0, 1.0};
 
bool inBounds (float2 p) {
  return all(lessThan(float2(0.0,0.0), p)) && all(lessThan(p, float2(1.0,1.0)));
}
 
float2 project (float2 p) {
  return p * float2(1.0, -1.2) + float2(0.0, 2.22);
}
 
float4 bgColor (float2 p, float2 pfr, float2 pto) {
  float4 c = background_color;
  pfr = project(pfr);
  if (inBounds(pfr)) {
    c += lerp(background_color, image_a.Sample(textureSampler, pfr), reflection * lerp(0.0, 1.0, pfr.y));
  }
  pto = project(pto);
  if (inBounds(pto)) {
    c += lerp(background_color, image_b.Sample(textureSampler, pto), reflection * lerp(0.0, 1.0, pto.y));
  }
  return c;
}
 
float4 mainImage(VertData v_in) : TARGET {
  float2 p = v_in.uv;
  float2 pfr = float2(-1.,-1.);
  float2 pto  = float2(-1.,-1.);
 
  float progress = transition_time;
  float size = lerp(1.0, depth, progress);
  float persp = perspective * progress;
  pfr = (p + float2(-0.0, -0.5)) * float2(size/(1.0-perspective*progress), size/(1.0-size*persp*p.x)) + float2(0.0, 0.5);
 
  size = lerp(1.0, depth, 1.-progress);
  persp = perspective * (1.-progress);
  pto = (p + float2(-1.0, -0.5)) * float2(size/(1.0-perspective*(1.0-progress)), size/(1.0-size*persp*(0.5-p.x))) + float2(1.0, 0.5);
 
  bool fromOver = progress < 0.5;
  float4 rgba = background_color;
  if (fromOver) {
    if (inBounds(pfr)) {
      rgba = image_a.Sample(textureSampler, pfr);
    }
    else if (inBounds(pto)) {
      rgba = image_b.Sample(textureSampler, pto);
    }
    else {
      rgba = bgColor(p, pfr, pto);
    }
  }
  else {
    if (inBounds(pto)) {
      rgba = image_b.Sample(textureSampler, pto);
    }
    else if (inBounds(pfr)) {
      rgba = image_a.Sample(textureSampler, pfr);
    }
    else {
      rgba = bgColor(p, pfr, pto);
    }
  }
  if (convert_linear)
		rgba.rgb = srgb_nonlinear_to_linear(rgba.rgb);
	return rgba;
}