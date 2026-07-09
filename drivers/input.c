#include <pureunix/arch.h>
#include <pureunix/input.h>
#include <pureunix/keyboard.h>

#define KEYBUF_SIZE 128

static int keybuf[KEYBUF_SIZE];
static volatile uint32_t key_head;
static volatile uint32_t key_tail;

void input_push_key(int key)
{
    if (key == KEY_NONE) {
        return;
    }
    uint32_t next = (key_head + 1) % KEYBUF_SIZE;
    if (next != key_tail) {
        keybuf[key_head] = key;
        key_head = next;
    }
}

int input_try_getkey(void)
{
    if (key_head == key_tail) {
        return KEY_NONE;
    }
    int key = keybuf[key_tail];
    key_tail = (key_tail + 1) % KEYBUF_SIZE;
    return key;
}

int input_getkey(void)
{
    int key;
    while ((key = input_try_getkey()) == KEY_NONE) {
        arch_halt();
    }
    return key;
}
