#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>

#define MAX_THREADS 4
#define MAX_WORD_LEN 128
#define BUFFER_SIZE 1024
#define MAX_WORDS 10000  // Increased to handle more words across multiple files

typedef struct {
    char *text_chunk;
    int chunk_size;
    pthread_mutex_t *mutex;
} ThreadData;

// Global word list and frequency count
char *words[MAX_WORDS];
int word_freq[MAX_WORDS];
int word_count = 0;
pthread_mutex_t global_mutex = PTHREAD_MUTEX_INITIALIZER;

// Normalize word: Convert to lowercase and remove special characters
int is_valid_word(char *word) {
    int len = strlen(word);
    char temp[MAX_WORD_LEN] = {0};
    int index = 0;
    int has_alpha = 0;

    // Process each character
    for (int i = 0; i < len; i++) {
        if (isalpha(word[i])) {  // Keep only alphabetic characters
            temp[index++] = tolower(word[i]);  // Convert to lowercase
            has_alpha = 1;
        }
    }

    temp[index] = '\0';  // Null-terminate the cleaned word

    // Update the original word if it has valid alphabetic characters
    if (has_alpha && index >= 3) {
        strcpy(word, temp);  // Replace word with cleaned version
        return 1;  // It's a valid word
    }

    return 0;  // Not a valid word
}

// Add or update word frequency in the global list
void update_word_count(char *word) {
    pthread_mutex_lock(&global_mutex);  // Lock to avoid race conditions
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

// Function for word counting in text chunk
void *count_words(void *arg) {
    ThreadData *data = (ThreadData *)arg;
    char *text_chunk = data->text_chunk;
    int chunk_size = data->chunk_size;

    char word[MAX_WORD_LEN];
    int word_idx = 0;

    for (int i = 0; i < chunk_size; i++) {
        if (isalpha(text_chunk[i])) {
            word[word_idx++] = text_chunk[i];
        } else {
            if (word_idx > 0) {
                word[word_idx] = '\0';
                if (is_valid_word(word)) {
                    update_word_count(word);
                }
                word_idx = 0;
            }
        }
    }

    // Check if a word was being formed at the end of the chunk
    if (word_idx > 0) {
        word[word_idx] = '\0';
        if (is_valid_word(word)) {
            update_word_count(word);
        }
    }

    return NULL;
}

// Function to process file in a separate process
void process_file(const char *filename, int pipe_out_fd) {
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
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

    // Ensure that no word is cut between chunks
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_data[i].text_chunk = buffer + i * chunk_size;
        thread_data[i].chunk_size = (i == MAX_THREADS - 1) ? (file_size - i * chunk_size) : chunk_size;
        thread_data[i].mutex = &global_mutex;
        pthread_create(&threads[i], NULL, count_words, &thread_data[i]);
    }

    // Wait for all threads to complete
    for (int i = 0; i < MAX_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    // Send word count results to parent via pipe
    for (int i = 0; i < word_count; i++) {
        write(pipe_out_fd, words[i], MAX_WORD_LEN);  // Send each word
        write(pipe_out_fd, &word_freq[i], sizeof(int));  // Send corresponding frequency
    }
    close(pipe_out_fd);
    free(buffer);
}

// Update word count in parent process
void update_word_count_in_parent(char *word, int freq) {
    pthread_mutex_lock(&global_mutex);
    for (int i = 0; i < word_count; i++) {
        if (strcmp(words[i], word) == 0) {
            word_freq[i] += freq;  // Add the frequency from the child process
            pthread_mutex_unlock(&global_mutex);
            return;
        }
    }
    // If word is not found, add it to the global list
    words[word_count] = strdup(word);
    word_freq[word_count] = freq;
    word_count++;
    pthread_mutex_unlock(&global_mutex);
}

// Main function to fork processes for each file
int main(int argc, char *argv[]) {
    // List of files to process
    const char *files[] = {"bib", "paper1", "paper2", "progc", "progl", "progp", "trans"};
    int num_files = sizeof(files) / sizeof(files[0]);
    int pipes[num_files][2];

    // Fork a process for each file
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
            char file_path[256];
            sprintf(file_path, "./%s", files[i]);  // Assuming files are in the current directory
            process_file(file_path, pipes[i][1]);
            exit(EXIT_SUCCESS);
        } else {
            close(pipes[i][1]);  // Parent closes write end
        }
    }

    // Parent process: Collect results from all child processes
    for (int i = 0; i < num_files; i++) {
        char word_buffer[MAX_WORD_LEN];
        int freq;
        while (read(pipes[i][0], word_buffer, MAX_WORD_LEN) > 0) {
            read(pipes[i][0], &freq, sizeof(int));
            update_word_count_in_parent(word_buffer, freq);  // Update global word count in parent
        }
        close(pipes[i][0]);
        wait(NULL);  // Wait for child process to finish
    }

    // Sort the words based on frequency (descending order)
    for (int i = 0; i < word_count - 1; i++) {
        for (int j = i + 1; j < word_count; j++) {
            if (word_freq[i] < word_freq[j]) {
                // Swap frequencies
                int temp_freq = word_freq[i];
                word_freq[i] = word_freq[j];
                word_freq[j] = temp_freq;

                // Swap corresponding words
                char *temp_word = words[i];
                words[i] = words[j];
                words[j] = temp_word;
            }
        }
    }

    // Print the top 50 words globally
    printf("Top 50 words:\n");
    for (int i = 0; i < 50 && i < word_count; i++) {
        printf("%s: %d\n", words[i], word_freq[i]);
    }

    return 0;
}
