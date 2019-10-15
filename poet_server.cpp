#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cctype>
#include <cstring>
#include <ctime>
#include <cassert>
#include <thread>
#include <cerrno>
#include <csignal>

#ifndef _WIN32

#include <unistd.h>
#include <json-parser/json.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <JSON-c/JSON_checker.h>
#ifdef __cplusplus
};
#endif

#else
#include <Windows.h>
#endif

#define MAX_THREADS 20
#define MAX_NODES 10000
#define TRUE 1
#define FALSE 0

#define DOMAIN AF_INET
#define TYPE SOCK_STREAM
#define PROTOCOL 0
#define PORT 9000
#define SERVER_IP "0.0.0.0"
#define BUFFER_SZ 1024

#include "socket_t.h"
#include "queue_t.h"
#include "poet_node_t.h"
#include "general_structs.h"
#include "poet_server_functions.h"
#include "poet_functions.h"

#ifdef ERR
#undef ERR
#endif

#ifndef NDEBUG
#define ERR(...) do {fprintf(stderr, __VA_ARGS__);} while(0);
#define ERRR(...) do {fprintf(stderr, "(%d)", __LINE__); fprintf(stderr, __VA_ARGS__);} while(0);
#else
#define ERR(...) /**/
#endif

struct thread_tuple {
    pthread_t *thread;
    void *data;
};

/********** GLOBAL VARIABLES **********/
int should_terminate = 0;

pthread_t threads[MAX_THREADS];
queue_t *threads_queue = nullptr;

/********** PoET variables **********/

queue_t *queue = nullptr;

node_t sgx_table[MAX_NODES];
pthread_mutex_t sgx_table_lock;

socket_t *server_socket = nullptr;

time_t current_time = 0;
uint current_id = 0;

size_t sgxt_lowerbound;
size_t sgxmax;

/********** PoET variables END **********/

/********** GLOBAL VARIABLES END **********/

static void global_variables_initialization() {
    queue = queue_constructor();
    threads_queue = queue_constructor();
    server_socket = socket_constructor(DOMAIN, TYPE, PROTOCOL, SERVER_IP, PORT);

    if (queue == nullptr || server_socket == nullptr || threads_queue == nullptr) {
        perror("queue, socket or threads_queue constructor");
        goto error;
    }

    if (socket_bind(server_socket) != FALSE) {
        perror("socket bind");
        goto error;
    }

    if (pthread_mutex_init(&sgx_table_lock, nullptr) != FALSE) {
        perror("sgx_table_lock init");
        goto error;
    }

    current_time = time(nullptr); // take current time

    for (int i = 0; i < MAX_THREADS; i++) {
        queue_push(threads_queue, threads + i);
    }

    return;

    error:
    fprintf(stderr, "Failure in global variable initialization\n");
    exit(EXIT_FAILURE);
}

static void global_variables_destruction() {
    queue_destructor(queue, 1);
    queue_destructor(threads_queue, 0);
    socket_destructor(server_socket);

    pthread_mutex_destroy(&sgx_table_lock);
}

// Define the function to be called when ctrl-c (SIGINT) signal is sent to process
static void signal_callback_handler(int signum) {
    should_terminate = 1;
//    global_variables_destruction();
    fprintf(stderr, "Caught signal %d\n",signum);
    exit(SIGINT);
}

static int received_termination_signal() { // dummy function
    return should_terminate;
}

bool check_json_compliance(const char *buffer, size_t buffer_len) {
    JSON_checker jc = new_JSON_checker(buffer_len);

    bool is_valid = true;
    for(int current_pos = 0; (current_pos < buffer_len) && (buffer[current_pos] != '\0') && is_valid ; current_pos++) {
        int next_char = buffer[current_pos];

        is_valid = JSON_checker_char(jc, next_char);

        if (!is_valid) {
            fprintf(stderr, "JSON_checker_char: syntax error\n");
        }
    }

    is_valid = is_valid && JSON_checker_done(jc);
    if (!is_valid) {
        fprintf(stderr, "JSON_checker_end: syntax error\n");
    }

    return is_valid;
}

/* Check whether the message has the intended JSON structure for communication */
static bool check_message_integrity(json_value *json) {
    assert(json != nullptr);
    bool valid = true;

    valid = valid && json->type == json_object;
    valid = valid && json->u.object.length >= 2;

    json_value *json_method = (valid ? find_value(json, "method") : nullptr);
    valid = valid && json_method != nullptr;
    valid = valid && json_method->type == json_string;

    if (valid) {
        char *s = json_method->u.string.ptr;
        int found = 0;
        for (struct function_handle *i = functions; i->name != nullptr && !found; i++) {
            found = found || strcmp(s, i->name) == 0;
        }

        valid = valid && found;
    }

    valid = valid && find_value(json, "data") != nullptr;

    return valid;
}

