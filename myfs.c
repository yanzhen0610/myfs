#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "myfs.h"

#define MY_INODE_SIZE 128
#define MY_BLOCK_SIZE 1 K

struct my_partition* my_make_partition(uint32_t size)
{
    if (size < 5 * MY_BLOCK_SIZE) return NULL;
    uint8_t* memory = (uint8_t*) malloc(size);
    struct my_partition* partition = (struct my_partition*) memory;
    partition->size = size;

    partition->inode_size = MY_INODE_SIZE;
    partition->block_size = MY_BLOCK_SIZE;

    uint32_t num_of_blocks = partition->size / partition->block_size;
    uint32_t size_of_bitmap = num_of_blocks / 8;
    if (num_of_blocks % 8) ++size_of_bitmap;
    uint32_t blocks_of_bitmap = size_of_bitmap / partition->block_size;
    if (size_of_bitmap % partition->block_size) ++blocks_of_bitmap;
    uint32_t tmp;

    partition->inode_bitmap = 1;
    partition->block_bitmap = partition->inode_bitmap + blocks_of_bitmap;

    partition->inodes = partition->block_bitmap + blocks_of_bitmap;

    partition->inode_count = (num_of_blocks - partition->inodes) *
        partition->inode_size / (partition->inode_size + partition->block_size);
    partition->block_count = num_of_blocks;
    tmp = partition->inode_count * partition->inode_size;
    partition->blocks = partition->inodes + tmp / partition->block_size;
    if (tmp % partition->block_size) ++partition->blocks;

    partition->inode_used = 0;
    partition->block_used = 0;

    memset(my_get_block_pointer(partition,
        partition->inode_bitmap), 0, partition->block_size);
    memset(my_get_block_pointer(partition,
        partition->block_bitmap), 0, partition->block_size);

    // printf("%d\n", partition->blocks);
    for (uint32_t i = 0; i < partition->blocks; ++i)
        my_mark_block_used(partition, i);
    printf("a %d\n", my_get_free_block(partition));

    my_mark_inode_used(partition, 0);
    partition->root = 0;

    struct my_inode* root = my_get_inode_pointer(partition, 0);
    root->reference_count = 1;
    root->mtime = time(NULL);
    root->size = 0;

    return partition;
}

struct my_partition* my_load_partition_from_file(FILE* file)
{
    return NULL;
}

void my_dump_partition_to_file(FILE* file)
{}

void my_free_partition(struct my_partition* partition)
{
    free(partition);
}

uint8_t* my_get_block_pointer(
    struct my_partition* partition, uint32_t block)
{
    return (uint8_t*) partition + block * partition->block_size;
}

struct my_inode* my_get_inode_pointer(
    struct my_partition* partition, uint32_t inode)
{
    return (struct my_inode*) (
        my_get_block_pointer(partition, partition->inodes) +
        partition->inode_size * inode);
}

static int32_t first_zero(uint64_t x)
{
    int32_t i;
    
    #ifndef MY_FS_BIG_ENDIAN
        uint8_t* p = ((uint8_t*) &x) - 1;
    #endif

    for (i = 63; i >= 0; --i)
    {
        #ifndef MY_FS_BIG_ENDIAN
            if ((i & 7) == 7) ++p;
            if (((*p >> (i & 7)) & 1) == 0) break;
        #else
            if ((x >> i & 1) == 0) break;
        #endif
    }

    return 63 - i;
}

uint32_t my_get_free_inode(struct my_partition* partition)
{
    uint32_t loop = partition->inode_count / 64;
    uint64_t *p = (uint64_t*) my_get_block_pointer(
        partition, partition->inode_bitmap);
    for (uint32_t i = 0; i < loop; ++i, ++p)
        if (*p != 0xffffffffffffffff)
            return first_zero(*p) + i * 64;
    uint32_t base = loop * 64;
    uint32_t z = first_zero(*p);
    if (base + z >= partition->inode_count) return -1;
    return base + z;
}

void my_mark_inode_used(struct my_partition* partition, uint32_t inode)
{
    uint8_t* bitmap = my_get_block_pointer(
        partition, partition->inode_bitmap) + (inode / 8);
    uint8_t bit = 0x80 >> (inode & 7);
    if (!(*bitmap & bit))
    {
        ++partition->inode_used;
        *bitmap |= bit;
    }
}

