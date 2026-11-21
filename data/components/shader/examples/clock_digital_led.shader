// based on https://www.shadertoy.com/view/MdfGzf
// cmarangu has linked all 7 segments in his comments
// https://www.shadertoy.com/view/3dtSRj

#ifndef OPENGL
#define mod(x,y) (x - y * floor(x / y))
#endif

uniform int current_time_sec;
uniform int current_time_min;
uniform int current_time_hour;

uniform int timeMode<
  string label = "Time mode";
  string widget_type = "select";
  int    option_0_value = 0;
  string option_0_label = "Time";
  int    option_1_value = 1;
  string option_1_label = "Enable duration";
  int    option_2_value = 2;
  string option_2_label = "Active duration";
  int    option_3_value = 3;
  string option_3_label = "Show duration";
  int    option_4_value = 4;
  string option_4_label = "Load duration";
> = 0;

uniform bool showMatrix = false;
uniform bool showOff = false;
uniform bool ampm = false;
uniform float4 ledColor = {1.0,0,0,1.0};
uniform int offsetHours = 0;
uniform int offsetSeconds = 0;

float segment(float2 uv, bool On)
{
	if (!On && !showOff)
		return 0.0;
	
	float seg = (1.0-smoothstep(0.08,0.09+float(On)*0.02,abs(uv.x)))*
			    (1.0-smoothstep(0.46,0.47+float(On)*0.02,abs(uv.y)+abs(uv.x)));
	
    //Fiddle with lights and matrix
	//uv.x += sin(elapsed_time*60.0*6.26)/14.0;
	//uv.y += cos(elapsed_time*60.0*6.26)/14.0;
	
	//led like brightness
	if (On){
		seg *= (1.0-length(uv*float2(3.8,0.9)));//-sin(elapsed_time*25.0*6.26)*0.04;
	} else {
		seg *= -(0.05+length(uv*float2(0.2,0.1)));
	}
	return seg;
}

float sevenSegment(float2 uv,int num)
{
	float seg= 0.0;
    seg += segment(uv.yx+float2(-1.0, 0.0),num!=-1 && num!=1 && num!=4                    );
	seg += segment(uv.xy+float2(-0.5,-0.5),num!=-1 && num!=1 && num!=2 && num!=3 && num!=7);
	seg += segment(uv.xy+float2( 0.5,-0.5),num!=-1 && num!=5 && num!=6                    );
   	seg += segment(uv.yx+float2( 0.0, 0.0),num!=-1 && num!=0 && num!=1 && num!=7          );
	seg += segment(uv.xy+float2(-0.5, 0.5),num==0 || num==2 || num==6 || num==8           );
	seg += segment(uv.xy+float2( 0.5, 0.5),num!=-1 && num!=2                              );
    seg += segment(uv.yx+float2( 1.0, 0.0),num!=-1 && num!=1 && num!=4 && num!=7          );
	
	return seg;
}

float showNum(float2 uv,int nr, bool zeroTrim)
{
	//Speed optimization, leave if pixel is not in segment
	if (abs(uv.x)>1.5 || abs(uv.y)>1.2)
		return 0.0;
	
	float seg= 0.0;
	if (uv.x>0.0)
	{
		nr /= 10;
		if (nr==0 && zeroTrim)
			nr = -1;
		seg += sevenSegment(uv+float2(-0.75,0.0),nr);
	} else {
		seg += sevenSegment(uv+float2( 0.75,0.0),int(mod(float(nr),10.0)));
	}

	return seg;
}

float dots(float2 uv)
{
	float seg = 0.0;
	uv.y -= 0.5;
	seg += (1.0-smoothstep(0.11,0.13,length(uv))) * (1.0-length(uv)*2.0);
	uv.y += 1.0;
	seg += (1.0-smoothstep(0.11,0.13,length(uv))) * (1.0-length(uv)*2.0);
	return seg;
}

float4 mainImage(VertData v_in) : TARGET
{
	float2 uv = ((float2(v_in.uv.x, 1.0-v_in.uv.y)  * uv_size).xy-0.5*uv_size) /
 		       min(uv_size.x,uv_size.y);
	
	if (uv_size.x>uv_size.y)
	{
		uv *= 6.0;
	}
	else
	{
		uv *= 12.0;
	}
	
	uv.x *= -1.0;
	uv.x += uv.y/12.0;
	//wobble
	//uv.x += sin(uv.y*3.0+elapsed_time*14.0)/25.0;
	//uv.y += cos(uv.x*3.0+elapsed_time*14.0)/25.0;
    uv.x += 3.5;
	float seg = 0.0;

	if(timeMode == 0){
		seg += showNum(uv,current_time_sec,false);
		uv.x -= 1.75;
		seg += dots(uv);
		uv.x -= 1.75;
		seg += showNum(uv,current_time_min,false);
		uv.x -= 1.75;
		seg += dots(uv);
		uv.x -= 1.75;
		if (ampm) {
			if(current_time_hour == 0){
				seg += showNum(uv,12,true);
			}else if(current_time_hour > 12){
				seg += showNum(uv,current_time_hour-12,true);
			}else{
				seg += showNum(uv,current_time_hour,true);
			}
		} else {
			seg += showNum(uv,current_time_hour,true);
		}
	}else{
		float timeSecs = 0.0;
		if(timeMode == 1){
			timeSecs = elapsed_time_enable;
		}else if(timeMode == 2){
			timeSecs = elapsed_time_active;
		}else if(timeMode == 3){
			timeSecs = elapsed_time_show;
		}else if(timeMode == 4){
			timeSecs = elapsed_time_start;
		}

		timeSecs += offsetSeconds + offsetHours*3600;
		if(timeSecs < 0)
			timeSecs = 0.9999-timeSecs;
		seg += showNum(uv,int(mod(timeSecs,60.0)),false);
		
		timeSecs = floor(timeSecs/60.0);
		
		uv.x -= 1.75;

		seg += dots(uv);
		
		uv.x -= 1.75;
		
		seg += showNum(uv,int(mod(timeSecs,60.0)),false);
		
		timeSecs = floor(timeSecs/60.0);
		if (ampm)
		{
			if(timeSecs == 0.0){
				timeSecs = 12.0;
			}else if (timeSecs > 12.0){
				timeSecs = mod(timeSecs,12.0);
			}
		}else if (timeSecs > 24.0) {
			timeSecs = mod(timeSecs,24.0);
		}
		
		uv.x -= 1.75;
		
		seg += dots(uv);
		
		uv.x -= 1.75;
		seg += showNum(uv,int(mod(timeSecs,60.0)),true);
	}

	
	if (seg==0.0){
		return image.Sample(textureSampler, v_in.uv);
	}
	// matrix over segment
	if (showMatrix)
	{
		seg *= 0.8+0.2*smoothstep(0.02,0.04,mod(uv.y+uv.x,0.06025));
		//seg *= 0.8+0.2*smoothstep(0.02,0.04,mod(uv.y-uv.x,0.06025));
	}
	if (seg<0.0)
	{
		seg = -seg;;
		return float4(seg,seg,seg,1.0);
	}
	return float4(ledColor.rgb * seg, ledColor.a);
}