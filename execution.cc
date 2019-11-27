#include <stdio.h>
#include <algorithm>
#include <new>
#include <stdarg.h>

#include "model.h"
#include "execution.h"
#include "action.h"
#include "schedule.h"
#include "common.h"
#include "clockvector.h"
#include "cyclegraph.h"
#include "datarace.h"
#include "threads-model.h"
#include "bugmessage.h"
#include "history.h"
#include "fuzzer.h"
#include "newfuzzer.h"

#define INITIAL_THREAD_ID       0

/**
 * Structure for holding small ModelChecker members that should be snapshotted
 */
struct model_snapshot_members {
	model_snapshot_members() :
		/* First thread created will have id INITIAL_THREAD_ID */
		next_thread_id(INITIAL_THREAD_ID),
		used_sequence_numbers(0),
		bugs(),
		asserted(false)
	{ }

	~model_snapshot_members() {
		for (unsigned int i = 0;i < bugs.size();i++)
			delete bugs[i];
		bugs.clear();
	}

	unsigned int next_thread_id;
	modelclock_t used_sequence_numbers;
	SnapVector<bug_message *> bugs;
	/** @brief Incorrectly-ordered synchronization was made */
	bool asserted;

	SNAPSHOTALLOC
};

/** @brief Constructor */
ModelExecution::ModelExecution(ModelChecker *m, Scheduler *scheduler) :
	model(m),
	params(NULL),
	scheduler(scheduler),
	action_trace(),
	thread_map(2),	/* We'll always need at least 2 threads */
	pthread_map(0),
	pthread_counter(1),
	obj_map(),
	condvar_waiters_map(),
	obj_thrd_map(),
	mutex_map(),
	thrd_last_action(1),
	thrd_last_fence_release(),
	priv(new struct model_snapshot_members ()),
	mo_graph(new CycleGraph()),
	fuzzer(new NewFuzzer()),
	thrd_func_list(),
	thrd_func_act_lists(),
	isfinished(false)
{
	/* Initialize a model-checker thread, for special ModelActions */
	model_thread = new Thread(get_next_id());
	add_thread(model_thread);
	fuzzer->register_engine(m->get_history(), this);
	scheduler->register_engine(this);
#ifdef TLS
	pthread_key_create(&pthreadkey, tlsdestructor);
#endif
}

/** @brief Destructor */
ModelExecution::~ModelExecution()
{
	for (unsigned int i = 0;i < get_num_threads();i++)
		delete get_thread(int_to_id(i));

	delete mo_graph;
	delete priv;
}

int ModelExecution::get_execution_number() const
{
	return model->get_execution_number();
}

static action_list_t * get_safe_ptr_action(HashTable<const void *, action_list_t *, uintptr_t, 2> * hash, void * ptr)
{
	action_list_t *tmp = hash->get(ptr);
	if (tmp == NULL) {
		tmp = new action_list_t();
		hash->put(ptr, tmp);
	}
	return tmp;
}

static SnapVector<action_list_t> * get_safe_ptr_vect_action(HashTable<const void *, SnapVector<action_list_t> *, uintptr_t, 2> * hash, void * ptr)
{
	SnapVector<action_list_t> *tmp = hash->get(ptr);
	if (tmp == NULL) {
		tmp = new SnapVector<action_list_t>();
		hash->put(ptr, tmp);
	}
	return tmp;
}

/** @return a thread ID for a new Thread */
thread_id_t ModelExecution::get_next_id()
{
	return priv->next_thread_id++;
}

/** @return the number of user threads created during this execution */
unsigned int ModelExecution::get_num_threads() const
{
	return priv->next_thread_id;
}

/** @return a sequence number for a new ModelAction */
modelclock_t ModelExecution::get_next_seq_num()
{
	return ++priv->used_sequence_numbers;
}

/** Restore the last used sequence number when actions of a thread are postponed by Fuzzer */
void ModelExecution::restore_last_seq_num()
{
	priv->used_sequence_numbers--;
}

/**
 * @brief Should the current action wake up a given thread?
 *
 * @param curr The current action
 * @param thread The thread that we might wake up
 * @return True, if we should wake up the sleeping thread; false otherwise
 */
bool ModelExecution::should_wake_up(const ModelAction *curr, const Thread *thread) const
{
	const ModelAction *asleep = thread->get_pending();
	/* Don't allow partial RMW to wake anyone up */
	if (curr->is_rmwr())
		return false;
	/* Synchronizing actions may have been backtracked */
	if (asleep->could_synchronize_with(curr))
		return true;
	/* All acquire/release fences and fence-acquire/store-release */
	if (asleep->is_fence() && asleep->is_acquire() && curr->is_release())
		return true;
	/* Fence-release + store can awake load-acquire on the same location */
	if (asleep->is_read() && asleep->is_acquire() && curr->same_var(asleep) && curr->is_write()) {
		ModelAction *fence_release = get_last_fence_release(curr->get_tid());
		if (fence_release && *(get_last_action(thread->get_id())) < *fence_release)
			return true;
	}
	/* The sleep is literally sleeping */
	if (asleep->is_sleep()) {
		if (fuzzer->shouldWake(asleep))
			return true;
	}

	return false;
}

void ModelExecution::wake_up_sleeping_actions(ModelAction *curr)
{
	for (unsigned int i = 0;i < get_num_threads();i++) {
		Thread *thr = get_thread(int_to_id(i));
		if (scheduler->is_sleep_set(thr)) {
			if (should_wake_up(curr, thr)) {
				/* Remove this thread from sleep set */
				scheduler->remove_sleep(thr);
				if (thr->get_pending()->is_sleep())
					thr->set_wakeup_state(true);
			}
		}
	}
}

void ModelExecution::assert_bug(const char *msg)
{
	priv->bugs.push_back(new bug_message(msg));
	set_assert();
}

/** @return True, if any bugs have been reported for this execution */
bool ModelExecution::have_bug_reports() const
{
	return priv->bugs.size() != 0;
}

SnapVector<bug_message *> * ModelExecution::get_bugs() const
{
	return &priv->bugs;
}

/**
 * Check whether the current trace has triggered an assertion which should halt
 * its execution.
 *
 * @return True, if the execution should be aborted; false otherwise
 */
bool ModelExecution::has_asserted() const
{
	return priv->asserted;
}

/**
 * Trigger a trace assertion which should cause this execution to be halted.
 * This can be due to a detected bug or due to an infeasibility that should
 * halt ASAP.
 */
void ModelExecution::set_assert()
{
	priv->asserted = true;
}

/**
 * Check if we are in a deadlock. Should only be called at the end of an
 * execution, although it should not give false positives in the middle of an
 * execution (there should be some ENABLED thread).
 *
 * @return True if program is in a deadlock; false otherwise
 */
bool ModelExecution::is_deadlocked() const
{
	bool blocking_threads = false;
	for (unsigned int i = 0;i < get_num_threads();i++) {
		thread_id_t tid = int_to_id(i);
		if (is_enabled(tid))
			return false;
		Thread *t = get_thread(tid);
		if (!t->is_model_thread() && t->get_pending())
			blocking_threads = true;
	}
	return blocking_threads;
}