void my_mark_inode_unused(struct my_partition* partition, uint32_t inode)
{
    uint8_t* bitmap = my_get_block_pointer(
        partition, partition->inode_bitmap) + (inode / 8);
    uint8_t bit = 0x80 >> (inode & 7);
    if (*bitmap & bit)
    {
        --partition->inode_used;
        *bitmap &= ~bit;
    }
}

uint32_t my_get_free_block(struct my_partition* partition)
{
    uint32_t loop = partition->block_count / 64;
    uint64_t *p = (uint64_t*) my_get_block_pointer(
        partition, partition->block_bitmap);
    for (uint32_t i = 0; i < loop; ++i, ++p)
        if (*p != 0xffffffffffffffff)
            return first_zero(*p) + i * 64;
    uint32_t base = loop * 64;
    uint32_t z = first_zero(*p);
    if (base + z >= partition->block_count) return 0;
    return base + z;
}

void my_mark_block_used(struct my_partition* partition, uint32_t block)
{
    uint8_t* bitmap = my_get_block_pointer(
        partition, partition->block_bitmap) + (block / 8);
    uint8_t bit = 0x80 >> (block & 7);
    if (!(*bitmap & bit))
    {
        ++partition->block_used;
        *bitmap |= bit;
    }
}

void my_mark_block_unused(struct my_partition* partition, uint32_t block)
{
    uint8_t* bitmap = my_get_block_pointer(
        partition, partition->block_bitmap) + (block / 8);
    uint8_t bit = 0x80 >> (block & 7);
    if (*bitmap & bit)
    {
        --partition->block_used;
        *bitmap &= ~bit;
    }
}

struct my_directory_file_list* my_list_directory(
    struct my_partition* partition, uint32_t dir)
{
    struct my_file* directory = my_file_open(partition, dir);
    struct my_directory_file_list *head = NULL, *node;
    uint32_t len = 1;
    uint32_t inode, type;
    int r;
    char buffer[512];
    char *p, *q;
    while (len != 0)
    {
        len = my_file_read_line(partition, directory, (uint8_t*) buffer, 512);
        if (len == 0 || len < 6) continue;
        q = p = buffer;

        // inode
        while (*p != '|' && *p != '\n' && (buffer + len - p) > 0) ++p;
        if (p == q) continue;
        *p = '\0';
        r = sscanf(q, "%x", &inode);
        if (r < 1) continue;

        // type
        q = ++p;
        while (*p != '|' && *p != '\n' && (buffer + len - p) > 0) ++p;
        if (p == q) continue;
        *p = '\0';
        r = sscanf(q, "%x", &type);
        if (r < 1) continue;

        // filename
        q = ++p;
        while (*p != '\n' && (buffer + len - p) > 0) ++p;
        if (p == q) continue;
        *p = '\0';

        // linked list
        node = (struct my_directory_file_list*) malloc(sizeof(struct my_directory_file_list));
        node->inode = inode;
        node->type = type;
        strcpy(node->filename, q);
        node->next = head;
        head = node;
    }
    my_file_close(partition, directory);
    return head;
}

void my_free_directory_file_list(
    struct my_partition* partition, struct my_directory_file_list* list)
{
    struct my_directory_file_list* next;
    while (list)
    {
        next = list->next;
        free(list);
        list = next;
    }
}

uint32_t my_touch(struct my_partition* partition)
{
    uint32_t inode = my_get_free_inode(partition);
    if (inode == -1) return -1;
    my_mark_inode_used(partition, inode);
    return inode;
}

struct my_directory_file_list* my_get_file(
    struct my_partition* partition,
    struct my_directory_file_list* file_list, const char* filename)
{
    while (file_list)
        if (strcmp(file_list->filename, filename) == 0) break;
        else file_list = file_list->next;
    return file_list;
}

bool my_dir_reference_file(
    struct my_partition* partition,
    uint32_t dir, uint32_t file, uint8_t type, const char* filename)
{
    struct my_directory_file_list* list = my_list_directory(partition, dir);
    if (strlen(filename) == 0 || my_get_file(partition, list, filename) != NULL)
    {
        my_free_directory_file_list(partition, list);
        return false;
    }
    my_free_directory_file_list(partition, list);
    char buffer[512];
    uint32_t line_len = snprintf(buffer, 512, "%x|%x|%s\n", file, type, filename);
    if (line_len == 511) buffer[line_len++] = '\n';
    struct my_file* directory = my_file_open_end(partition, dir);
    my_file_write(partition, directory, (uint8_t*) buffer, line_len);
    my_file_close(partition, directory);
    ++(my_get_inode_pointer(partition, file)->reference_count);
    return true;
}

