#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <stdbool.h>

#define NUM_CHILDREN 10
#define TIME_QUANTUM_DEFAULT 3
#define MAX_TICKS 10000
#define AGING_THRESHOLD 15

#define STATE_TICK_DONE 1
#define STATE_IO_REQ 2
#define STATE_FINISHED 3

typedef enum { READY, RUNNING, SLEEP, DONE} PState;

typedef struct {
	pid_t pid;
	int id;
	int cpu_burst;
	int remaining_quantum;
	int io_wait_time;

	// [analyzing field]
	int arrival_time;
	int start_time;
	int finish_time;
	int waiting_time;
	bool has_started;

	PState state;
	int pipe_fd_read;
} PCB;

PCB pcbs[NUM_CHILDREN];
int current_quantum_setting = TIME_QUANTUM_DEFAULT;
int total_ticks = 0;
int pipes[NUM_CHILDREN][2];

int gantt_history[MAX_TICKS];

int get_random(int min, int max) {
	return min + rand() % (max -min + 1);
}

//child
void child_process_logic(int id, int write_fd) {
	int cpu_burst = get_random(1, 10);
	write(write_fd, &cpu_burst, sizeof(int));

	sigset_t sigset;
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGUSR1);
	sigprocmask(SIG_BLOCK, &sigset, NULL);

	int sig;
	while (cpu_burst > 0){
		sigwait(&sigset, &sig);
		cpu_burst--;
		int result;

		if (cpu_burst ==0){
			if (get_random(0,1) == 0){
				result = STATE_FINISHED;
			} else {
				cpu_burst = get_random(2,5);
				result = STATE_IO_REQ;
			}
		} else {
			if (get_random(1,10) == 1) result = STATE_IO_REQ;
			else result = STATE_TICK_DONE;
		}
		write(write_fd, &result, sizeof(result));
		if (result == STATE_FINISHED) break;
	}
	exit(0);
}

//process execution history and status update function
void record_execution(int pid_idx) {
	PCB *p = &pcbs[pid_idx];

	//response time
	if (!p->has_started) {
		p ->start_time = total_ticks;
		p ->has_started = true;
	}

	//gantt chart
	if (total_ticks < MAX_TICKS) {
		gantt_history[total_ticks] = p->id;
	}
}

//round robin
void run_scheduler_rr() {
	int active_processes = NUM_CHILDREN;
	int current_idx = 0;
	int result_buf;

	printf("\n round robin\n");

	while (active_processes > 0){
		//sleep -> ready
		for (int i = 0; i< NUM_CHILDREN; i++){
			if (pcbs[i].state == SLEEP) {
				pcbs[i].io_wait_time--;
				if (pcbs[i].io_wait_time <= 0) {
					pcbs[i].state = READY;
					pcbs[i].remaining_quantum = current_quantum_setting;
				}
			}
		}

		int selected = -1;
		int checked = 0;
		while (checked < NUM_CHILDREN) {
			int idx = (current_idx + checked) % NUM_CHILDREN;
			if ((pcbs[idx].state == READY || pcbs[idx].state == RUNNING) && pcbs[idx].remaining_quantum > 0){
				selected = idx;
				break;
			}
			checked++;
		}

		if (selected == -1){
			bool need_reset = false;
			bool all_wait = true;
			for(int i = 0; i < NUM_CHILDREN ; i++){
				if(pcbs[i].state != DONE) all_wait = false;
				if(pcbs[i].state != DONE && pcbs[i].remaining_quantum <= 0) need_reset = true;
			}
			if (all_wait) { if (active_processes == 0) break; gantt_history[total_ticks++] = -1; continue;}
			if (need_reset) {
				for (int i = 0; i < NUM_CHILDREN; i++)
					if (pcbs[i].state != DONE) pcbs[i].remaining_quantum = current_quantum_setting;
				continue;
			}
			gantt_history[total_ticks++] = -1;
			continue;
		}

		//execution
		current_idx = selected;
		PCB *p = &pcbs[current_idx];

		for(int i = 0; i<NUM_CHILDREN; i++) if(i!= current_idx && pcbs[i].state == READY) pcbs[i].waiting_time++;

		p -> state = RUNNING;
		record_execution(current_idx);

		kill(p->pid, SIGUSR1);
		read(p->pipe_fd_read, &result_buf, sizeof(int));

		p->remaining_quantum--;
		total_ticks++;

		if (result_buf == STATE_FINISHED) {
			printf("[cpu] child %d finished\n", p->id);

			p->state = DONE;
			p->finish_time = total_ticks;
			active_processes--;
			waitpid(p->pid, NULL, 0);
		} else if (result_buf == STATE_IO_REQ) {
			printf("[cpu] child %d I/O request\n", p ->id);

			p->state = SLEEP;
			p->io_wait_time = get_random(1,5);
		} else {
			printf("[cpu] child %d is Running...(quantum: %d)\n", p->id,  p-> remaining_quantum);
				
			if (p->remaining_quantum == 0) {
				printf("	-> child %d quantum expired, switch.\n", p->id);
				p->state = READY;
				current_idx = (current_idx + 1) % NUM_CHILDREN;
			}
		}
	}
}