/**
 * Check if this is a complete execution. That is, have all thread completed
 * execution (rather than exiting because sleep sets have forced a redundant
 * execution).
 *
 * @return True if the execution is complete.
 */
bool ModelExecution::is_complete_execution() const
{
	for (unsigned int i = 0;i < get_num_threads();i++)
		if (is_enabled(int_to_id(i)))
			return false;
	return true;
}

ModelAction * ModelExecution::convertNonAtomicStore(void * location) {
	uint64_t value = *((const uint64_t *) location);
	modelclock_t storeclock;
	thread_id_t storethread;
	getStoreThreadAndClock(location, &storethread, &storeclock);
	setAtomicStoreFlag(location);
	ModelAction * act = new ModelAction(NONATOMIC_WRITE, memory_order_relaxed, location, value, get_thread(storethread));
	act->set_seq_number(storeclock);
	add_normal_write_to_lists(act);
	add_write_to_lists(act);
	w_modification_order(act);
//	model->get_history()->process_action(act, act->get_tid());
	return act;
}

/**
 * Processes a read model action.
 * @param curr is the read model action to process.
 * @param rf_set is the set of model actions we can possibly read from
 * @return True if processing this read updates the mo_graph.
 */
bool ModelExecution::process_read(ModelAction *curr, SnapVector<ModelAction *> * rf_set)
{
	SnapVector<const ModelAction *> * priorset = new SnapVector<const ModelAction *>();
	bool hasnonatomicstore = hasNonAtomicStore(curr->get_location());
	if (hasnonatomicstore) {
		ModelAction * nonatomicstore = convertNonAtomicStore(curr->get_location());
		rf_set->push_back(nonatomicstore);
	}

	// Remove writes that violate read modification order
	/*
	   for (uint i = 0; i < rf_set->size(); i++) {
	        ModelAction * rf = (*rf_set)[i];
	        if (!r_modification_order(curr, rf, NULL, NULL, true)) {
	                (*rf_set)[i] = rf_set->back();
	                rf_set->pop_back();
	        }
	   }*/

	while(true) {
		int index = fuzzer->selectWrite(curr, rf_set);
		if (index == -1)// no feasible write exists
			return false;

		ModelAction *rf = (*rf_set)[index];

		ASSERT(rf);
		bool canprune = false;
		if (r_modification_order(curr, rf, priorset, &canprune)) {
			for(unsigned int i=0;i<priorset->size();i++) {
				mo_graph->addEdge((*priorset)[i], rf);
			}
			read_from(curr, rf);
			get_thread(curr)->set_return_value(curr->get_return_value());
			delete priorset;
			if (canprune && curr->get_type() == ATOMIC_READ) {
				int tid = id_to_int(curr->get_tid());
				(*obj_thrd_map.get(curr->get_location()))[tid].pop_back();
			}
			return true;
		}
		priorset->clear();
		(*rf_set)[index] = rf_set->back();
		rf_set->pop_back();
	}
}

/**
 * Processes a lock, trylock, or unlock model action.  @param curr is
 * the read model action to process.
 *
 * The try lock operation checks whether the lock is taken.  If not,
 * it falls to the normal lock operation case.  If so, it returns
 * fail.
 *
 * The lock operation has already been checked that it is enabled, so
 * it just grabs the lock and synchronizes with the previous unlock.
 *
 * The unlock operation has to re-enable all of the threads that are
 * waiting on the lock.
 *
 * @return True if synchronization was updated; false otherwise
 */
bool ModelExecution::process_mutex(ModelAction *curr)
{
	cdsc::mutex *mutex = curr->get_mutex();
	struct cdsc::mutex_state *state = NULL;

	if (mutex)
		state = mutex->get_state();

	switch (curr->get_type()) {
	case ATOMIC_TRYLOCK: {
		bool success = !state->locked;
		curr->set_try_lock(success);
		if (!success) {
			get_thread(curr)->set_return_value(0);
			break;
		}
		get_thread(curr)->set_return_value(1);
	}
	//otherwise fall into the lock case
	case ATOMIC_LOCK: {
		//TODO: FIND SOME BETTER WAY TO CHECK LOCK INITIALIZED OR NOT
		//if (curr->get_cv()->getClock(state->alloc_tid) <= state->alloc_clock)
		//	assert_bug("Lock access before initialization");
		state->locked = get_thread(curr);
		ModelAction *unlock = get_last_unlock(curr);
		//synchronize with the previous unlock statement
		if (unlock != NULL) {
			synchronize(unlock, curr);
			return true;
		}
		break;
	}
	case ATOMIC_WAIT: {
		/* wake up the other threads */
		for (unsigned int i = 0;i < get_num_threads();i++) {
			Thread *t = get_thread(int_to_id(i));
			Thread *curr_thrd = get_thread(curr);
			if (t->waiting_on() == curr_thrd && t->get_pending()->is_lock())
				scheduler->wake(t);
		}

		/* unlock the lock - after checking who was waiting on it */
		state->locked = NULL;

		if (fuzzer->shouldWait(curr)) {
			/* disable this thread */
			get_safe_ptr_action(&condvar_waiters_map, curr->get_location())->push_back(curr);
			scheduler->sleep(get_thread(curr));
		}

		break;
	}
	case ATOMIC_TIMEDWAIT:
	case ATOMIC_UNLOCK: {
		//TODO: FIX WAIT SITUATION...WAITS CAN SPURIOUSLY FAIL...TIMED WAITS SHOULD PROBABLY JUST BE THE SAME AS NORMAL WAITS...THINK ABOUT PROBABILITIES THOUGH....AS IN TIMED WAIT MUST FAIL TO GUARANTEE PROGRESS...NORMAL WAIT MAY FAIL...SO NEED NORMAL WAIT TO WORK CORRECTLY IN THE CASE IT SPURIOUSLY FAILS AND IN THE CASE IT DOESN'T...  TIMED WAITS MUST EVENMTUALLY RELEASE...

		/* wake up the other threads */
		for (unsigned int i = 0;i < get_num_threads();i++) {
			Thread *t = get_thread(int_to_id(i));
			Thread *curr_thrd = get_thread(curr);
			if (t->waiting_on() == curr_thrd && t->get_pending()->is_lock())
				scheduler->wake(t);
		}

		/* unlock the lock - after checking who was waiting on it */
		state->locked = NULL;
		break;
	}
	case ATOMIC_NOTIFY_ALL: {
		action_list_t *waiters = get_safe_ptr_action(&condvar_waiters_map, curr->get_location());
		//activate all the waiting threads
		for (sllnode<ModelAction *> * rit = waiters->begin();rit != NULL;rit=rit->getNext()) {
			scheduler->wake(get_thread(rit->getVal()));
		}
		waiters->clear();
		break;
	}
	case ATOMIC_NOTIFY_ONE: {
		action_list_t *waiters = get_safe_ptr_action(&condvar_waiters_map, curr->get_location());
		if (waiters->size() != 0) {
			Thread * thread = fuzzer->selectNotify(waiters);
			scheduler->wake(thread);
		}
		break;
	}

	default:
		ASSERT(0);
	}
	return false;
}

