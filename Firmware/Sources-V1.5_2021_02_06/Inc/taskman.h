// ----------------------------------------------------------------------------
/*!
 * @file		taskman.h
 * @author    	John Steggall
 * @date       	19 March 2021
 * @brief       Header file for taskman.c
 * @note        Please refer to the .c file for a detailed description.
 *
 */
// ----------------------------------------------------------------------------

#ifndef TASKLOOP_H_
#define TASKLOOP_H_

typedef enum
{
	TASKMAN_RUNSTATE_NORMAL = 0u,
	TASKMAN_RUNSTATE_SLEEP = 1u,
	TASKMAN_RUNSTATE_LOW_POWER = 2u,
} TASKMAN_RunState_t;

void TASKMAN_Init(void);
void TASKMAN_Run(void);

TASKMAN_RunState_t TASKMAN_GetRunState(void);
bool TASKMAN_GetIOWakeEvent(void);
void TASKMAN_ClearIOWakeEvent(void);

#endif /* TASKLOOP_H_ */