void my_dir_unreference_file(
    struct my_partition* partition,
    uint32_t dir, const char* filename)
{
    struct my_directory_file_list* list = my_list_directory(partition, dir);
    struct my_directory_file_list* file = my_get_file(partition, list, filename);
    if (file == NULL)
    {
        my_free_directory_file_list(partition, list);
        return;
    }
    struct my_directory_file_list* iter = list;
    my_erase_file(partition, dir);
    struct my_file* directory = my_file_open(partition, dir);
    uint8_t buffer[512];
    while (iter)
    {
        if (iter != file)
        {
            uint32_t line_len = snprintf(
                (char*) buffer, 512, "%x|%x|%s\n",
                iter->inode,
                iter->type,
                iter->filename);
            if (line_len == 511) buffer[line_len++ - 1] = '\n';
            my_file_write(partition, directory, buffer, line_len);
        }
        iter = iter->next;
    }
    my_file_close(partition, directory);
    struct my_inode* inode = my_get_inode_pointer(partition, file->inode);
    --(inode->reference_count);
    if (inode->reference_count == 0)
        my_delete_file(partition, file->inode);
    my_free_directory_file_list(partition, list);
}

void my_delete_file(struct my_partition* partition, uint32_t inode)
{
    my_erase_file(partition, inode);
    my_mark_inode_unused(partition, inode);
}

void my_erase_file(struct my_partition* partition, uint32_t inode)
{
    struct my_inode* s_inode = my_get_inode_pointer(partition, inode);
    uint32_t blocks = s_inode->size / partition->block_size;
    if (s_inode->size % partition->block_size) ++blocks;

    // direct block
    uint32_t loop = blocks > NUM_OF_DIRECT_BLOCKS ? NUM_OF_DIRECT_BLOCKS : blocks;
    for (int i = 0; i < loop; ++i)
        my_mark_block_unused(partition, s_inode->direct_block[i]);

    // indirect block
    if (blocks <= NUM_OF_DIRECT_BLOCKS) return;
    blocks -= NUM_OF_DIRECT_BLOCKS;
    loop = partition->block_size / sizeof(uint32_t);
    if (blocks < loop) loop = blocks;
    for (int i = 0; i < loop; ++i)
        my_mark_block_unused(partition,
            my_get_block_pointer(partition, s_inode->indirect_block)[i]);

    // double indirect block
    // trible indirect block

    s_inode->size = 0;
}

struct my_file* my_file_open(
    struct my_partition* partition, uint32_t file_inode)
{
    struct my_file* file = (struct my_file*) malloc(sizeof(struct my_file));
    file->inode = my_get_inode_pointer(partition, file_inode);
    file->position = 0;
    file->block_position = partition->block_size;
    return file;
}

struct my_file* my_file_open_end(
    struct my_partition* partition, uint32_t file_inode)
{
    uint32_t tmp;
    struct my_file* file = (struct my_file*) malloc(sizeof(struct my_file));
    file->inode = my_get_inode_pointer(partition, file_inode);
    file->position = file->inode->size;
    file->block_position = file->position % partition->block_size;
    if (file->block_position == 0)
        file->block_position = partition->block_size; // let write function handle it
    else if ((tmp = file->position / partition->block_size) <
            NUM_OF_DIRECT_BLOCKS)
        file->block = file->inode->direct_block[tmp];
    else if ((tmp -= NUM_OF_DIRECT_BLOCKS) <
            partition->block_size / sizeof(uint32_t))
        file->block = my_get_block_pointer(partition,
            file->inode->indirect_block)[tmp];
    // else if ()
    // {}
    return file;
}

void my_file_close(struct my_partition* partition, struct my_file* file)
{
    free(file);
}

