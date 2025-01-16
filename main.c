#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <limits.h>

typedef struct {
    int fd;

    char *data;
    size_t len;
    size_t cap;
} FileBuffer;

static void buffer_flush(FileBuffer *b) {
    write(b->fd, b->data, b->len);
    b->len = 0;
}

static void buffer_write(FileBuffer *b, char *data, size_t size) {
    assert(size < b->cap);

    if (b->len + size > b->cap) {
        buffer_flush(b);
    }

    memcpy(b->data + b->len, data, size);
    b->len += size;
}

static FileBuffer buffer_init(int fd, size_t cap) {
    return (FileBuffer){
        .fd = fd,
        .data = malloc(cap),
        .len = 0,
        .cap = cap
    };
}

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("Expected <in_file>\n");
        return 1;
    }

    int in_fd = open(argv[1], O_RDWR);
    if (in_fd < 0) {
        printf("Unable to open %s to read\n", argv[1]);
        return 1;
    }

    char tmp_name[PATH_MAX+4] = {};
    sprintf(tmp_name, "%.*s_tmp", PATH_MAX, argv[1]);
    int out_fd = open(tmp_name, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (out_fd < 0) {
        printf("Unable to open %s to write\n", tmp_name);
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

    FileBuffer buf = buffer_init(out_fd, 512 * 1024);

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
    while (i < trunc_size) {

        uint64_t chunk;
        if ((i % 8) == 0 && (i + 8) <= trunc_size) {
            memcpy(&chunk, mem + i, sizeof(chunk));
            uint64_t xor_chunk = chunk ^ cr_mask;

            // There are no carriage returns here
            if (!has_zero(xor_chunk)) {
                buffer_write(&buf, (char *)&chunk, sizeof(chunk));
                i += 8;
                continue;
            }
        }

        while (i < trunc_size) {
            if (mem[i] == '\r' && mem[i+1] == '\n') {
                buffer_write(&buf, "\n", 1);
                i++;
            } else {
                buffer_write(&buf, &mem[i], 1);
            }
            i++;

            if ((i % 8) == 0) break;
        }
    }
    if (trunc_size == file_size) {
        goto end;
    }

    // Handle alignment leftovers
    assert(file_size > i);
    size_t new_leftovers = file_size - i - 1;

    for (; i < file_size; i++) {
        if (mem[i] == '\r' && mem[i+1] == '\n') {
            buffer_write(&buf, "\n", 1);
            i++;
        } else {
            buffer_write(&buf, &mem[i], 1);
        }
        i++;
    }

end:
    // If there's anything left in the buffer, flush it now
    buffer_flush(&buf);

    if (rename(tmp_name, argv[1])) {
        printf("Failed to move tmp!\n");
        return 1;
    }
    remove(tmp_name);
    return 0;
}