/**
 * Process a write ModelAction
 * @param curr The ModelAction to process
 * @return True if the mo_graph was updated or promises were resolved
 */
void ModelExecution::process_write(ModelAction *curr)
{
	w_modification_order(curr);
	get_thread(curr)->set_return_value(VALUE_NONE);
}

/**
 * Process a fence ModelAction
 * @param curr The ModelAction to process
 * @return True if synchronization was updated
 */
bool ModelExecution::process_fence(ModelAction *curr)
{
	/*
	 * fence-relaxed: no-op
	 * fence-release: only log the occurence (not in this function), for
	 *   use in later synchronization
	 * fence-acquire (this function): search for hypothetical release
	 *   sequences
	 * fence-seq-cst: MO constraints formed in {r,w}_modification_order
	 */
	bool updated = false;
	if (curr->is_acquire()) {
		action_list_t *list = &action_trace;
		sllnode<ModelAction *> * rit;
		/* Find X : is_read(X) && X --sb-> curr */
		for (rit = list->end();rit != NULL;rit=rit->getPrev()) {
			ModelAction *act = rit->getVal();
			if (act == curr)
				continue;
			if (act->get_tid() != curr->get_tid())
				continue;
			/* Stop at the beginning of the thread */
			if (act->is_thread_start())
				break;
			/* Stop once we reach a prior fence-acquire */
			if (act->is_fence() && act->is_acquire())
				break;
			if (!act->is_read())
				continue;
			/* read-acquire will find its own release sequences */
			if (act->is_acquire())
				continue;

			/* Establish hypothetical release sequences */
			ClockVector *cv = get_hb_from_write(act->get_reads_from());
			if (cv != NULL && curr->get_cv()->merge(cv))
				updated = true;
		}
	}
	return updated;
}

/**
 * @brief Process the current action for thread-related activity
 *
 * Performs current-action processing for a THREAD_* ModelAction. Proccesses
 * may include setting Thread status, completing THREAD_FINISH/THREAD_JOIN
 * synchronization, etc.  This function is a no-op for non-THREAD actions
 * (e.g., ATOMIC_{READ,WRITE,RMW,LOCK}, etc.)
 *
 * @param curr The current action
 * @return True if synchronization was updated or a thread completed
 */
void ModelExecution::process_thread_action(ModelAction *curr)
{
	switch (curr->get_type()) {
	case THREAD_CREATE: {
		thrd_t *thrd = (thrd_t *)curr->get_location();
		struct thread_params *params = (struct thread_params *)curr->get_value();
		Thread *th = new Thread(get_next_id(), thrd, params->func, params->arg, get_thread(curr));
		curr->set_thread_operand(th);
		add_thread(th);
		th->set_creation(curr);
		break;
	}
	case PTHREAD_CREATE: {
		(*(uint32_t *)curr->get_location()) = pthread_counter++;

		struct pthread_params *params = (struct pthread_params *)curr->get_value();
		Thread *th = new Thread(get_next_id(), NULL, params->func, params->arg, get_thread(curr));
		curr->set_thread_operand(th);
		add_thread(th);
		th->set_creation(curr);

		if ( pthread_map.size() < pthread_counter )
			pthread_map.resize( pthread_counter );
		pthread_map[ pthread_counter-1 ] = th;

		break;
	}
	case THREAD_JOIN: {
		Thread *blocking = curr->get_thread_operand();
		ModelAction *act = get_last_action(blocking->get_id());
		synchronize(act, curr);
		break;
	}
	case PTHREAD_JOIN: {
		Thread *blocking = curr->get_thread_operand();
		ModelAction *act = get_last_action(blocking->get_id());
		synchronize(act, curr);
		break;	// WL: to be add (modified)
	}

	case THREADONLY_FINISH:
	case THREAD_FINISH: {
		Thread *th = get_thread(curr);
		if (curr->get_type() == THREAD_FINISH &&
				th == model->getInitThread()) {
			th->complete();
			setFinished();
			break;
		}

		/* Wake up any joining threads */
		for (unsigned int i = 0;i < get_num_threads();i++) {
			Thread *waiting = get_thread(int_to_id(i));
			if (waiting->waiting_on() == th &&
					waiting->get_pending()->is_thread_join())
				scheduler->wake(waiting);
		}
		th->complete();
		break;
	}
	case THREAD_START: {
		break;
	}
	case THREAD_SLEEP: {
		Thread *th = get_thread(curr);
		th->set_pending(curr);
		scheduler->add_sleep(th);
		break;
	}
	default:
		break;
	}
}

/**
 * Initialize the current action by performing one or more of the following
 * actions, as appropriate: merging RMWR and RMWC/RMW actions,
 * manipulating backtracking sets, allocating and
 * initializing clock vectors, and computing the promises to fulfill.
 *
 * @param curr The current action, as passed from the user context; may be
 * freed/invalidated after the execution of this function, with a different
 * action "returned" its place (pass-by-reference)
 * @return True if curr is a newly-explored action; false otherwise
 */
bool ModelExecution::initialize_curr_action(ModelAction **curr)
{
	if ((*curr)->is_rmwc() || (*curr)->is_rmw()) {
		ModelAction *newcurr = process_rmw(*curr);
		delete *curr;

		*curr = newcurr;
		return false;
	} else {
		ModelAction *newcurr = *curr;

		newcurr->set_seq_number(get_next_seq_num());
		/* Always compute new clock vector */
		newcurr->create_cv(get_parent_action(newcurr->get_tid()));

		/* Assign most recent release fence */
		newcurr->set_last_fence_release(get_last_fence_release(newcurr->get_tid()));

		return true;	/* This was a new ModelAction */
	}
}

/**
 * @brief Establish reads-from relation between two actions
 *
 * Perform basic operations involved with establishing a concrete rf relation,
 * including setting the ModelAction data and checking for release sequences.
 *
 * @param act The action that is reading (must be a read)
 * @param rf The action from which we are reading (must be a write)
 *
 * @return True if this read established synchronization
 */

void ModelExecution::read_from(ModelAction *act, ModelAction *rf)
{
	ASSERT(rf);
	ASSERT(rf->is_write());

	act->set_read_from(rf);
	if (act->is_acquire()) {
		ClockVector *cv = get_hb_from_write(rf);
		if (cv == NULL)
			return;
		act->get_cv()->merge(cv);
	}
}

/**
 * @brief Synchronizes two actions
 *
 * When A synchronizes with B (or A --sw-> B), B inherits A's clock vector.
 * This function performs the synchronization as well as providing other hooks
 * for other checks along with synchronization.
 *
 * @param first The left-hand side of the synchronizes-with relation
 * @param second The right-hand side of the synchronizes-with relation
 * @return True if the synchronization was successful (i.e., was consistent
 * with the execution order); false otherwise
 */
bool ModelExecution::synchronize(const ModelAction *first, ModelAction *second)
{
	if (*second < *first) {
		ASSERT(0);	//This should not happend
		return false;
	}
	return second->synchronize_with(first);
}

