// https://www.shadertoy.com/view/dlBBz1 adopted for OBS by Exeldro

// width of a single color channel in pixels
uniform int channelWidth<
    string label = "Channel Width";
    string widget_type = "slider";
    int minimum = 1;
    int maximum = 20;
    int step = 1;
> = 1;

// height of color channels in pixels
uniform int channelHeight<
    string label = "Channel Height";
    string widget_type = "slider";
    int minimum = 1;
    int maximum = 20;
    int step = 1;
> = 3;

// horizontal distance between two neighboring pixels
uniform int hGap<
    string label = "Horizontal Gap";
    string widget_type = "slider";
    int minimum = 1;
    int maximum = 20;
    int step = 1;
> = 1;

// vertical distance between two neighboring pixels
uniform int vGap<
    string label = "Vertical Gap";
    string widget_type = "slider";
    int minimum = 1;
    int maximum = 20;
    int step = 1;
> = 1;

float4 mainImage(VertData v_in) : TARGET
{
    float columns = float(channelWidth * 3 + hGap);
    float pixelHeight = float(channelHeight + vGap);

    float2 fragCoord = v_in.uv * uv_size;
    float2 sampleRes = float2(uv_size.x / columns, uv_size.y / pixelHeight);
    float2 pixel = float2(floor(fragCoord.x / columns), floor(fragCoord.y / pixelHeight));
    float2 sampleUv = pixel / sampleRes;

    // color of sample point
    float4 col = image.Sample(textureSampler, sampleUv);
    
    int column = int(fragCoord.x) % (channelWidth * 3 + hGap);

    // set color based on which channel this fragment corresponds to
    if (column < channelWidth * 1) col = float4(col.r, 0.0, 0.0, col.a);
    else if (column < channelWidth * 2) col = float4(0.0, col.g, 0.0, col.a);
    else if (column < channelWidth * 3) col = float4(0.0, 0.0, col.b, col.a);
    else col = float4(0.0, 0.0, 0.0, col.a);

    // offset every other column of pixels
    int height = int(pixelHeight);
    if (int(pixel.x) % 2 == 0) {
        if (int(fragCoord.y) % height >= height - vGap) col = float4(0.0, 0.0, 0.0, col.a);
    } else {
        if (int(fragCoord.y) % height < vGap) col = float4(0.0, 0.0, 0.0, col.a);
    }

    // Output to screen
    return col;
}