static int delegate_message(char *buffer, size_t buffer_len, socket_t *soc) {
    json_value *json = nullptr;
    int ret = EXIT_SUCCESS;
    struct function_handle *function = nullptr;
    char *func_name = nullptr;

    if (! check_json_compliance(buffer, buffer_len)) {
        fprintf(stderr, "JSON format of message doesn't have a valid format for communication\n");
        goto error;
    }

    json = json_parse(buffer, buffer_len);

    if (! check_message_integrity(json)) {
        fprintf(stderr, "JSON format of message doesn't have a valid format for communication\n");
        goto error;
    }

    fprintf(stderr, "JSON message is valid\n");
    func_name = find_value(json, "method")->u.string.ptr;

    for (struct function_handle *i = functions; i->name != nullptr && function == nullptr; i++) {
        function = strcmp(func_name, i->name) == 0 ? i : nullptr;
    }
    assert(function != nullptr);

    printf("Calling: %s\n", function->name);

    ret = function->function(find_value(json, "data"), soc);
    goto terminate;

    error:
    fprintf(stderr, "method delegation finished with failure\n");
    ret = EXIT_FAILURE;

    terminate:
    if (json != nullptr) {
        json_value_free(json);
    }
    return ret;
}

static void *process_new_node(void *arg) {
    struct thread_tuple *curr_thread = (struct thread_tuple *) arg;
    socket_t *node_socket = (socket_t *) curr_thread->data;
    ERR("Processing node in thread: %p and socket %3d\n", curr_thread->thread, node_socket->socket_descriptor);

    char *buffer = nullptr;
    size_t buffer_size = 0;

    int socket_state;
    socket_state = socket_get_message(node_socket, (void **) &buffer, &buffer_size);

    while (socket_state > 0) {
        printf("message received from socket %d on thread %p\n: \"%s\"\n",
               node_socket->socket_descriptor,
               curr_thread->thread,
               buffer);

        if (delegate_message(buffer, buffer_size, node_socket) != 0) {
            goto error;
        }

        free(buffer);
        buffer = nullptr;

        socket_state = socket_get_message(node_socket, (void **) &buffer, &buffer_size);
    }

    if (socket_state < 0) {
        fprintf(stderr, "error receiving message from socket %d on thread %p\n",
                node_socket->socket_descriptor,
                curr_thread->thread);
    } else {
        printf("Connection was closed in socket %d on thread %p\n",
               node_socket->socket_descriptor,
               curr_thread->thread);
    }

    error:
    if (buffer != nullptr) {
        free(buffer);
    }
    socket_destructor(node_socket);
    queue_push(threads_queue, curr_thread->thread);
    free(curr_thread);

    pthread_exit(nullptr);
}

int main(int argc, char *argv[]) {
    // TODO: receive command line arguments for variable initialization

    signal(SIGINT, signal_callback_handler);

    global_variables_initialization();

    printf("Enter SGXt lowerbound: ");
    scanf("%lu", &sgxt_lowerbound);

    printf("Enter SGXt upperbound: ");
    scanf("%lu", &sgxmax);

    ERR("queue: %p | server_socket: %p\n", queue, server_socket);

    if (socket_listen(server_socket, MAX_THREADS) != FALSE) {
        goto error;
    }
    printf("Starting to listen\n");

    while (received_termination_signal() == FALSE) { // TODO: change condition to an OS signal
        socket_t *new_socket = socket_accept(server_socket);
        if (new_socket == nullptr) {
            fprintf(stderr, "Error accepting socket connection\n");
            continue;
        }

        // FIXME: Figure out a way to eliminate active waiting in here
        int checking;
        do {
            checking = queue_is_empty(threads_queue);
        } while (checking);

        pthread_t *next_thread = (pthread_t *) queue_front(threads_queue);
        queue_pop(threads_queue);
        struct thread_tuple *curr_thread = (struct thread_tuple *) malloc(sizeof(struct thread_tuple));
        curr_thread->thread = next_thread;
        curr_thread->data = new_socket;

        int error = pthread_create(next_thread, nullptr, &process_new_node, curr_thread);
        if (error != FALSE) {
            fprintf(stderr, "A thread could not be created: %p (error code: %d)\n", next_thread, error);
            perror("thread creation");
            exit(EXIT_FAILURE);
        }
        /* To avoid a memory leak of pthread, since there is a thread queue we dont want a pthread_join */
        pthread_detach(*next_thread);
    }

    global_variables_destruction();
    return EXIT_SUCCESS;

    error:
    fprintf(stderr, "server finished with error\n");
    global_variables_destruction();
    return EXIT_FAILURE;
}