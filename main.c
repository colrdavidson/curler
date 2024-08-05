#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>

int main(int argc, char **argv) {
	if (argc != 3) {
		printf("Expected <in_file> <out_file>\n");
		return 1;
	}

	int in_fd = open(argv[1], O_RDONLY);
	if (in_fd < 0) {
		printf("Unable to open %s to read\n", argv[1]);
		return 1;
	}

	int out_fd = open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0666);
	if (out_fd < 0) {
		printf("Unable to open %s to write\n", argv[2]);
		return 1;
	}

	struct stat desc;
	int ret = fstat(in_fd, &desc);
	if (ret == -1) {
		printf("Unable to stat %s\n", argv[1]);
		return 1;
	}

	size_t file_size = desc.st_size;
	if (file_size == 0) {
		return 0;
	}

	char *mem = mmap(NULL, file_size, PROT_WRITE, MAP_PRIVATE, in_fd, 0);
	if (mem == MAP_FAILED) {
		printf("Failed to map %s\n", argv[1]);
		return 1;
	}

	int buffer_size = 512 * 1024;
	char *buffer = malloc(buffer_size);

	size_t i = 0;

	// Skip over byte order mark if we recognize it
	if (file_size > 3) {
		uint8_t b1 = mem[0];
		uint8_t b2 = mem[1];
		uint8_t b3 = mem[2];

		// (utf-8)
		if (b1 == 0xEF && b2 == 0xBB && b3 == 0xBF) {
			i += 3;
		}
	}

	size_t leftover_size = file_size % 8;
	size_t trunc_size = file_size - leftover_size;

	uint64_t cr_mask = (~(uint64_t)0) / 255 * (uint64_t)('\r');
	#define has_zero(x) (((x)-(uint64_t)(0x0101010101010101)) & ~(x)&(uint64_t)(0x8080808080808080))

	// Loop for the happy path, we should fit nicely in registers here
	size_t w_idx = 0;
	while (i < trunc_size) {

		uint64_t chunk;
		memcpy(&chunk, mem + i, sizeof(chunk));
		uint64_t xor_chunk = chunk ^ cr_mask;

		// Flush first, so we don't overrun
		if (w_idx + 8 > buffer_size) {
			write(out_fd, buffer, w_idx);
			w_idx = 0;
		}

		// There are no carriage returns here
		if (!has_zero(xor_chunk)) {
			memcpy(buffer + w_idx, &chunk, sizeof(chunk));
			i += 8;
			w_idx += 8;
		} else {
			size_t start = i;
			while (i < start + 8) {
				if (mem[i] == '\r' && mem[i+1] == '\n') {
					buffer[w_idx] = '\n';
					i++;
				} else {
					buffer[w_idx] = mem[i];
				}
				w_idx++;
				i++;
			}
		}
	}
	// If there's anything left in the buffer, flush it now
	if (w_idx != 0) {
		write(out_fd, buffer, w_idx);
	}
	if (i == file_size) {
		return 0;
	}

	// Handle alignment leftovers
	w_idx = 0;
	for (; w_idx < leftover_size - 1; w_idx++) {
		if (mem[i] == '\r' && mem[i+1] == '\n') {
			buffer[w_idx] = '\n';
			i++;
		} else {
			buffer[w_idx] = mem[i];
		}
		i++;
	}
	// If we didn't end on a \r\n, make sure we grab the last char
	if (i != file_size) {
		buffer[w_idx++] = mem[i];
	}
	write(out_fd, buffer, w_idx);
	return 0;
}
