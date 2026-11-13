#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <semaphore.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <sched.h>   // for sched_yield()

#define MAX_CUSTOMERS 15
#define MAX_CONES 10
#define FLAVOR_SLOTS 2
#define REFILL_AMOUNT 10
#define FLAVOR_COUNT 5

const char* flavors[] = { "Chocolate", "Vanilla", "Strawberry", "Mint", "Caramel" };
const char* flavor_emojis[] = { "🍫", "🍦", "🍓", "🌿", "🍯" };

typedef struct {
    int available_cones;
    int total_cones_served;
    int refill_count;
    int active_customers;
    long long total_wait_time_ms; // milliseconds
    pthread_mutex_t cone_mutex;
    pthread_mutex_t output_mutex;
    pthread_mutex_t stats_mutex;
    sem_t flavor_sem;
    pthread_cond_t refill_cond;
    int needs_refill;
    int customer_counter;
    int simulation_complete;
} IceCreamMachine;

IceCreamMachine machine;

// Thread-safe print
void safe_print(const char* format, ...) {
    va_list args;
    va_start(args, format);
    pthread_mutex_lock(&machine.output_mutex);
    vprintf(format, args);
    fflush(stdout);
    pthread_mutex_unlock(&machine.output_mutex);
    va_end(args);
}

// Wait for cone availability (returns 0 on got cone, -1 if simulation ending)
int wait_for_cone(int customer_id) {
    int printed_wait = 0;
    pthread_mutex_lock(&machine.cone_mutex);
    if (machine.available_cones <= 0) {
        machine.needs_refill = 1;
        printed_wait = 1; // print after unlocking to avoid locking order issues
        pthread_cond_signal(&machine.refill_cond);
        while (machine.available_cones <= 0 && !machine.simulation_complete) {
            pthread_cond_wait(&machine.refill_cond, &machine.cone_mutex);
        }
    }
    if (machine.simulation_complete) {
        pthread_mutex_unlock(&machine.cone_mutex);
        return -1;
    }
    // consume one cone
    machine.available_cones--;
    pthread_mutex_unlock(&machine.cone_mutex);

    if (printed_wait) {
        safe_print("Customer %d: No cones! Waited and got notified after refill. ⏳\n", customer_id);
    }
    return 0;
}

// Refill bot thread
void* refill_bot(void* arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&machine.cone_mutex);
        while (!machine.needs_refill && !machine.simulation_complete)
            pthread_cond_wait(&machine.refill_cond, &machine.cone_mutex);

        if (machine.simulation_complete) {
            pthread_mutex_unlock(&machine.cone_mutex);
            break;
        }

        // compute how many to actually add (cap at MAX_CONES)
        int prev = machine.available_cones;
        int add = REFILL_AMOUNT;
        if (prev + add > MAX_CONES) add = MAX_CONES - prev;
        if (add <= 0) {
            machine.needs_refill = 0; // nothing to add
            pthread_mutex_unlock(&machine.cone_mutex);
            continue;
        }
        machine.available_cones = prev + add;
        machine.refill_count++;
        machine.needs_refill = 0;
        pthread_cond_broadcast(&machine.refill_cond);
        pthread_mutex_unlock(&machine.cone_mutex);

        safe_print("\n🤖 Refill Bot: Added %d cones ✅ (Refill #%d). Cones now: %d\n",
                   add, machine.refill_count, prev + add);
    }
    return NULL;
}

// Customer thread
void* customer_thread(void* arg) {
    (void)arg;
    int customer_id;

    // assign customer id safely
    pthread_mutex_lock(&machine.cone_mutex);
    customer_id = machine.customer_counter++;
    pthread_mutex_unlock(&machine.cone_mutex);

    // thread-local RNG seed
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)customer_id;
    // smoother, deterministic arrivals
    usleep(customer_id * 60000 + (rand_r(&seed) % 20000));

    safe_print("Customer %d: Arrived at machine 🚶‍♂️\n", customer_id);

    pthread_mutex_lock(&machine.stats_mutex);
    machine.active_customers++;
    pthread_mutex_unlock(&machine.stats_mutex);

    // measure wait time for cone
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    if (wait_for_cone(customer_id) != 0) {
        // simulation ending
        pthread_mutex_lock(&machine.stats_mutex);
        machine.active_customers--;
        pthread_mutex_unlock(&machine.stats_mutex);
        return NULL;
    }
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    long wait_ms = (t_end.tv_sec - t_start.tv_sec) * 1000 +
                   (t_end.tv_nsec - t_start.tv_nsec) / 1000000;

    // record wait time
    pthread_mutex_lock(&machine.stats_mutex);
    machine.total_wait_time_ms += wait_ms;
    pthread_mutex_unlock(&machine.stats_mutex);

    // snapshot cones left safely
    int cones_left_snapshot;
    pthread_mutex_lock(&machine.cone_mutex);
    cones_left_snapshot = machine.available_cones;
    pthread_mutex_unlock(&machine.cone_mutex);

    safe_print("Customer %d: Got cone! Cones left: %d 🍦 (waited %ld ms)\n",
               customer_id, cones_left_snapshot, wait_ms);

    safe_print("Customer %d: Waiting for flavor slot... 🕐\n", customer_id);
    sem_wait(&machine.flavor_sem);

    int flavor_index = (customer_id - 1) % FLAVOR_COUNT;
    safe_print("Customer %d: Dispensing %s %s 🎉\n", customer_id, flavors[flavor_index], flavor_emojis[flavor_index]);

    // keep service time quick and consistent (300–500 ms)
    usleep(300000 + (rand_r(&seed) % 200000));

    sem_post(&machine.flavor_sem);
    sched_yield(); // hint scheduler to hand slot immediately to next waiter

    safe_print("\n✅ Customer %d: Finished dispensing. Flavor slot freed ✅\n", customer_id);
    safe_print("🎊 Served Customer %d successfully! Enjoy your %s ice cream! 🎊\n\n", customer_id, flavors[flavor_index]);

    // update served count
    pthread_mutex_lock(&machine.stats_mutex);
    machine.total_cones_served++;
    machine.active_customers--;
    pthread_mutex_unlock(&machine.stats_mutex);

    return NULL;
}

