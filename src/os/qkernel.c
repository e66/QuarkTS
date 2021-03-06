#include "qkernel.h"

#define _QKERNEL_BIT_INIT          ( 0x00000001uL )  
#define _QKERNEL_BIT_FCALLIDLE     ( 0x00000002uL )
#define _QKERNEL_BIT_RELEASESCHED  ( 0x00000004uL )
#define _QKERNEL_BIT_FCALLRELEASED ( 0x00000008uL )

#define _QKERNEL_COREFLAG_SET(FLAG, BIT)       ( FLAG ) |= (qCoreFlags_t)(  BIT )     
#define _QKERNEL_COREFLAG_CLEAR(FLAG, BIT)     ( FLAG ) &= (qCoreFlags_t)( ~BIT ) 
#define _QKERNEL_COREFLAG_GET(FLAG, BIT)       ( ( 0uL != (( FLAG ) & ( BIT )) )? qTrue : qFalse )

/*an item of the priority-queue*/
typedef struct{
    qTask_t *Task;      /*< A pointer to the task. */
    void *QueueData;    /*< The data to queue. */
}qQueueStack_t;  

typedef qUINT32_t qCoreFlags_t;

typedef struct{
    qTask_NotifyMode_t mode;
    void *eventdata;
}qNotificationSpreader_t;

typedef struct{ /*KCB(Kernel Control Block) definition*/
    qList_t CoreLists[ Q_PRIORITY_LEVELS + 2 ];
    qTaskFcn_t IDLECallback;                            /*< The callback function that represents the idle-task activities. */
    qTask_t *CurrentRunningTask;                        /*< Points to the current running task. */    
    #if ( Q_ALLOW_SCHEDULER_RELEASE == 1 )
        qTaskFcn_t ReleaseSchedCallback;                /*< The callback function for the scheduler release action. */
    #endif    
    #if ( Q_PRIO_QUEUE_SIZE > 0 ) 
        void *QueueData;                                /*< Hold temporarily one item-data of the FIFO queue.*/
        qQueueStack_t QueueStack[ Q_PRIO_QUEUE_SIZE ];  /*< The required stack to build the FIFO priority queue. */
        volatile qBase_t QueueIndex;                    /*< The current index of the FIFO priority queue. */
    #endif 
    _qEvent_t_ EventInfo;                               /*< Used to hold the event info for a task that will be changed to the qRunning state.*/
    volatile qCoreFlags_t Flag;                         /*< The scheduler Core-Flags. */
    #if ( Q_NOTIFICATION_SPREADER == 1 )
        qNotificationSpreader_t NotificationSpreadRequest;
    #endif
    #if ( Q_PRESERVE_TASK_ENTRY_ORDER == 1)
        size_t TaskEntries;                             /*< Used to hold the number of task entries*/
    #endif
}qKernelControlBlock_t;

/*=========================== Kernel Control Block ===========================*/
static qKernelControlBlock_t kernel;
static qList_t *WaitingList = &kernel.CoreLists[ Q_PRIORITY_LEVELS ];
static qList_t *SuspendedList = &kernel.CoreLists[ Q_PRIORITY_LEVELS + 1 ];
static qList_t *ReadyList = &kernel.CoreLists[ 0 ];
/*=============================== Private Methods ============================*/
static qTask_t* qOS_Get_TaskRunning( void );
static qBool_t qOS_TaskDeadLineReached( qTask_t * const Task);
static qBool_t qOS_CheckIfReady( void *node, void *arg, qList_WalkStage_t stage );
static qBool_t qOS_Dispatch( void *node, void *arg, qList_WalkStage_t stage );    
static qTask_GlobalState_t qOS_GetTaskGlobalState( const qTask_t * const Task);
static void qOS_DummyTask_Callback( qEvent_t e );
static qTrigger_t qOS_Dispatch_xTask_FillEventInfo( qTask_t *Task );

#define _qAbs( x )    ((((x)<0) && ((x)!=qPeriodic))? -(x) : (x))

#if ( Q_PRIO_QUEUE_SIZE > 0 )  
    static qBool_t qOS_PriorityQueue_Insert(qTask_t * const Task, void *data);
    static size_t qOS_PriorityQueue_GetCount( void );
    static void qOS_PriorityQueue_ClearIndex( qIndex_t IndexToClear );
    static void qOS_PriorityQueue_CleanUp( const qTask_t * task );
    static qBool_t qOS_PriorityQueue_IsTaskInside( const qTask_t * const Task );
    static qTask_t* qOS_PriorityQueue_Get( void );
#endif

#if ( Q_ALLOW_SCHEDULER_RELEASE == 1 )
    static void qOS_TriggerReleaseSchedEvent( void );
#endif

#if ( Q_QUEUES == 1)
    static qTrigger_t qOS_AttachedQueue_CheckEvents( const qTask_t * const Task );
#endif

#if ( Q_ATCLI == 1)
    static void qOS_ATCLI_TaskCallback( qEvent_t  e );
    static void qOS_ATCLI_NotifyFcn( qATCLI_t * const cli );
#endif

#if ( Q_PRESERVE_TASK_ENTRY_ORDER == 1)
    static qBool_t qOS_TaskEntryOrderPreserver(const void *n1, const void *n2);
#endif

/*initialize the private-methods container*/
_qOS_PrivateMethodsContainer_t _qOS_PrivateMethods = {  
    #if ( Q_PRIO_QUEUE_SIZE > 0 )   
        &qOS_PriorityQueue_Insert,
        &qOS_PriorityQueue_IsTaskInside,
        &qOS_PriorityQueue_GetCount,
    #endif
    &qOS_DummyTask_Callback,
    &qOS_GetTaskGlobalState, 
    &qOS_Get_TaskRunning
};

