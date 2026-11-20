#include "work_queue.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "Falló %s en línea %d\n", #expression, __LINE__); exit(EXIT_FAILURE); \
} } while (0)

enum { PRODUCERS = 4, ITEMS = 2000 };
typedef struct { work_queue *queue; int index; int result; } producer_argument;
typedef struct { work_queue *queue; size_t count; } consumer_argument;

static void *produce_many(void *pointer)
{
    producer_argument *argument = pointer;
    for (int i = 1; i <= ITEMS; i++) {
        work_request request = { .kind = REQUEST_SALE, .amount = argument->index * ITEMS + i };
        CHECK(queue_put(argument->queue, request) == 0);
    }
    return NULL;
}

static void *consume_many(void *pointer)
{
    consumer_argument *argument = pointer;
    int expected[PRODUCERS] = {1, 1, 1, 1};
    work_request request;
    queue_stop_reason reason;
    while (queue_take(argument->queue, &request, &reason) == 0) {
        CHECK(reason == QUEUE_RUNNING && request.kind == REQUEST_SALE);
        int origin = (int)((request.amount - 1) / ITEMS);
        int sequence = (int)((request.amount - 1) % ITEMS) + 1;
        CHECK(origin >= 0 && origin < PRODUCERS);
        CHECK(sequence == expected[origin]++);
        argument->count++;
    }
    for (int i = 0; i < PRODUCERS; i++) CHECK(expected[i] == ITEMS + 1);
    return NULL;
}

static void stress(size_t capacity)
{
    work_queue queue;
    CHECK(queue_init(&queue, capacity) == 0);
    producer_argument arguments[PRODUCERS];
    consumer_argument consumer = { .queue = &queue };
    pthread_t producers[PRODUCERS], taker;
    CHECK(pthread_create(&taker, NULL, consume_many, &consumer) == 0);
    for (int i = 0; i < PRODUCERS; i++) {
        arguments[i] = (producer_argument){ .queue = &queue, .index = i };
        CHECK(pthread_create(&producers[i], NULL, produce_many, &arguments[i]) == 0);
    }
    for (int i = 0; i < PRODUCERS; i++) CHECK(pthread_join(producers[i], NULL) == 0);
    queue_finish(&queue);
    CHECK(pthread_join(taker, NULL) == 0);
    CHECK(consumer.count == PRODUCERS * ITEMS);
    queue_destroy(&queue);
}

static void *put_one(void *pointer)
{
    producer_argument *argument = pointer;
    work_request request = { .kind = REQUEST_SALE, .amount = 2 };
    argument->result = queue_put(argument->queue, request);
    return NULL;
}

static void *take_one(void *pointer)
{
    producer_argument *argument = pointer;
    work_request request;
    queue_stop_reason reason;
    argument->result = queue_take(argument->queue, &request, &reason);
    return NULL;
}

static void stop_full(int finish)
{
    work_queue queue;
    CHECK(queue_init(&queue, 1) == 0);
    work_request request = { .kind = REQUEST_SALE, .amount = 1 };
    CHECK(queue_put(&queue, request) == 0);
    producer_argument argument = { .queue = &queue, .result = -1 };
    pthread_t producer;
    CHECK(pthread_create(&producer, NULL, put_one, &argument) == 0);
    if (finish) queue_finish(&queue);
    else queue_stop(&queue, QUEUE_STOPPED);
    CHECK(pthread_join(producer, NULL) == 0 && argument.result == 1);
    queue_stop_reason reason;
    CHECK(queue_take(&queue, &request, &reason) == 0 && request.amount == 1);
    CHECK(queue_take(&queue, &request, &reason) == 1);
    queue_destroy(&queue);
}

int main(void)
{
    stress(1);
    stress(7);
    stop_full(0);
    stop_full(1);
    work_queue queue;
    CHECK(queue_init(&queue, 1) == 0);
    producer_argument argument = { .queue = &queue, .result = -1 };
    pthread_t consumer;
    CHECK(pthread_create(&consumer, NULL, take_one, &argument) == 0);
    queue_finish(&queue);
    CHECK(pthread_join(consumer, NULL) == 0 && argument.result == 1);
    queue_stop(&queue, QUEUE_ERROR);
    queue_stop(&queue, QUEUE_STOPPED);
    CHECK(queue_reason(&queue) == QUEUE_ERROR);
    queue_destroy(&queue);
    puts("Cola concurrente: 16000 solicitudes, FIFO por productor y cierres verificados.");
    return EXIT_SUCCESS;
}