// Initialize machine
void init_machine() {
    setvbuf(stdout, NULL, _IONBF, 0); // immediate prints for smoother logs
    machine.available_cones = MAX_CONES;
    machine.total_cones_served = 0;
    machine.refill_count = 0;
    machine.active_customers = 0;
    machine.total_wait_time_ms = 0;
    machine.needs_refill = 0;
    machine.customer_counter = 1;
    machine.simulation_complete = 0;

    if (pthread_mutex_init(&machine.cone_mutex, NULL) != 0) {
        perror("mutex init");
        exit(1);
    }
    if (pthread_mutex_init(&machine.output_mutex, NULL) != 0) {
        perror("mutex init");
        exit(1);
    }
    if (pthread_mutex_init(&machine.stats_mutex, NULL) != 0) {
        perror("mutex init");
        exit(1);
    }
    if (sem_init(&machine.flavor_sem, 0, FLAVOR_SLOTS) != 0) {
        perror("sem_init");
        exit(1);
    }
    if (pthread_cond_init(&machine.refill_cond, NULL) != 0) {
        perror("cond init");
        exit(1);
    }
}

int main() {
    srand(time(NULL));
    init_machine();

    safe_print("🧊 Ice Cream Machine Simulation Started!\n");
    safe_print("Initial Setup: %d cones, %d flavor slots, %d customers\n", MAX_CONES, FLAVOR_SLOTS, MAX_CUSTOMERS);
    safe_print("═════════════════════════════════════════════════════\n\n");

    pthread_t customers[MAX_CUSTOMERS];
    pthread_t refill_thread;
    int r;

    r = pthread_create(&refill_thread, NULL, refill_bot, NULL);
    if (r != 0) { fprintf(stderr, "refill thread create failed: %s\n", strerror(r)); return 1; }

    for (int i = 0; i < MAX_CUSTOMERS; i++) {
        r = pthread_create(&customers[i], NULL, customer_thread, NULL);
        if (r != 0) { fprintf(stderr, "customer thread create failed: %s\n", strerror(r)); return 1; }
        usleep(100000);  // simulate arrival delay
    }

    for (int i = 0; i < MAX_CUSTOMERS; i++)
        pthread_join(customers[i], NULL);

    // signal refill thread to exit
    pthread_mutex_lock(&machine.cone_mutex);
    machine.simulation_complete = 1;
    pthread_cond_signal(&machine.refill_cond);
    pthread_mutex_unlock(&machine.cone_mutex);

    pthread_join(refill_thread, NULL);

    safe_print("\n═══════════════════════════════════════════════════════════════════════\n");
    safe_print("🎉 Simulation Completed!\n");
    safe_print("Total Customers Served: %d\n", machine.total_cones_served);
    safe_print("Total Refills: %d\n", machine.refill_count);
    safe_print("Final Cones Available: %d\n", machine.available_cones);

    double avg_wait = 0.0;
    if (machine.total_cones_served > 0) {
        avg_wait = (double)machine.total_wait_time_ms / (double)machine.total_cones_served;
    }
    safe_print("Average wait for cone: %.2f ms\n", avg_wait);

    pthread_mutex_destroy(&machine.cone_mutex);
    pthread_mutex_destroy(&machine.output_mutex);
    pthread_mutex_destroy(&machine.stats_mutex);
    sem_destroy(&machine.flavor_sem);
    pthread_cond_destroy(&machine.refill_cond);

    return 0;
}
