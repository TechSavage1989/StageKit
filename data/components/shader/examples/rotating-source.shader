//spin speed higher the slower
uniform float spin_speed<
    string label = "Spin Speed";
    string widget_type = "slider";
    float minimum = -10.0;
    float maximum = 10.0;
    float step = 0.001;
> = 1.0;
uniform float rotation<
    string label = "Rotation";
    string widget_type = "slider";
    float minimum = -360.0;
    float maximum = 360.0;
    float step = 0.1;
> = 0.0;
uniform float zoomin<
    string label = "Zoom";
    string widget_type = "slider";
    float minimum = 0.01;
    float maximum = 10.0;
    float step = 0.01;
> = 1.0;
uniform bool keep_aspectratio = true;
uniform float x_center<
    string label = "Center x";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 1.0;
    float step = 0.001;
> = 0.5;
uniform float y_center<
    string label = "Center y";
    string widget_type = "slider";
    float minimum = 0.0;
    float maximum = 1.0;
    float step = 0.001;
> = 0.5;


//main fragment code
//from lioran to nutella with love
float4 mainImage(VertData v_in) : TARGET
{
    float x_aspectratio = keep_aspectratio ? uv_size.x : 1.0;
    float y_aspectratio = keep_aspectratio ? uv_size.y : 1.0;
    //get position on of the texture and focus on the middle
    float i_rotation;
    if (spin_speed == 0){
        //turn angle number into pi number
        i_rotation = rotation/57.295779513;
    }else{
        //use elapsed time for spinning if spin speed is not 0
        i_rotation = elapsed_time * spin_speed;
    }
    float2 i_point;
    i_point.x = (v_in.uv.x * x_aspectratio) - (x_aspectratio * x_center);
    i_point.y = (v_in.uv.y * y_aspectratio) - (y_aspectratio * y_center);
        
    //get the angle from center , returns pi number
    float i_dir = atan(i_point.y/i_point.x);
    if(i_point.x < 0.0){
        i_dir += 3.14159265359;
    }
        
    //get the distance from the centers
    float i_distance = sqrt(pow(i_point.x,2) + pow(i_point.y,2));
    //multiple distance by the zoomin value
    i_distance *= zoomin;
        
    //shift the texture position based on angle and distance from the middle
    i_point.x = ((x_aspectratio*x_center)+cos(i_dir-i_rotation)*i_distance)/x_aspectratio;
    i_point.y = ((y_aspectratio*y_center)+sin(i_dir-i_rotation)*i_distance)/y_aspectratio;
            
    //draw normally from new point
    return image.Sample(textureSampler, i_point);
}