/*========================== QuarkTS Private Macros ==========================*/
static void qOS_DummyTask_Callback( qEvent_t e ){
    (void)e; /*unused*/
}
/*============================================================================*/
/*void qOS_Setup( const qGetTickFcn_t TickProviderFcn, const qTimingBase_type BaseTimming, qTaskFcn_t IdleCallback )
        
Task Scheduler Setup. This function is required and must be called once in 
the application main thread before any task is being added to the OS.

Parameters:

    - TickProviderFcn :  The function that provides the tick value. If the user application 
                        uses the qClock_SysTick() from the ISR, this parameter can be NULL.
                        Note: Function should take void and return a 32bit value. 

    - BaseTimming (Optional) : This parameter specifies the ISR background timer base time.
                    This can be the period in seconds(Floating-point format) or frequency 
                    in Herzt(Only if Q_SETUP_TICK_IN_HERTZ is enabled).

    - IdleCallback : Callback function to the Idle Task. To disable the 
                    Idle Task activities, pass NULL as argument.

*/
#if (Q_SETUP_TIME_CANONICAL == 1)
    void qOS_Setup( const qGetTickFcn_t TickProvider, qTaskFcn_t IdleCallback ){
#else
    void qOS_Setup( const qGetTickFcn_t TickProvider, const qTimingBase_t BaseTimming, qTaskFcn_t IdleCallback ){
#endif
    qIndex_t i;
    qList_Initialize( SuspendedList );
    qList_Initialize( WaitingList );
    for( i = (qIndex_t)0; i< (qIndex_t)Q_PRIORITY_LEVELS; i++ ){
        qList_Initialize( &ReadyList[ i ] );
    }
    #if ( Q_SETUP_TIME_CANONICAL != 1 )
        qClock_SetTimeBase( BaseTimming );
    #endif
    kernel.IDLECallback = IdleCallback;
    #if ( Q_PRIO_QUEUE_SIZE > 0 )    
        /*init the priority queue*/
        for( i = 0u ; i < (qIndex_t)Q_PRIO_QUEUE_SIZE ; i++){
            kernel.QueueStack[i].Task = NULL;  /*set the priority queue as empty*/  
        }
        kernel.QueueIndex = -1;     
        kernel.QueueData = NULL;
    #endif
    #if ( Q_NOTIFICATION_SPREADER == 1 )
        kernel.NotificationSpreadRequest.mode = NULL;
        kernel.NotificationSpreadRequest.eventdata = NULL;
    #endif    
    kernel.Flag = 0uL; /*clear all the core flags*/
    #if ( Q_ALLOW_SCHEDULER_RELEASE == 1 )
        kernel.ReleaseSchedCallback = NULL;
    #endif
    #if ( Q_PRESERVE_TASK_ENTRY_ORDER == 1)
        kernel.TaskEntries = (size_t)0;
    #endif
    kernel.CurrentRunningTask = NULL;
    qClock_SetTickProvider( TickProvider );
}
/*============================================================================*/
static qTask_t* qOS_Get_TaskRunning( void ){
    return kernel.CurrentRunningTask; /*get the handle of the current running task*/
}
/*============================================================================*/
/*void qOS_Set_IdleTask( qTaskFcn_t Callback )

Establish the IDLE Task Callback

Parameters:

    - IDLE_Callback : A pointer to a void callback method with a qEvent_t 
                      parameter as input argument.
*/
void qOS_Set_IdleTask( qTaskFcn_t Callback ){
    kernel.IDLECallback = Callback;
}
#if ( Q_ALLOW_SCHEDULER_RELEASE == 1 )
/*============================================================================*/
/*void qOS_Scheduler_Release( void )

Disables the kernel scheduling. The main thread will continue after the
qOS_Run() call.
*/
void qOS_Scheduler_Release( void ){
    _QKERNEL_COREFLAG_SET( kernel.Flag, _QKERNEL_BIT_RELEASESCHED );
}
/*============================================================================*/
/*void qOS_Set_SchedulerReleaseCallback( qTaskFcn_t Callback )

Set/Change the scheduler release callback function

Parameters:

    - Callback : A pointer to a void callback method with a qEvent_t parameter 
                 as input argument.
*/
void qOS_Set_SchedulerReleaseCallback( qTaskFcn_t Callback ){
    kernel.ReleaseSchedCallback = Callback;
}
#endif /* #if ( Q_ALLOW_SCHEDULER_RELEASE == 1 ) */
/*============================================================================*/
/*qBool_t qOS_Notification_Spread( const void *eventdata, const qTaskNotifyMode_t mode)

Try to spread a notification among all the tasks in the scheduling scheme
Note: Operation will be performed in the next scheduling cycle. 

Parameters:

    - eventdata : Specific event user-data.
    - mode : the method used to spread the event:
              Q_NOTIFY_SIMPLE or Q_NOTIFY_QUEUED.

Return value:

    qTrue if success. Otherwise qFalse.              
*/ 
/*============================================================================*/
qBool_t qOS_Notification_Spread( void *eventdata, const qTask_NotifyMode_t mode ){
    qBool_t RetValue = qFalse;
    #if ( Q_NOTIFICATION_SPREADER == 1 )
        if( ( mode ==  Q_NOTIFY_SIMPLE ) || ( mode == Q_NOTIFY_QUEUED ) ){
            kernel.NotificationSpreadRequest.mode = mode;
            kernel.NotificationSpreadRequest.eventdata = eventdata;
            RetValue = qTrue;
        }
    #endif
    return RetValue;    
}
/*============================================================================*/
#if ( Q_PRIO_QUEUE_SIZE > 0 )  
static void qOS_PriorityQueue_CleanUp( const qTask_t * task ){
    qIndex_t i;
    for( i = 1u ; ( i < (qIndex_t)Q_PRIO_QUEUE_SIZE ) ; i++){ 
        if( kernel.QueueStack[ i ].Task == task ){
            qOS_PriorityQueue_ClearIndex( i );
        }
    }
}
/*============================================================================*/
static void qOS_PriorityQueue_ClearIndex( qIndex_t IndexToClear ){
    qIndex_t j;
    qBase_t QueueIndex;

    kernel.QueueStack[IndexToClear].Task = NULL; /*set the position in the queue as empty*/  
    QueueIndex = (qBase_t)kernel.QueueIndex; /*to avoid side effects*/
    for( j = IndexToClear ; (qBase_t)j < QueueIndex ; j++){ 
        kernel.QueueStack[j] = kernel.QueueStack[ j + (qIndex_t)1 ]; /*shift the remaining items of the queue*/
    }
    kernel.QueueIndex--;    /*decrease the index*/    
}
/*============================================================================*/
static qBool_t qOS_PriorityQueue_Insert( qTask_t * const Task, void *data ){
    #if ( Q_PRIO_QUEUE_SIZE > 0 )  
        qBool_t RetValue = qFalse;
        qQueueStack_t tmp;
        qBase_t QueueMaxIndex;
        qBase_t CurrentQueueIndex;
        QueueMaxIndex = Q_PRIO_QUEUE_SIZE - 1; /*to avoid side effects */
        CurrentQueueIndex = kernel.QueueIndex; /*to avoid side effects */
        if( ( NULL != Task )  && ( CurrentQueueIndex < QueueMaxIndex) ) {/*check if data can be queued*/
            tmp.QueueData = data;
            tmp.Task = Task;
            /*cstat -CERT-INT32-C_a*/
            kernel.QueueStack[ ++kernel.QueueIndex ] = tmp; /*insert task and the corresponding eventdata to the queue*/ /*CERT-INT32-C_a checked programatically*/
            /*cstat +CERT-INT32-C_a*/
            RetValue = qTrue;
        }
        return RetValue;
    #else
        return qFalse;
    #endif   
}
/*============================================================================*/
static qBool_t qOS_PriorityQueue_IsTaskInside( const qTask_t * const Task ){
    #if ( Q_PRIO_QUEUE_SIZE > 0 )
        qBool_t RetValue = qFalse;
        qBase_t CurrentQueueIndex, i;
        CurrentQueueIndex = kernel.QueueIndex + 1;
        if( CurrentQueueIndex > 0 ){ /*check first if the queue has items inside*/
            qCritical_Enter();
            for( i = 0 ; i < CurrentQueueIndex; i++ ){ /*loop the queue slots to check if the Task is inside*/
                if( Task == kernel.QueueStack[i].Task ){
                    RetValue = qTrue;
                    break;
                }
            }
            qCritical_Exit();
        }
        return RetValue;
    #else
        return qFalse;
    #endif   
}
/*============================================================================*/
static qTask_t* qOS_PriorityQueue_Get( void ){
    qTask_t *xTask = NULL;
    qIndex_t i;
    qIndex_t IndexTaskToExtract = 0u;
    qPriority_t MaxPriorityValue, iPriorityValue;
    
    if( kernel.QueueIndex >= 0 ){ /*queue has elements*/
        qCritical_Enter();
        MaxPriorityValue = kernel.QueueStack[0].Task->qPrivate.Priority;
        for( i = 1u ; ( i < (qIndex_t)Q_PRIO_QUEUE_SIZE ) ; i++){  /*walk through the queue to find the task with the highest priority*/
            if( NULL == kernel.QueueStack[i].Task ){ /* tail is reached */
                break;
            }
            iPriorityValue = kernel.QueueStack[i].Task->qPrivate.Priority;
            if( iPriorityValue > MaxPriorityValue ){ /*check if the queued task has the max priority value*/
                MaxPriorityValue = iPriorityValue; /*Reassign the max value*/
                IndexTaskToExtract = i;  /*save the index*/
            }
        }   
        kernel.QueueData = kernel.QueueStack[IndexTaskToExtract].QueueData; /*get the data from the queue*/
        xTask = kernel.QueueStack[IndexTaskToExtract].Task; /*assign the task to the output*/
        qOS_PriorityQueue_ClearIndex( IndexTaskToExtract );
        qCritical_Exit();
    }
    return xTask;
}
/*============================================================================*/
static size_t qOS_PriorityQueue_GetCount( void ){
    size_t RetValue = (size_t)0;
    if( kernel.QueueIndex >= 0 ){ 
        RetValue = (size_t)kernel.QueueIndex + (size_t)1;
    }
    return RetValue;
}
/*============================================================================*/
#endif /* #if ( Q_PRIORITY_QUEUE == 1 ) */
/*============================================================================*/
/*qBool_t qOS_Add_Task( qTask_t * const Task, qTaskFcn_t CallbackFcn, qPriority_t Priority, qTime_t Time, qIteration_t nExecutions, qState_t InitialState, void* arg )

Add a task to the scheduling scheme. The task is scheduled to run every <Time> 
seconds, <nExecutions> times and executing <CallbackFcn> method on every pass.

Parameters:
    - Task : A pointer to the task node.
    - CallbackFcn : A pointer to a void callback method with a qEvent_t parameter 
                 as input argument.
    - Priority : Task priority Value. [0(min) - Q_PRIORITY_LEVELS(max)]
    - Time : Execution interval defined in seconds (floating-point format). 
               For immediate execution (tValue = qTimeImmediate).
    - nExecutions : Number of task executions (Integer value). For indefinite 
               execution (nExecutions = qPeriodic or qIndefinite). Tasks do not 
               remember the number of iteration set initially. After the 
               iterations are done, internal iteration counter is 0. To perform 
               another set of iterations, set the number of iterations again.
                >Note 1: Tasks which performed all their iterations put their own 
                        state to qDisabled.
                >Note 2: Asynchronous triggers do not affect the iteration counter.
    - InitialState : Specifies the initial operational state of the task 
                    (qEnabled, qDisabled, qASleep or qAwake(implies qEnabled)).
    - arg : Represents the task arguments. All arguments must be passed by
            reference and cast to (void *). Only one argument is allowed, 
            so, for multiple arguments, create a structure that contains 
            all of the arguments and pass a pointer to that structure.

Return value:

    Returns qTrue on success, otherwise returns qFalse;
*/
qBool_t qOS_Add_Task( qTask_t * const Task, qTaskFcn_t CallbackFcn, qPriority_t Priority, qTime_t Time, qIteration_t nExecutions, qState_t InitialState, void* arg ){
    qBool_t RetValue = qFalse;
    if( ( NULL != Task ) ) {
        Task->qPrivate.Callback = CallbackFcn;
        (void)qSTimer_Set( &Task->qPrivate.timer, Time );
        Task->qPrivate.TaskData = arg;
        Task->qPrivate.Priority = ( Priority > ((qPriority_t)Q_PRIORITY_LEVELS - (qPriority_t)1u) )? 
                                  ( (qPriority_t)Q_PRIORITY_LEVELS - (qPriority_t)1u ) : 
                                  Priority;
        Task->qPrivate.Iterations = ( qPeriodic == nExecutions )? qPeriodic : -nExecutions;    
        Task->qPrivate.Notification = 0uL;
        Task->qPrivate.Trigger = qTriggerNULL;
        _qPrivate_TaskModifyFlags( Task,
                                 _QTASK_BIT_INIT | _QTASK_BIT_QUEUE_RECEIVER | 
                                 _QTASK_BIT_QUEUE_FULL | _QTASK_BIT_QUEUE_COUNT | 
                                 _QTASK_BIT_QUEUE_EMPTY | _QTASK_BIT_REMOVE_REQUEST, 
                                 qFalse);
        _qPrivate_TaskModifyFlags( Task, _QTASK_BIT_SHUTDOWN | _QTASK_BIT_ENABLED, qTrue );  /*task will be awaken and enabled*/ 
        qTask_Set_State( Task, InitialState );

        #if ( Q_TASK_COUNT_CYCLES == 1 )
            Task->qPrivate.Cycles = 0uL;
        #endif
        #if ( Q_QUEUES == 1)
            Task->qPrivate.Queue = NULL;
        #endif
        #if ( Q_FSM == 1)
            Task->qPrivate.StateMachine = NULL;
        #endif
        #if ( Q_PRESERVE_TASK_ENTRY_ORDER == 1)
            Task->qPrivate.Entry = kernel.TaskEntries++;
        #endif
        RetValue = qList_Insert( WaitingList, Task, qList_AtBack ); 
    }
    return RetValue;  
}
/*============================================================================*/
/*qBool_t qOS_Add_EventTask( qTask_t * const Task, qTaskFcn_t CallbackFcn, qPriority_t Priority, void* arg )

Add a task to the scheduling scheme.  This API creates a task with qDisabled 
state by default , so this task will be oriented to be executed only, when 
asynchronous events occurs. However, this behavior can be changed in execution
time using qTaskSetTime or qTaskSetIterations.

Parameters:

    - Task : A pointer to the task node.
    - CallbackFcn : A pointer to a void callback method with a qEvent_t parameter
                 as input argument.
    - Priority : Task priority Value. [0(min) - Q_PRIORITY_LEVELS(max)]
    - arg :      Represents the task arguments. All arguments must be passed by
                 reference and cast to (void *). Only one argument is allowed, 
                 so, for multiple arguments, create a structure that contains 
                 all of the arguments and pass a pointer to that structure.
     
Return value:

    Returns qTrue on success, otherwise returns qFalse;
     */
qBool_t qOS_Add_EventTask( qTask_t * const Task, qTaskFcn_t CallbackFcn, qPriority_t Priority, void* arg ){
    return qOS_Add_Task( Task, CallbackFcn, Priority, qTimeImmediate, qSingleShot, qDisabled, arg );
}
/*============================================================================*/
#if ( Q_FSM == 1)
/*qBool_t qOS_Add_StateMachineTask( qTask_t * const Task, qPriority_t Priority, qTime_t Time,
                         qSM_t * const StateMachine, qSM_State_t InitState, 
                         qSM_ExState_t BeforeAnyState, qSM_ExState_t SuccessState,
                         qSM_ExState_t FailureState, qSM_ExState_t UnexpectedState,
                         qState_t InitialTaskState, void *arg )

Add a task to the scheduling scheme running a dedicated state-machine. 
The task is scheduled to run every <Time> seconds in qPeriodic mode. The event info
will be available as a generic pointer inside the <Data> field of the qSM_t pointer
passed as input argument inside every state.

Parameters:
    - Task : A pointer to the task node.
    - Priority : Task priority Value. [0(min) - Q_PRIORITY_LEVELS(max)]
    - Time : Execution interval defined in seconds (floating-point format). 
               For immediate execution (tValue = qTimeImmediate).
    - StateMachine: A pointer to the Finite State-Machine (FSM) object
    - InitState : The first state to be performed. This argument is a pointer 
                  to a callback function, returning qSM_Status_t and with a 
                  qSM_t pointer as input argument.
    - BeforeAnyState : A state called before the normal state machine execution.
                  This argument is a pointer to a callback function,  with a 
                  qSM_t pointer as input argument.
    - SuccessState : State performed after the current state finish with return status 
                     qSM_EXIT_SUCCESS. This argument is a pointer to a callback
                     function with a qSM_t pointer as input argument.
    - FailureState : State performed after the current state finish with return status 
                     qSM_EXIT_FAILURE. This argument is a pointer to a callback
                     function with a qSM_t pointer as input argument.
    - UnexpectedState : State performed after the current state finish with return status
                        value between -32766 and 32767. This argument is a 
                        pointer to a callback function with a qSM_t pointer
                        as input argument.
    - InitialTaskState : Specifies the initial operational state of the task 
                        (qEnabled, qDisabled, qASleep, qAwake).
    - arg : Represents the task arguments. All arguments must be passed by
                     reference and cast to (void *). Only one argument is allowed, 
                     so, for multiple arguments, create a structure that contains 
                     all of the arguments and pass a pointer to that structure.
 
Return value:

    Returns qTrue on success, otherwise returns qFalse;
*/
qBool_t qOS_Add_StateMachineTask( qTask_t * const Task, qPriority_t Priority, qTime_t Time,
                            qSM_t * const StateMachine, qSM_State_t InitState, qSM_SubState_t BeforeAnyState, qSM_SubState_t SuccessState, qSM_SubState_t FailureState, qSM_SubState_t UnexpectedState,
                            qState_t InitialTaskState, void *arg ){
    qBool_t RetValue = qFalse;
    if( ( NULL != StateMachine ) && ( NULL != InitState ) ){
        if ( qTrue == qOS_Add_Task( Task, qOS_DummyTask_Callback, Priority, Time, qPeriodic, InitialTaskState, arg ) ){
            RetValue = qStateMachine_Setup( StateMachine, InitState, SuccessState, FailureState, UnexpectedState, BeforeAnyState );
            Task->qPrivate.StateMachine = StateMachine;
            StateMachine->qPrivate.Owner = Task;
        }
    }
    return RetValue;
}
/*============================================================================*/
/* qBool_t qOS_StateMachineTask_SigCon( qTask_t * const Task )

Improve the state-machine-task responsiveness by connecting the incoming signals 
from a state machine to the task's event flow.

Parameters:
    - Task : A pointer to the task node.

Return value :

    Returns qTrue on success, otherwise returns qFalse;

*/ 
qBool_t qOS_StateMachineTask_SigCon( qTask_t * const Task ){
    qBool_t RetValue = qFalse;
    qSM_t *StateMachine;
    if( NULL != Task){
        StateMachine = Task->qPrivate.StateMachine;
        if( NULL != StateMachine ){ /*signal connection is only possible if the task runs a dedicated state-machine*/
            if( qTrue == qQueue_IsReady( &StateMachine->qPrivate.SignalQueue ) ){ /*check if the state-machine has a properly instantiated signal queue*/
                RetValue = qTask_Attach_Queue( Task, &StateMachine->qPrivate.SignalQueue, qQUEUE_COUNT, 1u ); /*try to perform the queue connection */
            }
        }
    }
    return RetValue;
}
#endif /* #if ( Q_FSM == 1) */
/*============================================================================*/
#if ( Q_ATCLI == 1 )
/*qBool_t qOS_Add_ATCLITask( qTask_t * const Task, qATCLI_t *Parser, qPriority_t Priority )

Add a task to the scheduling scheme running an AT Command Parser. Task will be scheduled
as an event-triggered task. The parser address will be stored in the TaskData storage-Pointer.

Parameters:

    - Task : A pointer to the task node.
    - cli: A pointer to the AT Command Line Inteface instance.
    - Priority : Task priority Value. [0(min) - Q_PRIORITY_LEVELS(max)]

Return value:

    Returns qTrue on success, otherwise returns qFalse;
*/
qBool_t qOS_Add_ATCLITask( qTask_t * const Task, qATCLI_t *cli, qPriority_t Priority ){    
    qBool_t RetValue = qFalse;
    if( NULL != cli ){
        cli->qPrivate.xPublic.UserData = Task;
        cli->qPrivate.xNotifyFcn = &qOS_ATCLI_NotifyFcn;
        RetValue =  qOS_Add_Task( Task, qOS_ATCLI_TaskCallback, Priority, qTimeImmediate, qSingleShot, qDisabled, cli );
    }
    return RetValue;
}
/*============================================================================*/
static void qOS_ATCLI_TaskCallback( qEvent_t  e ){ /*wrapper for the task callback */
    /*cstat -MISRAC2012-Rule-11.5 -CERT-EXP36-C_b*/
    (void)qATCLI_Run( (qATCLI_t*)e->TaskData ); /* MISRAC2012-Rule-11.5,CERT-EXP36-C_b deviation allowed */
    /*cstat +MISRAC2012-Rule-11.5 +CERT-EXP36-C_b*/
}
/*============================================================================*/
static void qOS_ATCLI_NotifyFcn( qATCLI_t * const cli ){
    qTask_t *Task;
    /*cstat -MISRAC2012-Rule-11.5 -CERT-EXP36-C_b*/
    Task = (qTask_t *)cli->qPrivate.xPublic.UserData; /* MISRAC2012-Rule-11.5,CERT-EXP36-C_b deviation allowed */
    /*cstat +MISRAC2012-Rule-11.5 +CERT-EXP36-C_b*/
    (void)qTask_Notification_Queue( Task, NULL );
}
#endif /* #if ( Q_ATCLI == 1) */
/*============================================================================*/
/*qBool_t qOS_Remove_Task( qTask_t * const Task )

Remove the task from the scheduling scheme.

Parameters:

    - Task : A pointer to the task node.
     
Return value:

    Returns qTrue if success, otherwise returns qFalse.;     
    */
qBool_t qOS_Remove_Task( qTask_t * const Task ){
    qBool_t RetValue = qFalse;
    if( NULL != Task ){
        _qPrivate_TaskModifyFlags( Task, _QTASK_BIT_REMOVE_REQUEST, qTrue );
        RetValue = qTrue;
    }
    return RetValue;
}
#if ( Q_QUEUES == 1)
/*============================================================================*/
static qTrigger_t qOS_AttachedQueue_CheckEvents( const qTask_t * const Task ){
    qTrigger_t RetValue = qTriggerNULL;
    qBool_t FullFlag, CountFlag, ReceiverFlag, EmptyFlag;
    qBool_t IsFull, IsEmpty;
    size_t CurrentQueueCount;

    if( NULL != Task->qPrivate.Queue){
        FullFlag = _qPrivate_TaskGetFlag( Task, _QTASK_BIT_QUEUE_FULL );
        CountFlag = _qPrivate_TaskGetFlag( Task, _QTASK_BIT_QUEUE_COUNT );
        ReceiverFlag = _qPrivate_TaskGetFlag( Task, _QTASK_BIT_QUEUE_RECEIVER );
        EmptyFlag = _qPrivate_TaskGetFlag( Task, _QTASK_BIT_QUEUE_EMPTY );
        
        CurrentQueueCount = qQueue_Count( Task->qPrivate.Queue ); /*to avoid side effects*/
        IsFull = qQueue_IsFull( Task->qPrivate.Queue ); /*to avoid side effects*/
        IsEmpty = qQueue_IsEmpty( Task->qPrivate.Queue ); /*to avoid side effects*/
        /*check the queue events in the corresponding precedence order*/
        if( FullFlag && IsFull ){        
            RetValue =  byQueueFull;
        }
        else if( ( CountFlag ) && ( CurrentQueueCount >= Task->qPrivate.QueueCount ) ){
            RetValue =  byQueueCount;
        }
        else if( ReceiverFlag && ( CurrentQueueCount > 0u ) ){    
            RetValue = byQueueReceiver; 
        }
        else if( EmptyFlag && IsEmpty ){
            RetValue =  byQueueEmpty;
        }
        else{
            /*this case does not need to be handled*/
        }
    }
    return RetValue;
}
#endif
/*============================================================================*/
#if ( Q_ALLOW_SCHEDULER_RELEASE == 1 )
static void qOS_TriggerReleaseSchedEvent( void ){
    qTaskFcn_t Callback;
    _QKERNEL_COREFLAG_CLEAR( kernel.Flag, _QKERNEL_BIT_INIT ); 
    _QKERNEL_COREFLAG_CLEAR( kernel.Flag, _QKERNEL_BIT_RELEASESCHED );  
    kernel.EventInfo.FirstCall = ( qFalse == _QKERNEL_COREFLAG_GET( kernel.Flag, _QKERNEL_BIT_FCALLRELEASED ) )? qTrue : qFalse;    
    kernel.EventInfo.Trigger = bySchedulingRelease;
    kernel.EventInfo.TaskData = NULL;
    if( NULL != kernel.ReleaseSchedCallback ){
        Callback = kernel.ReleaseSchedCallback;
        Callback( &kernel.EventInfo ); /*some low-end compilers cant deal with function-pointers inside structs*/
    }    
    _QKERNEL_COREFLAG_SET( kernel.Flag, _QKERNEL_BIT_FCALLIDLE ); /*MISRAC2012-Rule-11.3 allowed*/
}
#endif
/*============================================================================*/
/*void qOS_Run( void )
    
Executes the scheduling scheme. It must be called once after the task
pool has been defined.

  Note : This call keeps the application in an endless loop
*/
void qOS_Run( void ){
    qIndex_t xPriorityListIndex; 
    qList_t *xList;
    do{           
        if( qList_ForEach( WaitingList, qOS_CheckIfReady, NULL, QLIST_FORWARD, NULL ) ){ /*check for ready tasks in the waiting-list*/
            xPriorityListIndex = (qIndex_t)Q_PRIORITY_LEVELS - (qIndex_t)1;
            do{ /*loop every ready-list in descending priority order*/
                xList = &ReadyList[ xPriorityListIndex ]; /*get the target ready-list*/
                if( xList->size > (size_t)0 ){ /*check for a non-empty target list */
                    (void)qList_ForEach( xList, qOS_Dispatch, xList, QLIST_FORWARD, NULL ); /*dispatch every task in the current ready-list*/
                }
            }while( (qIndex_t)0 != xPriorityListIndex-- ); /*move to the next ready-list*/
        }
        else{ /*no task in the scheme is ready*/
            if( NULL != kernel.IDLECallback ){ /*check if the idle-task is available*/
                (void)qOS_Dispatch( NULL, NULL, QLIST_WALKTHROUGH ); /*special call to dispatch idle-task already hardcoded in the kernel*/
            }
        }
        if( SuspendedList->size > (size_t)0 ){  /*check for a non-empty suspended-list*/
            (void)qList_Move( WaitingList, SuspendedList, qList_AtBack ); /*move the remaining suspended tasks to the waiting-list*/
            #if ( Q_PRESERVE_TASK_ENTRY_ORDER == 1)
                (void)qList_Sort( WaitingList, qOS_TaskEntryOrderPreserver ); /*preseve the entry order by sorting the new wainting-list arrangement*/
            #endif
        }
    }
    #if ( Q_ALLOW_SCHEDULER_RELEASE == 1 )
        while( qFalse == _QKERNEL_COREFLAG_GET( kernel.Flag, _QKERNEL_BIT_RELEASESCHED ) ); /*scheduling end-point*/ 
        qOS_TriggerReleaseSchedEvent(); /*check for a scheduling-release request*/
    #else
        while( qTrue == qTrue);
    #endif
}
/*============================================================================*/
#if ( Q_PRESERVE_TASK_ENTRY_ORDER == 1)
static qBool_t qOS_TaskEntryOrderPreserver(const void *n1, const void *n2){
    qTask_t *t1, *t2;
    t1 = (qTask_t*)n1;
    t2 = (qTask_t*)n1;
    return (qBool_t)(t1->qPrivate.Entry > t2->qPrivate.Entry);
}
#endif
/*============================================================================*/
static qBool_t qOS_CheckIfReady( void *node, void *arg, qList_WalkStage_t stage ){
    qTask_t *xTask;
    qList_t *xList;
    qTrigger_t trg;
    static qBool_t xReady = qFalse;
    qBool_t RetValue = qFalse;

    if( QLIST_WALKINIT == stage ){
        xReady = qFalse;
        #if ( Q_PRIO_QUEUE_SIZE > 0 )  
            xTask = qOS_PriorityQueue_Get(); /*try to extract a task from the front of the priority queue*/
            if( NULL != xTask ){  /*if we got a task from the priority queue,*/
                xTask->qPrivate.Trigger = byNotificationQueued; 
                _qPrivate_TaskModifyFlags( xTask, _QTASK_BIT_SHUTDOWN, qTrue ); /*wake-up the task!!*/
            }     
        #endif          
    }
    else if( QLIST_WALKTHROUGH == stage ){
        /*cstat -MISRAC2012-Rule-11.5 -CERT-EXP36-C_b*/
        xTask = (qTask_t*)node; /* MISRAC2012-Rule-11.5,CERT-EXP36-C_b deviation allowed */
        /*cstat +MISRAC2012-Rule-11.5 +CERT-EXP36-C_b*/
        #if ( Q_NOTIFICATION_SPREADER == 1 )
            if( NULL != kernel.NotificationSpreadRequest.mode ){
                (void)kernel.NotificationSpreadRequest.mode( xTask, kernel.NotificationSpreadRequest.eventdata );
                kernel.NotificationSpreadRequest.mode = NULL;
                kernel.NotificationSpreadRequest.eventdata = NULL;
                RetValue = qTrue;
            }
        #endif
        if( _qPrivate_TaskGetFlag( xTask, _QTASK_BIT_SHUTDOWN) ){
            #if ( Q_PRIO_QUEUE_SIZE > 0 )  
            if( byNotificationQueued == xTask->qPrivate.Trigger ){
                xReady = qTrue;
            }
            else
            #endif 
            if( qOS_TaskDeadLineReached( xTask ) ){ /*nested check for timed task, check the first requirement(the task must be enabled)*/
                (void)qSTimer_Reload( &xTask->qPrivate.timer );
                xTask->qPrivate.Trigger = byTimeElapsed;      
                xReady = qTrue;            
            }
            #if ( Q_QUEUES == 1)  
            else if( qTriggerNULL !=  ( trg = qOS_AttachedQueue_CheckEvents( xTask ) ) ){ /*If the deadline is not met, check if there is a queue event available*/
                xTask->qPrivate.Trigger = trg;      
                xReady = qTrue;
            }
            #endif
            else if( xTask->qPrivate.Notification > (qNotifier_t)0 ){   /*last check : task with a pending notification event?*/
                xTask->qPrivate.Trigger = byNotificationSimple;  
                xReady = qTrue;            
            }
            #if ( Q_TASK_EVENT_FLAGS == 1 )
            else if( 0uL != (QTASK_EVENTFLAGS_RMASK & xTask->qPrivate.Flags ) ){
                xTask->qPrivate.Trigger = byEventFlags;          
                xReady = qTrue;        
            }
            #endif
            else{
                xTask->qPrivate.Trigger = qTriggerNULL;
                /*the task has no available events, put it into a suspended state*/        
            }
        }
        (void)qList_Remove( WaitingList, NULL, QLIST_ATFRONT ); 
        if( _qPrivate_TaskGetFlag( xTask, _QTASK_BIT_REMOVE_REQUEST) ){ /*check if the task get a removal request*/
            #if ( Q_PRIO_QUEUE_SIZE > 0 )  
                qCritical_Enter(); 
                qOS_PriorityQueue_CleanUp( xTask ); /*clean any entry of this task from the priority queue */
                qCritical_Exit();
            #endif
            _qPrivate_TaskModifyFlags( xTask, _QTASK_BIT_REMOVE_REQUEST, qFalse );  /*clear the removal request*/
        }
        else{
            xList = ( qTriggerNULL != xTask->qPrivate.Trigger )? &ReadyList[ xTask->qPrivate.Priority ] : SuspendedList;
            (void)qList_Insert( xList, xTask, QLIST_ATBACK );
        }
    }
    else if( QLIST_WALKEND == stage ){ 
        RetValue = xReady; 
    }
    else{
        (void)arg; /*arg is never used*/  /*this should never enter here*/
    }
    return RetValue;
}
/*============================================================================*/
static qTrigger_t qOS_Dispatch_xTask_FillEventInfo( qTask_t *Task ){
    qTrigger_t Event;
    qIteration_t TaskIteration;
    
    Event = Task->qPrivate.Trigger;
    
    switch( Event ){ /*take the necessary actions before dispatching, depending on the event that triggered the task*/
        case byTimeElapsed:
            /*handle the iteration value and the FirstIteration flag*/
            TaskIteration = Task->qPrivate.Iterations;
            kernel.EventInfo.FirstIteration = ( ( TaskIteration != qPeriodic ) && ( TaskIteration < 0 ) )? qTrue : qFalse;
            Task->qPrivate.Iterations = ( kernel.EventInfo.FirstIteration )? -Task->qPrivate.Iterations : Task->qPrivate.Iterations;
            if( qPeriodic != Task->qPrivate.Iterations){
                Task->qPrivate.Iterations--; /*Decrease the iteration value*/
            }
            kernel.EventInfo.LastIteration = (0 == Task->qPrivate.Iterations )? qTrue : qFalse; 
            if( kernel.EventInfo.LastIteration ) {
                _qPrivate_TaskModifyFlags( Task, _QTASK_BIT_ENABLED, qFalse ); /*When the iteration value is reached, the task will be disabled*/ 
            }           
            kernel.EventInfo.StartDelay = qClock_GetTick() - Task->qPrivate.timer.Start;
            break;
        case byNotificationSimple:
            kernel.EventInfo.EventData = Task->qPrivate.AsyncData; /*Transfer async-data to the eventinfo structure*/
            Task->qPrivate.Notification--; /* = qFalse */ /*Clear the async flag*/            
            break;
        #if ( Q_QUEUES == 1)    
            case byQueueReceiver:
                kernel.EventInfo.EventData = qQueue_Peek( Task->qPrivate.Queue ); /*the EventData will point to the queue front-data*/
                break;
            case byQueueFull: case byQueueCount: case byQueueEmpty: 
                kernel.EventInfo.EventData = (void*)Task->qPrivate.Queue;  /*the EventData will point to the the linked queue*/
                break;
        #endif
        #if ( Q_PRIO_QUEUE_SIZE > 0 )  
            case byNotificationQueued:
                kernel.EventInfo.EventData = kernel.QueueData; /*get the extracted data from queue*/
                kernel.QueueData = NULL;
                break;
        #endif
        #if ( Q_TASK_EVENT_FLAGS == 1 )
            case byEventFlags:
                break;
        #endif        
            default: break;
    }     

    /*Fill the event info structure: Trigger, FirstCall and TaskData */       
    kernel.EventInfo.Trigger = Task->qPrivate.Trigger;
    kernel.EventInfo.FirstCall = ( qFalse == _qPrivate_TaskGetFlag( Task, _QTASK_BIT_INIT) )? qTrue : qFalse;
    kernel.EventInfo.TaskData = Task->qPrivate.TaskData;
    kernel.CurrentRunningTask = Task; /*needed for qTask_Self()*/
    return Event;
}
/*============================================================================*/
static qBool_t qOS_Dispatch( void *node, void *arg, qList_WalkStage_t stage ){
    qTrigger_t Event = byNoReadyTasks;
    qTask_t *Task; /*#!ok*/
    qList_t *xList;
    qTaskFcn_t TaskActivities;
    /*cstat -MISRAC2012-Rule-11.5 -MISRAC2012-Rule-14.3_a -MISRAC2012-Rule-14.3_b -CERT-EXP36-C_b*/
    xList = (qList_t*)arg; /* MISRAC2012-Rule-11.5,CERT-EXP36-C_b deviation allowed */
    if( QLIST_WALKTHROUGH == stage ){ /*#!ok*/
        if( NULL != xList){ /*#!ok*/     
            Task = (qTask_t*)node; /* MISRAC2012-Rule-11.5,CERT-EXP36-C_b deviation allowed */
            /*cstat +MISRAC2012-Rule-11.5 +MISRAC2012-Rule-14.3_a +MISRAC2012-Rule-14.3_b +CERT-EXP36-C_b*/   
            Event = qOS_Dispatch_xTask_FillEventInfo( Task );
            TaskActivities = Task->qPrivate.Callback;
            #if ( Q_FSM == 1)
                if ( ( NULL != Task->qPrivate.StateMachine ) && ( qOS_DummyTask_Callback == Task->qPrivate.Callback ) ){
                    (void)qStateMachine_Run( Task->qPrivate.StateMachine, (void*)&kernel.EventInfo );  /*If the task has a FSM attached, just run it*/  
                }
                else if ( NULL != TaskActivities ) {
                    TaskActivities( &kernel.EventInfo ); /*else, just launch the callback function*/ 
                }       
                else{
                    /*this case does not need to be handled*/
                }
            #else
                if ( NULL != TaskActivities ) {
                    TaskActivities( &kernel.EventInfo ); /*else, just launch the callback function*/ 
                }     
            #endif
            kernel.CurrentRunningTask = NULL;
            (void)qList_Remove( xList, NULL, qList_AtFront ); /*remove the task from the ready-list*/
            (void)qList_Insert( WaitingList, Task, QLIST_ATBACK );  /*and insert the task back to the waiting-list*/
            #if ( Q_QUEUES == 1) 
                if( byQueueReceiver == Event ){
                    (void)qQueue_RemoveFront( Task->qPrivate.Queue );  /*remove the data from the Queue, if the event was byQueueDequeue*/
                } 
            #endif
            
            
            _qPrivate_TaskModifyFlags( Task, _QTASK_BIT_INIT, qTrue ); /*set the init flag*/
            kernel.EventInfo.FirstIteration = qFalse;
            kernel.EventInfo.LastIteration =  qFalse; 
            kernel.EventInfo.StartDelay = (qClock_t)0uL;
            kernel.EventInfo.EventData = NULL; /*clear the eventdata*/
            #if ( Q_TASK_COUNT_CYCLES == 1 )
                Task->qPrivate.Cycles++; /*increase the task-cycles value*/
            #endif
            Task->qPrivate.Trigger = qTriggerNULL;
        }
        else{ /*run the idle*/
            kernel.EventInfo.FirstCall = (qFalse == _QKERNEL_COREFLAG_GET( kernel.Flag, _QKERNEL_BIT_FCALLIDLE ) )? qTrue : qFalse;
            kernel.EventInfo.TaskData = NULL;
            kernel.EventInfo.Trigger = Event;
            TaskActivities = kernel.IDLECallback; /*some compilers can´t deal with function pointers inside structs*/
            TaskActivities( &kernel.EventInfo ); /*run the idle callback*/ 
            _QKERNEL_COREFLAG_SET( kernel.Flag, _QKERNEL_BIT_FCALLIDLE );
        }
    }
    return qFalse;
}
/*============================================================================*/
static qBool_t qOS_TaskDeadLineReached( qTask_t * const Task ){
    qBool_t RetValue = qFalse;
    qIteration_t TaskIterations;
    qClock_t TaskInterval;
    qBool_t DeadLineReached;
    
    if( _qPrivate_TaskGetFlag( Task, _QTASK_BIT_ENABLED ) ){ /*nested-check for timed task, check the first requirement(the task must be enabled)*/
        TaskIterations = Task->qPrivate.Iterations; /*avoid side efects in the next check*/
        if( ( _qAbs( TaskIterations ) > 0 ) || ( qPeriodic == TaskIterations ) ){ /*then task should be periodic or must have available iters*/
            TaskInterval = Task->qPrivate.timer.TV;
            DeadLineReached = qSTimer_Expired( &Task->qPrivate.timer );
            if( ( 0uL == TaskInterval ) || DeadLineReached ){ /*finally, check the time deadline*/
                RetValue = qTrue;                
            }
        }
    }
    return RetValue;
}
/*============================================================================*/
static qTask_GlobalState_t qOS_GetTaskGlobalState( const qTask_t * const Task ){
    qTask_GlobalState_t RetValue = qUndefinedGlobalState;
    qList_t *xList;

    if( NULL != Task ){
        /*cstat -MISRAC2012-Rule-11.5 -CERT-EXP36-C_b*/
        xList = Task->qPrivate.container; /* MISRAC2012-Rule-11.5,CERT-EXP36-C_b deviation allowed */
        /*cstat +MISRAC2012-Rule-11.5 +CERT-EXP36-C_b*/
        if( kernel.CurrentRunningTask == Task ){
            RetValue = qRunning;
        }
        else if( WaitingList == xList ){
            RetValue = qWaiting;
        }
        else if( SuspendedList == xList ){
            RetValue = qSuspended;
        }
        else if( NULL == xList ){
            /*undefined*/  
        }
        else{
            RetValue = qReady;      /*by discard, it must be ready*/
        }
    }
    return RetValue;
}
/*============================================================================*/