//sjf (non_preemptive) + aging

void run_scheduler_sjf() {
	int active_processes = NUM_CHILDREN;
	int current_idx = -1;
	int result_buf;

	printf("\n sjf non_preemptive\n");

	while (active_processes > 0) {
		for (int i = 0; i <NUM_CHILDREN; i++) {
			if (pcbs[i].state == SLEEP) {
				pcbs[i].io_wait_time--;
				if (pcbs[i].io_wait_time <= 0) {
					pcbs[i].state = READY;
					pcbs[i].cpu_burst = get_random(2,5);
				}
			}
		}

		if (current_idx != -1 && pcbs[current_idx].state == RUNNING) {
		} else {
			int min_effective_burst = 9999;
			int selected = -1;

			for (int i = 0; i < NUM_CHILDREN ; i++) {
				if (pcbs[i].state == READY) {

					int aging_bonus = pcbs[i].waiting_time / AGING_THRESHOLD;
					int effective_burst = pcbs[i].cpu_burst - aging_bonus;
					if (effective_burst < 0) effective_burst = 0;

					if (effective_burst < min_effective_burst) {
						min_effective_burst = effective_burst;
						selected = i;
					}
				}
			}
			if (selected != -1) {
				current_idx = selected;
				pcbs[current_idx].state = RUNNING;
			} else {
				current_idx = -1;
			}
		}

		if (current_idx == -1) {
			bool all_done = true;
			for(int i = 0; i <NUM_CHILDREN; i++) if (pcbs[i].state != DONE) all_done = false;
			if(all_done) break;
			gantt_history[total_ticks++] = -1;
			continue;
		}

		PCB *p = &pcbs[current_idx];
		for(int i =0; i < NUM_CHILDREN ; i++) if(i != current_idx && pcbs[i].state == READY) pcbs[i].waiting_time++;

		record_execution(current_idx);

		kill(p ->pid, SIGUSR1);
		read(p -> pipe_fd_read, &result_buf, sizeof(int));

		p ->cpu_burst--;
		total_ticks++;

		//execution

		if (result_buf == STATE_FINISHED) {
			printf("[cpu] child %d finished\n", p->id);

			p ->state = DONE;
			p -> finish_time = total_ticks;
			active_processes--;
			waitpid(p -> pid, NULL, 0);
			current_idx = -1;
		} else if (result_buf == STATE_IO_REQ) {
			printf("[cpu] child %d I/O request\n", p ->id);
			p ->state = SLEEP;
			p -> io_wait_time = get_random(1,5);
			current_idx = -1;
		} else {
			printf("[cpu] child %d is Running...\n", p-> id);
		}
	}
}