uint32_t my_file_read(
    struct my_partition* partition, struct my_file* file,
    uint8_t* buffer, uint32_t buffer_size)
{
    uint8_t* current_block = my_get_block_pointer(partition, file->block);
    uint32_t tmp, buffer_position = 0;
    if (file->position >= file->inode->size) return 0;
    
    while (buffer_position < buffer_size && file->position < file->inode->size)
    {
        if (file->block_position >= partition->block_size) // without / and %
        // if reached block ending then go to next block
        {
            if ((tmp = file->position / partition->block_size) <
                    NUM_OF_DIRECT_BLOCKS)
                file->block = file->inode->direct_block[tmp];
            else if ((tmp -= NUM_OF_DIRECT_BLOCKS) <
                    partition->block_size / sizeof(uint32_t))
                file->block = ((uint32_t*) my_get_block_pointer(partition,
                    file->inode->indirect_block))[tmp];
            // else if ()
            // {}
            current_block = my_get_block_pointer(partition, file->block);
            file->block_position = 0;
        }
        buffer[buffer_position++] = current_block[file->block_position++];
        ++file->position;
    }
    return buffer_position;
}

uint32_t my_file_read_line(
    struct my_partition* partition, struct my_file* file,
    uint8_t* buffer, uint32_t buffer_size)
{
    uint8_t* current_block = my_get_block_pointer(partition, file->block);
    uint32_t tmp, buffer_position = 0;
    if (file->position >= file->inode->size) return 0;
    
    while (buffer_position + 1 < buffer_size &&
        file->position < file->inode->size)
    {
    // puts("1");
        if (file->block_position >= partition->block_size) // without / and %
        // if reached block ending then go to next block
        {
            if ((tmp = file->position / partition->block_size) <
                    NUM_OF_DIRECT_BLOCKS)
                file->block = file->inode->direct_block[tmp];
            else if ((tmp -= NUM_OF_DIRECT_BLOCKS) <
                    partition->block_size / sizeof(uint32_t))
                file->block = ((uint32_t*) my_get_block_pointer(partition,
                    file->inode->indirect_block))[tmp];
            // else if ()
            // {}
            current_block = my_get_block_pointer(partition, file->block);
            file->block_position = 0;
        }
    // puts("2");
    // printf("buffer position %d\n", buffer_position);
    // printf("block position %d\n", file->block_position);
    // printf("file position %d\n", file->position);
    // printf("buffer %x\n", buffer);
    // printf("block %d\n", file->block);
        buffer[buffer_position] = current_block[file->block_position++];
    // puts("3");
        ++file->position;
    // puts("4");
        if (buffer[buffer_position++] == '\n') break;
    // puts("5");
    }
    buffer[buffer_position] = '\0';
    // puts("6");
    return buffer_position - 1;
}

uint32_t my_file_write(
    struct my_partition* partition, struct my_file* file,
    uint8_t* buffer, uint32_t buffer_size)
{
    uint8_t* current_block = my_get_block_pointer(partition, file->block);
    uint32_t tmp, buffer_position = 0;

    while (buffer_position < buffer_size)
    {
        if (file->block_position >= partition->block_size) // without / and %
        // if reached block ending then go to next block
        {
            uint32_t free_block = my_get_free_block(partition);
            if (free_block == 0 ||
                free_block >= partition->block_count) break; // no more blocks
            my_mark_block_used(partition, free_block);

            if ((tmp = file->position / partition->block_size) < NUM_OF_DIRECT_BLOCKS)
            {
                file->block = file->inode->direct_block[tmp] = free_block;
            }
            else if ((tmp -= NUM_OF_DIRECT_BLOCKS) <
                partition->block_size / sizeof(uint32_t))
            {
                if (tmp == 0)
                {
                    uint32_t free_block_indrect = my_get_free_block(partition);
                    if (free_block_indrect == 0 ||
                        free_block_indrect >= partition->block_count)
                    {
                        my_mark_block_unused(partition, free_block); // free pervious
                        break;
                    }
                    my_mark_block_used(partition, free_block_indrect);
                    file->inode->indirect_block = free_block_indrect;
                }
                file->block = ((uint32_t*) my_get_block_pointer(partition,
                    file->inode->indirect_block))[tmp] = free_block;
            }
            // else if ()
            // {}
            current_block = my_get_block_pointer(partition, file->block);
            file->block_position = 0;
        }

        current_block[file->block_position++] = buffer[buffer_position++];
        file->inode->size = ++(file->position);
    }
    return buffer_position;
}