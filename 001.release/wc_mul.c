#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_PROC 100
#define MAX_FORK 1000

typedef struct count_t {
	int linecount;
	int wordcount;
	int charcount;
} count_t;

typedef struct plist_t {
	pid_t pid;
	long offset;
	long size;
	int pipefd[2];
} plist_t;

int CRASH = 0;

count_t word_count(FILE *fp, long offset, long size)
{
	int ch;
	long rbytes = 0;
	count_t count;

	count.linecount = 0;
	count.wordcount = 0;
	count.charcount = 0;

	if (fseek(fp, offset, SEEK_SET) < 0)
		fprintf(stderr, "Seek error.\n");

	while ((ch = getc(fp)) != EOF && rbytes < size) {
		if (ch != ' ' && ch != '\n') count.charcount++;
		if (ch == ' ' || ch == '\n') count.wordcount++;
		if (ch == '\n') count.linecount++;
		rbytes++;
	}

	srand(getpid());
	if (CRASH > 0 && rand() % 100 < CRASH) {
		abort();
	}

	return count;
}

static int write_all(int fd, const void *buffer, size_t size)
{
	const char *p = buffer;
	size_t done = 0;

	while (done < size) {
		ssize_t n = write(fd, p + done, size - done);
		if (n < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		if (n == 0) return -1;
		done += (size_t)n;
	}

	return 0;
}

static int read_all(int fd, void *buffer, size_t size)
{
	char *p = buffer;
	size_t done = 0;

	while (done < size) {
		ssize_t n = read(fd, p + done, size - done);
		if (n < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		if (n == 0) return -1;
		done += (size_t)n;
	}

	return 0;
}

static void close_pipes(plist_t jobs[], int numJobs)
{
	int i;

	for (i = 0; i < numJobs; i++) {
		if (jobs[i].pipefd[0] >= 0) {
			close(jobs[i].pipefd[0]);
			jobs[i].pipefd[0] = -1;
		}
		if (jobs[i].pipefd[1] >= 0) {
			close(jobs[i].pipefd[1]);
			jobs[i].pipefd[1] = -1;
		}
	}
}

static int spawn_child(plist_t jobs[], int numJobs, int job,
			       const char *filename, int *nFork)
{
	int fd[2];
	int i;
	pid_t pid;
	FILE *fp;
	count_t count;

	if (*nFork >= MAX_FORK) {
		fprintf(stderr, "Fork limit reached.\n");
		return -1;
	}

	if (pipe(fd) < 0) {
		fprintf(stderr, "Pipe error.\n");
		return -1;
	}

	jobs[job].pipefd[0] = fd[0];
	jobs[job].pipefd[1] = fd[1];
	(*nFork)++;

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "Fork error.\n");
		close(fd[0]);
		close(fd[1]);
		jobs[job].pipefd[0] = -1;
		jobs[job].pipefd[1] = -1;
		return -1;
	}

	if (pid == 0) {
		for (i = 0; i < numJobs; i++) {
			if (i == job) {
				close(jobs[i].pipefd[0]);
			} else {
				if (jobs[i].pipefd[0] >= 0) close(jobs[i].pipefd[0]);
				if (jobs[i].pipefd[1] >= 0) close(jobs[i].pipefd[1]);
			}
		}

		fp = fopen(filename, "r");
		if (fp == NULL) {
			fprintf(stderr, "File error.\n");
			close(fd[1]);
			_exit(1);
		}

		count = word_count(fp, jobs[job].offset, jobs[job].size);
		if (write_all(fd[1], &count, sizeof(count)) < 0) {
			fprintf(stderr, "Write error.\n");
			fclose(fp);
			close(fd[1]);
			_exit(1);
		}

		fclose(fp);
		close(fd[1]);
		_exit(0);
	}

	jobs[job].pid = pid;
	close(fd[1]);
	jobs[job].pipefd[1] = -1;
	return 0;
}