/**
 * @brief Check whether a model action is enabled.
 *
 * Checks whether an operation would be successful (i.e., is a lock already
 * locked, or is the joined thread already complete).
 *
 * For yield-blocking, yields are never enabled.
 *
 * @param curr is the ModelAction to check whether it is enabled.
 * @return a bool that indicates whether the action is enabled.
 */
bool ModelExecution::check_action_enabled(ModelAction *curr) {
	if (curr->is_lock()) {
		cdsc::mutex *lock = curr->get_mutex();
		struct cdsc::mutex_state *state = lock->get_state();
		if (state->locked)
			return false;
	} else if (curr->is_thread_join()) {
		Thread *blocking = curr->get_thread_operand();
		if (!blocking->is_complete()) {
			return false;
		}
	} else if (curr->is_sleep()) {
		if (!fuzzer->shouldSleep(curr))
			return false;
	}

	return true;
}

/**
 * This is the heart of the model checker routine. It performs model-checking
 * actions corresponding to a given "current action." Among other processes, it
 * calculates reads-from relationships, updates synchronization clock vectors,
 * forms a memory_order constraints graph, and handles replay/backtrack
 * execution when running permutations of previously-observed executions.
 *
 * @param curr The current action to process
 * @return The ModelAction that is actually executed; may be different than
 * curr
 */
ModelAction * ModelExecution::check_current_action(ModelAction *curr)
{
	ASSERT(curr);
	bool second_part_of_rmw = curr->is_rmwc() || curr->is_rmw();
	bool newly_explored = initialize_curr_action(&curr);

	DBG();

	wake_up_sleeping_actions(curr);

	/* Add uninitialized actions to lists */
	if (!second_part_of_rmw)
		add_uninit_action_to_lists(curr);

	SnapVector<ModelAction *> * rf_set = NULL;
	/* Build may_read_from set for newly-created actions */
	if (newly_explored && curr->is_read())
		rf_set = build_may_read_from(curr);

	if (curr->is_read() && !second_part_of_rmw) {
		process_read(curr, rf_set);
		delete rf_set;

/*		bool success = process_read(curr, rf_set);
                delete rf_set;
                if (!success)
                        return curr;	// Do not add action to lists
 */
	} else
		ASSERT(rf_set == NULL);

	/* Add the action to lists */
	if (!second_part_of_rmw)
		add_action_to_lists(curr);

	if (curr->is_write())
		add_write_to_lists(curr);

	process_thread_action(curr);

	if (curr->is_write())
		process_write(curr);

	if (curr->is_fence())
		process_fence(curr);

	if (curr->is_mutex_op())
		process_mutex(curr);

	return curr;
}

/** Close out a RMWR by converting previous RMWR into a RMW or READ. */
ModelAction * ModelExecution::process_rmw(ModelAction *act) {
	ModelAction *lastread = get_last_action(act->get_tid());
	lastread->process_rmw(act);
	if (act->is_rmw()) {
		mo_graph->addRMWEdge(lastread->get_reads_from(), lastread);
	}
	return lastread;
}

/**
 * @brief Updates the mo_graph with the constraints imposed from the current
 * read.
 *
 * Basic idea is the following: Go through each other thread and find
 * the last action that happened before our read.  Two cases:
 *
 * -# The action is a write: that write must either occur before
 * the write we read from or be the write we read from.
 * -# The action is a read: the write that that action read from
 * must occur before the write we read from or be the same write.
 *
 * @param curr The current action. Must be a read.
 * @param rf The ModelAction or Promise that curr reads from. Must be a write.
 * @param check_only If true, then only check whether the current action satisfies
 *        read modification order or not, without modifiying priorset and canprune.
 *        False by default.
 * @return True if modification order edges were added; false otherwise
 */

bool ModelExecution::r_modification_order(ModelAction *curr, const ModelAction *rf,
																					SnapVector<const ModelAction *> * priorset, bool * canprune, bool check_only)
{
	SnapVector<action_list_t> *thrd_lists = obj_thrd_map.get(curr->get_location());
	unsigned int i;
	ASSERT(curr->is_read());

	/* Last SC fence in the current thread */
	ModelAction *last_sc_fence_local = get_last_seq_cst_fence(curr->get_tid(), NULL);

	int tid = curr->get_tid();
	ModelAction *prev_same_thread = NULL;
	/* Iterate over all threads */
	for (i = 0;i < thrd_lists->size();i++, tid = (((unsigned int)(tid+1)) == thrd_lists->size()) ? 0 : tid + 1) {
		/* Last SC fence in thread tid */
		ModelAction *last_sc_fence_thread_local = NULL;
		if (i != 0)
			last_sc_fence_thread_local = get_last_seq_cst_fence(int_to_id(tid), NULL);

		/* Last SC fence in thread tid, before last SC fence in current thread */
		ModelAction *last_sc_fence_thread_before = NULL;
		if (last_sc_fence_local)
			last_sc_fence_thread_before = get_last_seq_cst_fence(int_to_id(tid), last_sc_fence_local);

		//Only need to iterate if either hb has changed for thread in question or SC fence after last operation...
		if (prev_same_thread != NULL &&
				(prev_same_thread->get_cv()->getClock(tid) == curr->get_cv()->getClock(tid)) &&
				(last_sc_fence_thread_local == NULL || *last_sc_fence_thread_local < *prev_same_thread)) {
			continue;
		}

		/* Iterate over actions in thread, starting from most recent */
		action_list_t *list = &(*thrd_lists)[tid];
		sllnode<ModelAction *> * rit;
		for (rit = list->end();rit != NULL;rit=rit->getPrev()) {
			ModelAction *act = rit->getVal();

			/* Skip curr */
			if (act == curr)
				continue;
			/* Don't want to add reflexive edges on 'rf' */
			if (act->equals(rf)) {
				if (act->happens_before(curr))
					break;
				else
					continue;
			}

			if (act->is_write()) {
				/* C++, Section 29.3 statement 5 */
				if (curr->is_seqcst() && last_sc_fence_thread_local &&
						*act < *last_sc_fence_thread_local) {
					if (mo_graph->checkReachable(rf, act))
						return false;
					if (!check_only)
						priorset->push_back(act);
					break;
				}
				/* C++, Section 29.3 statement 4 */
				else if (act->is_seqcst() && last_sc_fence_local &&
								 *act < *last_sc_fence_local) {
					if (mo_graph->checkReachable(rf, act))
						return false;
					if (!check_only)
						priorset->push_back(act);
					break;
				}
				/* C++, Section 29.3 statement 6 */
				else if (last_sc_fence_thread_before &&
								 *act < *last_sc_fence_thread_before) {
					if (mo_graph->checkReachable(rf, act))
						return false;
					if (!check_only)
						priorset->push_back(act);
					break;
				}
			}

			/*
			 * Include at most one act per-thread that "happens
			 * before" curr
			 */
			if (act->happens_before(curr)) {
				if (i==0) {
					if (last_sc_fence_local == NULL ||
							(*last_sc_fence_local < *act)) {
						prev_same_thread = act;
					}
				}
				if (act->is_write()) {
					if (mo_graph->checkReachable(rf, act))
						return false;
					if (!check_only)
						priorset->push_back(act);
				} else {
					const ModelAction *prevrf = act->get_reads_from();
					if (!prevrf->equals(rf)) {
						if (mo_graph->checkReachable(rf, prevrf))
							return false;
						if (!check_only)
							priorset->push_back(prevrf);
					} else {
						if (act->get_tid() == curr->get_tid()) {
							//Can prune curr from obj list
							if (!check_only)
								*canprune = true;
						}
					}
				}
				break;
			}
		}
	}
	return true;
}

