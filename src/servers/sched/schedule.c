/* This file contains the scheduling policy for SCHED
 *
 * The entry points are:
 *   do_noquantum:        Called on behalf of process' that run out of quantum
 *   do_start_scheduling  Request to start scheduling a proc
 *   do_stop_scheduling   Request to stop scheduling a proc
 *   do_nice		  Request to change the nice level on a proc
 *   init_scheduling      Called from main.c to set up/prepare scheduling
 */
#include "sched.h"
#include "schedproc.h"
#include <assert.h>
#include <minix/com.h>
#include <machine/archtypes.h>
//no idea include for what
#include <minix/syslib.h>
//the .h for sef_receive_status
#include <minix/sef.h>
//the .h for PRIO_MAX & PRIO_MIN 
#include <sys/resource.h>


#include "kernel/proc.h" /* for queue constants */

static timer_t sched_timer;
static unsigned balance_timeout;

#define BALANCE_TIMEOUT	5 /* how often to balance queues in seconds */

static int schedule_process(struct schedproc * rmp, unsigned flags);
static void balance_queues(struct timer *tp);

#define SCHEDULE_CHANGE_PRIO	0x1
#define SCHEDULE_CHANGE_QUANTUM	0x2
#define SCHEDULE_CHANGE_CPU	0x4

#define SCHEDULE_CHANGE_ALL	(	\
		SCHEDULE_CHANGE_PRIO	|	\
		SCHEDULE_CHANGE_QUANTUM	|	\
		SCHEDULE_CHANGE_CPU		\
		)

#define schedule_process_local(p)	\
	schedule_process(p, SCHEDULE_CHANGE_PRIO | SCHEDULE_CHANGE_QUANTUM)
#define schedule_process_migrate(p)	\
	schedule_process(p, SCHEDULE_CHANGE_CPU)

#define CPU_DEAD	-1

#define cpu_is_available(c)	(cpu_proc[c] >= 0)

#define DEFAULT_USER_TIME_SLICE 200

//A micro for test if x in range of priority
#define PROCESS_IN_USER_Q(x) ((x)->priority >= MAX_USER_Q && \
		(x)->priority <= MIN_USER_Q)

/* processes created by RS are sysytem processes */
#define is_system_proc(p)	((p)->parent == RS_PROC_NR)

static unsigned cpu_proc[CONFIG_MAX_CPUS];

static void pick_cpu(struct schedproc * proc)
{
#ifdef CONFIG_SMP
	unsigned cpu, c;
	unsigned cpu_load = (unsigned) -1;
	
	if (machine.processors_count == 1) {
		proc->cpu = machine.bsp_id;
		return;
	}

	/* schedule sysytem processes only on the boot cpu */
	if (is_system_proc(proc)) {
		proc->cpu = machine.bsp_id;
		return;
	}

	/* if no other cpu available, try BSP */
	cpu = machine.bsp_id;
	for (c = 0; c < machine.processors_count; c++) {
		/* skip dead cpus */
		if (!cpu_is_available(c))
			continue;
		if (c != machine.bsp_id && cpu_load > cpu_proc[c]) {
			cpu_load = cpu_proc[c];
			cpu = c;
		}
	}
	proc->cpu = cpu;
	cpu_proc[cpu]++;
#else
	proc->cpu = 0;
#endif
}

/*==========================================================================*
  *				do_lottery				     *
  *===========================================================================*/
int do_lottery()
{
    struct schedproc *rmp;
	int proc_nr;
	int rv;
	int lucky;
 	int old_priority;
 	int flag = -1;
 	int nTickets = 0;

 	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
 		if ((rmp->flags & IN_USE) && PROCESS_IN_USER_Q(rmp)) {
 			if (USER_Q == rmp->priority) {
 				nTickets += rmp->ticketsNum;
 			}
 		}
 	}

 	lucky = nTickets ? random() % nTickets : 0;
 	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
 		if ((rmp->flags & IN_USE) && PROCESS_IN_USER_Q(rmp) &&
 				USER_Q == rmp->priority) {
 			old_priority = rmp->priority;
 			/* rmp->priority = USER_Q; */
 			if (lucky >= 0) {
 				lucky -= rmp->ticketsNum;
 				/*
 				   printf("lucky - %d = %d\n", rmp->ticketsNum, lucky);
 				 */
 				if (lucky < 0) {
 					rmp->priority = MAX_USER_Q;
 					flag = OK;
 					/* printf("endpoint %d\n", rmp->endpoint); */
 				}
 			}
 			if (old_priority != rmp->priority) {
                schedule_process_local(rmp);//if the priority is changed, then we need to call schedule_process to update the priority
 			}
 		}
 	}
 	/*
 	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
 		if ((rmp->flags & IN_USE) && PROCESS_IN_USER_Q(rmp)) {
 			if (USER_Q == rmp->priority)
 				count_17++;
 			else if (MAX_USER_Q == rmp->priority)
 				count_16++;
 		}
 	}
 	printf("in 16: %d; in 17: %d\n", count_16, count_17);
 	*/
 	/* printf("do_lottery OK? %d lucky=%d\n", flag, lucky); */
 	return nTickets ? flag : OK;
}



