#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>

#define MAX_THREADS 4  // Number of threads per process
#define MAX_WORD_LEN 128
#define BUFFER_SIZE 1024

typedef struct {
    char *text_chunk;
    int *word_freq;  // Word frequency array
    pthread_mutex_t *mutex;
} ThreadData;

// Global word list and frequency count (for demonstration, you may want to use hash maps for large texts)
char *words[1000];
int word_freq[1000];
int word_count = 0;
pthread_mutex_t global_mutex = PTHREAD_MUTEX_INITIALIZER;

// Function to process words (strips punctuations and turns to lowercase)
void process_word(char *word) {
    int len = strlen(word);
    for (int i = 0; i < len; i++) {
        word[i] = tolower(word[i]);
        if (ispunct(word[i])) {
            word[i] = '\0';
        }
    }
}

// Function to add or update word in the global frequency list
void update_word_count(char *word) {
    pthread_mutex_lock(&global_mutex);
    for (int i = 0; i < word_count; i++) {
        if (strcmp(words[i], word) == 0) {
            word_freq[i]++;
            pthread_mutex_unlock(&global_mutex);
            return;
        }
    }
    // New word
    words[word_count] = strdup(word);
    word_freq[word_count] = 1;
    word_count++;
    pthread_mutex_unlock(&global_mutex);
}

// Function for counting words in a text chunk
void *count_words(void *arg) {
    ThreadData *data = (ThreadData *)arg;
    char *token = strtok(data->text_chunk, " \n");
    while (token != NULL) {
        process_word(token);  // Normalize the word (lowercase, remove punctuation)
        if (strlen(token) > 0) {
            update_word_count(token);
        }
        token = strtok(NULL, " \n");
    }
    return NULL;
}

// Process file in each child process
void process_file(const char *filename, int pipe_out_fd) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Error opening file");
        exit(EXIT_FAILURE);
    }

    // Read the entire file into a buffer
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);

    char *buffer = malloc(file_size + 1);
    fread(buffer, 1, file_size, file);
    buffer[file_size] = '\0';
    fclose(file);

    // Multithreading part
    int chunk_size = file_size / MAX_THREADS;
    pthread_t threads[MAX_THREADS];
    ThreadData thread_data[MAX_THREADS];

    // Create threads and assign chunks
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_data[i].text_chunk = buffer + i * chunk_size;
        thread_data[i].word_freq = word_freq;
        thread_data[i].mutex = &global_mutex;
        pthread_create(&threads[i], NULL, count_words, &thread_data[i]);
    }

    // Wait for all threads to finish
    for (int i = 0; i < MAX_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    // Send results to parent via pipe
    write(pipe_out_fd, word_freq, sizeof(int) * word_count);
    close(pipe_out_fd);
    free(buffer);
}

// Main function to fork processes and handle files
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <file1> <file2> ...\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int num_files = argc - 1;
    char **files = &argv[1];
    int pipes[num_files][2];

    // Fork processes for each file
    for (int i = 0; i < num_files; i++) {
        if (pipe(pipes[i]) == -1) {
            perror("pipe failed");
            exit(EXIT_FAILURE);
        }

        pid_t pid = fork();
        if (pid == -1) {
            perror("fork failed");
            exit(EXIT_FAILURE);
        } else if (pid == 0) {  // Child process
            close(pipes[i][0]);  // Close read end
            process_file(files[i], pipes[i][1]);
            exit(EXIT_SUCCESS);
        } else {
            close(pipes[i][1]);  // Parent closes write end
        }
    }

    // Parent process: collect results from pipes
    for (int i = 0; i < num_files; i++) {
        int file_word_freq[1000] = {0};
        read(pipes[i][0], file_word_freq, sizeof(int) * word_count);
        close(pipes[i][0]);
        wait(NULL);  // Wait for child process to finish
    }

    // Print the top 50 words
    printf("Top 50 words:\n");
    for (int i = 0; i < 50 && i < word_count; i++) {
        printf("%s: %d\n", words[i], word_freq[i]);
    }

    return 0;
}