/**
 * Updates the mo_graph with the constraints imposed from the current write.
 *
 * Basic idea is the following: Go through each other thread and find
 * the lastest action that happened before our write.  Two cases:
 *
 * (1) The action is a write => that write must occur before
 * the current write
 *
 * (2) The action is a read => the write that that action read from
 * must occur before the current write.
 *
 * This method also handles two other issues:
 *
 * (I) Sequential Consistency: Making sure that if the current write is
 * seq_cst, that it occurs after the previous seq_cst write.
 *
 * (II) Sending the write back to non-synchronizing reads.
 *
 * @param curr The current action. Must be a write.
 * @param send_fv A vector for stashing reads to which we may pass our future
 * value. If NULL, then don't record any future values.
 * @return True if modification order edges were added; false otherwise
 */
void ModelExecution::w_modification_order(ModelAction *curr)
{
	SnapVector<action_list_t> *thrd_lists = obj_thrd_map.get(curr->get_location());
	unsigned int i;
	ASSERT(curr->is_write());

	SnapList<ModelAction *> edgeset;

	if (curr->is_seqcst()) {
		/* We have to at least see the last sequentially consistent write,
		         so we are initialized. */
		ModelAction *last_seq_cst = get_last_seq_cst_write(curr);
		if (last_seq_cst != NULL) {
			edgeset.push_back(last_seq_cst);
		}
		//update map for next query
		obj_last_sc_map.put(curr->get_location(), curr);
	}

	/* Last SC fence in the current thread */
	ModelAction *last_sc_fence_local = get_last_seq_cst_fence(curr->get_tid(), NULL);

	/* Iterate over all threads */
	for (i = 0;i < thrd_lists->size();i++) {
		/* Last SC fence in thread i, before last SC fence in current thread */
		ModelAction *last_sc_fence_thread_before = NULL;
		if (last_sc_fence_local && int_to_id((int)i) != curr->get_tid())
			last_sc_fence_thread_before = get_last_seq_cst_fence(int_to_id(i), last_sc_fence_local);

		/* Iterate over actions in thread, starting from most recent */
		action_list_t *list = &(*thrd_lists)[i];
		sllnode<ModelAction*>* rit;
		for (rit = list->end();rit != NULL;rit=rit->getPrev()) {
			ModelAction *act = rit->getVal();
			if (act == curr) {
				/*
				 * 1) If RMW and it actually read from something, then we
				 * already have all relevant edges, so just skip to next
				 * thread.
				 *
				 * 2) If RMW and it didn't read from anything, we should
				 * whatever edge we can get to speed up convergence.
				 *
				 * 3) If normal write, we need to look at earlier actions, so
				 * continue processing list.
				 */
				if (curr->is_rmw()) {
					if (curr->get_reads_from() != NULL)
						break;
					else
						continue;
				} else
					continue;
			}

			/* C++, Section 29.3 statement 7 */
			if (last_sc_fence_thread_before && act->is_write() &&
					*act < *last_sc_fence_thread_before) {
				edgeset.push_back(act);
				break;
			}

			/*
			 * Include at most one act per-thread that "happens
			 * before" curr
			 */
			if (act->happens_before(curr)) {
				/*
				 * Note: if act is RMW, just add edge:
				 *   act --mo--> curr
				 * The following edge should be handled elsewhere:
				 *   readfrom(act) --mo--> act
				 */
				if (act->is_write())
					edgeset.push_back(act);
				else if (act->is_read()) {
					//if previous read accessed a null, just keep going
					edgeset.push_back(act->get_reads_from());
				}
				break;
			}
		}
	}
	mo_graph->addEdges(&edgeset, curr);

}

/**
 * Arbitrary reads from the future are not allowed. Section 29.3 part 9 places
 * some constraints. This method checks one the following constraint (others
 * require compiler support):
 *
 *   If X --hb-> Y --mo-> Z, then X should not read from Z.
 *   If X --hb-> Y, A --rf-> Y, and A --mo-> Z, then X should not read from Z.
 */
bool ModelExecution::mo_may_allow(const ModelAction *writer, const ModelAction *reader)
{
	SnapVector<action_list_t> *thrd_lists = obj_thrd_map.get(reader->get_location());
	unsigned int i;
	/* Iterate over all threads */
	for (i = 0;i < thrd_lists->size();i++) {
		const ModelAction *write_after_read = NULL;

		/* Iterate over actions in thread, starting from most recent */
		action_list_t *list = &(*thrd_lists)[i];
		sllnode<ModelAction *>* rit;
		for (rit = list->end();rit != NULL;rit=rit->getPrev()) {
			ModelAction *act = rit->getVal();

			/* Don't disallow due to act == reader */
			if (!reader->happens_before(act) || reader == act)
				break;
			else if (act->is_write())
				write_after_read = act;
			else if (act->is_read() && act->get_reads_from() != NULL)
				write_after_read = act->get_reads_from();
		}

		if (write_after_read && write_after_read != writer && mo_graph->checkReachable(write_after_read, writer))
			return false;
	}
	return true;
}

/**
 * Computes the clock vector that happens before propagates from this write.
 *
 * @param rf The action that might be part of a release sequence. Must be a
 * write.
 * @return ClockVector of happens before relation.
 */

ClockVector * ModelExecution::get_hb_from_write(ModelAction *rf) const {
	SnapVector<ModelAction *> * processset = NULL;
	for ( ;rf != NULL;rf = rf->get_reads_from()) {
		ASSERT(rf->is_write());
		if (!rf->is_rmw() || (rf->is_acquire() && rf->is_release()) || rf->get_rfcv() != NULL)
			break;
		if (processset == NULL)
			processset = new SnapVector<ModelAction *>();
		processset->push_back(rf);
	}

	int i = (processset == NULL) ? 0 : processset->size();

	ClockVector * vec = NULL;
	while(true) {
		if (rf->get_rfcv() != NULL) {
			vec = rf->get_rfcv();
		} else if (rf->is_acquire() && rf->is_release()) {
			vec = rf->get_cv();
		} else if (rf->is_release() && !rf->is_rmw()) {
			vec = rf->get_cv();
		} else if (rf->is_release()) {
			//have rmw that is release and doesn't have a rfcv
			(vec = new ClockVector(vec, NULL))->merge(rf->get_cv());
			rf->set_rfcv(vec);
		} else {
			//operation that isn't release
			if (rf->get_last_fence_release()) {
				if (vec == NULL)
					vec = rf->get_last_fence_release()->get_cv();
				else
					(vec=new ClockVector(vec, NULL))->merge(rf->get_last_fence_release()->get_cv());
			}
			rf->set_rfcv(vec);
		}
		i--;
		if (i >= 0) {
			rf = (*processset)[i];
		} else
			break;
	}
	if (processset != NULL)
		delete processset;
	return vec;
}