/*===========================================================================*
 *				do_noquantum				     *
 *===========================================================================*/

int do_noquantum(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n;

	if (sched_isokendpt(m_ptr->m_source, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg %u.\n",
		m_ptr->m_source);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
    //add else condition
 	if (PROCESS_IN_USER_Q(rmp)) {
 		rmp->priority = USER_Q;
 	} else if (rmp->priority < MAX_USER_Q - 1)
    {
 		rmp->priority = rmp->priority + 1;
 	}

	if ((rv = schedule_process_local(rmp)) != OK) {
		return rv;
	}

    //call do_lottery() after each finish each quantum
 	if ((rv = do_lottery()) != OK) {
 		return rv;
 	}

	return OK;
}

/*===========================================================================*
 *				do_stop_scheduling			     *
 *===========================================================================*/
int do_stop_scheduling(message *m_ptr)
{
	register struct schedproc *rmp;
	int proc_nr_n,rv;

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
		"%ld\n", m_ptr->SCHEDULING_ENDPOINT);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
#ifdef CONFIG_SMP
	cpu_proc[rmp->cpu]--;
#endif
	rmp->flags = 0; /*&= ~IN_USE;*/

    //call do_lottery after each scheduling
    //why return do_lottery result, and why do_loterry return flag when success
    if ((rv = do_lottery()) != OK) {
        return rv;
    }

	return OK;
}

/*===========================================================================*
 *				do_start_scheduling			     *
 *===========================================================================*/
int do_start_scheduling(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n, parent_nr_n;
	
	/* we can handle two kinds of messages here */
	assert(m_ptr->m_type == SCHEDULING_START || 
		m_ptr->m_type == SCHEDULING_INHERIT);

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	/* Resolve endpoint to proc slot. */
	if ((rv = sched_isemtyendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n))
			!= OK) {
		return rv;
	}
	rmp = &schedproc[proc_nr_n];

	/* Populate process slot */
	rmp->endpoint     = m_ptr->SCHEDULING_ENDPOINT;
	rmp->parent       = m_ptr->SCHEDULING_PARENT;
	rmp->max_priority = (unsigned) m_ptr->SCHEDULING_MAXPRIO;   //3.21 has this line, attention, this line is not the same as 3.17, it replace the function of rmp->nice
	rmp->ticketsNum   = 40-rmp->max_priority;//actually we modified shcedule.c in pm so here MAXPRIO has number of nice instead of prio_queue, smaller nice has more tickets
	//(rmp->max_priority - USER_Q) * (PRIO_MAX-PRIO_MIN+1) / (MIN_USER_Q-MAX_USER_Q+1) + PRIO_MAX-PRIO_MIN+1;//give tickets
	printf("Endpoint %s process has tickets: %d\n",rmp->endpoint,rmp->ticketsNum);
	//initial tickets a proc has, we set it to be the number of priority
	//after transfer ticketNum=nice, set max_prio to normal
	rmp->max_priority = MAX_USER_Q + (rmp->ticketsNum-PRIO_MIN) * (MIN_USER_Q-MAX_USER_Q+1) / (PRIO_MAX-PRIO_MIN+1);

    if (rmp->max_priority >= NR_SCHED_QUEUES) {
		return EINVAL;
	}

	/* Inherit current priority and time slice from parent. Since there
	 * is currently only one scheduler scheduling the whole system, this
	 * value is local and we assert that the parent endpoint is valid */
	if (rmp->endpoint == rmp->parent) {
		/* We have a special case here for init, which is the first
		   process scheduled, and the parent of itself. */
		rmp->priority   = USER_Q;
		rmp->time_slice = DEFAULT_USER_TIME_SLICE;

		/*
		 * Since kernel never changes the cpu of a process, all are
		 * started on the BSP and the userspace scheduling hasn't
		 * changed that yet either, we can be sure that BSP is the
		 * processor where the processes run now.
		 */
#ifdef CONFIG_SMP
		rmp->cpu = machine.bsp_id;
		/* FIXME set the cpu mask */
#endif
	}
	
	switch (m_ptr->m_type) {

	case SCHEDULING_START:
		/* We have a special case here for system processes, for which
		 * quanum and priority are set explicitly rather than inherited 
		 * from the parent */
		rmp->priority   = rmp->max_priority;
		rmp->time_slice = (unsigned) m_ptr->SCHEDULING_QUANTUM;
		break;
		
	case SCHEDULING_INHERIT:
		/* Inherit current priority and time slice from parent. Since there
		 * is currently only one scheduler scheduling the whole system, this
		 * value is local and we assert that the parent endpoint is valid */
		if ((rv = sched_isokendpt(m_ptr->SCHEDULING_PARENT,
				&parent_nr_n)) != OK)
			return rv;

        if (is_system_proc(rmp))
		    rmp->priority = schedproc[parent_nr_n].priority;
        //set default priority of current process to be the middle of all new added priority queues
            else
            rmp->priority = USER_Q;

		rmp->time_slice = schedproc[parent_nr_n].time_slice;
		break;
		
	default: 
		/* not reachable */
		assert(0);
	}

	/* Take over scheduling the process. The kernel reply message populates
	 * the processes current priority and its time slice */
	if ((rv = sys_schedctl(0, rmp->endpoint, 0, 0, 0)) != OK) {
		printf("Sched: Error taking over scheduling for %d, kernel said %d\n",
			rmp->endpoint, rv);
		return rv;
	}
	rmp->flags = IN_USE;

	/* Schedule the process, giving it some quantum */
	pick_cpu(rmp);
	while ((rv = schedule_process(rmp, SCHEDULE_CHANGE_ALL)) == EBADCPU) {
		/* don't try this CPU ever again */
		cpu_proc[rmp->cpu] = CPU_DEAD;
		pick_cpu(rmp);
	}

	if (rv != OK) {
		printf("Sched: Error while scheduling process, kernel replied %d\n",
			rv);
		return rv;
	}

	/* Mark ourselves as the new scheduler.
	 * By default, processes are scheduled by the parents scheduler. In case
	 * this scheduler would want to delegate scheduling to another
	 * scheduler, it could do so and then write the endpoint of that
	 * scheduler into SCHEDULING_SCHEDULER
	 */

	m_ptr->SCHEDULING_SCHEDULER = SCHED_PROC_NR;

	return OK;
}

