#include "chassis_control.h"
#include "chassis_task.h"
#include "PID.h"

#include "main.h"
#include "arm_math.h"
#include "string.h"
#include "protocol.h"
#include "cmsis_os.h"

#define CHASSIS_MOTOR_RPM_TO_VECTOR_SEN 0.00005f
#define AB 0.5f

extern uint8_t chassis_odom_pack_solve(
  float x,
  float y,
  float odom_yaw,
  float vx,
  float vy,
  float vw,
  float gyro_z,
  float gyro_yaw);
	
extern uint8_t CDC_Transmit_FS(uint8_t* Buf, uint16_t Len);

//�����˶�����
chassis_move_t chassis_move;
uint8_t usb_tx[128];

// auto_control unpacked data
extern chassis_ctrl_info_t ch_auto_control_data;

//-hk-���µ���4������ٶ�		
void chassis_motor_speed_update(chassis_move_t *chassis_move_update)
{
    uint8_t i = 0;
    for (i = 0; i < 4; i++)
    {
        //���µ���ٶȣ����ٶ����ٶȵ�PID΢��
        chassis_move_update->motor_chassis[i].speed = CHASSIS_MOTOR_RPM_TO_VECTOR_SEN * chassis_move_update->motor_chassis[i].chassis_motor_measure->speed_rpm;
    }
}

//-hk-�������ٶ�ת��4�������ٶ�
void chassis_vector_to_mecanum_wheel_speed(const fp32 vx_set, const fp32 vy_set, const fp32 wz_set, fp32 wheel_speed[4])
{
  wheel_speed[0]=vx_set+vy_set+wz_set*AB;
	wheel_speed[1]=vx_set-vy_set-wz_set*AB;
	wheel_speed[2]=vx_set+vy_set-wz_set*AB;
	wheel_speed[3]=-vx_set+vy_set-wz_set*AB;    
}

int32_t v[4]={0,0,0,0};
int32_t sv[4]={0,0,0,0};
int32_t dv[4]={0,0,0,0};

static fp32 distance_x = 0.0f, distance_y = 0.0f, distance_wz = 0.0f;

//-hk-�����񡷼���������ξ���
void chassis_distance_calc_task(void const * argument)
{		
	float x,y,theta,s[4];

	while(1)
	{
		for(int i=0;i<4;i++)
		{
			s[i]=chassis_move.motor_chassis[i].speed;
		}
		
		for(int i=0;i<4;i++)
		{
			sv[i]=v[i];
			v[i]=chassis_move.motor_chassis[i].chassis_motor_measure->total_ecd;
			dv[i]=v[i]-sv[i];
		}
		
		x=(-dv[0]+ dv[1]+dv[2]-dv[3])/4;
		y=(dv[0]- dv[1]+dv[2]-dv[3])/4;   
		theta=(dv[0]- dv[1]-dv[2]+dv[3])/(4*AB);  
		
		chassis_move.vx=(-s[0]+ s[1]+s[2]-s[3])/4;
		chassis_move.vy=(s[0]- s[1]+s[2]-s[3])/4;   
		chassis_move.wz=(s[0]- s[1]-s[2]+s[3])/(4*AB); 
		distance_x+=(cos(theta)*x-sin(theta)*y);
		distance_y+=(sin(theta)*x+sin(theta)*y);
		distance_wz+=theta;
								
		osDelay(1);
	}
}

//-hk-�����񡷷��͵������о��롢�ٶȡ�gyro_z��yaw�Ȳ���
void chassis_distance_send_task(void const * argument)
{   
	while(1)
	{
		uint8_t send_len;
		send_len = chassis_odom_pack_solve( distance_x, distance_y, distance_wz, chassis_move.vx, chassis_move.vy, chassis_move.wz, chassis_move.chassis_gyro_z, chassis_move.chassis_yaw);
		osDelay(10);
	}   
}
//-hk-������������ʱ������õ����̵�vx_set��vy_set
void chassis_normal_control(fp32 *vx_set, fp32 *vy_set, fp32 *wz_set, chassis_move_t *chassis_move_rc_to_vector)
{
	if (vx_set == NULL || vy_set == NULL || wz_set == NULL || chassis_move_rc_to_vector == NULL)
	{
			return;
	}
	//-hk-����ң�����ֱ�λ������õ����̵�vx_set��vy_setֵ
	chassis_rc_to_control_vector(vx_set, vy_set, chassis_move_rc_to_vector);
	*wz_set = -CHASSIS_WZ_RC_SEN * chassis_move_rc_to_vector->chassis_RC->rc.ch[CHASSIS_WZ_CHANNEL];
}

//-hk-�����Զ�����ʱ����vx_set��vy_set�ȸ�ֵ
void chassis_auto_control(fp32 *vx_set, fp32 *vy_set, fp32 *wz_set, chassis_move_t *chassis_move_rc_to_vector)
{
	if (vx_set == NULL || vy_set == NULL || wz_set == NULL || chassis_move_rc_to_vector == NULL)
	{
			return;
	}
	*vx_set =ch_auto_control_data.vx;
	*vy_set =ch_auto_control_data.vy;
	*wz_set =ch_auto_control_data.vw;
	
    return;
}

//-hk-����PID��ʼ��
void chassis_PID_init(void)
{
    //�����ٶȻ�pidֵ
    const static fp32 motor_speed_pid[3] = {M3505_MOTOR_SPEED_PID_KP, M3505_MOTOR_SPEED_PID_KI, M3505_MOTOR_SPEED_PID_KD};

    const static fp32 chassis_rotation_pid[3] = {CHASSIS_ROTATION_PID_KP, CHASSIS_ROTATION_PID_KI, CHASSIS_ROTATION_PID_KD};
    //������ת��pidֵ
    const static fp32 chassis_angle_pid[3] = {CHASSIS_ANGLE_PID_KP, CHASSIS_ANGLE_PID_KI, CHASSIS_ANGLE_PID_KD};

    uint8_t i;

    //��ʼ��PID �˶�
    for (i = 0; i < 4; i++)
    {
        PID_Init(&chassis_move.motor_speed_pid[i], PID_POSITION, motor_speed_pid, M3505_MOTOR_SPEED_PID_MAX_OUT, M3505_MOTOR_SPEED_PID_MAX_IOUT);
    }

    //��ʼ����תPID
    PID_Init(&chassis_move.chassis_rotation_pid, PID_POSITION, chassis_rotation_pid, CHASSIS_ROTATION_PID_MAX_OUT, CHASSIS_ROTATION_PID_MAX_IOUT);

    PID_Init(&chassis_move.chassis_angle_pid, PID_POSITION, chassis_angle_pid, CHASSIS_ANGLE_PID_MAX_OUT, CHASSIS_ANGLE_PID_MAX_IOUT);
}




