#ifndef __FLOW_DIFFUSION_H__
#define __FLOW_DIFFUSION_H__

#define USE_CUDA			1
#define ENTER_GLUT_LOOP		1

#define NR_OF_STREAMLINES	50

#define SCALE_SIZE			5

#define SHOW_COMPUTE_STREAMLINE_TIMING	1

// ADD-BY-LEETEN 01/22/2010-BEGIN
#define KERNEL_SIZE			8
// ADD-BY-LEETEN 01/22/2010-END


#if	0	// DEL-BY-LEETEN 12/07/2009-BEGIN
	#pragma comment (lib, "cutil32.lib ")      // link with my own library libfps
	#pragma comment (lib, "cudart.lib")      // link with my own library libfps
#endif	// DEL-BY-LEETEN 12/07/2009-END
// #if	USE_CUDA
	#include "FlowDiffusionCudaLib/FlowDiffusion.h"
// #endif

#if	0	// DEL-BY-LEETEN 12/07/2009-BEGIN
	#define MALLOC(p, type, size)\
		{	\
			if( NULL == (p) )\
			{\
				(p) = (type*)calloc((size), sizeof(type));\
			}\
		}\


	#define FREE(p)	\
		{	\
			if( NULL != (p) )	\
			{\
				free(p);\
			}\
			p = NULL;\
		}\

#endif	// DEL-BY-LEETEN 12/07/2009-END

#endif	// __FLOW_DIFFUSION_H__

/*

$Log: not supported by cvs2svn $
Revision 1.1.1.1  2009/12/07 20:01:41  leeten

[12/07/2009]
1. [1ST] First time checkin.


*/
