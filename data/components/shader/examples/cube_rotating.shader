uniform int images<
  string label = "Images";
  string widget_type = "select";
  int    option_0_value = 1;
  string option_0_label = "1";
  int    option_1_value = 2;
  string option_1_label = "2";
  int    option_2_value = 4;
  string option_2_label = "4";
> = 1;

uniform float speed<
    string label = "Speed";
    string widget_type = "slider";
    float minimum = -5.0;
    float maximum = 5.0;
    float step = 0.001;
> = 0.5;

uniform float shadow<
    string label = "Shadow";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 2.5;
    float step = 0.001;
> = 1.0;

uniform texture2d other_image1;
uniform texture2d other_image2;
uniform texture2d other_image3;

#define PI 3.14159265359
float4 mainImage(VertData v_in) : TARGET
{
	float t = elapsed_time * speed;
	float4 c = float4(0,0,0,0);
	for(float side = 0.0; side<4.0; side += 1.0){
		float left = cos(t+((side*0.5-0.25)*PI))/2+0.5;
		float right = cos(t+((side*0.5+0.25)*PI))/2+0.5;
		if(left < right){
			float2 uv;
			uv.x = (v_in.uv.x-left)/(right-left);
			float left_size = 1.0 +sin(t+((side*0.5-0.25)*PI))/2+0.5;
			float right_size = 1.0 + sin(t+((side*0.5+0.25)*PI))/2+0.5;
			float size = (uv.x * right_size) + ((1.0-uv.x) * left_size);
			uv.y = (v_in.uv.y-0.5)*size+0.5;
			float4 sample = float4(0,0,0,0);
			if(images <= 1 || side == 0.0){
				sample = image.Sample(textureSampler, uv);
			}else if(images == 2){
				if(side == 1.0 || side == 3.0){
					sample = other_image1.Sample(textureSampler, uv);
				}else{
					sample = image.Sample(textureSampler, uv);
				}
			}else if(images == 4){
				if(side == 1.0){
					sample = other_image1.Sample(textureSampler, uv);
				}else if(side == 2.0){
					sample = other_image2.Sample(textureSampler, uv);
				}else if(side == 3.0){
					sample = other_image3.Sample(textureSampler, uv);
				}else{
					sample = image.Sample(textureSampler, uv);
				}
			}
			if(sample.a > 0.0){
				c += float4(sample.rgb*(1.0-abs((left+right)/2.0-0.5)*shadow),sample.a);
			}
		}
	}
	return c;
}
