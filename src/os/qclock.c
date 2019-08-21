#include "qclock.h"

static volatile qClock_t _qSysTick_Epochs_ = 0ul;
static qGetTickFcn_t GetSysTick = NULL;

#if (Q_SETUP_TIME_CANONICAL != 1)

static qTimingBase_type TimmingBase;

/*============================================================================*/
void qClock_SetTimeBase( qTimingBase_type tb ){
    TimmingBase = tb;
} 
#endif
/*============================================================================*/
void qClock_SetTickProvider( qGetTickFcn_t provider ){
    GetSysTick = provider;
}
/*============================================================================*/
/*qTime_t qClock2Time(const qClock_t t)

Convert the specified input time(epochs) to time(seconds)

Parameters:

    - t : time in epochs   

Return value:

    time (t) in seconds
*/
qTime_t qClock2Time( const qClock_t t ){
    #if ( Q_SETUP_TIME_CANONICAL == 1 )
        return (qTime_t)t;
    #else
        #if ( Q_SETUP_TICK_IN_HERTZ == 1 )
            return (qTime_t)(t/TimmingBase);
        #else
            return (qTime_t)(TimmingBase*((qTime_t)t));
        #endif      
    #endif      
}
/*============================================================================*/
/*qCLock_t qTime2Clock(const qTime_t t)

Convert the specified input time(seconds) to time(epochs)

Parameters:

    - t : time in seconds   

Return value:

    time (t) in epochs
*/
qClock_t qTime2Clock( const qTime_t t ){
    #if ( Q_SETUP_TIME_CANONICAL == 1 )
        return (qClock_t)t;
    #else 
        #if ( Q_SETUP_TICK_IN_HERTZ == 1 )
            return (qClock_t)(t*QUARKTS.TimmingBase);
        #else
            qTime_t epochs;
            epochs = t/TimmingBase;
            return (qClock_t)epochs;
        #endif    
    #endif
}
/*============================================================================*/
/*
void qSchedulerSysTick(void)

Feed the scheduler system tick. If TickProviderFcn is not provided in qSchedulerSetup, this 
call is mandatory and must be called once inside the dedicated timer interrupt service routine (ISR). 
*/    
void qSchedulerSysTick( void ){ 
    _qSysTick_Epochs_++; 
}
/*============================================================================*/
/*qClock_t qSchedulerGetTick( void )

Return the current tick used by the scheduler

Parameters:

    - t : time in epochs   

Return value:

    time (t) in seconds
*/
qClock_t qSchedulerGetTick( void ){   
    qGetTickFcn_t TickProvider;
    TickProvider = GetSysTick;
	return ( NULL != TickProvider )? TickProvider() : _qSysTick_Epochs_; /*some compilers can deal with function pointers inside structs*/
}
/*============================================================================*/
qBool_t qClock_TimeDeadlineCheck( qClock_t ti, qClock_t td ){
    qBool_t RetValue = qFalse;
    if( ( qSchedulerGetTick() - ti ) >= td ){
        RetValue = qTrue;
    }
    return RetValue; 
}
/*============================================================================*/