/**
 * Performs various bookkeeping operations for the current ModelAction when it is
 * the first atomic action occurred at its memory location.
 *
 * For instance, adds uninitialized action to the per-object, per-thread action vector
 * and to the action trace list of all thread actions.
 *
 * @param act is the ModelAction to process.
 */
void ModelExecution::add_uninit_action_to_lists(ModelAction *act)
{
	int tid = id_to_int(act->get_tid());
	ModelAction *uninit = NULL;
	int uninit_id = -1;
	action_list_t *list = get_safe_ptr_action(&obj_map, act->get_location());
	if (list->empty() && act->is_atomic_var()) {
		uninit = get_uninitialized_action(act);
		uninit_id = id_to_int(uninit->get_tid());
		list->push_front(uninit);
		SnapVector<action_list_t> *vec = get_safe_ptr_vect_action(&obj_wr_thrd_map, act->get_location());
		if ((int)vec->size() <= uninit_id) {
			int oldsize = (int) vec->size();
			vec->resize(uninit_id + 1);
			for(int i = oldsize;i < uninit_id + 1;i++) {
				new (&(*vec)[i]) action_list_t();
			}
		}
		(*vec)[uninit_id].push_front(uninit);
	}

	// Update action trace, a total order of all actions
	if (uninit)
		action_trace.push_front(uninit);

	// Update obj_thrd_map, a per location, per thread, order of actions
	SnapVector<action_list_t> *vec = get_safe_ptr_vect_action(&obj_thrd_map, act->get_location());
	if ((int)vec->size() <= tid) {
		uint oldsize = vec->size();
		vec->resize(priv->next_thread_id);
		for(uint i = oldsize;i < priv->next_thread_id;i++)
			new (&(*vec)[i]) action_list_t();
	}
	if (uninit)
		(*vec)[uninit_id].push_front(uninit);

	// Update thrd_last_action, the last action taken by each thrad
	if ((int)thrd_last_action.size() <= tid)
		thrd_last_action.resize(get_num_threads());
	if (uninit)
		thrd_last_action[uninit_id] = uninit;
}


/**
 * Performs various bookkeeping operations for the current ModelAction. For
 * instance, adds action to the per-object, per-thread action vector and to the
 * action trace list of all thread actions.
 *
 * @param act is the ModelAction to add.
 */
void ModelExecution::add_action_to_lists(ModelAction *act)
{
	int tid = id_to_int(act->get_tid());
	action_list_t *list = get_safe_ptr_action(&obj_map, act->get_location());
	list->push_back(act);

	// Update action trace, a total order of all actions
	action_trace.push_back(act);

	// Update obj_thrd_map, a per location, per thread, order of actions
	SnapVector<action_list_t> *vec = get_safe_ptr_vect_action(&obj_thrd_map, act->get_location());
	if ((int)vec->size() <= tid) {
		uint oldsize = vec->size();
		vec->resize(priv->next_thread_id);
		for(uint i = oldsize;i < priv->next_thread_id;i++)
			new (&(*vec)[i]) action_list_t();
	}
	(*vec)[tid].push_back(act);

	// Update thrd_last_action, the last action taken by each thrad
	if ((int)thrd_last_action.size() <= tid)
		thrd_last_action.resize(get_num_threads());
	thrd_last_action[tid] = act;

	// Update thrd_last_fence_release, the last release fence taken by each thread
	if (act->is_fence() && act->is_release()) {
		if ((int)thrd_last_fence_release.size() <= tid)
			thrd_last_fence_release.resize(get_num_threads());
		thrd_last_fence_release[tid] = act;
	}

	if (act->is_wait()) {
		void *mutex_loc = (void *) act->get_value();
		get_safe_ptr_action(&obj_map, mutex_loc)->push_back(act);

		SnapVector<action_list_t> *vec = get_safe_ptr_vect_action(&obj_thrd_map, mutex_loc);
		if ((int)vec->size() <= tid) {
			uint oldsize = vec->size();
			vec->resize(priv->next_thread_id);
			for(uint i = oldsize;i < priv->next_thread_id;i++)
				new (&(*vec)[i]) action_list_t();
		}
		(*vec)[tid].push_back(act);
	}
}

void insertIntoActionList(action_list_t *list, ModelAction *act) {
	sllnode<ModelAction*> * rit = list->end();
	modelclock_t next_seq = act->get_seq_number();
	if (rit == NULL || (rit->getVal()->get_seq_number() == next_seq))
		list->push_back(act);
	else {
		for(;rit != NULL;rit=rit->getPrev()) {
			if (rit->getVal()->get_seq_number() == next_seq) {
				list->insertAfter(rit, act);
				break;
			}
		}
	}
}

void insertIntoActionListAndSetCV(action_list_t *list, ModelAction *act) {
	sllnode<ModelAction*> * rit = list->end();
	modelclock_t next_seq = act->get_seq_number();
	if (rit == NULL) {
		act->create_cv(NULL);
	} else if (rit->getVal()->get_seq_number() == next_seq) {
		act->create_cv(rit->getVal());
		list->push_back(act);
	} else {
		for(;rit != NULL;rit=rit->getPrev()) {
			if (rit->getVal()->get_seq_number() == next_seq) {
				act->create_cv(rit->getVal());
				list->insertAfter(rit, act);
				break;
			}
		}
	}
}

/**
 * Performs various bookkeeping operations for a normal write.  The
 * complication is that we are typically inserting a normal write
 * lazily, so we need to insert it into the middle of lists.
 *
 * @param act is the ModelAction to add.
 */

void ModelExecution::add_normal_write_to_lists(ModelAction *act)
{
	int tid = id_to_int(act->get_tid());
	insertIntoActionListAndSetCV(&action_trace, act);

	action_list_t *list = get_safe_ptr_action(&obj_map, act->get_location());
	insertIntoActionList(list, act);

	// Update obj_thrd_map, a per location, per thread, order of actions
	SnapVector<action_list_t> *vec = get_safe_ptr_vect_action(&obj_thrd_map, act->get_location());
	if (tid >= (int)vec->size()) {
		uint oldsize =vec->size();
		vec->resize(priv->next_thread_id);
		for(uint i=oldsize;i<priv->next_thread_id;i++)
			new (&(*vec)[i]) action_list_t();
	}
	insertIntoActionList(&(*vec)[tid],act);

	// Update thrd_last_action, the last action taken by each thrad
	if (thrd_last_action[tid]->get_seq_number() == act->get_seq_number())
		thrd_last_action[tid] = act;
}