static int wait_for_child(plist_t jobs[], int numJobs, int job,
				  const char *filename, int *nFork, count_t *total)
{
	int status;
	count_t count;

	while (1) {
		if (waitpid(jobs[job].pid, &status, 0) < 0) {
			if (errno == EINTR) continue;
			fprintf(stderr, "Wait error.\n");
			return -1;
		}

		if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			if (read_all(jobs[job].pipefd[0], &count, sizeof(count)) < 0) {
				fprintf(stderr, "Read error.\n");
				close(jobs[job].pipefd[0]);
				jobs[job].pipefd[0] = -1;
				return -1;
			}

			total->linecount += count.linecount;
			total->wordcount += count.wordcount;
			total->charcount += count.charcount;
			close(jobs[job].pipefd[0]);
			jobs[job].pipefd[0] = -1;
			return 0;
		}

		close(jobs[job].pipefd[0]);
		jobs[job].pipefd[0] = -1;

		if (spawn_child(jobs, numJobs, job, filename, nFork) < 0)
			return -1;
	}
}

int main(int argc, char **argv)
{
	long fsize;
	long base;
	long remainder;
	long offset;
	FILE *fp;
	int numJobs;
	int i;
	int j;
	int nFork = 0;
	plist_t jobs[MAX_PROC];
	count_t total;

	if (argc < 3) {
		printf("Usage: wc_mul <processes> <file> [crash rate]\n");
		return 1;
	}

	if (argc > 3) {
		CRASH = atoi(argv[3]);
		if (CRASH < 0) CRASH = 0;
		if (CRASH > 50) CRASH = 50;
	}

	numJobs = atoi(argv[1]);
	if (numJobs <= 0) {
		fprintf(stderr, "Processes must be positive.\n");
		return 1;
	}
	if (numJobs > MAX_PROC) {
		numJobs = MAX_PROC;
	}

	total.linecount = 0;
	total.wordcount = 0;
	total.charcount = 0;

	for (i = 0; i < numJobs; i++) {
		jobs[i].pid = -1;
		jobs[i].offset = 0;
		jobs[i].size = 0;
		jobs[i].pipefd[0] = -1;
		jobs[i].pipefd[1] = -1;
	}

	fp = fopen(argv[2], "r");
	if (fp == NULL) {
		fprintf(stderr, "File error.\n");
		return 1;
	}

	if (fseek(fp, 0L, SEEK_END) != 0) {
		fprintf(stderr, "Seek error.\n");
		fclose(fp);
		return 1;
	}
	fsize = ftell(fp);
	if (fsize < 0) {
		fprintf(stderr, "Size error.\n");
		fclose(fp);
		return 1;
	}
	fclose(fp);

	base = fsize / numJobs;
	remainder = fsize % numJobs;
	offset = 0;
	for (i = 0; i < numJobs; i++) {
		jobs[i].offset = offset;
		jobs[i].size = base;
		if (i < remainder) jobs[i].size++;
		offset += jobs[i].size;
	}

	for (i = 0; i < numJobs; i++) {
		if (spawn_child(jobs, numJobs, i, argv[2], &nFork) < 0) {
			for (j = 0; j < i; j++)
				if (jobs[j].pid > 0)
					while (waitpid(jobs[j].pid, NULL, 0) < 0 && errno == EINTR) {}
			close_pipes(jobs, numJobs);
			return 1;
		}
	}

	for (i = 0; i < numJobs; i++) {
		if (wait_for_child(jobs, numJobs, i, argv[2], &nFork, &total) < 0) {
			for (j = i + 1; j < numJobs; j++)
				if (jobs[j].pid > 0)
					while (waitpid(jobs[j].pid, NULL, 0) < 0 && errno == EINTR) {}
			close_pipes(jobs, numJobs);
			return 1;
		}
	}

	printf("Total Lines: %d\n", total.linecount);
	printf("Total Words: %d\n", total.wordcount);
	printf("Total Characters: %d\n", total.charcount);

	return 0;
}
