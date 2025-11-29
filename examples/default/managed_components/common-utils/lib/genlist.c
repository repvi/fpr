#include "lib/genlist.h"
#include "stdio.h"

void __list_add(struct list_head *new,
                struct list_head *prev,
                struct list_head *next)
{
    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}

void list_add(struct list_head *new_element, struct list_head *head)
{
    __list_add(new_element, head, head->next);
}

void list_add_tail(struct list_head *new_element, struct list_head *head)
{
    __list_add(new_element, head->prev, head);
}

void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    prev->next = next;
}

void list_del(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    entry->next = NULL;
    entry->prev = NULL;
}