void ModelExecution::add_write_to_lists(ModelAction *write) {
	SnapVector<action_list_t> *vec = get_safe_ptr_vect_action(&obj_wr_thrd_map, write->get_location());
	int tid = id_to_int(write->get_tid());
	if (tid >= (int)vec->size()) {
		uint oldsize =vec->size();
		vec->resize(priv->next_thread_id);
		for(uint i=oldsize;i<priv->next_thread_id;i++)
			new (&(*vec)[i]) action_list_t();
	}
	(*vec)[tid].push_back(write);
}

/**
 * @brief Get the last action performed by a particular Thread
 * @param tid The thread ID of the Thread in question
 * @return The last action in the thread
 */
ModelAction * ModelExecution::get_last_action(thread_id_t tid) const
{
	int threadid = id_to_int(tid);
	if (threadid < (int)thrd_last_action.size())
		return thrd_last_action[id_to_int(tid)];
	else
		return NULL;
}

/**
 * @brief Get the last fence release performed by a particular Thread
 * @param tid The thread ID of the Thread in question
 * @return The last fence release in the thread, if one exists; NULL otherwise
 */
ModelAction * ModelExecution::get_last_fence_release(thread_id_t tid) const
{
	int threadid = id_to_int(tid);
	if (threadid < (int)thrd_last_fence_release.size())
		return thrd_last_fence_release[id_to_int(tid)];
	else
		return NULL;
}

/**
 * Gets the last memory_order_seq_cst write (in the total global sequence)
 * performed on a particular object (i.e., memory location), not including the
 * current action.
 * @param curr The current ModelAction; also denotes the object location to
 * check
 * @return The last seq_cst write
 */
ModelAction * ModelExecution::get_last_seq_cst_write(ModelAction *curr) const
{
	void *location = curr->get_location();
	return obj_last_sc_map.get(location);
}

/**
 * Gets the last memory_order_seq_cst fence (in the total global sequence)
 * performed in a particular thread, prior to a particular fence.
 * @param tid The ID of the thread to check
 * @param before_fence The fence from which to begin the search; if NULL, then
 * search for the most recent fence in the thread.
 * @return The last prior seq_cst fence in the thread, if exists; otherwise, NULL
 */
ModelAction * ModelExecution::get_last_seq_cst_fence(thread_id_t tid, const ModelAction *before_fence) const
{
	/* All fences should have location FENCE_LOCATION */
	action_list_t *list = obj_map.get(FENCE_LOCATION);

	if (!list)
		return NULL;

	sllnode<ModelAction*>* rit = list->end();

	if (before_fence) {
		for (;rit != NULL;rit=rit->getPrev())
			if (rit->getVal() == before_fence)
				break;

		ASSERT(rit->getVal() == before_fence);
		rit=rit->getPrev();
	}

	for (;rit != NULL;rit=rit->getPrev()) {
		ModelAction *act = rit->getVal();
		if (act->is_fence() && (tid == act->get_tid()) && act->is_seqcst())
			return act;
	}
	return NULL;
}

/**
 * Gets the last unlock operation performed on a particular mutex (i.e., memory
 * location). This function identifies the mutex according to the current
 * action, which is presumed to perform on the same mutex.
 * @param curr The current ModelAction; also denotes the object location to
 * check
 * @return The last unlock operation
 */
ModelAction * ModelExecution::get_last_unlock(ModelAction *curr) const
{
	void *location = curr->get_location();

	action_list_t *list = obj_map.get(location);
	/* Find: max({i in dom(S) | isUnlock(t_i) && samevar(t_i, t)}) */
	sllnode<ModelAction*>* rit;
	for (rit = list->end();rit != NULL;rit=rit->getPrev())
		if (rit->getVal()->is_unlock() || rit->getVal()->is_wait())
			return rit->getVal();
	return NULL;
}

ModelAction * ModelExecution::get_parent_action(thread_id_t tid) const
{
	ModelAction *parent = get_last_action(tid);
	if (!parent)
		parent = get_thread(tid)->get_creation();
	return parent;
}

/**
 * Returns the clock vector for a given thread.
 * @param tid The thread whose clock vector we want
 * @return Desired clock vector
 */
ClockVector * ModelExecution::get_cv(thread_id_t tid) const
{
	ModelAction *firstaction=get_parent_action(tid);
	return firstaction != NULL ? firstaction->get_cv() : NULL;
}

bool valequals(uint64_t val1, uint64_t val2, int size) {
	switch(size) {
	case 1:
		return ((uint8_t)val1) == ((uint8_t)val2);
	case 2:
		return ((uint16_t)val1) == ((uint16_t)val2);
	case 4:
		return ((uint32_t)val1) == ((uint32_t)val2);
	case 8:
		return val1==val2;
	default:
		ASSERT(0);
		return false;
	}
}

/**
 * Build up an initial set of all past writes that this 'read' action may read
 * from, as well as any previously-observed future values that must still be valid.
 *
 * @param curr is the current ModelAction that we are exploring; it must be a
 * 'read' operation.
 */
SnapVector<ModelAction *> *  ModelExecution::build_may_read_from(ModelAction *curr)
{
	SnapVector<action_list_t> *thrd_lists = obj_wr_thrd_map.get(curr->get_location());
	unsigned int i;
	ASSERT(curr->is_read());

	ModelAction *last_sc_write = NULL;

	if (curr->is_seqcst())
		last_sc_write = get_last_seq_cst_write(curr);

	SnapVector<ModelAction *> * rf_set = new SnapVector<ModelAction *>();

	/* Iterate over all threads */
	for (i = 0;i < thrd_lists->size();i++) {
		/* Iterate over actions in thread, starting from most recent */
		action_list_t *list = &(*thrd_lists)[i];
		sllnode<ModelAction *> * rit;
		for (rit = list->end();rit != NULL;rit=rit->getPrev()) {
			ModelAction *act = rit->getVal();

			if (act == curr)
				continue;

			/* Don't consider more than one seq_cst write if we are a seq_cst read. */
			bool allow_read = true;

			if (curr->is_seqcst() && (act->is_seqcst() || (last_sc_write != NULL && act->happens_before(last_sc_write))) && act != last_sc_write)
				allow_read = false;

			/* Need to check whether we will have two RMW reading from the same value */
			if (curr->is_rmwr()) {
				/* It is okay if we have a failing CAS */
				if (!curr->is_rmwrcas() ||
						valequals(curr->get_value(), act->get_value(), curr->getSize())) {
					//Need to make sure we aren't the second RMW
					CycleNode * node = mo_graph->getNode_noCreate(act);
					if (node != NULL && node->getRMW() != NULL) {
						//we are the second RMW
						allow_read = false;
					}
				}
			}

			if (allow_read) {
				/* Only add feasible reads */
				rf_set->push_back(act);
			}

			/* Include at most one act per-thread that "happens before" curr */
			if (act->happens_before(curr))
				break;
		}
	}

	if (DBG_ENABLED()) {
		model_print("Reached read action:\n");
		curr->print();
		model_print("End printing read_from_past\n");
	}
	return rf_set;
}