/*===========================================================================*
 *				do_nice					     *
 *===========================================================================*/
int do_nice(message *m_ptr)
{
	struct schedproc *rmp;
	int rv;
	int proc_nr_n;
	unsigned new_q, old_q, old_max_q;
    int old_ticketsNum;

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
		"%ld\n", m_ptr->SCHEDULING_ENDPOINT);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
	new_q = (unsigned) m_ptr->SCHEDULING_MAXPRIO;
    //unsure should or not comment this if
    /*
    if (new_q >= NR_SCHED_QUEUES) {
		return EINVAL;
	}
    */
	/* Store old values, in case we need to roll back the changes */
	old_q     = rmp->priority;
	old_max_q = rmp->max_priority;

    old_ticketsNum = rmp->ticketsNum;

	/* Update the proc entry and reschedule the process */

	//rmp->max_priority = rmp->priority = new_q;


	if ((rv = schedule_process_local(rmp)) != OK) {
		/* Something went wrong when rescheduling the process, roll
		 * back the changes to proc struct */
		rmp->priority     = old_q;
		rmp->max_priority = old_max_q;
        //add a line
        rmp->ticketsNum   = old_ticketsNum;
	}
    /* printf("result of do_nice %d\n", rmp->ticketsNum); */

    //do_lottery instead
    return do_lottery();
	//return rv;
}

/*===========================================================================*
 *				schedule_process			     *
 *===========================================================================*/
static int schedule_process(struct schedproc * rmp, unsigned flags)
{
	int err;
	int new_prio, new_quantum, new_cpu;

	pick_cpu(rmp);

	if (flags & SCHEDULE_CHANGE_PRIO)
		new_prio = rmp->priority;
	else
		new_prio = -1;

	if (flags & SCHEDULE_CHANGE_QUANTUM)
		new_quantum = rmp->time_slice;
	else
		new_quantum = -1;

	if (flags & SCHEDULE_CHANGE_CPU)
		new_cpu = rmp->cpu;
	else
		new_cpu = -1;

	if ((err = sys_schedule(rmp->endpoint, new_prio,
		new_quantum, new_cpu)) != OK) {
		printf("PM: An error occurred when trying to schedule %d: %d\n",
		rmp->endpoint, err);
	}

	return err;
}


/*===========================================================================*
 *				start_scheduling			     *
 *===========================================================================*/

void init_scheduling(void)
{
    //add r
    u64_t r;

	balance_timeout = BALANCE_TIMEOUT * sys_hz();
	init_timer(&sched_timer);
	set_timer(&sched_timer, balance_timeout, balance_queues, 0);

    //add the following
    read_tsc_64(&r);
    srandom();
}

/*===========================================================================*
 *				balance_queues				     *
 *===========================================================================*/

/* This function in called every 100 ticks to rebalance the queues. The current
 * scheduler bumps processes down one priority when ever they run out of
 * quantum. This function will find all proccesses that have been bumped down,
 * and pulls them back up. This default policy will soon be changed.
 */
static void balance_queues(struct timer *tp)
{
	struct schedproc *rmp;
	int proc_nr;

	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
		if (rmp->flags & IN_USE) {
            if (rmp->priority > rmp->max_priority &&
                !PROCESS_IN_USER_Q(rmp))
            {
				rmp->priority -= 1; /* increase priority */
				schedule_process_local(rmp);
			}
		}
	}

	set_timer(&sched_timer, balance_timeout, balance_queues, 0);
}
