#ifdef _CORE_

#define MAX_TIMEOUT (1000*3600*24-1)

enum{
	wheel_sec = 0,  
	wheel_hour,     
	wheel_day,      
};


typedef struct {
	int8_t       type;
	int16_t      cur;
	chk_dlist    tlist[0]; 
}wheel;

struct chk_timermgr {
	wheel 		*wheels[wheel_day+1];
	uint64_t    *ptrtick;
	uint64_t     lasttick;
};

#endif