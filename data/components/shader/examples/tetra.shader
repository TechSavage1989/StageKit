// Tetrahedral Interpolation Shader for OBS

uniform float redR< 
    string label = "Red in Red"; 
    string widget_type = "slider";
    float minimum = 0.0; 
    float maximum = 2.0; 
    float step = 0.01;
> = 1.0;

uniform float redG< 
    string label = "Green in Red"; 
    string widget_type = "slider";
    float minimum = -1.0; 
    float maximum = 1.0; 
    float step = 0.01;
> = 0.0;

uniform float redB< 
    string label = "Blue in Red"; 
    string widget_type = "slider";
    float minimum = -1.0; 
    float maximum = 1.0; 
    float step = 0.01;
> = 0.0;

uniform float yelR< 
    string label = "Red in Yellow"; 
    string widget_type = "slider";
    float minimum = 0.0; 
    float maximum = 2.0; 
    float step = 0.01;
> = 1.0;

uniform float yelG< 
    string label = "Green in Yellow"; 
    string widget_type = "slider";
    float minimum = 0.0; 
    float maximum = 2.0; 
    float step = 0.01;
> = 1.0;

uniform float yelB< 
    string label = "Blue in Yellow"; 
    string widget_type = "slider";
    float minimum = -1.0; 
    float maximum = 1.0; 
    float step = 0.01;
> = 0.0;

uniform float grnR< 
    string label = "Red in Green"; 
    string widget_type = "slider";
    float minimum = -1.0; 
    float maximum = 1.0; 
    float step = 0.01;
> = 0.0;

uniform float grnG< 
    string label = "Green in Green"; 
    string widget_type = "slider";
    float minimum = 0.0; 
    float maximum = 2.0; 
    float step = 0.01;
> = 1.0;

uniform float grnB< 
    string label = "Blue in Green"; 
    string widget_type = "slider";
    float minimum = -1.0; 
    float maximum = 1.0; 
    float step = 0.01;
> = 0.0;

uniform float cynR< 
    string label = "Red in Cyan"; 
    string widget_type = "slider";
    float minimum = -1.0; 
    float maximum = 1.0; 
    float step = 0.01;
> = 0.0;

uniform float cynG< 
    string label = "Green in Cyan"; 
    string widget_type = "slider";
    float minimum = 0.0; 
    float maximum = 2.0; 
    float step = 0.01;
> = 1.0;

uniform float cynB< 
    string label = "Blue in Cyan"; 
    string widget_type = "slider";
    float minimum = 0.0; 
    float maximum = 2.0; 
    float step = 0.01;
> = 1.0;

uniform float bluR< 
    string label = "Red in Blue"; 
    string widget_type = "slider";
    float minimum = -1.0; 
    float maximum = 1.0; 
    float step = 0.01;
> = 0.0;

uniform float bluG< 
    string label = "Green in Blue"; 
    string widget_type = "slider";
    float minimum = -1.0; 
    float maximum = 1.0; 
    float step = 0.01;
> = 0.0;

uniform float bluB< 
    string label = "Blue in Blue"; 
    string widget_type = "slider";
    float minimum = 0.0; 
    float maximum = 2.0; 
    float step = 0.01;
> = 1.0;

uniform float magR< 
    string label = "Red in Magenta"; 
    string widget_type = "slider";
    float minimum = 0.0; 
    float maximum = 2.0; 
    float step = 0.01;
> = 1.0;

uniform float magG< 
    string label = "Green in Magenta"; 
    string widget_type = "slider";
    float minimum = -1.0; 
    float maximum = 1.0; 
    float step = 0.01;
> = 0.0;

uniform float magB< 
    string label = "Blue in Magenta"; 
    string widget_type = "slider";
    float minimum = 0.0; 
    float maximum = 2.0; 
    float step = 0.01;
> = 1.0;


float3 tetra(float3 RGBimage, float3 red, float3 yel, float3 grn, float3 cyn, float3 blu, float3 mag) {
    float r = RGBimage.x;
    float g = RGBimage.y;		
    float b = RGBimage.z;		

    float3 wht = float3(1.0, 1.0, 1.0);

    if (r > g) {
        if (g > b) {
            // r > g > b
            return r * red + g * (yel - red) + b * (wht - yel);
        } else if (r > b) {
            // r > b > g
            return r * red + g * (wht - mag) + b * (mag - red);
        } else {
            // b > r > g
            return r * (mag - blu) + g * (wht - mag) + b * blu;
        }
    } else {
        if (b > g) {
            // b > g > r
            return r * (wht - cyn) + g * (cyn - blu) + b * blu;
        } else if (b > r) {
            // g > b > r
            return r * (wht - cyn) + g * grn + b * (cyn - grn);
        } else {
            // g > r > b
            return r * (yel - grn) + g * grn + b * (wht - yel);
        }
    }
}

float4 mainImage(VertData v_in) : TARGET
{
    float4 inputColor = image.Sample(textureSampler, v_in.uv);
    float alpha = inputColor.a;

    float3 red = float3(redR, redG, redB);
    float3 yel = float3(yelR, yelG, yelB);
    float3 grn = float3(grnR, grnG, grnB);
    float3 cyn = float3(cynR, cynG, cynB);
    float3 blu = float3(bluR, bluG, bluB);
    float3 mag = float3(magR, magG, magB);

    float3 outputColor = tetra(inputColor.rgb, red, yel, grn, cyn, blu, mag);
    return float4(outputColor, alpha);
}