// srtf preemptive + aging
void run_scheduler_srtf() {
	int active_processes = NUM_CHILDREN;
	int current_idx = -1;
	int result_buf;

	printf("\n srtf preemptive + aging\n");

	while (active_processes > 0) {
		for (int i = 0 ; i < NUM_CHILDREN; i++) {
			if (pcbs[i].state == SLEEP) {
				pcbs[i].io_wait_time--;
				if (pcbs[i].io_wait_time <= 0) {
					pcbs[i].state = READY;
					pcbs[i].cpu_burst = get_random(2,5);
				}
			}
		}

		int min_effective_burst = 9999;
		int selected = -1;

		for (int i = 0; i< NUM_CHILDREN; i++) {
			if (pcbs[i].state == READY || pcbs[i].state == RUNNING) {
				int aging_bonus = pcbs[i].waiting_time / AGING_THRESHOLD;
				int effective_burst = pcbs[i].cpu_burst - aging_bonus;
				if (effective_burst < 0) effective_burst = 0;

				if (effective_burst < min_effective_burst) {
					min_effective_burst = effective_burst;
					selected = i;
				}
			}
		}

		if (selected != -1) {
			if (current_idx != -1 && current_idx != selected) {
				pcbs[current_idx].state = READY;
			}
			current_idx = selected;
			pcbs[current_idx].state = RUNNING;
		} else {
			current_idx = -1;
		}

		if (current_idx == -1) {
			bool all_done = true;
			for(int i = 0; i < NUM_CHILDREN; i++) if (pcbs[i].state != DONE) all_done = false;
			if (all_done) break;
			gantt_history[total_ticks++] = -1;
			continue;
		}

		PCB *p = &pcbs[current_idx];
		for(int i = 0; i < NUM_CHILDREN; i++) if ( i != current_idx && pcbs[i].state == READY) pcbs[i].waiting_time++;

		record_execution(current_idx);

		kill(p->pid, SIGUSR1);
		read(p->pipe_fd_read, &result_buf, sizeof(int));

		p -> cpu_burst--;
		total_ticks++;

		//execution

		if (result_buf == STATE_FINISHED){
			printf("[cpu] child %d finished\n", p->id);

			p-> state = DONE;
			p-> finish_time = total_ticks;
			active_processes--;
			waitpid(p->pid, NULL, 0);
			current_idx = -1;
		} else if (result_buf == STATE_IO_REQ) {
			printf("[cpu] child %d I/O request\n", p ->id);

			p->state =SLEEP;
			p ->io_wait_time = get_random(1,5);
			current_idx = -1;
		} else {
			printf("[cpu] child %d is running...\n", p-> id);
		}
	}
}

// print result
void print_stats() {
	printf("                      PERFORMANCE ANALYSIS                         \n");
	printf("===================================================================\n");
	printf("PID\tWait Time\tResponse Time\tTurnaround Time\n");
	printf("                                                                   \n");
	
	double total_wait = 0, total_resp = 0, total_turn = 0;

	for(int i = 0; i <NUM_CHILDREN; i++) {
		int resp = pcbs[i].start_time - pcbs[i].arrival_time;
		int turn = pcbs[i].finish_time - pcbs[i].arrival_time;

		printf("P%d\t%d\t\t%d\t\t%d\n", pcbs[i].id, pcbs[i].waiting_time, resp, turn);

		total_wait += pcbs[i].waiting_time;
		total_resp += resp;
		total_turn += turn;
	}

	printf("-------------------------------------------------------------------\n");
	printf("AVG\t%.2f\t\t%.2f\t\t%.2f\n",
			total_wait/NUM_CHILDREN, total_resp/NUM_CHILDREN, total_turn/NUM_CHILDREN);

	printf("\n[gantt chart [100ticks]\n");
	int limit = total_ticks < 100 ? total_ticks : 100;

	printf("time : ");
	for (int i = 0; i < limit; i++) printf("%-2d ", i);
	printf("\nproc : ");
	for(int i = 0; i <limit; i++) {
		if (gantt_history[i] == -1) printf("-- ");
		else printf("P%d ", gantt_history[i]);
	}
	
	printf("...\n");
	printf("\n[total simulation ticks]: %d\n", total_ticks);
}

int main(int argc, char *argv[]) {
	srand(time(NULL));

	for(int i = 0; i <MAX_TICKS; i++) gantt_history[i] = -1;

	for (int i = 0; i< NUM_CHILDREN; i++) {
		if (pipe(pipes[i]) == -1) exit(1);
		pid_t pid = fork();

		if (pid < 0) exit(1);
		else if (pid == 0) {
			close(pipes[i][0]);
			child_process_logic(i, pipes[i][1]);
		} else {
			close(pipes[i][1]);
			pcbs[i].pid = pid;
			pcbs[i].id = i;
			pcbs[i].state =READY;
			pcbs[i].remaining_quantum = current_quantum_setting;
			pcbs[i].pipe_fd_read = pipes[i][0];

			//reset
			pcbs[i].arrival_time = 0;
			pcbs[i].waiting_time = 0;
			pcbs[i].has_started = false;

			read(pipes[i][0], &pcbs[i].cpu_burst, sizeof(int));
		}
	}

	int mode = 1;
	if (argc > 1) mode = atoi(argv[1]);

	if (argc > 2) {
		current_quantum_setting = atoi(argv[2]);
		if (current_quantum_setting < 1) current_quantum_setting = 1;
	}

	if (mode == 2) run_scheduler_sjf();
	else if (mode == 3) run_scheduler_srtf();
	else {
		printf("[Init] Round Robin with Quantum = %d\n", current_quantum_setting);
		run_scheduler_rr();
	}

	print_stats();

	return 0;
}