/**
 * @brief Get an action representing an uninitialized atomic
 *
 * This function may create a new one.
 *
 * @param curr The current action, which prompts the creation of an UNINIT action
 * @return A pointer to the UNINIT ModelAction
 */
ModelAction * ModelExecution::get_uninitialized_action(ModelAction *curr) const
{
	ModelAction *act = curr->get_uninit_action();
	if (!act) {
		act = new ModelAction(ATOMIC_UNINIT, std::memory_order_relaxed, curr->get_location(), params->uninitvalue, model_thread);
		curr->set_uninit_action(act);
	}
	act->create_cv(NULL);
	return act;
}

static void print_list(action_list_t *list)
{
	sllnode<ModelAction*> *it;

	model_print("------------------------------------------------------------------------------------\n");
	model_print("#    t    Action type     MO       Location         Value               Rf  CV\n");
	model_print("------------------------------------------------------------------------------------\n");

	unsigned int hash = 0;

	for (it = list->begin();it != NULL;it=it->getNext()) {
		const ModelAction *act = it->getVal();
		if (act->get_seq_number() > 0)
			act->print();
		hash = hash^(hash<<3)^(it->getVal()->hash());
	}
	model_print("HASH %u\n", hash);
	model_print("------------------------------------------------------------------------------------\n");
}

#if SUPPORT_MOD_ORDER_DUMP
void ModelExecution::dumpGraph(char *filename)
{
	char buffer[200];
	sprintf(buffer, "%s.dot", filename);
	FILE *file = fopen(buffer, "w");
	fprintf(file, "digraph %s {\n", filename);
	mo_graph->dumpNodes(file);
	ModelAction **thread_array = (ModelAction **)model_calloc(1, sizeof(ModelAction *) * get_num_threads());

	for (sllnode<ModelAction*>* it = action_trace.begin();it != NULL;it=it->getNext()) {
		ModelAction *act = it->getVal();
		if (act->is_read()) {
			mo_graph->dot_print_node(file, act);
			mo_graph->dot_print_edge(file,
															 act->get_reads_from(),
															 act,
															 "label=\"rf\", color=red, weight=2");
		}
		if (thread_array[act->get_tid()]) {
			mo_graph->dot_print_edge(file,
															 thread_array[id_to_int(act->get_tid())],
															 act,
															 "label=\"sb\", color=blue, weight=400");
		}

		thread_array[act->get_tid()] = act;
	}
	fprintf(file, "}\n");
	model_free(thread_array);
	fclose(file);
}
#endif

/** @brief Prints an execution trace summary. */
void ModelExecution::print_summary()
{
#if SUPPORT_MOD_ORDER_DUMP
	char buffername[100];
	sprintf(buffername, "exec%04u", get_execution_number());
	mo_graph->dumpGraphToFile(buffername);
	sprintf(buffername, "graph%04u", get_execution_number());
	dumpGraph(buffername);
#endif

	model_print("Execution trace %d:", get_execution_number());
	if (scheduler->all_threads_sleeping())
		model_print(" SLEEP-SET REDUNDANT");
	if (have_bug_reports())
		model_print(" DETECTED BUG(S)");

	model_print("\n");

	print_list(&action_trace);
	model_print("\n");

}

/**
 * Add a Thread to the system for the first time. Should only be called once
 * per thread.
 * @param t The Thread to add
 */
void ModelExecution::add_thread(Thread *t)
{
	unsigned int i = id_to_int(t->get_id());
	if (i >= thread_map.size())
		thread_map.resize(i + 1);
	thread_map[i] = t;
	if (!t->is_model_thread())
		scheduler->add_thread(t);
}

/**
 * @brief Get a Thread reference by its ID
 * @param tid The Thread's ID
 * @return A Thread reference
 */
Thread * ModelExecution::get_thread(thread_id_t tid) const
{
	unsigned int i = id_to_int(tid);
	if (i < thread_map.size())
		return thread_map[i];
	return NULL;
}

/**
 * @brief Get a reference to the Thread in which a ModelAction was executed
 * @param act The ModelAction
 * @return A Thread reference
 */
Thread * ModelExecution::get_thread(const ModelAction *act) const
{
	return get_thread(act->get_tid());
}

/**
 * @brief Get a Thread reference by its pthread ID
 * @param index The pthread's ID
 * @return A Thread reference
 */
Thread * ModelExecution::get_pthread(pthread_t pid) {
	union {
		pthread_t p;
		uint32_t v;
	} x;
	x.p = pid;
	uint32_t thread_id = x.v;
	if (thread_id < pthread_counter + 1) return pthread_map[thread_id];
	else return NULL;
}

/**
 * @brief Check if a Thread is currently enabled
 * @param t The Thread to check
 * @return True if the Thread is currently enabled
 */
bool ModelExecution::is_enabled(Thread *t) const
{
	return scheduler->is_enabled(t);
}

/**
 * @brief Check if a Thread is currently enabled
 * @param tid The ID of the Thread to check
 * @return True if the Thread is currently enabled
 */
bool ModelExecution::is_enabled(thread_id_t tid) const
{
	return scheduler->is_enabled(tid);
}

/**
 * @brief Select the next thread to execute based on the curren action
 *
 * RMW actions occur in two parts, and we cannot split them. And THREAD_CREATE
 * actions should be followed by the execution of their child thread. In either
 * case, the current action should determine the next thread schedule.
 *
 * @param curr The current action
 * @return The next thread to run, if the current action will determine this
 * selection; otherwise NULL
 */
Thread * ModelExecution::action_select_next_thread(const ModelAction *curr) const
{
	/* Do not split atomic RMW */
	if (curr->is_rmwr() && !paused_by_fuzzer(curr))
		return get_thread(curr);
	/* Follow CREATE with the created thread */
	/* which is not needed, because model.cc takes care of this */
	if (curr->get_type() == THREAD_CREATE)
		return curr->get_thread_operand();
	if (curr->get_type() == PTHREAD_CREATE) {
		return curr->get_thread_operand();
	}
	return NULL;
}

/** @param act A read atomic action */
bool ModelExecution::paused_by_fuzzer(const ModelAction * act) const
{
	ASSERT(act->is_read());

	// Actions paused by fuzzer have their sequence number reset to 0
	return act->get_seq_number() == 0;
}

/**
 * Takes the next step in the execution, if possible.
 * @param curr The current step to take
 * @return Returns the next Thread to run, if any; NULL if this execution
 * should terminate
 */
Thread * ModelExecution::take_step(ModelAction *curr)
{
	Thread *curr_thrd = get_thread(curr);
	ASSERT(curr_thrd->get_state() == THREAD_READY);

	ASSERT(check_action_enabled(curr));	/* May have side effects? */
	curr = check_current_action(curr);
	ASSERT(curr);

	/* Process this action in ModelHistory for records */
//	model->get_history()->process_action( curr, curr->get_tid() );

	if (curr_thrd->is_blocked() || curr_thrd->is_complete())
		scheduler->remove_thread(curr_thrd);

	return action_select_next_thread(curr);
}

Fuzzer * ModelExecution::getFuzzer() {
	return fuzzer;
}
